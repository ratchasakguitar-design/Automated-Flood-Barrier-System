#define BLYNK_TEMPLATE_ID   "TMPL6-JUumo6A"
#define BLYNK_TEMPLATE_NAME "Automated Flood Barrier"
#define BLYNK_AUTH_TOKEN    "A3q_W39mnIA3gsYbqY7XhTRUT-2V7qdx"

#define BLYNK_PRINT Serial
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <BlynkSimpleEsp32.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <UniversalTelegramBot.h>
#include <HTTPClient.h> // ไลบรารีสำหรับยิง HTTP Request ไปยัง Open-Source Platform (ntfy)
#include "LittleFS.h"   // ไลบรารีบันทึกข้อมูลออฟไลน์ในหน่วยความจำภายใน

// --- ตั้งค่า Wi-Fi ---
char ssid[] = "TaTa";                     
char password[] = "0825074916"; 

// --- ตั้งค่า Telegram Bot ---
#define BOT_TOKEN "8740247469:AAHLEHZmjPyDsXF5Wiq_niF23mW7TaZiSlU"  
#define CHAT_ID   "6197769799"                                   

// --- ตั้งค่า Open-Source Notification Platform (ntfy.sh) ---
const String ntfyServer = "https://ntfy.sh/my_afbs_flood_alert_2026";

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

// --- กำหนดค่าพินฮาร์ดแวร์ ---
#define PIN_WL1     34   // Water Level 1 (Drain)
#define PIN_WL2     35   // Water Level 2 (Gate)
#define PIN_RAIN_AO 32   // Raindrop Analog
#define PIN_RAIN_DO 33   // Raindrop Digital
#define PIN_DHT     14   // DHT11 Data
#define PIN_SERVO   12   // Servo Signal
#define PIN_LED     4    // Warning LED

#define DHTTYPE DHT11  
DHT dht(PIN_DHT, DHTTYPE);
Servo myServo;

unsigned long lastTime = 0;
unsigned long lastDHTTime = 0;
unsigned long lastForecastTime = 0; 
unsigned long lastReconnectAttempt = 0; // ตัวแปรสำหรับจับเวลา Reconnect แบบ Non-blocking

// ตัวแปรหน่วงเวลาการแจ้งเตือน (60000 = 1 นาที)
unsigned long lastTelegramWL1 = 0;
unsigned long lastTelegramWL2 = 0;
unsigned long lastTelegramForecast = 0; 
unsigned long lastTelegramRain = 0;
const unsigned long ALERT_COOLDOWN = 60000; 

float currentTemp = 25.0;
float currentHum = 50.0;
float lastValidRainAO = 4095.0; 
float previousWL1 = 0.0; 

// ค่าชดเชยความร้อน DHT11 (Temperature Offset) แก้ปัญหาความร้อนสะสมจากบอร์ด
const float TEMP_OFFSET = 5.5; 

// ฟังก์ชันแปลงค่าระดับน้ำ (0.1 - 4.0 เมตร)
float convertToWaterMeters(int val) {
  int maxSensorVal = 2500; 
  if (val > maxSensorVal) val = maxSensorVal;
  
  float meters = 0.1 + ((float)val / (float)maxSensorVal) * 3.9;
  if (meters > 4.0) meters = 4.0;
  return meters;
}

// ฟังก์ชันจำแนกสถานะฝน
String getRainStatus(int analogVal, int digitalVal) {
  if (analogVal > 3000 && digitalVal == HIGH) {
    return "No Rain";       
  } else if (analogVal > 1200 && analogVal <= 3000) {
    return "Light Rain";    
  } else {
    return "Heavy Rain";    
  }
}

// ฟังก์ชันแปลงค่า Raindrop เป็นเปอร์เซ็นต์ (0% - 100%)
int getRainPercentage(int analogVal) {
  int percent = map(analogVal, 4095, 0, 0, 100);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return percent;
}

// ฟังก์ชันบันทึกข้อมูลลงหน่วยความจำภายใน (LittleFS) เมื่อไม่มีสัญญาณอินเทอร์เน็ต
void logDataToFlash(float wl1, float wl2, String rain, float temp, float hum) {
  File file = LittleFS.open("/datalog.txt", FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open file for appending");
    return;
  }
  String logEntry = "WL1:" + String(wl1, 2) + "m | WL2:" + String(wl2, 2) + "m | Rain:" + rain + " | Temp:" + String(temp, 1) + "C | Hum:" + String(hum, 1) + "%\n";
  if (file.print(logEntry)) {
    Serial.println(">>> Data saved to internal Flash (Offline Mode)");
  } else {
    Serial.println(">>> Write failed");
  }
  file.close();
}

// ฟังก์ชันสำหรับอ่านไฟล์ข้อมูลออฟไลน์จาก LittleFS ออกมาแสดงทาง Serial Monitor
void printLogFromFile() {
  Serial.println("\n--- [ อ่านข้อมูล Log ออฟไลน์จากหน่วยความจำภายใน ] ---");
  
  if (!LittleFS.exists("/datalog.txt")) {
    Serial.println("ไม่พบไฟล์บันทึกข้อมูล (ยังไม่มีการบันทึกข้อมูลออฟไลน์)");
    return;
  }

  File file = LittleFS.open("/datalog.txt", FILE_READ);
  if (!file) {
    Serial.println("เกิดข้อผิดพลาดในการเปิดไฟล์เพื่ออ่าน");
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    Serial.println(line);
  }
  
  file.close();
  Serial.println("--- [ สิ้นสุดข้อมูล Log ] ---\n");
}

// ฟังก์ชันส่งแจ้งเตือนผ่าน Open-Source Platform (ntfy.sh)
void sendOpenSourceAlert(String title, String message) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(ntfyServer);
    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    http.addHeader("Title", title);
    http.addHeader("Priority", "high");
    
    int httpResponseCode = http.POST(message);
    if (httpResponseCode > 0) {
      Serial.printf(">>> Open Source Alert Sent! Code: %d\n", httpResponseCode);
    } else {
      Serial.printf(">>> Open Source Alert Failed, Error: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    http.end();
  }
}

// ฟังก์ชันตรวจสอบและเชื่อมต่อ Wi-Fi และ Blynk แบบ Non-blocking (แก้ไขไม่ให้ค้างลูป)
void checkConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long currentMillis = millis();
    // ลองเชื่อมต่อใหม่ทุกๆ 10 วินาที โดยไม่บล็อกการทำงานหลักของโปรแกรม
    if (currentMillis - lastReconnectAttempt >= 10000) {
      lastReconnectAttempt = currentMillis;
      Serial.println("WiFi Lost! Reconnecting in background...");
      WiFi.begin(ssid, password);
    }
  } else {
    if (!Blynk.connected()) {
      Blynk.connect();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- AFBS Smart Alert System Starting ---");

  // เริ่มต้นใช้งานระบบไฟล์ภายใน LittleFS
  if(!LittleFS.begin(true)){
    Serial.println("An Error has occurred while mounting LittleFS");
  } else {
    Serial.println("LittleFS Mounted Successfully");
    printLogFromFile();
  }

  pinMode(PIN_RAIN_DO, INPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  dht.begin();
  secured_client.setInsecure();
  
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  
  myServo.setPeriodHertz(50);
  myServo.attach(PIN_SERVO, 500, 2400);
  myServo.write(0); 

  WiFi.begin(ssid, password);
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, password);
}

void loop() {
  checkConnection();
  
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.run();
  }
  
  unsigned long currentMillis = millis();

  // 1. อ่านค่า DHT11 ทุกๆ 15 วินาที พร้อมหักลบค่าความร้อนสะสม (Offset)
  if (currentMillis - lastDHTTime >= 15000) {
    lastDHTTime = currentMillis;
    float rawTemp = dht.readTemperature();
    float rawHum = dht.readHumidity();
    
    if (!isnan(rawTemp)) {
      currentTemp = rawTemp - TEMP_OFFSET; 
    }
    if (!isnan(rawHum)) {
      currentHum = rawHum;
    }
  }

  // 2. ลูปการทำงานหลักทุกๆ 2 วินาที
  if (currentMillis - lastTime >= 2000) {
    lastTime = currentMillis;

    int valWL1 = analogRead(PIN_WL1);
    int valWL2 = analogRead(PIN_WL2);
    int rawRainAO = analogRead(PIN_RAIN_AO);
    int valRainDO = digitalRead(PIN_RAIN_DO);
    
    // กรองสัญญาณรบกวนเซนเซอร์หยดน้ำด้วย Moving Average Filter
    lastValidRainAO = (lastValidRainAO * 0.6) + (rawRainAO * 0.4);
    int valRainAO = (int)lastValidRainAO;

    float wl1_m = convertToWaterMeters(valWL1);
    float wl2_m = convertToWaterMeters(valWL2);
    String rainStatusStr = getRainStatus(valRainAO, valRainDO);
    int rainPercent = getRainPercentage(valRainAO);

    Serial.println("========================================");5
    .
    ...3333333333333333333333333333333333333333333333333333333333333333333333333333333333333

    333333333

    3333333333333333333333333333333333333333333333333333333333333333333333336
    Serial.printf("WL1: %.2fm | WL2: %.2fm\n", wl1_m, wl2_m);
    Serial.printf("Rain Status: %s | Temp: %.2f C | Hum: %.2f %%\n", rainStatusStr.c_str(), currentTemp, currentHum);

    // หากออนไลน์ส่งขึ้น Blynk หากออฟไลน์บันทึกลงหน่วยความจำภายในอย่างถูกต้องทันที
    if (WiFi.status() == WL_CONNECTED && Blynk.connected()) {
      Blynk.virtualWrite(V0, wl1_m);           
      Blynk.virtualWrite(V1, wl2_m);           
      Blynk.virtualWrite(V2, rainStatusStr);   
      Blynk.virtualWrite(V3, currentTemp);   
      Blynk.virtualWrite(V4, currentHum);    
      Blynk.virtualWrite(V6, rainPercent);     
    } else {
      logDataToFlash(wl1_m, wl2_m, rainStatusStr, currentTemp, currentHum);
    }

    // ลอจิกควบคุมบานกั้นน้ำ (WL2)
    bool isBarrierOpen = (wl2_m >= 1.5);
    String barrierStatusStr = isBarrierOpen ? "BARRIER ON" : "BARRIER OFF";

    if (isBarrierOpen) {
      myServo.write(90);      
      digitalWrite(PIN_LED, HIGH);
      if (WiFi.status() == WL_CONNECTED) Blynk.virtualWrite(V5, barrierStatusStr);
    } else {
      myServo.write(0);        
      digitalWrite(PIN_LED, LOW);
      if (WiFi.status() == WL_CONNECTED) Blynk.virtualWrite(V5, barrierStatusStr);
    }

    // 3. ระบบพยากรณ์เตือนภัยล่วงหน้า (Predictive Rate of Change) ทุกๆ 30 วินาที
    if (currentMillis - lastForecastTime >= 30000) {
      float rateOfRise = wl1_m - previousWL1; 
      previousWL1 = wl1_m;
      lastForecastTime = currentMillis;

      if (rateOfRise > 0.2 && wl1_m < 3.0 && (rainStatusStr != "No Rain" || currentHum > 80.0)) {
        if (currentMillis - lastTelegramForecast >= ALERT_COOLDOWN && WiFi.status() == WL_CONNECTED) {
          String forecastMsg = "⚠️ *พยากรณ์เตือนภัยล่วงหน้า!*\n• ตรวจพบระดับน้ำ WL1 เพิ่มขึ้นอย่างรวดเร็ว\n• อัตราการขึ้น: " + String(rateOfRise, 2) + " เมตร\n• สถานะอากาศ: " + rainStatusStr + " (ความชื้น " + String(currentHum, 1) + "%)";
          
          bot.sendMessage(CHAT_ID, forecastMsg, "Markdown");
          sendOpenSourceAlert("พยากรณ์เตือนภัยล่วงหน้า (AFBS)", "ตรวจพบระดับน้ำ WL1 เพิ่มขึ้นอย่างรวดเร็ว อัตราการขึ้น: " + String(rateOfRise, 2) + " เมตร");
          
          lastTelegramForecast = currentMillis;
        }
      }
    }

// เช็คว่ามีการพิมพ์ข้อความมาทาง Serial Monitor หรือไม่
  if (Serial.available() > 0) {
    char incomingChar = Serial.read();
    if (incomingChar == 'r' || incomingChar == 'R') {
      printLogFromFile(); // เรียกฟังก์ชันอ่าน Log ออกมาแสดงทันที
    }
  }
    // 4. เงื่อนไขการส่งข้อความแจ้งเตือนผ่าน Telegram และ Open-Source ครบทุกเซนเซอร์
    bool wl1_alert = (wl1_m >= 3.0);                 
    bool wl2_touched = (wl2_m > 0.15);                  
    bool isLightRain = (rainStatusStr == "Light Rain"); 
    bool isHeavyRain = (rainStatusStr == "Heavy Rain"); 

    if (WiFi.status() == WL_CONNECTED) {
      // 4.1 แจ้งเตือนระดับน้ำ WL1
      if (wl1_alert && (currentMillis - lastTelegramWL1 >= ALERT_COOLDOWN)) {
        bot.sendMessage(CHAT_ID, "🚨 *Alert: Water Level 1 ถึงเกณฑ์!*\n• ระดับน้ำ WL1: " + String(wl1_m, 2) + " เมตร", "Markdown");
        sendOpenSourceAlert("เตือนภัย WL1 (AFBS)", "ระดับน้ำ WL1 สูงถึงเกณฑ์กำหนด: " + String(wl1_m, 2) + " เมตร");
        lastTelegramWL1 = currentMillis;
      }

      // 4.2 แจ้งเตือนระดับน้ำ WL2 / บานกั้นน้ำ
      if ((wl2_touched || isBarrierOpen) && (currentMillis - lastTelegramWL2 >= ALERT_COOLDOWN)) {
        bot.sendMessage(CHAT_ID, "🚨 *Alert: Water Level 2 / Barrier Action!*\n• ระดับน้ำ WL2: " + String(wl2_m, 2) + " เมตร", "Markdown");
        sendOpenSourceAlert("เตือนภัยวิกฤต WL2 (AFBS)", "ระดับน้ำ WL2 วิกฤต! สั่งเปิดบานกั้นน้ำอัตโนมัติ");
        lastTelegramWL2 = currentMillis;
      }

      // 4.3 แจ้งเตือนฝนตกเล็กน้อย (Light Rain)
      if (isLightRain && (currentMillis - lastTelegramRain >= ALERT_COOLDOWN)) {
        bot.sendMessage(CHAT_ID, "🌧 *Alert: ตรวจพบฝนตกปรอยๆ!*\n• ความชื้นฝน: " + String(rainPercent) + "%", "Markdown");
        sendOpenSourceAlert("แจ้งเตือนสภาพอากาศ (AFBS)", "ตรวจพบฝนตกปรอยๆ ความชื้นฝน: " + String(rainPercent) + "%");
        lastTelegramRain = currentMillis;
      }

      // 4.4 แจ้งเตือนฝนตกหนัก (Heavy Rain)
      if (isHeavyRain && (currentMillis - lastTelegramRain >= ALERT_COOLDOWN)) {
        bot.sendMessage(CHAT_ID, "⛈ *Alert: ฝนตกหนักต่อเนื่อง!*\n• ความชื้นฝน: " + String(rainPercent) + "%", "Markdown");
        sendOpenSourceAlert("แจ้งเตือนฝนตกหนัก (AFBS)", "ตรวจพบฝนตกหนักต่อเนื่อง ความชื้นฝน: " + String(rainPercent) + "%");
        lastTelegramRain = currentMillis;
      }
    }
  }
}