#include <WiFi.h>
#include <Firebase_ESP_Client.h>

// --- AYARLAR ---
#define WIFI_SSID "Esptestwifi"
#define WIFI_PASSWORD "12345678"

#define API_KEY "AIzaSyAR1cyO7pFPM5RLvO308uAcSyf5HaqAizM"
#define FIREBASE_PROJECT_ID "akillisera-b71d4"
#define APP_ID "master-iot-final-v3"

// --- PİNLER ---
#define LED_PIN 2 // ESP32 üzerindeki dahili mavi LED (Salon Işığı)

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

String documentPath = "artifacts/" + String(APP_ID) + "/public/data/home/status";

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  // WiFi Bağlantısı
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi baglaniliyor");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi baglandi!");

  // Firebase Bağlantısı
  config.api_key = API_KEY;
  config.signer.test_mode = true;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  if (!Firebase.ready()) return;

  static unsigned long lastCheck = 0;

  // Her 2 saniyede bir veritabanını kontrol et
  if (millis() - lastCheck > 2000) {
    lastCheck = millis();

    // Veriyi Çek
    if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", documentPath.c_str(), "")) {
      String payload = fbdo.payload();
      
      // "light" bilgisini bul
      int pos = payload.indexOf("\"light\"");
      bool ledState = false;

      if (pos != -1) {
        String sub = payload.substring(pos, pos + 100);
        if (sub.indexOf("true") > -1) {
          ledState = true;
        }
      }

      // Işığı aç veya kapat
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
      Serial.println(ledState ? "AKILLI EV: Salon Işığı AÇIK" : "AKILLI EV: Salon Işığı KAPALI");
    }
  }
}