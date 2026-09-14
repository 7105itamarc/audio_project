#include "FS.h"
#include "SD.h"
#include "SPI.h"

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println("Initializing SD card...");

  // אתחול כרטיס ה-SD (משתמש בפיני ה-SPI הסטנדרטיים של ESP32)
  if (!SD.begin()) {
    Serial.println("Card Mount Failed! Check wiring or insert card.");
    return;
  }
  
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return;
  }

  Serial.print("SD Card Type: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD Card Size: %lluMB\n", cardSize);

  // כתיבת קובץ בדיקה חדש לכרטיס
  writeFile(SD, "/test.txt", "Hello from ESP32!\n");
  
  // קריאת הקובץ בחזרה כדי לוודא שעבד
  readFile(SD, "/test.txt");
}

void loop() {
  // שום דבר לא צריך לרוץ בלולאה כרגע
}

// פונקציה לכתיבת קובץ
void writeFile(fs::FS &fs, const char * path, const char * message) {
  Serial.printf("Writing file: %s\n", path);

  File file = fs.open(path, FILE_WRITE);
  if(!file){
    Serial.println("Failed to open file for writing");
    return;
  }
  if(file.print(message)){
    Serial.println("File written successfully");
  } else {
    Serial.println("Write failed");
  }
  file.close();
}

// פונקציה לקריאת קובץ והצגתו ב-Serial Monitor
void readFile(fs::FS &fs, const char * path) {
  Serial.printf("Reading file: %s\n", path);

  File file = fs.open(path);
  if(!file || file.isDirectory()){
    Serial.println("Failed to open file for reading");
    return;
  }

  Serial.print("Read from file: ");
  while(file.available()){
    Serial.write(file.read());
  }
  file.close();
}