/*
 * MIC_LFS_RECORDER — recording to LittleFS storage and playback
 * Board : ESP32 WROOM 32D
 *
 *  INMP441 (microphone)    ESP32
 *  SCK   → GPIO 26
 *  WS    → GPIO 25
 *  SD    → GPIO 34
 *  VDD   → 3.3V  |  GND → GND  |  L/R → GND
 *
 *  MAX98357A (speaker)     ESP32
 *  BCLK  → GPIO 26  (shared)
 *  LRC   → GPIO 25  (shared)
 *  DIN   → GPIO 14
 *  SD_MODE → GPIO 21
 */

#include <driver/i2s.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include "freertos/stream_buffer.h"

#include "html_page.h"

// ─── Pins ────────────────────────────────────────────────────
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

// ─── Recording settings ──────────────────────────────────────
#define MAX_RECORDINGS   3         // number of recordings
#define MAX_REC_SECONDS  60        // time limit — LittleFS is much smaller than an SD card
#define MIN_FREE_BYTES   4096      // stop recording once free space drops below this

// ─── System state ────────────────────────────────────────────
enum State { IDLE, RECORDING, PLAYING };
volatile State  g_state    = IDLE;
volatile int    g_slot     = 0;    // current slot (1-3)
volatile int    g_micGain  = 2;
volatile int    g_spkGain  = 4;
char g_status[64]          = "מוכן";

int32_t   oBuf[BUF_SAMPLES];
WebServer server(80);

// ─── Timing buffer between I2S (real-time) and flash (variable-time) ───
#define AUDIO_SB_BYTES     (32 * 1024)   // ~1s @16kHz/16-bit — absorbs flash delays
StreamBufferHandle_t        g_audioSB      = nullptr;
TaskHandle_t                g_audioTask    = nullptr;
volatile bool                g_audioTaskRun  = false;
volatile bool                g_audioTaskDone = false;
volatile int32_t              g_recPeak       = 0;   // level meter for serial output

// ─── File names ──────────────────────────────────────────────
String recFile(int slot) { return "/rec" + String(slot) + ".raw"; }

// ─── WAV header (16-bit PCM mono) ────────────────────────────
// Files are stored as 32-bit RAW and converted to 16-bit during playback



// ─── I2S full-duplex setup ────────────────────────────────────
void setupI2S() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 16,   // extra margin to absorb flash write delays
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

// ─── Check whether a slot has a recording ────────────────────
bool hasRecording(int slot) {
    return LittleFS.exists(recFile(slot).c_str());
}

// ─── HTTP routes ──────────────────────────────────────────────
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

// ─── Dedicated task: reads I2S at a fixed rate, no flash access ───
void i2sRecordTask(void *pv) {
    static int32_t rawBuf[BUF_SAMPLES];
    static int16_t s16Buf[BUF_SAMPLES];
    size_t bytesRead;

    while (g_audioTaskRun) {
        i2s_read(I2S_PORT, rawBuf, sizeof(rawBuf), &bytesRead, pdMS_TO_TICKS(100));
        int n = bytesRead / sizeof(int32_t);
        if (n == 0) continue;

        // store as 16-bit (saves space) — shift by 8 + apply mic gain
        int micG = g_micGain;
        int32_t peak = 0;
        for (int i = 0; i < n; i++) {
            int32_t s24 = rawBuf[i] >> 8;
            int64_t amp = (int64_t)s24 * micG;
            amp = constrain(amp, -8388607LL, 8388607LL);
            s16Buf[i] = (int16_t)(amp >> 8);    // 24-bit → 16-bit
            int32_t a = abs(s24);
            if (a > peak) peak = a;
        }
        g_recPeak = peak;

        // blocks until there's room in the buffer — so flash write delay doesn't drop samples
        xStreamBufferSend(g_audioSB, s16Buf, n * 2, pdMS_TO_TICKS(500));
    }
    g_audioTaskDone = true;
    vTaskDelete(NULL);
}

// ─── Recording to a file ──────────────────────────────────────
void doRecord(int slot) {
    LittleFS.remove(recFile(slot).c_str());
    File f = LittleFS.open(recFile(slot).c_str(), FILE_WRITE);
    if (!f) {
        strlcpy(g_status, "שגיאה: לא ניתן לפתוח קובץ", sizeof(g_status));
        g_state  = IDLE;
        return;
    }

    g_audioSB      = xStreamBufferCreate(AUDIO_SB_BYTES, 1);
    g_audioTaskRun  = true;
    g_audioTaskDone = false;
    xTaskCreatePinnedToCore(i2sRecordTask, "i2sRec", 4096, NULL,
                             configMAX_PRIORITIES - 2, &g_audioTask, 0);

    int recorded = 0;
    uint8_t chunk[1024];
    uint32_t lastPoll = 0;

    Serial.printf("[REC] חריץ %d — עד %ds\n", slot, MAX_REC_SECONDS);

    while (g_state == RECORDING) {
        uint32_t now = millis();
        if (now - lastPoll >= 20) { server.handleClient(); lastPoll = now; }

        if (recorded / SAMPLE_RATE >= MAX_REC_SECONDS) {
            snprintf(g_status, sizeof(g_status), "הגעת למגבלת הזמן (%ds)", MAX_REC_SECONDS);
            break;
        }
        if (LittleFS.totalBytes() - LittleFS.usedBytes() < MIN_FREE_BYTES) {
            strlcpy(g_status, "האחסון מלא", sizeof(g_status));
            break;
        }

        // drains the audio buffer and writes to flash — delay here no longer affects I2S
        size_t got = xStreamBufferReceive(g_audioSB, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        if (got > 0) {
            if (f.write(chunk, got) != got) {
                strlcpy(g_status, "האחסון מלא", sizeof(g_status));
                break;
            }
            recorded += got / 2;
        }

        int lvl = constrain(map(g_recPeak, 0, 800000, 0, 40), 0, 40);
        Serial.printf("\r[REC] |");
        for (int i = 0; i < 40; i++) Serial.print(i < lvl ? "=" : " ");
        Serial.printf("| %ds", recorded / SAMPLE_RATE);
    }

    // stop the I2S task and drain whatever remains in the buffer before closing the file
    g_audioTaskRun = false;
    uint32_t waitStart = millis();
    while (!g_audioTaskDone && millis() - waitStart < 1000) delay(5);
    size_t got;
    while ((got = xStreamBufferReceive(g_audioSB, chunk, sizeof(chunk), 0)) > 0) {
        f.write(chunk, got);
        recorded += got / 2;
    }
    vStreamBufferDelete(g_audioSB);
    g_audioSB = nullptr;

    f.close();
    Serial.printf("\n[REC] נשמר: %s (%d דגימות)\n",
                  recFile(slot).c_str(), recorded);
    if (g_state == RECORDING) snprintf(g_status, sizeof(g_status), "נשמר! חריץ %d", slot);
    g_state  = IDLE;
}

// ─── Dedicated task: writes I2S at a fixed rate, no flash access ───
void i2sPlayTask(void *pv) {
    static int16_t s16buf[BUF_SAMPLES];
    static int32_t outBuf[BUF_SAMPLES];
    size_t written;

    while (g_audioTaskRun) {
        size_t got = xStreamBufferReceive(g_audioSB, s16buf, sizeof(s16buf), pdMS_TO_TICKS(100));
        int n = got / 2;
        if (n == 0) continue;

        int spkG = g_spkGain;
        for (int i = 0; i < n; i++) {
            int64_t amp = (int64_t)s16buf[i] * spkG;
            amp = constrain(amp, -32767LL, 32767LL);
            outBuf[i] = (int32_t)((int16_t)amp) << 16;  // 16-bit → 32-bit frame
        }
        i2s_write(I2S_PORT, outBuf, n * sizeof(int32_t), &written, portMAX_DELAY);
    }
    g_audioTaskDone = true;
    vTaskDelete(NULL);
}

// ─── Playback from a file ─────────────────────────────────────
void doPlay(int slot) {
    File f = LittleFS.open(recFile(slot).c_str(), FILE_READ);
    if (!f) {
        strlcpy(g_status, "שגיאה: קובץ לא נמצא", sizeof(g_status));
        g_state  = IDLE;
        return;
    }

    g_audioSB      = xStreamBufferCreate(AUDIO_SB_BYTES, 1);
    g_audioTaskRun  = true;
    g_audioTaskDone = false;
    xTaskCreatePinnedToCore(i2sPlayTask, "i2sPlay", 4096, NULL,
                             configMAX_PRIORITIES - 2, &g_audioTask, 0);

    size_t written;
    uint8_t chunk[1024];
    uint32_t lastPoll = 0;

    Serial.printf("[PLAY] חריץ %d — %lu bytes\n", slot, f.size());

    // pre-loads the buffer from flash — read delays no longer cause playback gaps
    while (g_state == PLAYING && f.available() >= 2) {
        uint32_t now = millis();
        if (now - lastPoll >= 20) { server.handleClient(); lastPoll = now; }

        size_t toRead = min((size_t)sizeof(chunk), (size_t)f.available());
        toRead -= toRead % 2;
        if (toRead == 0) break;
        int got = f.read(chunk, toRead);
        xStreamBufferSend(g_audioSB, chunk, got, portMAX_DELAY);
    }

    // wait until the task has played everything already loaded into the buffer
    while (g_state == PLAYING && xStreamBufferBytesAvailable(g_audioSB) > 0) delay(10);

    g_audioTaskRun = false;
    uint32_t waitStart = millis();
    while (!g_audioTaskDone && millis() - waitStart < 1000) delay(5);
    vStreamBufferDelete(g_audioSB);
    g_audioSB = nullptr;

    f.close();
    // brief silence at the end
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

    // LittleFS — true = auto-format if the first mount fails
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
