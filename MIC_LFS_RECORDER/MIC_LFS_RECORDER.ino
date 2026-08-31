/*
 * MIC_LFS_RECORDER — הקלטה לזיכרון LittleFS והשמעה
 * לוח : ESP32 WROOM 32D
 *
 *  INMP441 (מיקרופון)      ESP32
 *  SCK   → GPIO 26
 *  WS    → GPIO 25
 *  SD    → GPIO 34
 *  VDD   → 3.3V  |  GND → GND  |  L/R → GND
 *
 *  MAX98357A (רמקול)       ESP32
 *  BCLK  → GPIO 26  (משותף)
 *  LRC   → GPIO 25  (משותף)
 *  DIN   → GPIO 14
 *  SD_MODE → GPIO 21
 */

#include <driver/i2s.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>

// ─── פינים ───────────────────────────────────────────────────
#define PIN_BCLK      26
#define PIN_WS        25
#define PIN_MIC_DATA  34
#define PIN_SPK_DATA  14
#define PIN_SD_MODE   21

// ─── I2S ─────────────────────────────────────────────────────
#define I2S_PORT      I2S_NUM_0
#define SAMPLE_RATE   16000
#define BUF_SAMPLES   512

// ─── WiFi ─────────────────────────────────────────────────────
const char* WIFI_SSID = "testing123";
const char* WIFI_PASS = "blahblah";

// ─── הגדרות הקלטה ────────────────────────────────────────────
#define MAX_RECORDINGS   3         // מספר ההקלטות
#define MAX_REC_SECONDS  60        // מגבלת זמן — LittleFS קטן בהרבה מכרטיס SD
#define MIN_FREE_BYTES   4096      // עצור הקלטה כשנשאר פחות מזה פנוי

// ─── מצב מערכת ───────────────────────────────────────────────
enum State { IDLE, RECORDING, PLAYING };
volatile State  g_state    = IDLE;
volatile int    g_slot     = 0;    // חריץ נוכחי (1-3)
volatile int    g_micGain  = 2;
volatile int    g_spkGain  = 4;
char g_status[64]          = "מוכן";

int32_t   iBuf[BUF_SAMPLES];
int32_t   oBuf[BUF_SAMPLES];
WebServer server(80);

// ─── שמות קבצים ──────────────────────────────────────────────
String recFile(int slot) { return "/rec" + String(slot) + ".raw"; }

// ─── WAV header (16-bit PCM mono) ────────────────────────────
// קבצים נשמרים כ-RAW 32-bit ונמירים ל-16-bit בעת השמעה

// ─── דף ווב ───────────────────────────────────────────────────
static const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="he" dir="rtl">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>מקליט קול</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:Arial,sans-serif;background:#0f0f1a;color:#eee;
         display:flex;flex-direction:column;align-items:center;
         justify-content:center;min-height:100vh;gap:20px;padding:20px}
    h2{font-size:1.4em;letter-spacing:1px}

    /* בחירת חריץ */
    .slots{display:flex;gap:12px}
    .slot{width:80px;height:80px;border-radius:12px;border:2px solid #444;
          background:#1a1a2e;font-size:1.1em;font-weight:bold;cursor:pointer;
          display:flex;flex-direction:column;align-items:center;
          justify-content:center;gap:4px;transition:all 0.15s;color:#eee}
    .slot.active{border-color:#00e676;background:#003320}
    .slot.has-rec{border-color:#2196f3}
    .slot .dot{width:10px;height:10px;border-radius:50%;background:#444}
    .slot.has-rec .dot{background:#2196f3}
    .slot.active .dot{background:#00e676}

    /* כפתורי פעולה */
    .actions{display:flex;gap:14px}
    button{padding:12px 26px;border:none;border-radius:10px;font-size:1em;
           font-weight:bold;cursor:pointer;transition:opacity 0.15s}
    button:active{opacity:0.65}
    button:disabled{opacity:0.3;cursor:default}
    #btnRec  {background:#f44336;color:#fff}
    #btnStop {background:#ff9800;color:#000}
    #btnPlay {background:#00c853;color:#000}

    /* מד עוצמה */
    #meter{width:520px;background:#222;border-radius:10px;
           height:44px;overflow:hidden;border:1px solid #333}
    #bar{height:100%;width:0%;border-radius:10px;
         background:linear-gradient(90deg,#00e676 0%,#ffeb3b 65%,#f44336 100%);
         transition:width 0.1s ease}

    /* סליידרים */
    .sliders{display:flex;gap:40px}
    .sl-box{display:flex;flex-direction:column;align-items:center;gap:8px}
    .sl-box label{font-size:0.85em;color:#aaa}
    .sl-box .val{font-size:1.6em;font-weight:bold;color:#00e676}
    input[type=range]{-webkit-appearance:none;width:180px;height:7px;
                      background:#333;border-radius:4px;outline:none}
    input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;
      width:22px;height:22px;border-radius:50%;background:#00e676;cursor:pointer}
    #micSlider::-webkit-slider-thumb{background:#2196f3}

    #status{font-size:0.85em;color:#777;min-height:1.2em;text-align:center}
    .rec-dot{display:inline-block;width:10px;height:10px;border-radius:50%;
             background:#f44336;margin-left:6px;animation:blink 0.7s infinite}
    @keyframes blink{0%,100%{opacity:1}50%{opacity:0}}
  </style>
</head>
<body>
  <h2>🎙️ מקליט קול — LittleFS</h2>

  <div class="slots">
    <div class="slot active" id="slot1" onclick="selectSlot(1)">
      <div class="dot"></div><span>הקלטה 1</span>
    </div>
    <div class="slot" id="slot2" onclick="selectSlot(2)">
      <div class="dot"></div><span>הקלטה 2</span>
    </div>
    <div class="slot" id="slot3" onclick="selectSlot(3)">
      <div class="dot"></div><span>הקלטה 3</span>
    </div>
  </div>

  <div id="meter"><div id="bar"></div></div>

  <div class="actions">
    <button id="btnRec"  onclick="cmd('record')">⏺ הקלט</button>
    <button id="btnStop" onclick="cmd('stop')" disabled>⏹ עצור</button>
    <button id="btnPlay" onclick="cmd('play')">▶ השמע</button>
  </div>

  <div class="sliders">
    <div class="sl-box">
      <label>🎤 עוצמת מיקרופון</label>
      <div class="val" id="micVal">2</div>
      <input type="range" id="micSlider" min="1" max="10" value="2"
             oninput="setGain('mic',this.value)">
    </div>
    <div class="sl-box">
      <label>🔊 עוצמת השמעה</label>
      <div class="val" id="spkVal">4</div>
      <input type="range" id="spkSlider" min="1" max="10" value="4"
             oninput="setGain('spk',this.value)">
    </div>
  </div>

  <div id="status">מוכן</div>

<script>
let currentSlot = 1;
let hasRec = [false, false, false];
let updates = 0;

function selectSlot(n) {
  currentSlot = n;
  document.querySelectorAll('.slot').forEach((el,i) => {
    el.classList.toggle('active', i+1 === n);
  });
}

async function cmd(action) {
  await fetch('/cmd?a=' + action + '&slot=' + currentSlot);
}

async function setGain(who, val) {
  document.getElementById(who + 'Val').textContent = val;
  await fetch('/gain?who=' + who + '&v=' + val);
}

async function poll() {
  try {
    const d = await (await fetch('/data')).json();
    document.getElementById('bar').style.width = d.level + '%';

    // עדכון כפתורים
    const rec  = d.state === 'recording';
    const play = d.state === 'playing';
    document.getElementById('btnRec').disabled  = rec || play;
    document.getElementById('btnStop').disabled = !rec && !play;
    document.getElementById('btnPlay').disabled = rec || play;

    // עדכון חריצים — יש הקלטה?
    d.slots.forEach((has, i) => {
      document.getElementById('slot'+(i+1)).classList.toggle('has-rec', has);
    });

    // סטטוס
    let st = d.status;
    if (rec) st = '<span class="rec-dot"></span> ' + st;
    document.getElementById('status').innerHTML = st;

  } catch(e) {
    document.getElementById('status').textContent = 'שגיאת חיבור';
  }
}

setInterval(poll, 150);
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
        .tx_desc_auto_clear   = true,
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

// ─── בדיקה אם קיימת הקלטה בחריץ ─────────────────────────────
bool hasRecording(int slot) {
    return LittleFS.exists(recFile(slot).c_str());
}

// ─── נתבי HTTP ────────────────────────────────────────────────
void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleData() {
    String st = (g_state == RECORDING) ? "recording" :
                (g_state == PLAYING)   ? "playing"   : "idle";
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"level\":%d,\"status\":\"%s\","
             "\"slots\":[%s,%s,%s]}",
             st.c_str(), 0,
             g_status,
             hasRecording(1)?"true":"false",
             hasRecording(2)?"true":"false",
             hasRecording(3)?"true":"false");
    server.send(200, "application/json", buf);
}

void handleCmd() {
    String action = server.arg("a");
    int    slot   = constrain(server.arg("slot").toInt(), 1, MAX_RECORDINGS);

    if (action == "record" && g_state == IDLE) {
        g_slot  = slot;
        g_state = RECORDING;
        snprintf(g_status, sizeof(g_status), "מקליט חריץ %d...", slot);
    } else if (action == "stop") {
        g_state  = IDLE;
        strlcpy(g_status, "עצר", sizeof(g_status));
    } else if (action == "play" && g_state == IDLE) {
        if (hasRecording(slot)) {
            g_slot   = slot;
            g_state  = PLAYING;
            snprintf(g_status, sizeof(g_status), "משמיע חריץ %d...", slot);
        } else {
            snprintf(g_status, sizeof(g_status), "אין הקלטה בחריץ %d", slot);
        }
    }
    server.send(200, "text/plain", "ok");
}

void handleGain() {
    String who = server.arg("who");
    int    val = constrain(server.arg("v").toInt(), 1, 10);
    if (who == "mic") g_micGain = val;
    else              g_spkGain = val;
    server.send(200, "text/plain", "ok");
}

// ─── הקלטה לקובץ ─────────────────────────────────────────────
void doRecord(int slot) {
    LittleFS.remove(recFile(slot).c_str());
    File f = LittleFS.open(recFile(slot).c_str(), FILE_WRITE);
    if (!f) {
        strlcpy(g_status, "שגיאה: לא ניתן לפתוח קובץ", sizeof(g_status));
        g_state  = IDLE;
        return;
    }

    int recorded = 0;
    size_t bytesRead;

    Serial.printf("[REC] חריץ %d — עד %ds\n", slot, MAX_REC_SECONDS);

    while (g_state == RECORDING) {
        server.handleClient();

        if (recorded / SAMPLE_RATE >= MAX_REC_SECONDS) {
            snprintf(g_status, sizeof(g_status), "הגעת למגבלת הזמן (%ds)", MAX_REC_SECONDS);
            break;
        }
        if (LittleFS.totalBytes() - LittleFS.usedBytes() < MIN_FREE_BYTES) {
            strlcpy(g_status, "האחסון מלא", sizeof(g_status));
            break;
        }

        i2s_read(I2S_PORT, iBuf, sizeof(iBuf), &bytesRead, 100);
        int n = bytesRead / sizeof(int32_t);

        // שמור 16-bit (חיסכון במקום) — הסט ב-8 + הגברת מיקרופון
        int micG = g_micGain;
        bool writeFailed = false;
        for (int i = 0; i < n; i++) {
            int32_t s24 = iBuf[i] >> 8;
            int64_t amp = (int64_t)s24 * micG;
            amp = constrain(amp, -8388607LL, 8388607LL);
            int16_t s16 = (int16_t)(amp >> 8);    // 24-bit → 16-bit
            if (f.write((uint8_t*)&s16, 2) != 2) { writeFailed = true; break; }
        }
        recorded += n;
        if (writeFailed) {
            strlcpy(g_status, "האחסון מלא", sizeof(g_status));
            break;
        }

        // מד עוצמה בסריאל
        int32_t peak = 0;
        for (int i = 0; i < n; i++) {
            int32_t a = abs(iBuf[i] >> 8);
            if (a > peak) peak = a;
        }
        int lvl = constrain(map(peak, 0, 800000, 0, 40), 0, 40);
        Serial.printf("\r[REC] |");
        for (int i = 0; i < 40; i++) Serial.print(i < lvl ? "=" : " ");
        Serial.printf("| %ds", recorded / SAMPLE_RATE);
    }

    f.close();
    Serial.printf("\n[REC] נשמר: %s (%d דגימות)\n",
                  recFile(slot).c_str(), recorded);
    if (g_state == RECORDING) snprintf(g_status, sizeof(g_status), "נשמר! חריץ %d", slot);
    g_state  = IDLE;
}

// ─── השמעה מקובץ ─────────────────────────────────────────────
void doPlay(int slot) {
    File f = LittleFS.open(recFile(slot).c_str(), FILE_READ);
    if (!f) {
        strlcpy(g_status, "שגיאה: קובץ לא נמצא", sizeof(g_status));
        g_state  = IDLE;
        return;
    }

    int spkG = g_spkGain;
    size_t written;
    int16_t s16buf[BUF_SAMPLES];

    Serial.printf("[PLAY] חריץ %d — %lu bytes\n", slot, f.size());

    while (g_state == PLAYING && f.available() >= 2) {
        server.handleClient();

        int n = f.read((uint8_t*)s16buf, BUF_SAMPLES * 2) / 2;
        for (int i = 0; i < n; i++) {
            int64_t amp = (int64_t)s16buf[i] * spkG;
            amp = constrain(amp, -32767LL, 32767LL);
            oBuf[i] = (int32_t)((int16_t)amp) << 16;  // 16-bit → 32-bit frame
        }
        i2s_write(I2S_PORT, oBuf, n * sizeof(int32_t), &written, portMAX_DELAY);
    }

    f.close();
    // שתיקה קצרה בסוף
    memset(oBuf, 0, sizeof(oBuf));
    i2s_write(I2S_PORT, oBuf, sizeof(oBuf), &written, portMAX_DELAY);

    Serial.println("[PLAY] סיום");
    strlcpy(g_status, "הסתיים", sizeof(g_status));
    g_state  = IDLE;
}

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n==============================");
    Serial.println("  MIC LFS RECORDER");
    Serial.println("==============================");

    pinMode(PIN_SD_MODE, OUTPUT);
    digitalWrite(PIN_SD_MODE, HIGH);

    // LittleFS — true = פרמט אוטומטי אם ה-mount הראשון נכשל
    if (!LittleFS.begin(true)) {
        Serial.println("  [!] LittleFS נכשל");
    } else {
        Serial.println("  LittleFS: OK");
    }

    setupI2S();
    Serial.printf("  I2S OK — BCLK=%d WS=%d MIC=%d SPK=%d\n",
                  PIN_BCLK, PIN_WS, PIN_MIC_DATA, PIN_SPK_DATA);

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
        server.on("/cmd",  handleCmd);
        server.on("/gain", handleGain);
        server.begin();
        Serial.println("  Web server: OK");
    } else {
        Serial.println("\n  WiFi נכשל — סריאל בלבד");
    }
    Serial.println("==============================");
}

void loop() {
    server.handleClient();

    if      (g_state == RECORDING) doRecord(g_slot);
    else if (g_state == PLAYING)   doPlay(g_slot);
    else    delay(10);
}
