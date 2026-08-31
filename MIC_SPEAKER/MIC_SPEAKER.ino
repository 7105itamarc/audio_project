/*
 * MIC_SPEAKER — INMP441 + MAX98357A Full-Duplex I2S
 * לוח  : ESP32 WROOM 32D
 *
 *  רכיב       פין      GPIO
 *  ─────────────────────────────────────────
 *  BCLK     (משותף)  →  GPIO 26
 *  WS/LRC   (משותף)  →  GPIO 25
 *  INMP441  SD/DATA  →  GPIO 34  (קלט בלבד)
 *  MAX98357 DIN      →  GPIO 14  (פלט)
 *  MAX98357 SD_MODE  →  GPIO 21  (HIGH=הפעל, LOW=כבה)
 *
 *  MAX98357A:  VIN→5V   GND→GND
 *  INMP441 :  VDD→3.3V  GND→GND  L/R→GND
 */

#include <driver/i2s.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

// ─── פינים ───────────────────────────────────────────────────
#define PIN_BCLK        26
#define PIN_WS          25
#define PIN_MIC_DATA    34   // INMP441 → ESP32 (קלט)
#define PIN_SPK_DATA    14   // ESP32 → MAX98357A (פלט)
#define PIN_SD_MODE     21   // MAX98357A SD_MODE

// ─── I2S ─────────────────────────────────────────────────────
#define I2S_PORT        I2S_NUM_0
#define SAMPLE_RATE     16000
#define BUFFER_SAMPLES  256

// ─── WiFi ─────────────────────────────────────────────────────
const char* WIFI_SSID = "testing123";
const char* WIFI_PASS = "blahblah";

// ─── גלובליים ─────────────────────────────────────────────────
int32_t     rxBuf[BUFFER_SAMPLES];
int32_t     txBuf[BUFFER_SAMPLES];
WebServer   server(80);

volatile int32_t g_peak  = 0;
volatile int32_t g_rms   = 0;
volatile int     g_level = 0;

// מצב הפעלה: LOOPBACK=מיקרופון לרמקול / TONE=צליל קבוע
enum Mode { LOOPBACK, TONE };
volatile Mode g_mode = LOOPBACK;

// עוצמת קול — הגדל את המספר לעוצמה גבוהה יותר (1=רגיל, 8=x8)
#define VOLUME_GAIN     8

// ─── דף ווב ───────────────────────────────────────────────────
static const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="he" dir="rtl">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>MIC + SPEAKER</title>
  <style>
    * { box-sizing:border-box; margin:0; padding:0; }
    body {
      font-family: Arial, sans-serif;
      background: #0f0f1a; color: #eee;
      display: flex; flex-direction: column;
      align-items: center; justify-content: center;
      height: 100vh; gap: 18px;
    }
    h2 { font-size:1.4em; }
    #meter { width:560px; background:#222; border-radius:10px;
             height:46px; overflow:hidden; border:1px solid #444; }
    #bar   { height:100%; width:0%;
             background:linear-gradient(90deg,#00e676 0%,#ffeb3b 60%,#f44336 100%);
             transition:width 0.12s ease; border-radius:10px; }
    #stats { display:flex; gap:36px; }
    .stat label { color:#888; font-size:0.82em; display:block; text-align:center; }
    .stat span  { font-size:1.35em; font-weight:bold; display:block; text-align:center; }
    .btn-row { display:flex; gap:16px; }
    button {
      padding:10px 28px; border:none; border-radius:8px;
      font-size:1em; cursor:pointer; font-weight:bold;
      transition: opacity 0.2s;
    }
    #btnLoop { background:#00c853; color:#000; }
    #btnTone { background:#2196f3; color:#fff; }
    #btnMute { background:#f44336; color:#fff; }
    button:active { opacity:0.7; }
    #status { font-size:0.82em; color:#555; }
  </style>
</head>
<body>
  <h2>🎤 INMP441 + MAX98357A 🔊</h2>
  <div id="meter"><div id="bar"></div></div>
  <div id="stats">
    <div class="stat"><label>רמה</label><span id="lvl">0</span>%</div>
    <div class="stat"><label>Peak</label><span id="peak">0</span></div>
    <div class="stat"><label>RMS</label><span id="rms">0.0</span></div>
  </div>
  <div class="btn-row">
    <button id="btnLoop" onclick="setMode('loopback')">🎤 → 🔊 Loopback</button>
    <button id="btnTone" onclick="setMode('tone')">♪ טון 1kHz</button>
    <button id="btnMute" onclick="setMode('mute')">🔇 השתק</button>
  </div>
  <div id="status">מתחבר...</div>
<script>
let updates = 0;
async function poll() {
  try {
    const d = await (await fetch('/data')).json();
    document.getElementById('bar').style.width  = d.level + '%';
    document.getElementById('lvl').textContent  = d.level;
    document.getElementById('peak').textContent = d.peak;
    document.getElementById('rms').textContent  = (d.rms/1000).toFixed(1);
    document.getElementById('status').textContent =
      'פעיל ✓  מצב: ' + d.mode + '  עדכונים: ' + (++updates);
  } catch(e) {
    document.getElementById('status').textContent = 'שגיאת חיבור...';
  }
}
async function setMode(m) {
  await fetch('/mode?v=' + m);
}
setInterval(poll, 120);
poll();
</script>
</body>
</html>
)HTML";

// ─── I2S Full-Duplex ──────────────────────────────────────────
void setupI2S() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 64,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,   // מנע רעשים כשאין נתונים
        .fixed_mclk           = 0
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = PIN_BCLK,
        .ws_io_num    = PIN_WS,
        .data_out_num = PIN_SPK_DATA,
        .data_in_num  = PIN_MIC_DATA
    };
    i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);
}

// ─── צליל ים סינתטי ──────────────────────────────────────────
// רעש ורוד + מודולציית גל איטית = תחושת גלי ים
void playOceanSound(int durationMs) {
    const int CHUNK = 128;
    int32_t   buf[CHUNK];
    size_t    written;

    // מסנן IIR פשוט לרעש ורוד (מדמה גלים)
    float b0 = 0.98f, prev = 0.0f;
    // פרמטרי גל איטי (תנודת אמפליטודה ~0.15 Hz = גל כל ~7 שניות)
    float wavePhase = 0.0f;
    float waveStep  = 2.0f * M_PI * 0.15f / SAMPLE_RATE;
    // פרמטרי "שבירת גל" גבוה (200 Hz)
    float crackPhase = 0.0f;
    float crackStep  = 2.0f * M_PI * 200.0f / SAMPLE_RATE;

    int totalSamples = (SAMPLE_RATE / 1000) * durationMs;
    int produced     = 0;

    while (produced < totalSamples) {
        int n = min(CHUNK, totalSamples - produced);
        for (int i = 0; i < n; i++) {
            // רעש לבן → רעש ורוד (IIR)
            float white = ((float)esp_random() / (float)UINT32_MAX) * 2.0f - 1.0f;
            prev  = b0 * prev + (1.0f - b0) * white;

            // גל ים איטי (עלייה + ירידה חלקה)
            float wave = 0.5f + 0.5f * sinf(wavePhase);
            wavePhase += waveStep;

            // שבירת גל — קצת תוכן גבוה בפסגות הגל
            float crack = sinf(crackPhase) * wave * wave * 0.15f;
            crackPhase += crackStep;

            // מיזוג: רעש ורוד עם תנודת גל + שבירה
            float s = (prev * 0.85f + crack) * wave * 0.9f;

            buf[i] = (int32_t)(s * (float)0x5FFFFFFF);
        }
        i2s_write(I2S_PORT, buf, n * sizeof(int32_t), &written, portMAX_DELAY);
        produced += n;
    }
    // דהייה קצרה בסוף
    int fade = SAMPLE_RATE / 4;  // 250ms
    produced = 0;
    prev = 0.0f;
    while (produced < fade) {
        int n = min(CHUNK, fade - produced);
        float ratio = 1.0f - (float)produced / fade;
        for (int i = 0; i < n; i++) {
            float white = ((float)esp_random() / (float)UINT32_MAX) * 2.0f - 1.0f;
            prev  = b0 * prev + (1.0f - b0) * white;
            buf[i] = (int32_t)(prev * ratio * (float)0x3FFFFFFF);
        }
        i2s_write(I2S_PORT, buf, n * sizeof(int32_t), &written, portMAX_DELAY);
        produced += n;
    }
}

// ─── יצירת גל סינוס 1kHz ─────────────────────────────────────
static float tonePhase = 0.0f;
void fillTone(int32_t* buf, int samples, float freqHz) {
    float step = 2.0f * M_PI * freqHz / SAMPLE_RATE;
    for (int i = 0; i < samples; i++) {
        buf[i] = (int32_t)(sinf(tonePhase) * 0x7FFFFF00);  // 100% amplitude
        tonePhase += step;
        if (tonePhase > 2.0f * M_PI) tonePhase -= 2.0f * M_PI;
    }
}

// ─── מד סריאל ────────────────────────────────────────────────
void printBar(int lvl, int32_t peak) {
    int bars = map(lvl, 0, 100, 0, 40);
    Serial.print("|");
    for (int i = 0; i < 40; i++) {
        if (i < bars) Serial.print(i < 16 ? "=" : (i < 30 ? "+" : "!"));
        else          Serial.print(" ");
    }
    Serial.printf("| %3d%%  peak=%-8d\n", lvl, (int)peak);
}

// ─── נתבי HTTP ───────────────────────────────────────────────
void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleData() {
    const char* modeStr = (g_mode == LOOPBACK) ? "loopback" :
                          (g_mode == TONE)     ? "tone"     : "mute";
    char buf[100];
    snprintf(buf, sizeof(buf),
             "{\"level\":%d,\"peak\":%d,\"rms\":%d,\"mode\":\"%s\"}",
             g_level, (int)g_peak, (int)g_rms, modeStr);
    server.send(200, "application/json", buf);
}

void handleMode() {
    String v = server.arg("v");
    if      (v == "loopback") { g_mode = LOOPBACK; digitalWrite(PIN_SD_MODE, HIGH); }
    else if (v == "tone")     { g_mode = TONE;     digitalWrite(PIN_SD_MODE, HIGH); }
    else                      {                     digitalWrite(PIN_SD_MODE, LOW);  }
    server.send(200, "text/plain", "ok");
}

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n==============================");
    Serial.println("  INMP441 + MAX98357A I2S");
    Serial.println("==============================");

    // SD_MODE — הפעלת המגבר
    pinMode(PIN_SD_MODE, OUTPUT);
    digitalWrite(PIN_SD_MODE, HIGH);
    Serial.println("  MAX98357A: ON");

    setupI2S();
    Serial.printf("  I2S Full-Duplex OK\n");
    Serial.printf("  BCLK=%d  WS=%d  MIC=%d  SPK=%d\n",
                  PIN_BCLK, PIN_WS, PIN_MIC_DATA, PIN_SPK_DATA);

    // בדיקת רמקול — צליל ים לפני חיבור WiFi
    Serial.println("  Speaker test: Ocean sound (4 sec)...");
    playOceanSound(4000);
    Serial.println("  Speaker test: Done");

    // WiFi
    Serial.printf("  WiFi: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
        delay(400); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n  IP: http://%s\n", WiFi.localIP().toString().c_str());
        server.on("/",     handleRoot);
        server.on("/data", handleData);
        server.on("/mode", handleMode);
        server.begin();
        Serial.println("  Web server: OK");
    } else {
        Serial.println("\n  WiFi נכשל — סריאל בלבד");
    }
    Serial.println("==============================");
    Serial.println("|===16===|+++30+++|!40!| lvl  peak");
}

void loop() {
    server.handleClient();

    // ── קריאה מהמיקרופון ──
    size_t bytesRead = 0;
    i2s_read(I2S_PORT, rxBuf, sizeof(rxBuf), &bytesRead, portMAX_DELAY);
    int samples = bytesRead / sizeof(int32_t);

    // חישוב Peak ו-RMS
    int32_t peak = 0;
    int64_t sumSq = 0;
    for (int i = 0; i < samples; i++) {
        int32_t s = rxBuf[i] >> 8;
        int32_t a = abs(s);
        if (a > peak) peak = a;
        sumSq += (int64_t)s * s;
    }
    int32_t rms = (samples > 0) ? (int32_t)(sqrt((double)sumSq / samples) * 1000.0) : 0;
    int     lvl = constrain(map(peak, 0, 800000, 0, 100), 0, 100);
    g_peak = peak; g_rms = rms; g_level = lvl;

    // ── כתיבה לרמקול לפי מצב ──
    size_t bytesWritten = 0;
    Mode m = g_mode;
    if (m == LOOPBACK) {
        // הגבר את הסיגנל לפני שליחה לרמקול
        for (int i = 0; i < samples; i++) {
            int64_t s = (int64_t)rxBuf[i] * VOLUME_GAIN;
            txBuf[i] = (int32_t)constrain(s, INT32_MIN, INT32_MAX);
        }
        i2s_write(I2S_PORT, txBuf, samples * sizeof(int32_t), &bytesWritten, portMAX_DELAY);
    } else if (m == TONE) {
        fillTone(txBuf, samples, 1000.0f);
        i2s_write(I2S_PORT, txBuf, samples * sizeof(int32_t), &bytesWritten, portMAX_DELAY);
    } else {
        // Mute — שלח אפסים
        memset(txBuf, 0, samples * sizeof(int32_t));
        i2s_write(I2S_PORT, txBuf, samples * sizeof(int32_t), &bytesWritten, portMAX_DELAY);
    }

    printBar(lvl, peak);
}
