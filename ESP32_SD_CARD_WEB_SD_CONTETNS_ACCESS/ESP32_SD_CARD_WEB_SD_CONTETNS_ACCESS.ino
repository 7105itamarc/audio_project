#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>

// ----- SD Card Pins (Standard ESP32 Layout) -----
#define SD_CS       5
#define SD_MISO     19
#define SD_SCLK     18
#define SD_MOSI     23

// ----- WiFi Credentials -----
const char* ssid = "testing123";
const char* password = "blahblah";

// Initialize WebServer on port 80
WebServer server(80);

// Function to generate and serve the HTML directory listing
void handleRoot() {
    String html = "<!DOCTYPE html><html><head><title>ESP32 SD Card Audio</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; margin: 40px; background-color: #f4f4f9; color: #333; }";
    html += "h1 { color: #0056b3; }";
    html += "ul { list-style-type: none; padding: 0; }";
    html += "li { background: #fff; margin-bottom: 15px; padding: 15px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }";
    html += "a { text-decoration: none; color: #0056b3; font-weight: bold; font-size: 1.1em; }";
    html += "audio { width: 100%; margin-top: 10px; }";
    html += ".size { color: #777; font-size: 0.9em; margin-left: 10px; }";
    html += "</style></head><body>";
    html += "<h1>SD Card Audio Files</h1><ul>";

    File root = SD.open("/");
    if (!root) {
        html += "<p>Failed to open SD card root directory.</p>";
    } else {
        File file = root.openNextFile();
        bool filesFound = false;

        while (file) {
            if (!file.isDirectory()) {
                String filename = String(file.name());
                // Ensure filename has a leading slash for the URL path
                if (!filename.startsWith("/")) {
                    filename = "/" + filename;
                }

                html += "<li>";
                html += "<a href='" + filename + "' target='_blank'>" + filename + "</a>";
                html += "<span class='size'>(" + String(file.size()) + " bytes)</span><br>";

                // Embed an audio player if it's a WAV or MP3 file
                if (filename.endsWith(".wav") || filename.endsWith(".WAV")) {
                    html += "<audio controls><source src='" + filename + "' type='audio/wav'>Your browser does not support the audio element.</audio>";
                } else if (filename.endsWith(".mp3") || filename.endsWith(".MP3")) {
                    html += "<audio controls><source src='" + filename + "' type='audio/mpeg'>Your browser does not support the audio element.</audio>";
                }
                
                html += "</li>";
                filesFound = true;
            }
            file = root.openNextFile();
        }
        if (!filesFound) {
            html += "<p>No files found on the SD card.</p>";
        }
    }

    html += "</ul></body></html>";
    server.send(200, "text/html", html);
}

// Function to handle raw file requests (streaming the audio data to the browser)
bool handleFileRead(String path) {
    Serial.println("Requested: " + path);

    // Map extensions to MIME types
    String contentType = "text/plain";
    if (path.endsWith(".wav") || path.endsWith(".WAV")) contentType = "audio/wav";
    else if (path.endsWith(".mp3") || path.endsWith(".MP3")) contentType = "audio/mpeg";
    else if (path.endsWith(".htm") || path.endsWith(".html")) contentType = "text/html";

    if (SD.exists(path)) {
        File file = SD.open(path, "r");
        if (file) {
            // Stream the file directly from SD to the HTTP client chunk by chunk
            server.streamFile(file, contentType);
            file.close();
            return true;
        }
    }
    return false;
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n--- ESP32 SD Web Server ---");

    // Initialize SPI and SD Card
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS)) {
        Serial.println("SD Card mount failed! Check wiring.");
        while (1);
    }
    Serial.println("SD Card initialized successfully.");

    // Connect to WiFi
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected.");
    Serial.print("Web Server IP address: ");
    Serial.println(WiFi.localIP());

    // Define web server routing
    server.on("/", HTTP_GET, handleRoot);

    // Catch-all handler for file requests (like /recording.wav)
    server.onNotFound([]() {
        if (!handleFileRead(server.uri())) {
            server.send(404, "text/plain", "404: File Not Found");
        }
    });

    // Start the server
    server.begin();
    Serial.println("HTTP server started.");
}

void loop() {
    // Listen for incoming client requests
    server.handleClient();
    delay(2);
}