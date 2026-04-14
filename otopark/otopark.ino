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
#define SERVO_OPEN_ANGLE   120  // Açık bariyer açısı
#define SERVO_CLOSE_ANGLE  0    // Kapalı bariyer açısı
#define SERVO_HOLD_MS      3500 // Araç geçişi için bariyerin açık kalma süresi

FirebaseData fbdoRead, fbdoWrite;
FirebaseAuth auth;
FirebaseConfig config;
Servo girisServosu;
Servo cikisServosu;

String docPath = "artifacts/" + String(APP_ID) + "/public/data/parking/status";

// --- GECİKME (DEBOUNCE) ZAMANLAYICILARI ---
unsigned long entryDetectTime = 0;
unsigned long exitDetectTime = 0;

// Her park alanı için ayrı ayrı sayaçlar
unsigned long slotDetectTime[6] = {0,0,0,0,0,0}; // Araç ne zamandır alanda?
unsigned long slotClearTime[6] = {0,0,0,0,0,0};  // Alan ne zamandır boş?

// Son Olarak Firebase'e Onaylanıp Gönderilmiş Durumlar
bool lastSlotStates[6] = {false, false, false, false, false, false};
bool carPaid = false; 

// Bariyer Durumları
unsigned long girisOpenTime = 0;
bool girisOpen = false;
unsigned long cikisOpenTime = 0;
bool cikisOpen = false;

int getOccupiedCount() {
  int count = 0;
  for(int i=0; i<6; i++) {
    if(lastSlotStates[i]) count++;
  }
  return count;
}

void updateFirestoreBool(String key, bool value) {
  String json = "{\"fields\":{\"" + key + "\":{\"booleanValue\":" + (value ? "true" : "false") + "}}}";
  Firebase.Firestore.patchDocument(&fbdoWrite, FIREBASE_PROJECT_ID, "", docPath.c_str(), json.c_str(), key.c_str());
}

// --- STANDART SERVO AÇMA FONKSİYONU ---
void openGate(Servo &servo, int pin, bool &isOpenFlag, unsigned long &timer) {
  servo.write(SERVO_OPEN_ANGLE); // Bariyeri havaya kaldır
  
  isOpenFlag = true;
  timer = millis();
}

// --- STANDART SERVO KAPATMA FONKSİYONU ---
void closeGateIfTimeout(Servo &servo, bool &isOpenFlag, unsigned long timer, String firebaseUrl) {
  if (isOpenFlag && (millis() - timer > SERVO_HOLD_MS)) {
    servo.write(SERVO_CLOSE_ANGLE);
    // DÜZELTME: detach() KALDIRILDI! Motor gücü kesilmeyecek, kapıyı taş gibi kilitli tutacak.
    isOpenFlag = false;
    updateFirestoreBool(firebaseUrl, false);
    Serial.println("Bariyer kapatildi.");
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(IR_GIRIS, INPUT);
  pinMode(IR_CIKIS, INPUT);
  for(int i=0; i<6; i++) pinMode(IR_PARK[i], INPUT);

  // DÜZELTME: ESP32 Servo zamanlayıcıları tahsis edildi (titremeyi ve düşmeyi önler)
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
  
  // DÜZELTME: detach() KOMUTLARI BURADAN DA KALDIRILDI!
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi Baglandi!");

  config.api_key = API_KEY;
  config.signer.test_mode = true;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  if (!Firebase.ready()) return;

  unsigned long currentMillis = millis();

  // 1. GİRİŞ KAPISI MANTIĞI (3 Saniye Bekleme)
  if (digitalRead(IR_GIRIS) == LOW) {
    if (entryDetectTime == 0) {
      entryDetectTime = currentMillis; // Gözlem başladı
    } 
    else if (currentMillis - entryDetectTime > 3000 && !girisOpen) { // 3 Saniye Doldu
      if (getOccupiedCount() < 6) { 
        openGate(girisServosu, SERVO_GIRIS_PIN, girisOpen, girisOpenTime);
        updateFirestoreBool("entryGateOpen", true);
        Serial.println("Arac geldi. 3 sn onaylandi. Giris bariyeri acildi.");
      }
      entryDetectTime = 0; // Tekrar okumaması için sıfırla
    }
  } else {
    entryDetectTime = 0; // Araç 3 saniyeden önce giderse sayacı iptal et
  }

  // 2. PARK ALANLARI MANTIĞI (10 sn Giriş, 5 sn Çıkış Bekleme)
  for(int i=0; i<6; i++) {
    bool carPresent = (digitalRead(IR_PARK[i]) == LOW);
    
    if (carPresent) {
      slotClearTime[i] = 0; // Araç var, çıkış sayacını iptal et
      
      if (slotDetectTime[i] == 0) {
        slotDetectTime[i] = currentMillis;
      } 
      else if (!lastSlotStates[i] && (currentMillis - slotDetectTime[i] > 10000)) {
        // Araç 10 SANİYE KESİNTİSİZ durdu. Firebase'e "Dolu" yaz.
        lastSlotStates[i] = true;
        updateFirestoreBool("p" + String(i) + "_occupied", true);
        Serial.println("P" + String(i+1) + " Park Edildi (10sn Onayli).");
      }
    } 
    else {
      slotDetectTime[i] = 0; // Araç yok, giriş sayacını iptal et
      
      if (slotClearTime[i] == 0) {
        slotClearTime[i] = currentMillis;
      } 
      else if (lastSlotStates[i] && (currentMillis - slotClearTime[i] > 5000)) {
        // Araç 5 SANİYE KESİNTİSİZ gitti. Firebase'e "Boş" yaz.
        lastSlotStates[i] = false;
        updateFirestoreBool("p" + String(i) + "_occupied", false);
        Serial.println("P" + String(i+1) + " Bosaldi (5sn Onayli).");
      }
    }
  }

  // 3. ÇIKIŞ KAPISI MANTIĞI (2 Saniye Bekleme & Ödeme Kontrolü)
  static unsigned long lastDbCheck = 0;
  if (currentMillis - lastDbCheck > 2500) {
    lastDbCheck = currentMillis;
    if (Firebase.Firestore.getDocument(&fbdoRead, FIREBASE_PROJECT_ID, "", docPath.c_str(), "carPaid")) {
      carPaid = (fbdoRead.payload().indexOf("\"booleanValue\": true") > -1);
    }
  }

  if (digitalRead(IR_CIKIS) == LOW) {
    if (exitDetectTime == 0) {
      exitDetectTime = currentMillis;
    }
    else if (currentMillis - exitDetectTime > 2000 && !cikisOpen && carPaid) { // 2 Saniye Doldu
      openGate(cikisServosu, SERVO_CIKIS_PIN, cikisOpen, cikisOpenTime);
      updateFirestoreBool("exitGateOpen", true);
      updateFirestoreBool("carPaid", false); // Diğer aracın kaçmaması için ödemeyi sıfırla
      carPaid = false;
      exitDetectTime = 0;
      Serial.println("Odeme alinmis. Arac cikiyor. Bariyer acildi.");
    }
  } else {
    exitDetectTime = 0;
  }

  // 4. BARİYERLERİ OTOMATİK KAPATMA
  closeGateIfTimeout(girisServosu, girisOpen, girisOpenTime, "entryGateOpen");
  closeGateIfTimeout(cikisServosu, cikisOpen, cikisOpenTime, "exitGateOpen");
}