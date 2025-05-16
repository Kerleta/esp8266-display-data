#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define EEPROM_SIZE 512
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// == Waktu & WiFi ==
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 25200, 60000);  // UTC+7

// == Pesan berjalan ==
String message = "Selamat ulang tahun!";
int textX = SCREEN_WIDTH;
int textY = 50;
unsigned long colonPreviousMillis = 0;
unsigned long textPreviousMillis = 0;
const long interval = 1000;
const long textInterval = 50;
bool showColon = true;

// == Gambar ==
const int maxImages = 5;  // Ditambah kapasitas gambar
String imageURLs[maxImages] = {
  "https://raw.githubusercontent.com/Kerleta/esp8266-display-data/main/epd_bitmap_.bin",
  "https://raw.githubusercontent.com/Kerleta/esp8266-display-data/main/epd_bitmap_%202.bin",
  "https://raw.githubusercontent.com/Kerleta/esp8266-display-data/main/epd_bitmap_3.bin",
  "",  // Slot kosong untuk gambar tambahan
  ""   // Slot kosong untuk gambar tambahan
};
uint8_t imageBuffers[maxImages][1024]; // Array buffer untuk menyimpan semua gambar
bool imageReady[maxImages] = {false, false, false, false, false};
int currentImageIndex = 0;
int activeImageCount = 3;

// == Config update ==
// URL ke file konfigurasi di repo GitHub
const String configURL = "https://raw.githubusercontent.com/Kerleta/esp8266-display-data/main/config.json";
unsigned long lastConfigCheck = 0;
const unsigned long configCheckInterval = 3600000;  // Cek update setiap 1 jam

// == Status Tampilan ==
enum DisplayState { SHOW_CLOCK, SHOW_IMAGE };
DisplayState currentState = SHOW_CLOCK;
unsigned long lastSwitchMillis = 0;
const unsigned long switchInterval = 9000;
unsigned long nextSwitchMillis = 0;
bool newImageNeeded = true;

// Debug functions
void printDetailedHTTPInfo(HTTPClient& http, int httpCode) {
  Serial.println("HTTP Code: " + String(httpCode));
  if (httpCode != 200) {
    Serial.println("Error message: " + http.errorToString(httpCode));
  }
}

// == Simpan konfigurasi ke EEPROM ==
void saveConfig() {
  EEPROM.begin(EEPROM_SIZE);

  int addr = 0;
  // Simpan message
  int msgLen = message.length();
  EEPROM.write(addr, msgLen);
  addr++;

  for (int i = 0; i < msgLen; i++) {
    EEPROM.write(addr, message[i]);
    addr++;
  }

  // Simpan jumlah gambar aktif
  EEPROM.write(addr, activeImageCount);
  addr++;

  // Simpan URL gambar
  for (int i = 0; i < maxImages; i++) {
    int urlLen = imageURLs[i].length();
    EEPROM.write(addr, urlLen);
    addr++;

    for (int j = 0; j < urlLen; j++) {
      EEPROM.write(addr, imageURLs[i][j]);
      addr++;
    }
  }

  EEPROM.commit();
}

// == Muat konfigurasi dari EEPROM ==
void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);

  int addr = 0;
  // Muat message
  int msgLen = EEPROM.read(addr);
  addr++;

  if (msgLen > 0 && msgLen < 100) {
    message = "";
    for (int i = 0; i < msgLen; i++) {
      message += (char)EEPROM.read(addr);
      addr++;
    }
  }

  // Muat jumlah gambar aktif
  activeImageCount = EEPROM.read(addr);
  addr++;
  if (activeImageCount < 1 || activeImageCount > maxImages) {
    activeImageCount = 3;  // Default jika tidak valid
  }

  // Muat URL gambar
  for (int i = 0; i < maxImages; i++) {
    int urlLen = EEPROM.read(addr);
    addr++;

    if (urlLen > 0 && urlLen < 200) {
      imageURLs[i] = "";
      for (int j = 0; j < urlLen; j++) {
        imageURLs[i] += (char)EEPROM.read(addr);
        addr++;
      }
    }
  }
}

// == Ambil konfigurasi dari URL ==
void fetchConfig() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skip config update");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  Serial.println("[HTTP] Memulai koneksi ke: " + configURL);

  if (https.begin(client, configURL)) {
    int httpCode = https.GET();
    Serial.printf("[HTTP] GET... code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      String payload = https.getString();
      Serial.println("Payload config.json:");
      Serial.println(payload);

      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.print("Gagal parsing JSON: ");
        Serial.println(error.c_str());
        return;
      }

      // Update pesan
      if (doc.containsKey("message")) {
        String newMessage = doc["message"].as<String>();
        if (newMessage != message) {
          message = newMessage;
          Serial.println("Pesan diperbarui: " + message);
        }
      }

      // Update URL gambar
      if (doc.containsKey("image_urls")) {
        JsonArray urls = doc["image_urls"];
        activeImageCount = urls.size();
        if(activeImageCount > maxImages) activeImageCount = maxImages;

        Serial.println("Daftar URL gambar:");
        for (int i = 0; i < activeImageCount; i++) {
          String newURL = urls[i].as<String>();
          imageURLs[i] = newURL;
          imageReady[i] = false;
          Serial.printf("  imageURLs[%d] = %s\n", i, imageURLs[i].c_str());
        }
        
        // Clear unused slots
        for (int i = activeImageCount; i < maxImages; i++) {
          imageURLs[i] = "";
          imageReady[i] = false;
        }
      }

      saveConfig();
    } else {
      Serial.printf("[HTTP] GET failed, error: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();
  } else {
    Serial.println("[HTTP] Gagal terhubung ke server");
  }
}

// == Ambil gambar dari URL ==
void fetchImageFromURL(const String& binURL, int imageIndex) {
  if (WiFi.status() != WL_CONNECTED || binURL.isEmpty()) {
    imageReady[imageIndex] = false;
    return;
  }

  // Debug URL
  Serial.println("Mengunduh gambar " + String(imageIndex) + " dari: " + binURL);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  if (https.begin(client, binURL)) {
    int httpCode = https.GET();
    printDetailedHTTPInfo(https, httpCode);

    if (httpCode == 200) {
      WiFiClient* stream = https.getStreamPtr();
      int index = 0;
      while (https.connected() && index < 1024) {
        if (stream->available()) {
          imageBuffers[imageIndex][index++] = stream->read();
        }
      }
      imageReady[imageIndex] = (index == 1024);
      if (imageReady[imageIndex]) {
        Serial.println("Gambar " + String(imageIndex) + " berhasil diunduh");
      } else {
        Serial.println("Gambar " + String(imageIndex) + " tidak lengkap: " + String(index) + " bytes");
      }
    } else {
      Serial.println("Gagal mengunduh gambar " + String(imageIndex) + ", HTTP code: " + String(httpCode));
      imageReady[imageIndex] = false;
    }
    https.end();
  } else {
    Serial.println("Tidak dapat membuat koneksi ke URL gambar " + String(imageIndex));
  }
}

// == Tampilkan jam & tanggal ==
void updateTimeAndDate() {
  display.fillRect(0, 0, SCREEN_WIDTH, 40, SSD1306_BLACK);
  display.setTextSize(4);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(5, 0);
  if (hour() < 10) display.print("0");
  display.print(hour());
  display.print(showColon ? ":" : " ");
  if (minute() < 10) display.print("0");
  display.print(minute());

  display.setTextSize(1);
  display.setCursor(33, 32);
  String months[] = {"Jan", "Feb", "Mar", "Apr", "Mei", "Jun", "Jul", "Agu", "Sep", "Okt", "Nov", "Des"};
  String formattedDate = String(day()) + " " + months[month() - 1] + " " + String(year());
  display.print(formattedDate);
}

// == Teks berjalan ==
void drawScrollingText(String text, int &x, int y) {
  int textLength = text.length();
  int textWidth = textLength * 6;
  display.fillRect(0, y - 5, SCREEN_WIDTH, 20, SSD1306_BLACK);
  for (int i = 0; i < textLength; i++) {
    int charX = x + i * 6;
    if (charX >= 0 && (charX + 6) <= SCREEN_WIDTH) {
      display.setCursor(charX, y);
      display.print(text[i]);
    }
  }
  x--;
  if (x < -textWidth) x = SCREEN_WIDTH;
}

// == Tampilan jam ==
void showClock() {
  unsigned long currentMillis = millis();

  if (currentMillis - colonPreviousMillis >= interval) {
    colonPreviousMillis = currentMillis;
    showColon = !showColon;
    updateTimeAndDate();
  }

  if (currentMillis - textPreviousMillis >= textInterval) {
    textPreviousMillis = currentMillis;
    drawScrollingText(message, textX, textY);
  }
}

// == Tampilkan gambar ==
void showImage() {
  if (imageReady[currentImageIndex]) {
    display.drawBitmap(0, 0, imageBuffers[currentImageIndex], 128, 64, SSD1306_WHITE);
  } else {
    display.setTextSize(1);
    display.setCursor(20, 25);
    display.print("Loading image...");

    if (!imageReady[currentImageIndex] && WiFi.status() == WL_CONNECTED) {
      fetchImageFromURL(imageURLs[currentImageIndex], currentImageIndex);
    }
  }
}

// == Ganti status tampilan ==
void updateDisplayState() {
  unsigned long currentMillis = millis();

  if (currentMillis >= nextSwitchMillis) {
    fadeTransition();

    // Ganti state
    if (currentState == SHOW_CLOCK) {
      currentState = SHOW_IMAGE;
      if (!imageReady[currentImageIndex] && WiFi.status() == WL_CONNECTED) {
        fetchImageFromURL(imageURLs[currentImageIndex], currentImageIndex);
      }
    } else {
      currentState = SHOW_CLOCK;
      if (activeImageCount > 0) {
        currentImageIndex = (currentImageIndex + 1) % activeImageCount;
      } else {
        currentImageIndex = 0;
      }
    }

    nextSwitchMillis = currentMillis + switchInterval;
    display.clearDisplay();
  }
}

// == Transisi dengan efek fade ==
void fadeTransition() {
  for (int i = 0; i < 5; i++) {
    display.clearDisplay();
    display.display();
    delay(30);
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(D2, D1);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 25);
  display.println("Connecting WiFi...");
  display.display();

  loadConfig();

  WiFiManager wm;
  if (!wm.autoConnect("Hadiah_UlangTahun_AP", "12345678")) {
    Serial.println("Failed to connect. Restarting...");
    ESP.restart();
  }

  Serial.println("WiFi connected!");
  display.clearDisplay();
  display.setCursor(10, 25);
  display.println("WiFi Connected!");
  display.display();
  delay(1500);

  timeClient.begin();
  timeClient.update();
  setTime(timeClient.getEpochTime());

  // Cek konfigurasi terbaru
  fetchConfig();

  // Ambil semua gambar di awal
  display.clearDisplay();
  display.setCursor(10, 25);
  display.println("Downloading images...");
  display.display();

  for (int i = 0; i < activeImageCount; i++) {
    if (!imageURLs[i].isEmpty()) {
      display.clearDisplay();
      display.setCursor(10, 25);
      display.println("Downloading image " + String(i+1));
      display.setCursor(10, 35);
      display.println("of " + String(activeImageCount));
      display.display();

      
      Serial.println("Setup: Downloading image " + String(i) + " from URL: " + imageURLs[i]);
      fetchImageFromURL(imageURLs[i], i);
      delay(500); 
    }
  }

  display.clearDisplay();
  display.setCursor(10, 25);
  display.println("Siap?!");
  display.display();
  
  nextSwitchMillis = millis() + switchInterval;
  delay(1000);
}

void loop() {
  timeClient.update();
  setTime(timeClient.getEpochTime());

  // Cek update konfigurasi secara periodik
  unsigned long currentMillis = millis();
  if (currentMillis - lastConfigCheck >= configCheckInterval) {
    lastConfigCheck = currentMillis;
    fetchConfig();

    for (int i = 0; i < activeImageCount; i++) {
      if (!imageReady[i] && !imageURLs[i].isEmpty()) {
        fetchImageFromURL(imageURLs[i], i);
      }
    }
  }

  updateDisplayState();

  if (currentState == SHOW_CLOCK) {
    showClock();
  } else if (currentState == SHOW_IMAGE) {
    showImage();
  }

  display.display();
}
