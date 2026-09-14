#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <SPI.h>

// ─── הגדרות רשת (הכנס את הפרטים שלך) ───
const char* ssid = "iPhone";
const char* password = "itamar2001";

// יצירת אובייקט השרת על פורט 80 (הפורט הסטנדרטי לאינטרנט)
WebServer server(80);

void setup() {
  Serial.begin(115200);
  while (!Serial);

  // 1. התחברות ל-WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP()); // הכתובת שתצטרך להקליד בדפדפן

  // 2. אתחול כרטיס ה-SD (עם הפינים הסטנדרטיים)
  if (!SD.begin()) {
    Serial.println("Card Mount Failed! Is the card inserted?");
    return;
  }
  Serial.println("SD Card Initialized Successfully.");

  // 3. הגדרת הראוטר של האתר - מה קורה כשנכנסים לדף הבית ("/")
  server.on("/", handleRoot);

  // הפעלת השרת
  server.begin();
  Serial.println("HTTP server started. Waiting for clients...");
}

void loop() {
  // השרת ממתין לבקשות מלקוחות
  server.handleClient();
}

// ─── הפונקציה שבונה את האתר וקוראת מה-SD ───
void handleRoot() {
  Serial.println("Client connected! Reading file from SD...");
  
  // ניסיון לפתוח את הקובץ שיצרנו בבדיקה הקודמת
  File file = SD.open("/test.txt");
  if (!file) {
    // אם הקובץ לא נמצא, נשלח שגיאה 404
    server.send(404, "text/plain", "Error: test.txt not found on SD card");
    return;
  }

  // קריאת תוכן הקובץ לתוך משתנה מחרוזת (String)
  String fileContent = "";
  while (file.available()) {
    fileContent += (char)file.read();
  }
  file.close(); // סגירת הקובץ כדי לפנות זיכרון

  // בניית דף ה-HTML שיוצג למשתמש
  String html = "<!DOCTYPE html><html lang='he' dir='rtl'>";
  html += "<head><meta charset='UTF-8'><title>ESP32 Web & SD</title>";
  html += "<style>body { font-family: Arial; text-align: center; margin-top: 50px; background-color: #f0f0f0; }</style></head>";
  html += "<body><h1>היי! זה שרת ה-Web של ה-ESP32</h1>";
  html += "<p>זה מה שקראתי הרגע מכרטיס ה-SD שלך (מתוך test.txt):</p>";
  // כאן אנחנו מזריקים את הטקסט שקראנו מהכרטיס לתוך ה-HTML
  html += "<h2 style='color: #d35400;'>" + fileContent + "</h2>";
  html += "</body></html>";

  // שליחת הדף לדפדפן עם קוד 200 (OK)
  server.send(200, "text/html", html);
}