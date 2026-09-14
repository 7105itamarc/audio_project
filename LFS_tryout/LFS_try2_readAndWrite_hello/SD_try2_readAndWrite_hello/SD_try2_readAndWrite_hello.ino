#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

// ─── הגדרות רשת ───
const char* ssid = "iPhone";
const char* password = "itamar2001";

WebServer server(80);

void setup() {
  Serial.begin(115200);
  while (!Serial);

  // 1. אתחול הזיכרון הפנימי (LittleFS)
  if (!LittleFS.begin(true)) {
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }
  
  // 2. יצירת קובץ ה-HTML בתוך הזיכרון הפנימי
  File file = LittleFS.open("/index.html", FILE_WRITE);
  if (file) {
    file.print("<!DOCTYPE html><html lang='he' dir='rtl'><head><meta charset='UTF-8'>");
    file.print("<title>אתר מה-LittleFS</title>");
    file.print("<style>body{background-color:#282c34;color:#61dafb;font-family:Arial;text-align:center;padding:50px;}</style>");
    file.print("</head><body>");
    file.print("<h1>האתר הזה רץ מהזיכרון הפנימי! 🚀</h1>");
    file.print("<p>הדף שאתה רואה נטען מקובץ index.html ששמור בתוך ה-LittleFS של ה-ESP32.</p>");
    file.print("</body></html>");
    file.close();
  }

  // 3. התחברות ל-WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // 4. ניתוב: שרת ה-Web מגיש את הקובץ ישירות מהזיכרון הפנימי
  server.serveStatic("/", LittleFS, "/index.html");

  server.begin();
  Serial.println("HTTP server started.");
}

void loop() {
  server.handleClient();
}