/****************************************************
 *  Project   : ESP32 AI Smart Home (voice -> hardware)
 *  Board     : ESP32-S3 / ESP32-C3 / ESP32 CLASSIC
 *  Based on  : ESP32_AI_TTS_ON_BOARD_TEST (OceanLabz)
 *
 *  Description:
 *  --------------------------------------------------------
 *  - Records voice using INMP441 I2S microphone
 *  - Converts speech to text using OpenAI Whisper
 *  - Sends text to ChatGPT, which returns a structured JSON intent:
 *      {"type":"command|chat","device":"led|none",
 *       "action":"on|off|toggle|none","reply":"..."}
 *  - Executes commands on hardware (on-board LED)
 *  - Speaks the reply using ElevenLabs via external I2S DAC
 *
 *  Credentials live in secrets.h (git-ignored).
 *  See secrets.example.h for the template.
 ****************************************************/
// ==================== INCLUDES ====================
// ````````  
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <FS.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <ArduinoHttpClient.h>

// Audio libraries for MP3 decoding:
// included in external arduino libraries as follows:
// ArduinoJson - version: 7.4.3
// ArduinoHttpClient - version: 0.6.1
// ESP8266Audio - version: 1.9.7
#include "AudioFileSourceSD.h"  
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#include "secrets.h"


// ===== SELECT TARGET BOARD (uncomment ONE only) =====
// #define BOARD_ESP32_S3
#define BOARD_ESP32
// #define BOARD_ESP32_C3

// ==================== PIN DEFINITIONS ====================

#if defined(BOARD_ESP32_S3)

  // ----- SD Card -----
  #define SD_CS       42
  #define SD_MISO     46
  #define SD_SCLK     2
  #define SD_MOSI     3

  // ----- External DAC (I2S Output) -----
  #define I2S_BCLK    48
  #define I2S_LRC     21
  #define I2S_DOUT    47

  // ----- INMP441 Microphone (I2S Input) -----
  #define I2S_MIC_SERIAL_CLOCK     40
  #define I2S_MIC_LEFT_RIGHT_CLOCK 39
  #define I2S_MIC_SERIAL_DATA      41

  // ----- On-board LED -----
  // TODO: DevKitC-S3 RGB LED is on GPIO 48 which is used by I2S_BCLK here,
  // and GPIO 2 is SD_SCLK. Pick a free pin before using this board.
  #define LED_PIN          -1
  #define LED_ACTIVE_HIGH  1


#elif defined(BOARD_ESP32)

  // ----- SD Card -----
  #define SD_CS       5
  #define SD_MISO     19
  #define SD_SCLK     18
  #define SD_MOSI     23

  // ----- External DAC (I2S Output) -----
  #define I2S_BCLK    26
  #define I2S_LRC     25
  #define I2S_DOUT    22

  // ----- INMP441 Microphone (I2S Input) -----
  #define I2S_MIC_SERIAL_CLOCK     26
  #define I2S_MIC_LEFT_RIGHT_CLOCK 25
  #define I2S_MIC_SERIAL_DATA      21

  // ----- On-board LED -----
  // NOTE: GPIO 2 is a strapping pin - no external pull-up.
  #define LED_PIN          2
  #define LED_ACTIVE_HIGH  1


#elif defined(BOARD_ESP32_C3)

  // ----- SD Card -----
  #define SD_CS       10
  #define SD_MISO     4
  #define SD_SCLK     6
  #define SD_MOSI     7

  // ----- External DAC (I2S Output) -----
  #define I2S_BCLK    8
  #define I2S_LRC     9
  #define I2S_DOUT    18

  // ----- INMP441 Microphone (I2S Input) -----
  #define I2S_MIC_SERIAL_CLOCK     5
  #define I2S_MIC_LEFT_RIGHT_CLOCK 3
  #define I2S_MIC_SERIAL_DATA      2

  // ----- On-board LED -----
  // TODO: GPIO 2 is used by the microphone here. Pick a free pin before using this board.
  #define LED_PIN          -1
  #define LED_ACTIVE_HIGH  1


#else
    #error " No board selected! Please define BOARD_ESP32_S3, BOARD_ESP32, or BOARD_ESP32_C3"
#endif


// Recording settings
#define RECORDING_DURATION 5            // seconds
#define SAMPLE_RATE 16000               // Hz
#define BUFFER_SIZE 1024

// ==================== CONFIGURATION ====================
// Values come from secrets.h (git-ignored)
const char* ssid     = "hadas";
const char* password = "0523760404";

const char* openaiApiKey = "OPEN_AI_KEY";
const char* elevenLabsApiKey = "eleven labs api key";

// API URLs
const char* openaiChatUrl = "https://api.openai.com/v1/chat/completions";
const char* openaiTtsUrl = "https://api.openai.com/v1/audio/speech";
const char* openaiSttUrl = "https://api.openai.com/v1/audio/transcriptions";
const char* elevenLabsTtsUrl = "https://api.elevenlabs.io/v1/text-to-speech/";

const char* voiceId = "ELEVEN_LABS_VOID_ID (BELLA)";

// ==================== TYPES ====================

/**
 * @brief Structured intent returned by ChatGPT.
 */
struct Intent {
  String type;    // "command" | "chat"
  String device;  // "led" | "none"
  String action;  // "on" | "off" | "toggle" | "none"
  String reply;   // Short spoken confirmation / answer
  bool valid = false;
};

// ==================== GLOBALS ====================
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S *out = nullptr;

i2s_config_t i2s_mic_config;
i2s_pin_config_t i2s_mic_pins;

bool gettingResponse = false;
bool recordingMode = false;
bool ledState = false;

// Moment the user input ended (recording window closed / AI: text entered).
// Used to print end-to-end latency.
unsigned long recordingEndTime = 0;

/**
 * @brief Arduino setup function.
 *
 * Initializes Serial communication, SD card, WiFi connection,
 * I2S microphone configuration, and performs microphone diagnostics.
 * Also prints available user commands.
 */

void setup() {
  delay(500);
  Serial.begin(115200);
  delay(500);
  Serial.println("AI Smart Home on ESP32");

  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    setLed(false);
  }

  // Initialize SD card
  if (!setupSDCard()) {
    Serial.println("SD Card initialization failed!");
    while(1);
  }
  
  // Connect to WiFi
  connectToWiFi();
  
  // Initialize I2S microphone
  setupI2SMicrophone();
  
  // Run diagnostics
  testMicrophone();
  testMicrophoneDetailed();
  
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  
  Serial.println("\n=== ESP32 AI Smart Home ===");
  Serial.println("Commands:");
  Serial.println("1. Type 'RECORD' to speak a command or question");
  Serial.println("2. Type 'AI:your sentence' to test intent extraction without the mic");
  Serial.println("3. Type 'ON' / 'OFF' / 'TOGGLE' / 'STATUS' for direct LED control");
  Serial.println("4. Type 'TTS:your text' for direct TTS");
  Serial.println("5. Type any other text and press Enter for TTS");
}

/**
 * @brief Main Arduino loop.
 *
 * Handles MP3 audio playback, processes serial commands,
 * and keeps the system responsive.
 */

void loop() {
  // Handle audio playback
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      Serial.println("Playback finished");
      cleanupAudio();
    }
  }
  
  // Handle serial commands
  handleSerialCommands();
  
  delay(10);
}

/**
 * @brief Handles user commands received via Serial.
 *
 * Supports:
 * - RECORD command for voice input
 * - AI:text to run intent extraction on typed text
 * - ON / OFF / TOGGLE / STATUS for direct LED control
 * - TTS:text for direct text-to-speech
 * - Plain text input for spoken output
 */

void handleSerialCommands() {
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();

    if (command.length() > 0) {
      String upper = command;
      upper.toUpperCase();

      if (upper == "RECORD") {
        startRecordingAndTranscription();
      } else if (upper == "ON" || upper == "OFF" || upper == "TOGGLE") {
        executeCommand("led", upper);
      } else if (upper == "STATUS") {
        Serial.printf("LED is %s\n", ledState ? "ON" : "OFF");
      } else if (upper.startsWith("AI:")) {
        String aiText = command.substring(3);
        aiText.trim();
        if (aiText.length() > 0) {
          recordingEndTime = millis();
          handleUserUtterance(aiText);
        } else {
          Serial.println("AI text is empty. Use 'AI:your sentence' format");
        }
      } else if (command.startsWith("TTS:")) {
        String ttsText = command.substring(4);
        ttsText.trim();
        if (ttsText.length() > 0) {
          generateAndPlayAudio(ttsText);
        } else {
          Serial.println("TTS text is empty. Use 'TTS:your text' format");
        }
      } else if (command != "END") {  // Regular text as TTS input
        Serial.print("TTS: ");
        Serial.println(command);
        generateAndPlayAudio(command);
      }
    }
  }
}

/**
 * @brief Generates speech audio from text and plays it.
 *
 * Sends text to the TTS API, saves the MP3 response to SD card,
 * and starts playback using the external I2S DAC.
 *
 * @param text Text to convert into speech
 */

void generateAndPlayAudio(String text) {
  Serial.println("Generating audio...");
  
  if (generateAudio(text)) {
    Serial.println("Audio generated successfully!");
    playAudioFromSD();
  } else {
    Serial.println("Failed to generate audio!");
  }
}

/**
 * @brief Generates MP3 audio using a cloud TTS API.
 *
 * Sends the given text to ElevenLabs (or OpenAI TTS),
 * downloads the generated MP3 audio stream,
 * and stores it on the SD card.
 *
 * @param text Text to convert to speech
 * @return true if audio generation succeeds, false otherwise
 */

bool generateAudio(String text) {
  HTTPClient http;
  
  // NOTE: Currently using ElevenLabs. Switch to OpenAI TTS if preferred
  String url = String(elevenLabsTtsUrl) + voiceId;
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("xi-api-key", elevenLabsApiKey);
  http.addHeader("Accept", "audio/mpeg");
  
  String jsonPayload = "{\"text\":\"" + escapeJsonString(text) + 
                      "\",\"model_id\":\"eleven_multilingual_v2\"," +
                      "\"voice_settings\":{\"stability\":0.5,\"similarity_boost\":0.5}}";
  
  int httpCode = http.POST(jsonPayload);
  


  if (httpCode == HTTP_CODE_OK) {
  int audioSize = http.getSize();
  
    if (audioSize > 0) {
      // Create unique filename
      String filename = "/audio_response.mp3";
      
      // Open file for writing
      File audioFile = SD.open(filename.c_str(), FILE_WRITE);
      if (!audioFile) {
        Serial.println("Failed to open file for writing");
        http.end();
        return false;
      }
      
      // Read audio data and write to file
      WiFiClient* stream = http.getStreamPtr();
      size_t bytesRead = 0;
      uint8_t buffer[1024];
      
      while (http.connected() && bytesRead < (size_t)audioSize) {
        size_t available = stream->available();
        if (available) {
          size_t toRead = min(available, sizeof(buffer));
          size_t read = stream->readBytes(buffer, toRead);
          if (read > 0) {
            audioFile.write(buffer, read);
            bytesRead += read;
          }
        }
        delay(1);
      }
      
      audioFile.close();
      Serial.printf("Audio saved: %s (%d bytes)\n", filename.c_str(), bytesRead);
      
      http.end();
      return true;
    }
  }
  else
  {
    Serial.printf("HTTP Error: %d\n", httpCode);
    Serial.println(http.getString());
  }
  
  http.end();
  return false;
}

/**
 * @brief Plays an MP3 audio file stored on the SD card.
 *
 * Initializes MP3 decoder and I2S output,
 * then streams audio to the external DAC.
 */


void playAudioFromSD() {
  String filename = "/audio_response.mp3";
  
  Serial.printf("Playing: %s\n", filename.c_str());
  
  file = new AudioFileSourceSD(filename.c_str());
  if (!file->isOpen()) {
    Serial.println("Failed to open MP3 file");
    cleanupAudio();
    return;
  }
  
  // Forcefully release I2S_NUM_1 in case the previous cleanup left it installed
  static bool hasPlayedAudio = false;
  if (hasPlayedAudio) {
    i2s_driver_uninstall(I2S_NUM_1);
  }
  hasPlayedAudio = true;

  out = new AudioOutputI2S(1); // Force initialization on secondary I2S block (I2S_NUM_1)
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(0.9);

  mp3 = new AudioGeneratorMP3();
  if (!mp3->begin(file, out)) {
    Serial.println("MP3 decoder begin failed");
    cleanupAudio();
    return;
  }

  Serial.printf("Audio about to play! Latency from input end: %.2f seconds\n",
                (millis() - recordingEndTime) / 1000.0);
  Serial.println("Playback started");
}

/**
 * @brief Frees all audio-related resources.
 *
 * Stops playback and deletes MP3 decoder,
 * file source, and I2S output objects to prevent memory leaks.
 */

void cleanupAudio() {
  if (mp3) { delete mp3; mp3 = nullptr; }
  if (out) { delete out; out = nullptr; }
  if (file) { delete file; file = nullptr; }
}


/**
 * @brief Records microphone audio and converts speech to text.
 *
 * Streams the recording live to Whisper while speaking,
 * then forwards the transcription to the smart-home intent handler.
 */

void startRecordingAndTranscription() {
  Serial.println("\n----- Starting Recording -----");
  Serial.println("Please speak... (recording for " + String(RECORDING_DURATION) + " seconds)");

  String transcribedText = streamRecordingAndTranscription();

  if (transcribedText.length() > 0) {
    Serial.println("\nRecognition result: " + transcribedText);
    Serial.println("\nSending to ChatGPT...");
    handleUserUtterance(transcribedText);
  } else {
    Serial.println("Failed to recognize text or an error occurred.");
  }
}

/**
 * @brief Streams I2S audio directly to the Whisper API socket.
 *
 * Latency optimization: instead of recording to SD and uploading afterwards,
 * the HTTPS request is opened first and audio samples are written to the
 * socket while recording. Content-Length is known in advance (fixed duration),
 * so if the network stalls the remaining audio is padded with silence.
 *
 * @return Transcribed text (empty string on failure)
 */

String streamRecordingAndTranscription() {
  Serial.println("\n----- Preparing Live HTTP Stream -----");

  // Payload size is fixed by the recording duration
  uint32_t total_samples = SAMPLE_RATE * RECORDING_DURATION;
  uint32_t audio_bytes_target = total_samples * 2; // 16-bit mono = 2 bytes per sample

  String boundary = "----WebKitFormBoundary" + String(millis());
  String bodyStart = "--" + boundary + "\r\n";
  bodyStart += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  bodyStart += "Content-Type: audio/wav\r\n\r\n";

  String bodyEnd = "\r\n--" + boundary + "\r\n";
  bodyEnd += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
  bodyEnd += "whisper-1\r\n";
  bodyEnd += "--" + boundary + "--\r\n";

  size_t contentLength = bodyStart.length() + 44 + audio_bytes_target + bodyEnd.length();

  WiFiClientSecure client;
  client.setInsecure();

  Serial.println("Connecting to api.openai.com...");
  if (!client.connect("api.openai.com", 443)) {
    Serial.println("Connection failed!");
    return "";
  }

  // 1. HTTP POST headers
  client.println("POST /v1/audio/transcriptions HTTP/1.1");
  client.println("Host: api.openai.com");
  client.println("Authorization: Bearer " + String(openaiApiKey));
  client.println("Content-Type: multipart/form-data; boundary=" + boundary);
  client.println("Connection: close");
  client.print("Content-Length: ");
  client.println(contentLength);
  client.println();

  // 2. Multipart prefix and WAV header
  client.print(bodyStart);

  uint8_t wavHeader[44];
  createWavHeader(wavHeader, SAMPLE_RATE, 16, 1, total_samples);
  client.write(wavHeader, 44);

  // 3. Start I2S microphone
  if (i2s_driver_install(I2S_NUM_0, &i2s_mic_config, 0, NULL) != ESP_OK) {
    Serial.println("Failed to install I2S driver");
    client.stop();
    return "";
  }
  if (i2s_set_pin(I2S_NUM_0, &i2s_mic_pins) != ESP_OK) {
    Serial.println("Failed to set I2S pins");
    i2s_driver_uninstall(I2S_NUM_0);
    client.stop();
    return "";
  }
  delay(100);

  Serial.println("Recording and Streaming... Speak now!");

  uint32_t total_bytes_sent = 0;
  const size_t buffer_size = 512;
  int16_t i2s_buffer[buffer_size / 2];    // stereo frames from I2S
  int16_t mono_buffer[buffer_size / 4];   // left channel only

  unsigned long start_time = millis();

  // 4. Live audio routing loop
  while (total_bytes_sent < audio_bytes_target) {
    if (millis() - start_time > (RECORDING_DURATION * 1000 + 3000)) {
      Serial.println("\nTimeout! Network congestion detected. Padding with zeroes.");
      break;
    }

    size_t bytes_read = 0;
    esp_err_t result = i2s_read(I2S_NUM_0, (char*)i2s_buffer, sizeof(i2s_buffer), &bytes_read, 100);

    if (result == ESP_OK && bytes_read > 0) {
      size_t samples_read = bytes_read / sizeof(int16_t);
      size_t mono_samples = 0;

      // Keep left channel and apply noise gate
      for (size_t i = 0; i < samples_read; i += 2) {
        int16_t sample = i2s_buffer[i];
        if (abs(sample) < 200) sample = 0;
        mono_buffer[mono_samples++] = sample;
      }

      size_t bytes_to_send = mono_samples * sizeof(int16_t);

      // Never send more than the announced Content-Length
      if (total_bytes_sent + bytes_to_send > audio_bytes_target) {
        bytes_to_send = audio_bytes_target - total_bytes_sent;
      }

      client.write((uint8_t*)mono_buffer, bytes_to_send);
      total_bytes_sent += bytes_to_send;
    }
  }

  // 5. Zero-padding failsafe (keeps the request valid)
  if (total_bytes_sent < audio_bytes_target) {
    uint8_t zero_pad[128] = {0};
    while (total_bytes_sent < audio_bytes_target) {
      size_t to_send = min((size_t)(audio_bytes_target - total_bytes_sent), sizeof(zero_pad));
      client.write(zero_pad, to_send);
      total_bytes_sent += to_send;
    }
  }

  i2s_driver_uninstall(I2S_NUM_0);

  // Latency timer starts when the recording window closes
  recordingEndTime = millis();
  Serial.println("Recording window closed!");

  // 6. Multipart suffix
  client.print(bodyEnd);
  client.flush();

  Serial.println("Upload complete, awaiting transcription...");

  // 7. Wait for the response
  unsigned long respTimeout = millis() + 15000;
  while (!client.available() && client.connected() && millis() < respTimeout) {
    delay(10);
  }

  if (!client.available()) {
    Serial.println("Error: Response timed out waiting for OpenAI");
    client.stop();
    return "";
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();

  // Skip HTTP headers
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) {
      break;
    }
  }

  // Read body
  String responseBody = "";
  respTimeout = millis() + 4000;
  while ((client.connected() || client.available()) && millis() < respTimeout) {
    while (client.available()) {
      responseBody += (char)client.read();
      respTimeout = millis() + 2000;
    }
    delay(2);
  }
  client.stop();

  Serial.printf("Whisper responded in %.2f s (%s)\n",
                (millis() - recordingEndTime) / 1000.0, statusLine.c_str());

  // Tolerate chunked transfer encoding: keep only the JSON object
  int jsonStart = responseBody.indexOf('{');
  int jsonEnd = responseBody.lastIndexOf('}');
  String json = (jsonStart >= 0 && jsonEnd > jsonStart) ? responseBody.substring(jsonStart, jsonEnd + 1) : responseBody;

  String transcription = "";
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);

  if (!error && doc["text"].is<const char*>()) {
    transcription = doc["text"].as<String>();
  } else {
    Serial.println("JSON Error or server error response:");
    Serial.println(responseBody);
  }

  return transcription;
}

// ==================== DEVICE CONTROL ====================

/**
 * @brief Drives the on-board LED respecting its polarity and stores its state.
 *
 * @param on true to turn the LED on, false to turn it off
 */

void setLed(bool on) {
  ledState = on;
  if (LED_PIN < 0) return;
  digitalWrite(LED_PIN, (on == (bool)LED_ACTIVE_HIGH) ? HIGH : LOW);
}

/**
 * @brief Executes a device action after validating it against a whitelist.
 *
 * This is the ONLY function that touches hardware. Serial commands and
 * AI intents both go through it.
 *
 * Supported devices : "led"
 * Supported actions : "on", "off", "toggle"
 *
 * @param device Target device name
 * @param action Action to perform on the device
 * @return true if the command was recognized and executed, false otherwise
 */

bool executeCommand(const String& device, const String& action) {
  String dev = device;
  String act = action;
  dev.trim(); dev.toLowerCase();
  act.trim(); act.toLowerCase();

  if (dev == "led") {
    if (LED_PIN < 0) {
      Serial.println("LED_PIN is not configured for this board");
      return false;
    }
    if (act == "on")          setLed(true);
    else if (act == "off")    setLed(false);
    else if (act == "toggle") setLed(!ledState);
    else {
      Serial.printf("Unknown action '%s' for device '%s'\n", act.c_str(), dev.c_str());
      return false;
    }
    Serial.printf("[EXEC] %s -> %s (LED is now %s)\n", dev.c_str(), act.c_str(), ledState ? "ON" : "OFF");
    return true;
  }

  Serial.printf("Unknown device '%s'\n", dev.c_str());
  return false;
}

// ==================== AI INTENT ====================

// Structured Outputs schema - guarantees ChatGPT returns a valid intent
static const char* INTENT_RESPONSE_FORMAT = R"json({
  "type": "json_schema",
  "json_schema": {
    "name": "home_intent",
    "strict": true,
    "schema": {
      "type": "object",
      "additionalProperties": false,
      "required": ["type", "device", "action", "reply"],
      "properties": {
        "type":   { "type": "string", "enum": ["command", "chat"] },
        "device": { "type": "string", "enum": ["led", "none"] },
        "action": { "type": "string", "enum": ["on", "off", "toggle", "none"] },
        "reply":  { "type": "string" }
      }
    }
  }
})json";

static const char* INTENT_SYSTEM_PROMPT =
  "You are the voice controller of a smart home. "
  "Available devices: led (the room light; actions: on, off, toggle). "
  "If the user wants to change a device state, even implicitly "
  "(e.g. 'it is dark here' means turn the light on, 'turn off all the lights' means off), "
  "return type=command with the matching device and action. "
  "Otherwise return type=chat with device=none and action=none, and answer the question. "
  "'reply' is spoken aloud: a short confirmation for commands or a brief answer (max 30 words) for chat, "
  "written in the same language the user spoke, with no emojis or markdown.";

/**
 * @brief Handles a user sentence (typed or transcribed) end to end.
 *
 * - Extracts a structured intent via ChatGPT
 * - Executes the command on hardware if it is one
 * - Speaks the reply through TTS
 *
 * @param message User input text
 */

void handleUserUtterance(String message) {
  Serial.println("Sending request to ChatGPT...");
  gettingResponse = true;

  Intent intent = sendMessage(message);

  if (!intent.valid) {
    Serial.println("Failed to get ChatGPT intent");
    gettingResponse = false;
    return;
  }

  Serial.printf("Intent -> type: %s, device: %s, action: %s\n",
                intent.type.c_str(), intent.device.c_str(), intent.action.c_str());
  Serial.println("Reply: " + intent.reply);

  String reply = intent.reply;
  if (intent.type == "command") {
    if (executeCommand(intent.device, intent.action)) {
      Serial.printf("Command executed! Latency from input end: %.2f seconds\n",
                    (millis() - recordingEndTime) / 1000.0);
    } else {
      reply = "לא הבנתי את הפקודה";
    }
  }

  if (reply.length() > 0) {
    Serial.println("Speaking reply...");
    generateAndPlayAudio(reply);
  }

  gettingResponse = false;
}

/**
 * @brief Sends a message to the OpenAI Chat Completion API.
 *
 * Builds the request payload, performs HTTP POST,
 * and parses the structured intent from the response.
 *
 * @param message User input text
 * @return Parsed intent (valid == false on failure)
 */

Intent sendMessage(String message) {
  HTTPClient http;
  http.begin(openaiChatUrl);
  http.setTimeout(20000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(openaiApiKey));

  String payload = buildChatGptPayload(message);
  int httpResponseCode = http.POST(payload);

  Intent intent;
  if (httpResponseCode == 200) {
    intent = processChatGptResponse(http.getString());
  } else {
    Serial.printf("ChatGPT HTTP Error: %d\n", httpResponseCode);
    Serial.println(http.getString());
  }

  http.end();
  return intent;
}

/**
 * @brief Builds a ChatGPT request JSON payload.
 *
 * Adds the smart-home system prompt, the user message,
 * and the Structured Outputs response format.
 *
 * @param message User input text
 * @return Serialized JSON payload
 */

String buildChatGptPayload(String message) {
  JsonDocument doc;
  doc["model"] = "gpt-4.1-nano";

  JsonArray messages = doc["messages"].to<JsonArray>();

  JsonObject sysMsg = messages.add<JsonObject>();
  sysMsg["role"] = "system";
  sysMsg["content"] = INTENT_SYSTEM_PROMPT;

  JsonObject userMsg = messages.add<JsonObject>();
  userMsg["role"] = "user";
  userMsg["content"] = message;

  JsonDocument responseFormat;
  deserializeJson(responseFormat, INTENT_RESPONSE_FORMAT);
  doc["response_format"] = responseFormat;

  String output;
  serializeJson(doc, output);
  return output;
}

/**
 * @brief Extracts the structured intent from a ChatGPT JSON response.
 *
 * The assistant message content is itself a JSON string,
 * so it is parsed a second time into an Intent.
 *
 * @param response Raw JSON response from ChatGPT
 * @return Parsed intent (valid == false on failure)
 */

Intent processChatGptResponse(String response) {
  Intent intent;

  JsonDocument jsonDoc;
  DeserializationError error = deserializeJson(jsonDoc, response);
  if (error) {
    Serial.printf("ChatGPT response parsing failed: %s\n", error.c_str());
    return intent;
  }

  String content = jsonDoc["choices"][0]["message"]["content"] | "";
  Serial.println("Raw intent JSON: " + content);

  JsonDocument intentDoc;
  error = deserializeJson(intentDoc, content);
  if (error) {
    Serial.printf("Intent JSON parsing failed: %s\n", error.c_str());
    return intent;
  }

  intent.type   = intentDoc["type"]   | "";
  intent.device = intentDoc["device"] | "none";
  intent.action = intentDoc["action"] | "none";
  intent.reply  = intentDoc["reply"]  | "";
  intent.reply.replace("\n", " ");
  intent.valid  = (intent.type == "command" || intent.type == "chat");

  return intent;
}


/**
 * @brief Creates a WAV file header.
 *
 * Generates a standard PCM WAV header based on
 * sample rate, bit depth, channel count, and sample size.
 *
 * @param header Buffer to store WAV header
 * @param sampleRate Audio sample rate
 * @param bitDepth Audio bit depth
 * @param channels Number of audio channels
 * @param numSamples Total number of samples
 */

void createWavHeader(uint8_t* header, uint32_t sampleRate, uint16_t bitDepth, uint16_t channels, uint32_t numSamples) {
  // RIFF header
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  uint32_t fileSize = numSamples * (bitDepth / 8) * channels + 36;
  header[4] = (fileSize) & 0xFF;
  header[5] = (fileSize >> 8) & 0xFF;
  header[6] = (fileSize >> 16) & 0xFF;
  header[7] = (fileSize >> 24) & 0xFF;
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  
  // fmt chunk
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
  header[20] = 1; header[21] = 0; // PCM format
  header[22] = channels & 0xFF; header[23] = (channels >> 8) & 0xFF;
  header[24] = sampleRate & 0xFF; 
  header[25] = (sampleRate >> 8) & 0xFF;
  header[26] = (sampleRate >> 16) & 0xFF; 
  header[27] = (sampleRate >> 24) & 0xFF;
  
  uint32_t byteRate = sampleRate * channels * (bitDepth / 8);
  header[28] = byteRate & 0xFF; 
  header[29] = (byteRate >> 8) & 0xFF;
  header[30] = (byteRate >> 16) & 0xFF; 
  header[31] = (byteRate >> 24) & 0xFF;
  
  header[32] = channels * (bitDepth / 8);
  header[33] = 0;
  header[34] = bitDepth; header[35] = 0;
  
  // data chunk
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  uint32_t dataSize = numSamples * channels * (bitDepth / 8);
  header[40] = dataSize & 0xFF;
  header[41] = (dataSize >> 8) & 0xFF;
  header[42] = (dataSize >> 16) & 0xFF;
  header[43] = (dataSize >> 24) & 0xFF;
}

/**
 * @brief Configures I2S interface for INMP441 microphone.
 *
 * Sets sample rate, bit depth, channel format,
 * DMA buffers, and I2S pin mapping.
 */

void setupI2SMicrophone() {
  i2s_mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX), // Full Duplex prevents bus contention
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0, // Allow dynamic OS interrupt routing
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  
  i2s_mic_pins = {
    .bck_io_num = I2S_MIC_SERIAL_CLOCK,
    .ws_io_num = I2S_MIC_LEFT_RIGHT_CLOCK,
    .data_out_num = I2S_DOUT, // Actively drive the shared DAC clock
    .data_in_num = I2S_MIC_SERIAL_DATA
  };
  
  Serial.println("I2S Microphone configured for INMP441 (Full Duplex 16-bit)");
}

/**
 * @brief Performs a basic microphone functionality test.
 *
 * Listens for non-zero audio samples to verify
 * microphone wiring and signal presence.
 */

void testMicrophone() {
  Serial.println("\n=== Testing Microphone ===");
  
  if (i2s_driver_install(I2S_NUM_0, &i2s_mic_config, 0, NULL) != ESP_OK) {
    Serial.println("Failed to install I2S driver");
    return;
  }
  
  if (i2s_set_pin(I2S_NUM_0, &i2s_mic_pins) != ESP_OK) {
    Serial.println("Failed to set I2S pins");
    i2s_driver_uninstall(I2S_NUM_0);
    return;
  }
  
  delay(100);
  
  Serial.println("Listening for audio input... Speak into microphone!");
  
  int16_t samples[2];
  size_t bytes_read;
  unsigned long start = millis();
  int non_zero_count = 0;
  
  while (millis() - start < 3000) {
    if (i2s_read(I2S_NUM_0, (char*)samples, sizeof(samples), &bytes_read, 100) == ESP_OK) {
      if (bytes_read == sizeof(samples)) {
        if (abs(samples[0]) > 100) { // Check left channel
          non_zero_count++;
        }
      }
    }
    delay(10);
  }
  
  if (non_zero_count > 0) {
    Serial.printf("Microphone test PASSED: %d audio events detected\n", non_zero_count);
  } else {
    Serial.println("Microphone test FAILED: No audio detected");
  }
  
  i2s_driver_uninstall(I2S_NUM_0);
}

/**
 * @brief Performs an advanced microphone diagnostic test.
 *
 * Reads raw I2S data, measures amplitude levels,
 * detects silent samples, and reports audio quality.
 */

void testMicrophoneDetailed() {
  Serial.println("\n=== Detailed Microphone Test ===");
  
  if (i2s_driver_install(I2S_NUM_0, &i2s_mic_config, 0, NULL) != ESP_OK) {
    Serial.println("Failed to install I2S driver");
    return;
  }
  
  if (i2s_set_pin(I2S_NUM_0, &i2s_mic_pins) != ESP_OK) {
    Serial.println("Failed to set I2S pins");
    i2s_driver_uninstall(I2S_NUM_0);
    return;
  }
  
  delay(500);
  
  Serial.println("I2S driver installed successfully");
  Serial.println("Reading raw I2S data for 5 seconds...");
  
  int16_t samples[100];
  size_t bytes_read;
  unsigned long start = millis();
  int total_samples = 0;
  int non_zero_samples = 0;
  int max_amplitude = 0;
  
  while (millis() - start < 5000) {
    if (i2s_read(I2S_NUM_0, (char*)samples, sizeof(samples), &bytes_read, 100) == ESP_OK) {
      int samples_read = bytes_read / sizeof(int16_t);
      total_samples += (samples_read / 2);
      
      // Process every alternate sample (Left channel)
      for (int i = 0; i < samples_read; i += 2) {
        if (samples[i] != 0) {
          non_zero_samples++;
          int amplitude = abs(samples[i]);
          if (amplitude > max_amplitude) max_amplitude = amplitude;
        }
      }
    }
  }
  
  Serial.println("\n=== Test Results ===");
  Serial.printf("Total samples read: %d\n", total_samples);
  Serial.printf("Non-zero samples: %d\n", non_zero_samples);
  Serial.printf("Max amplitude: %d\n", max_amplitude);
  Serial.printf("Zero sample percentage: %.1f%%\n", 
                (total_samples - non_zero_samples) * 100.0 / total_samples);
  
  if (total_samples == 0) {
    Serial.println("CRITICAL: No samples read from I2S");
  } else if (non_zero_samples == 0) {
    Serial.println("PROBLEM: All samples are zero");
  } else if (max_amplitude < 1000) {
    Serial.println("WARNING: Very low amplitude detected");
  } else {
    Serial.println("SUCCESS: Audio data detected!");
  }
  
  i2s_driver_uninstall(I2S_NUM_0);
}

/**
 * @brief Initializes and mounts the SD card.
 *
 * Configures SPI interface, verifies SD card presence,
 * detects card type, and prints storage information.
 *
 * @return true if SD card is ready, false otherwise
 */

bool setupSDCard() {
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  
  if (!SD.begin(SD_CS)) {
    Serial.println("SD card mount failed!");
    return false;
  }
  
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return false;
  }
  
  Serial.print("SD Card Type: ");
  if (cardType == CARD_MMC) Serial.println("MMC");
  else if (cardType == CARD_SD) Serial.println("SDSC");
  else if (cardType == CARD_SDHC) Serial.println("SDHC");
  else Serial.println("UNKNOWN");
  
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD Card Size: %lluMB\n", cardSize);
  
  return true;
}

/**
 * @brief Connects ESP32 to a WiFi network.
 *
 * Attempts WiFi connection using configured credentials
 * and prints the assigned IP address.
 */

void connectToWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed!");
    while(1);
  }
}


/**
 * @brief Escapes special characters for JSON compatibility.
 *
 * Converts quotes, newlines, tabs, and backslashes
 * into valid JSON-safe sequences.
 *
 * @param input Raw input string
 * @return Escaped JSON-safe string
 */

String escapeJsonString(String input) {
  input.replace("\\", "\\\\");
  input.replace("\"", "\\\"");
  input.replace("\n", "\\n");
  input.replace("\r", "\\r");
  input.replace("\t", "\\t");
  return input;
}