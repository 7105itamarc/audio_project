#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <SPI.h>

// ─── הגדרות רשת ───
const char* ssid = "iPhone";
const char* password = "itamar2001";

WebServer server(80);
File uploadFile; // אובייקט שיחזיק את הקובץ בזמן הכתיבה

// הצהרה על הפונקציות כדי שהקומפיילר יכיר אותן
void handleFileUpload();
void listDir(fs::FS &fs, const char * dirname);

void setup() {
  Serial.begin(115200);
  while (!Serial);

  // 1. אתחול כרטיס ה-SD
  if (!SD.begin()) {
    Serial.println("SD Card Mount Failed! Check wiring or insert card.");
    return;
  }
  Serial.println("SD Card Initialized.");

  // הדפסת כל הקבצים שנמצאים על הכרטיס
  Serial.println("--- קבצים על כרטיס ה-SD ---");
  listDir(SD, "/");
  Serial.println("---------------------------");

  // 2. התחברות ל-WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // 3. הגדרת ניתובים של השרת
  
  // דף הבית - מציג טופס HTML להעלאת קובץ
  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html lang='he' dir='rtl'><head><meta charset='UTF-8'>";
    html += "<title>העלאה ל-SD</title></head><body style='font-family:Arial; text-align:center; padding:50px;'>";
    html += "<h1>שרת העלאות ל-ESP32</h1>";
    html += "<p>בחר את קובץ ה-MP3 שלך ולחץ על העלה:</p>";
    html += "<form method='POST' action='/upload' enctype='multipart/form-data'>";
    html += "<input type='file' name='f' accept='.mp3'><br><br>"; // שונה כדי לקבל קבצי MP3
    html += "<input type='submit' value='העלה קובץ' style='padding:10px 20px;'>";
    html += "</form></body></html>";
    server.send(200, "text/html", html);
  });

  // ניתוב לטיפול בהעלאה עצמה
  server.on("/upload", HTTP_POST, []() {
    server.send(200, "text/html", "<html lang='he' dir='rtl'><body style='text-align:center; padding:50px;'><h2>הקובץ הועלה ונשמר ב-SD בהצלחה! 🚀</h2><a href='/'>חזור לדף הבית</a></body></html>");
  }, handleFileUpload);

  server.begin();
  Serial.println("HTTP server started.");
}

void loop() {
  server.handleClient();
}

// ─── פונקציה לטיפול בזרם הנתונים מהדפדפן לכרטיס ה-SD ───
void handleFileUpload() {
  HTTPUpload& upload = server.upload(); // קבלת אובייקט ההעלאה

  if (upload.status == UPLOAD_FILE_START) {
    // שלב 1: תחילת העלאה - פתיחת קובץ חדש ב-SD
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    Serial.print("Receiving file: "); Serial.println(filename);
    
    // פתיחת הקובץ לכתיבה (ידרוס קובץ קיים באותו שם)
    uploadFile = SD.open(filename, FILE_WRITE);
    if (!uploadFile) {
      Serial.println("Failed to open file for writing on SD.");
    }
    
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // שלב 2: קבלת חתיכת נתונים (Chunk) וכתיבתה לכרטיס
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
    
  } else if (upload.status == UPLOAD_FILE_END) {
    // שלב 3: סיום ההעלאה - סגירת הקובץ
    if (uploadFile) {
      uploadFile.close();
    }
    Serial.print("Upload completed! Size: "); 
    Serial.print(upload.totalSize); Serial.println(" bytes.");
  }
}

// ─── פונקציה להדפסת כל הקבצים שעל הכרטיס אל ה-Serial Monitor ───
void listDir(fs::FS &fs, const char * dirname) {
  Serial.printf("Listing directory: %s\n", dirname);

  File root = fs.open(dirname);
  if (!root || !root.isDirectory()) {
    Serial.println("Failed to open directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  DIR : ");
      Serial.println(file.name());
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("  SIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}