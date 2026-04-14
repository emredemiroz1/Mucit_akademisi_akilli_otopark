#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ESP32Servo.h>

// --- WI-FI AYARLARI ---
#define WIFI_SSID "Esptestwifi"
#define WIFI_PASSWORD "12345678"

// --- FIREBASE AYARLARI ---
#define API_KEY "AIzaSyAR1cyO7pFPM5RLvO308uAcSyf5HaqAizM"
#define FIREBASE_PROJECT_ID "akillisera-b71d4"
#define APP_ID "master-iot-final-v3"

// --- PİNLER (GİRİŞ/ÇIKIŞ) ---
#define IR_GIRIS 32
#define IR_CIKIS 33
const int IR_PARK[6] = {13, 14, 25, 26, 27, 34}; // P1 - P6

#define SERVO_GIRIS_PIN 18
#define SERVO_CIKIS_PIN 19

// --- STANDART SERVO (SG90) AYARLARI ---
#define SERVO_MIN_US       500
#define SERVO_MAX_US       2400
#define SERVO_OPEN_ANGLE   10
#define SERVO_CLOSE_ANGLE  170
#define SERVO_HOLD_MS      4000

// --- SENSÖR FİLTRE AYARLARI ---
#define GIRIS_DEBOUNCE_MS  150
#define CIKIS_DEBOUNCE_MS  150

FirebaseData fbdoRead, fbdoWrite;
FirebaseAuth auth;
FirebaseConfig config;

Servo girisServosu;
Servo cikisServosu;

String docPath = "artifacts/" + String(APP_ID) + "/public/data/parking/status";

// Her park alanı için ayrı sayaçlar
unsigned long slotDetectTime[6] = {0, 0, 0, 0, 0, 0};
unsigned long slotClearTime[6]  = {0, 0, 0, 0, 0, 0};

// Firebase'e en son gönderilmiş durumlar
bool lastSlotStates[6] = {false, false, false, false, false, false};
bool carPaid = false;

// Bariyer durumları
unsigned long girisOpenTime = 0;
bool girisOpen = false;

unsigned long cikisOpenTime = 0;
bool cikisOpen = false;

// Giriş / çıkış sensör filtre değişkenleri
unsigned long girisDetectStart = 0;
unsigned long cikisDetectStart = 0;

int getOccupiedCount() {
  int count = 0;
  for (int i = 0; i < 6; i++) {
    if (lastSlotStates[i]) count++;
  }
  return count;
}

void updateFirestoreBool(String key, bool value) {
  String json = "{\"fields\":{\"" + key + "\":{\"booleanValue\":" + (value ? "true" : "false") + "}}}";
  Firebase.Firestore.patchDocument(&fbdoWrite, FIREBASE_PROJECT_ID, "", docPath.c_str(), json.c_str(), key.c_str());
}

void openGate(Servo &servo, bool &isOpenFlag, unsigned long &timer) {
  servo.write(SERVO_OPEN_ANGLE);
  isOpenFlag = true;
  timer = millis();
}

void closeGateIfTimeout(Servo &servo, bool &isOpenFlag, unsigned long timer, String firebaseKey, bool sensorActive) {
  if (isOpenFlag && !sensorActive && (millis() - timer > SERVO_HOLD_MS)) {
    servo.write(SERVO_CLOSE_ANGLE);
    isOpenFlag = false;
    updateFirestoreBool(firebaseKey, false);
    Serial.println(firebaseKey + " kapatildi.");
  }
}

void setup() {
  Serial.begin(115200);

  // IR sensörler için iç pull-up açıldı
  pinMode(IR_GIRIS, INPUT_PULLUP);
  pinMode(IR_CIKIS, INPUT_PULLUP);

  // 34 numara ESP32'de input-only pin, pullup desteklemez.
  // Bu yüzden park sensörlerinde ayrı ayrı ayarlıyoruz.
  pinMode(IR_PARK[0], INPUT_PULLUP); // 13
  pinMode(IR_PARK[1], INPUT_PULLUP); // 14
  pinMode(IR_PARK[2], INPUT_PULLUP); // 25
  pinMode(IR_PARK[3], INPUT_PULLUP); // 26
  pinMode(IR_PARK[4], INPUT_PULLUP); // 27
  pinMode(IR_PARK[5], INPUT);        // 34 -> dahili pullup yok

  // Servo PWM zamanlayıcıları
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  girisServosu.setPeriodHertz(50);
  girisServosu.attach(SERVO_GIRIS_PIN, SERVO_MIN_US, SERVO_MAX_US);
  girisServosu.write(SERVO_CLOSE_ANGLE);

  cikisServosu.setPeriodHertz(50);
  cikisServosu.attach(SERVO_CIKIS_PIN, SERVO_MIN_US, SERVO_MAX_US);
  cikisServosu.write(SERVO_CLOSE_ANGLE);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi baglaniyor");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Baglandi!");

  config.api_key = API_KEY;
  config.signer.test_mode = true;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Sistem hazir.");
}

void loop() {
  if (!Firebase.ready()) return;

  unsigned long currentMillis = millis();

  // =========================================================
  // 1. GİRİŞ KAPISI MANTIĞI (YENİ VE KUSURSUZ)
  // =========================================================
  bool girisSensorAktif = (digitalRead(IR_GIRIS) == LOW);

  if (girisSensorAktif) {
    if (girisOpen) {
      // Kapı ZATEN açıksa filtreye bakma, süreyi direkt uzat! (Titremeyi bitirir)
      girisOpenTime = currentMillis;
    } else {
      // Kapı kapalıysa ilk açılış için 150ms filtreden geçmesini bekle
      if (girisDetectStart == 0) {
        girisDetectStart = currentMillis;
      }
      if ((currentMillis - girisDetectStart >= GIRIS_DEBOUNCE_MS)) {
        if (getOccupiedCount() < 6) {
          openGate(girisServosu, girisOpen, girisOpenTime);
          updateFirestoreBool("entryGateOpen", true);
          Serial.println("Arac geldi. Giris bariyeri acildi.");
        }
      }
    }
  } else {
    girisDetectStart = 0;
  }

  // =========================================================
  // 2. PARK ALANLARI MANTIĞI
  // =========================================================
  for (int i = 0; i < 6; i++) {
    bool carPresent = (digitalRead(IR_PARK[i]) == LOW);

    if (carPresent) {
      slotClearTime[i] = 0;

      if (slotDetectTime[i] == 0) {
        slotDetectTime[i] = currentMillis;
      } 
      else if (!lastSlotStates[i] && (currentMillis - slotDetectTime[i] > 10000)) {
        lastSlotStates[i] = true;
        updateFirestoreBool("p" + String(i) + "_occupied", true);
        Serial.println("P" + String(i + 1) + " Park Edildi.");
      }
    } 
    else {
      slotDetectTime[i] = 0;

      if (slotClearTime[i] == 0) {
        slotClearTime[i] = currentMillis;
      } 
      else if (lastSlotStates[i] && (currentMillis - slotClearTime[i] > 5000)) {
        lastSlotStates[i] = false;
        updateFirestoreBool("p" + String(i) + "_occupied", false);
        Serial.println("P" + String(i + 1) + " Bosaldi.");
      }
    }
  }

  // =========================================================
  // 3. ÇIKIŞ KAPISI MANTIĞI (GİRİŞ KAPISIYLA BİREBİR AYNI)
  // =========================================================
  static unsigned long lastDbCheck = 0;
  if (currentMillis - lastDbCheck > 2500) {
    lastDbCheck = currentMillis;
    if (Firebase.Firestore.getDocument(&fbdoRead, FIREBASE_PROJECT_ID, "", docPath.c_str(), "carPaid")) {
      carPaid = (fbdoRead.payload().indexOf("\"booleanValue\": true") > -1);
    }
  }

  bool cikisSensorAktif = (digitalRead(IR_CIKIS) == LOW);

  if (cikisSensorAktif) {
    if (cikisOpen) {
      // Kapı ZATEN açıksa süreyi direkt uzat
      cikisOpenTime = currentMillis;
    } else {
      // Kapı kapalıysa açılış için filtreyi bekle
      if (cikisDetectStart == 0) {
        cikisDetectStart = currentMillis;
      }
      if ((currentMillis - cikisDetectStart >= CIKIS_DEBOUNCE_MS)) {
        if (carPaid) {
          openGate(cikisServosu, cikisOpen, cikisOpenTime);
          updateFirestoreBool("exitGateOpen", true);
          updateFirestoreBool("carPaid", false);
          carPaid = false;
          Serial.println("Odeme alinmis. Arac cikiyor. Cikis bariyeri acildi.");
        }
      }
    }
  } else {
    cikisDetectStart = 0;
  }

  // =========================================================
  // 4. BARİYERLERİ OTOMATİK KAPATMA
  // =========================================================
  closeGateIfTimeout(girisServosu, girisOpen, girisOpenTime, "entryGateOpen", girisSensorAktif);
  closeGateIfTimeout(cikisServosu, cikisOpen, cikisOpenTime, "exitGateOpen", cikisSensorAktif);

  // =========================================================
  // 5. DEBUG
  // =========================================================
  static unsigned long lastDebug = 0;
  if (currentMillis - lastDebug > 500) {
    lastDebug = currentMillis;

    Serial.print("GIRIS=");
    Serial.print(digitalRead(IR_GIRIS));
    Serial.print(" | CIKIS=");
    Serial.print(digitalRead(IR_CIKIS));
    Serial.print(" | DOLU=");
    Serial.print(getOccupiedCount());
    Serial.print(" | carPaid=");
    Serial.println(carPaid ? "true" : "false");
  }
}