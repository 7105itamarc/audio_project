#include "SD.h"
#include "FS.h"
#include "Audio.h"

// ─── הגדרות פינים לרמקול (I2S) ───
#define I2S_DOUT  14  // מחובר ל-DIN ברמקול
#define I2S_BCLK  26  // מחובר ל-BCLK ברמקול
#define I2S_LRC   25  // מחובר ל-WS/LRC ברמקול
#define SDM_PIN   32  // פין ההפעלה של המגבר

// יצירת אובייקט הנגן
Audio audio;

void setup() {
  Serial.begin(115200);
  while (!Serial);

  // הדלקת המגבר של הרמקול (מוציא אותו ממצב שינה)
  pinMode(SDM_PIN, OUTPUT);
  digitalWrite(SDM_PIN, HIGH);

  // 1. אתח ול כרטיס ה-SD
  if (!SD.begin()) {
    Serial.println("SD Card Mount Failed!");
    return;
  }
  Serial.println("SD Card Initialized.");

  // 2. הגדרת פיני השמע של הנגן
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  
  // 3. הגדרת עוצמת השמע (0 עד 21)
  audio.setVolume(12); // אפשר לשנות אם חלש או חזק מדי

  // 4. התחלת ניגון הקובץ מכרטיס ה-SD
  Serial.println("Starting playback...");
  bool isPlaying = audio.connecttoFS(SD, "/tryMp3i.mp3");
  
  if (isPlaying) {
    Serial.println("Playing audio file...");
  } else {
    Serial.println("Failed to open file for playing.");
  }
}

void loop() {
  // הפקודה הזו חייבת לרוץ כל הזמן
  audio.loop(); 
}

// פונקציות עזר שהספרייה צריכה כדי לדווח לנו על המצב
void audio_info(const char *info){
    Serial.print("info        "); Serial.println(info);
}
void audio_eof_mp3(const char *info){  // קופץ כשהשיר מסתיים
    Serial.print("eof_mp3     ");Serial.println(info);
}