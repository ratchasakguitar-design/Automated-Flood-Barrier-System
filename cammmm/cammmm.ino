
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

// =========================
// WiFi
// =========================
const char* ssid = "TaTa";
const char* password = "0825074916";

// =========================
// AI-Thinker ESP32-CAM
// =========================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

WebServer server(80);

// =========================
// หน้าเว็บ Live Stream
// =========================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-CAM Live Stream</title>

  <style>
    body {
      margin: 0;
      background: #111;
      color: white;
      font-family: Arial, sans-serif;
      text-align: center;
    }

    h2 {
      margin: 15px 0;
      font-weight: normal;
    }

    img {
      width: 100%;
      max-width: 800px;
      height: auto;
      display: block;
      margin: auto;
    }
  </style>
</head>

<body>

  <h2>ESP32-CAM Live Stream</h2>

  <img src="/stream">

</body>
</html>
)rawliteral";

// =========================
// Live Stream
// =========================
void handleStream()
{
  WiFiClient client = server.client();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Cache-Control: no-cache");
  client.println("Connection: close");
  client.println();

  while (client.connected())
  {
    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb)
    {
      Serial.println("Camera capture failed");
      break;
    }

    client.print("--frame\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.print("Content-Length: ");
    client.print(fb->len);
    client.print("\r\n\r\n");

    client.write(fb->buf, fb->len);
    client.print("\r\n");

    esp_camera_fb_return(fb);

    delay(10);
  }

  client.stop();
}

// =========================
// หน้าเว็บหลัก
// =========================
void handleRoot()
{
  server.send_P(200, "text/html", INDEX_HTML);
}

// =========================
// Setup
// =========================
void setup()
{
  Serial.begin(115200);
  Serial.setDebugOutput(false);

  Serial.println();
  Serial.println("==============================");
  Serial.println("ESP32-CAM Live Stream");
  Serial.println("==============================");

  // =========================
  // ตั้งค่ากล้อง
  // =========================
  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;

  // JPEG สำหรับ Live Stream
  config.pixel_format = PIXFORMAT_JPEG;

  // =========================
  // ความละเอียด
  // =========================
  if (psramFound())
  {
    config.frame_size = FRAMESIZE_VGA;   // 640x480
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  }
  else
  {
    config.frame_size = FRAMESIZE_QVGA;  // 320x240
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  // =========================
  // เริ่มต้นกล้อง
  // =========================
  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK)
  {
    Serial.printf(
      "Camera init failed with error 0x%x\n",
      err
    );

    while (true)
    {
      delay(1000);
    }
  }

  Serial.println("Camera initialized.");

  // =========================
  // ตั้งค่า Sensor
  // =========================
  sensor_t *s = esp_camera_sensor_get();

  if (s != NULL)
  {
    // =========================
    // กลับหัวภาพ 180 องศา
    // =========================
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);

    // ปรับภาพ
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);

    // ควบคุมแสงอัตโนมัติ
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_awb_gain(s, 1);
  }

  // =========================
  // เชื่อมต่อ WiFi
  // =========================
  WiFi.mode(WIFI_STA);

  // ปิด WiFi Sleep เพื่อช่วยลดความหน่วง
  WiFi.setSleep(false);

  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected.");

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // =========================
  // Web Server
  // =========================
  server.on("/", HTTP_GET, handleRoot);
  server.on("/stream", HTTP_GET, handleStream);

  server.begin();

  Serial.println("Web Server started.");
  Serial.println();
  Serial.print("Open: http://");
  Serial.println(WiFi.localIP());
}

// =========================
// Loop
// =========================
void loop()
{
  server.handleClient();
}
