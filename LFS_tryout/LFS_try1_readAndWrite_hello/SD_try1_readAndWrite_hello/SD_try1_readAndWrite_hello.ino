#include <LittleFS.h>

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println("\nMounting LittleFS...");

  // הפרמטר true אומר: אם זו פעם ראשונה שאתה מופעל ואין מערכת קבצים, תפרמט את הזיכרון
  if (!LittleFS.begin(true)) {
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }
  
  Serial.println("LittleFS Mounted Successfully!");

  // כתיבת קובץ חדש לזיכרון הפנימי
  File file = LittleFS.open("/internal_test.txt", FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  file.println("Hello from the internal Flash memory!");
  file.close();
  Serial.println("File written successfully.");

  // קריאת הקובץ חזרה אל המסך
  file = LittleFS.open("/internal_test.txt", FILE_READ);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }
  
  Serial.print("Read from internal file: ");
  while (file.available()) {
    Serial.write(file.read());
  }
  file.close();
}

void loop() {
  // הלולאה ריקה
}