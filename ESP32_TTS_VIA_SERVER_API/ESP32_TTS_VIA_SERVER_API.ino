#include <WiFi.h>
#include <driver/i2s.h>

// ─── הגדרות פינים לרמקול (MAX98357A) ───
#define PIN_BCLK     26
#define PIN_WS       25
#define PIN_SPK_DATA 14
#define PIN_SD_MODE  32 

// ─── הגדרות רשת ───
const char* ssid     = "hadas";
const char* password = "0523760404";

// ⚠️ ודא שזו אכן הכתובת המעודכנת
const char* server_ip = "10.100.102.30"; 
const int   server_port = 5000;

#define I2S_PORT I2S_NUM_0

// לקוח גלובלי לחיבור רציף
WiFiClient client;

void setupI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 24000,                         
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, 
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = PIN_BCLK,
        .ws_io_num = PIN_WS,
        .data_out_num = PIN_SPK_DATA,
        .data_in_num = I2S_PIN_NO_CHANGE 
    };

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_PORT);
}

void setup() {
    Serial.begin(115200);
    
    pinMode(PIN_SD_MODE, OUTPUT);
    digitalWrite(PIN_SD_MODE, HIGH);

    setupI2S();

    Serial.print("\nConnecting to WiFi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected!");
    Serial.print("ESP32 IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.println("\n>>> Ready! Type your prompt and press Enter <<<");
}

void sendToGeminiAndPlay(String prompt) {
    if (!client.connected()) {
        Serial.printf("[*] Connecting to Server at %s:%d...\n", server_ip, server_port);
        if (!client.connect(server_ip, server_port)) {
            Serial.println("[!] Connection failed. Is the Python server running?");
            return;
        }
    }

    client.print(prompt);
    Serial.println("[*] Receiving audio stream...");

    uint8_t buffer[1024];
    size_t bytes_written;
    
    // מנגנון חיפוש ברמת הבית (לא נכשל בגלל אפסים)
    const char* marker = "[END_OF_MESSAGE]";
    int markerLen = strlen(marker);
    int matchIndex = 0; 

    while (client.connected() || client.available()) {
        if (client.available()) {
            int len = client.read(buffer, sizeof(buffer));
            
            bool finished = false;
            int writeLen = len;

            for (int i = 0; i < len; i++) {
                if (buffer[i] == marker[matchIndex]) {
                    matchIndex++;
                    if (matchIndex == markerLen) {
                        finished = true;
                        writeLen = i - markerLen + 1; 
                        break;
                    }
                } else {
                    matchIndex = (buffer[i] == marker[0]) ? 1 : 0;
                }
            }

            if (writeLen > 0) {
                i2s_write(I2S_PORT, buffer, writeLen, &bytes_written, portMAX_DELAY);
            }

            if (finished) {
                Serial.println("\n[*] Finished playing answer.");
                break; // הלולאה עכשיו תסתיים בוודאות
            }
        }
    }
}

void loop() {
    if (Serial.available() > 0) {
        String prompt = Serial.readStringUntil('\n');
        prompt.trim(); 
        
        if (prompt.length() > 0) {
            Serial.printf("\n[You]: %s\n", prompt.c_str());
            sendToGeminiAndPlay(prompt);
            
            // תוספת: בקשה להזנת הפרומפט הבא בסוף התהליך
            Serial.println("\n>>> Ready for next prompt! Type here: <<<");
        }
    }
}