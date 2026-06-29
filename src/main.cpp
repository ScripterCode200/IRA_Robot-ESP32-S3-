#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <math.h> 
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// --- ESP32-S3 CAMERA LIBRARIES & CONFIGURATION ---
#include "esp_camera.h"
#include "esp_http_server.h"

// Freenove ESP32-S3 WROOM Camera Pinout
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

// --- HARDWARE PINS ---
#define SERVO_PIN         47 // Head Servo (Up/Down)
#include <ESP32Servo.h>
Servo headServo;

// --- WEBSOCKET SERVER ---
WebSocketsServer webSocket(81);
WiFiUDP udp;
WiFiUDP videoUdp; // Dedicated UDP socket for video streaming

bool cameraEnabled = false;
bool screenEnabled = true;
bool isWsConnected = false;
bool cameraStreaming = false; // TRUE = Push mode active
IPAddress videoClientIP; // Stores the phone's IP address for UDP targeting
uint8_t videoFrameId = 0; // Wraps around 0-255
unsigned long lastFrameTime = 0;

// Mutex to protect WebSocket operations across cores
SemaphoreHandle_t wsMutex = NULL;

// --- PSRAM AUDIO BUFFERING ---
#define AUDIO_BUFFER_MAX_SIZE 450000 // 10 seconds of 22050Hz 16-bit Mono (450 KB)
uint8_t* psramAudioBuffer = NULL;
uint32_t audioBufferLen = 0;
bool isReceivingAudio = false;

EventGroupHandle_t audioEventGroup = NULL;
#define AUDIO_PLAY_EVENT (1 << 0)

// Camera Log forwarder (defined ahead)
void logToApp(String msg);

#define I2S_PORT I2S_NUM_0

void initI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 11025, // Downsampled from 22050Hz with low-pass filter
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Restored to ONLY_LEFT to read Mono correctly
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false, // APLL disabled: Can cause clock corruption on some S3 revisions
    .tx_desc_auto_clear = true
  };
  
  if (i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL) != ESP_OK) {
    Serial.println("I2S driver install failed");
    return;
  }
  
  i2s_pin_config_t pin_config = {
    .bck_io_num = 40,
    .ws_io_num = 39,
    .data_out_num = 3,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  
  if (i2s_set_pin(I2S_PORT, &pin_config) != ESP_OK) {
    Serial.println("I2S pin setup failed");
  }

  // Explicitly start the I2S peripheral (required on some ESP32 cores)
  i2s_start(I2S_PORT);
}

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
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
  config.xclk_freq_hz = 10000000; // Lower to 10MHz to heavily reduce PSRAM DMA contention and heat
  config.pixel_format = PIXFORMAT_JPEG;
  
  // Default to QVGA to prevent network saturation
  config.frame_size = FRAMESIZE_QVGA; 
  config.jpeg_quality = 20; // 0-63 (lower = higher quality). 20 is a great balance of size and clarity.
  config.fb_count = 2; // DOUBLE BUFFERING: Allows DMA to capture a frame while the CPU sends the previous one!
  config.fb_location = CAMERA_FB_IN_PSRAM; // Store frame buffers in OPI PSRAM
  config.grab_mode = CAMERA_GRAB_LATEST; // Always grab the freshest frame on demand

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    logToApp("Camera init failed with error 0x" + String(err, HEX));
    return;
  }
  logToApp("Camera hardware successfully initialized.");

  // Apply vertical flip + horizontal mirror to correct the physically
  // upside-down camera mount orientation at the OV2640 sensor register level.
  sensor_t* s = esp_camera_sensor_get();
  if (s != NULL) {
    s->set_vflip(s, 1);    // Flip vertically (fixes upside-down image)
    s->set_hmirror(s, 1);  // Mirror horizontally (corrects left-right after vflip)
    logToApp("Camera orientation corrected: vflip=1, hmirror=1.");
  }
}


// Enter your Mobile Hotspot Name and Password here!
const char* STA_SSID = "Shivam";
const char* STA_PASSWORD = "1234567891";

// Log Buffer for transmitting detailed logs to Cockpit App
void logToApp(String msg) {
  if (wsMutex != NULL) {
    if (xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY)) {
      webSocket.broadcastTXT("🤖 [ROBOT] " + msg);
      xSemaphoreGiveRecursive(wsMutex);
    }
  } else {
    webSocket.broadcastTXT("🤖 [ROBOT] " + msg);
  }
  Serial.println(msg);
}

// --- FREE RTOS CORE 1 TASK (Audio Playback) ---
void audioTask(void * pvParameters) {
  while (true) {
    // Wait infinitely until playback is triggered by audio_end
    if (audioEventGroup != NULL) {
      xEventGroupWaitBits(audioEventGroup, AUDIO_PLAY_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);
      
      if (psramAudioBuffer != NULL && audioBufferLen > 0) {
        Serial.printf("Playing buffered audio: %u bytes\n", audioBufferLen);
        
        size_t bytes_written = 0;
        uint32_t offset = 0;
        
        // Pump the entire PSRAM buffer into the I2S DMA in chunks
        while (offset < audioBufferLen) {
          uint32_t chunk = (audioBufferLen - offset > 2048) ? 2048 : (audioBufferLen - offset);
          i2s_write(I2S_PORT, psramAudioBuffer + offset, chunk, &bytes_written, portMAX_DELAY);
          offset += chunk;
        }
        
        Serial.println("Audio playback complete.");
      }
    }
  }
}

// --- FREE RTOS CORE 0 TASK (Camera & WebSockets) ---
void cameraTask(void * pvParameters) {
  while (true) {
    // 1. Process WebSocket messages
    if (wsMutex != NULL) {
      if (xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY)) {
        webSocket.loop();
        xSemaphoreGiveRecursive(wsMutex);
      }
    }

    // 2. Camera Frame Grabbing (UDP Streaming)
    if (cameraEnabled && isWsConnected && cameraStreaming) {
      if (millis() - lastFrameTime >= 100) { // Throttled to ~10 FPS to prevent WiFi Saturation
        lastFrameTime = millis();
        camera_fb_t * fb = esp_camera_fb_get();
        if (fb) {
          if (fb->len < 100000 && videoClientIP[0] != 0) {  
            // Custom UDP Fragmentation Engine
            videoFrameId++;
            int chunkSize = 1400; // Safe MTU size
            int totalChunks = (fb->len + chunkSize - 1) / chunkSize;
            if (totalChunks <= 255) { // Prevent header overflow
              for (int i = 0; i < totalChunks; i++) {
                int offset = i * chunkSize;
                int currentChunkSize = fb->len - offset;
                if (currentChunkSize > chunkSize) currentChunkSize = chunkSize;

                uint8_t header[4];
                header[0] = 0x56; // Magic byte 'V'
                header[1] = videoFrameId;
                header[2] = (uint8_t)i;
                header[3] = (uint8_t)totalChunks;

                videoUdp.beginPacket(videoClientIP, 8889);
                videoUdp.write(header, 4);
                videoUdp.write(fb->buf + offset, currentChunkSize);
                if (videoUdp.endPacket() == 0) {
                  // If the lwIP stack runs out of memory (ENOMEM), abort sending the rest of this frame
                  break;
                }

                // Brief pause to allow the FreeRTOS WiFi stack to breathe and prevent packet drops
                vTaskDelay(1); // Non-blocking yield
              }
            }
          }
          esp_camera_fb_return(fb);
        }
      }
    }
    
    // Give the RTOS watchdog time to breathe
    vTaskDelay(pdMS_TO_TICKS(10)); // Increased from 5 to 10 to further reduce CPU load
  }
}

// Onboard LED setup 
// Note: Changed from Pin 48 to 46 because GPIO 48 is used by INMP441 WS pin!
Adafruit_NeoPixel LED_RGB(1, 46, NEO_GRB + NEO_KHZ800); 

// Hardware I2C Constructor for SH1106 OLED
// SCL (Clock) is 19, SDA (Data) is 20
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ 19, /* data=*/ 20);

int frame = 0; // Animation frame counter

// --- MOTOR DRIVER PINS (TB6612FNG / L298N compatible) ---
// Left Wheel  : AIN1 = GPIO 19 (forward),  AIN2 = GPIO 20 (backward)
// Right Wheel : BIN1 = GPIO 48 (forward),  BIN2 = GPIO 47 (backward)
// ⚠️ FIX: We need 4 absolutely bulletproof pins that are NOT used by the Camera, USB, PSRAM, or internal Flash Memory.
// Using GPIO 38 (Flash memory) caused the motor to spin wildly and the ESP32 to crash!
// Please update your wiring to these 4 clean, safe pins:
#define MOTOR_AIN1  1
#define MOTOR_AIN2  2
#define MOTOR_BIN1  14
#define MOTOR_BIN2  42

// --- MOTOR CONTROL (D-Pad Only: 5 discrete states) ---
// 
// Pin logic for each motor:
//   IN1=HIGH, IN2=LOW  → Motor spins FORWARD
//   IN1=LOW,  IN2=HIGH → Motor spins BACKWARD
//   IN1=LOW,  IN2=LOW  → Motor STOPS
//

//
// D-Pad commands from the app:
//   ▲ FORWARD:  speed=+1, turn=0  → Both motors forward
//   ▼ BACKWARD: speed=-1, turn=0  → Both motors backward
//   ► RIGHT:    speed=0, turn=+1  → Left forward + Right backward (pivot right)
//   ◄ LEFT:     speed=0, turn=-1  → Left backward + Right forward (pivot left)
//   ■ STOP:     speed=0, turn=0   → Both motors stop
//
bool isRobotActive = false; // Safe tracking of motor state

void driveMotors(float speed, float turn) {

  // Update global state
  isRobotActive = (abs(speed) > 0.05f || abs(turn) > 0.05f);

  // --- LEFT MOTOR (AIN1 + AIN2) ---
  float leftCmd = speed + turn;
  if (leftCmd > 0.1f) {
    // Left motor FORWARD
    digitalWrite(MOTOR_AIN1, HIGH);
    digitalWrite(MOTOR_AIN2, LOW);
  } else if (leftCmd < -0.1f) {
    // Left motor BACKWARD
    digitalWrite(MOTOR_AIN1, LOW);
    digitalWrite(MOTOR_AIN2, HIGH);
  } else {
    // Left motor STOP
    digitalWrite(MOTOR_AIN1, LOW);
    digitalWrite(MOTOR_AIN2, LOW);
  }

  // --- RIGHT MOTOR (BIN1 + BIN2) ---
  float rightCmd = speed - turn;
  if (rightCmd > 0.1f) {
    // Right motor FORWARD
    digitalWrite(MOTOR_BIN1, HIGH);
    digitalWrite(MOTOR_BIN2, LOW);
  } else if (rightCmd < -0.1f) {
    // Right motor BACKWARD
    digitalWrite(MOTOR_BIN1, LOW);
    digitalWrite(MOTOR_BIN2, HIGH);
  } else {
    // Right motor STOP
    digitalWrite(MOTOR_BIN1, LOW);
    digitalWrite(MOTOR_BIN2, LOW);
  }

  // DEBUG: Show exact pin states
  Serial.printf("[MOTOR] spd=%.0f trn=%.0f => AIN1(pin1)=%d AIN2(pin2)=%d BIN1(pin14)=%d BIN2(pin42)=%d\n",
    speed, turn,
    digitalRead(MOTOR_AIN1), digitalRead(MOTOR_AIN2),
    digitalRead(MOTOR_BIN1), digitalRead(MOTOR_BIN2));
}

// --- EMOTION STATE MACHINE ---
// 0: IDLE, 1: HAPPY, 2: WONDER, 3: SLEEPY, 4: WINK
int currentState = 0;
int stateTimer = 200; // Frames before changing emotions

// --- BLINKING LOGIC ---
int blinkTimer = 150; // Countdown to next blink
bool isBlinking = false;

// --- GAZE/LOOKING LOGIC (Smooth transition) ---
int gazeTimer = 80;      // Countdown to changing gaze direction
int targetLookX = 0;     // Target eye pupil offset X
int targetLookY = 0;     // Target eye pupil offset Y
float currentLookX = 0;  // Interpolated eye pupil offset X
float currentLookY = 0;  // Interpolated eye pupil offset Y

// --- CUSTOM FACE BUFFER ---
uint8_t customFaceBuffer[1024];

void processDriveCommand(String text) {
  int mIdx = text.indexOf("M:");
  int gIdx = text.indexOf(";G:");
  int hIdx = text.indexOf(";H:");
  int sIdx = text.indexOf(";S:");

  if (mIdx != -1) {
    int commaIdx = text.indexOf(",", mIdx);
    if (commaIdx != -1 && (gIdx == -1 || commaIdx < gIdx)) {
      String turnStr = text.substring(mIdx + 2, commaIdx);
      String speedStr = text.substring(commaIdx + 1, gIdx != -1 ? gIdx : text.length());
      float turn = turnStr.toFloat();
      float speed = speedStr.toFloat();
      driveMotors(speed, turn);
    }
  }

  if (gIdx != -1) {
    int commaIdx = text.indexOf(",", gIdx);
    if (commaIdx != -1 && (hIdx == -1 || commaIdx < hIdx)) {
      String servoStr = text.substring(gIdx + 3, commaIdx);
      int angle = servoStr.toInt();
      if (angle < 15) angle = 15;
      if (angle > 180) angle = 180;
      headServo.write(angle);
    }
  }
  
  if (hIdx != -1) {
    int hVal = text.substring(hIdx + 3, hIdx + 4).toInt();
    static int lastHVal = -1;
    if (hVal != lastHVal) {
      lastHVal = hVal;
    }
    if (hVal == 1 && currentState == 0) {
      currentState = 4;
      stateTimer = 60;
    }
  }
  
  if (sIdx != -1) {
    int sVal = text.substring(sIdx + 3, sIdx + 4).toInt();
    static int lastSVal = -1;
    if (sVal != lastSVal) {
      lastSVal = sVal;
    }
    if (sVal == 1 && currentState == 0) {
      currentState = 1;
      stateTimer = 90;
    }
    // TCP Disconnect Emergency Brake (replaces the UDP Watchdog)
    if (!isWsConnected) {
      driveMotors(0.0, 0.0);
    }
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) {
    isWsConnected = true;
    // IMPORTANT: Send role JSON as the VERY FIRST message before anything else
    webSocket.sendTXT(num, "{\"role\":\"robot\"}");
    Serial.printf("[%u] Connected!\n", num);
  } else if (type == WStype_DISCONNECTED) {
    isWsConnected = false;
    cameraStreaming = false;
    Serial.printf("[%u] Disconnected!\n", num);
  } else if (type == WStype_BIN) {
    if (isReceivingAudio && psramAudioBuffer != NULL) {
      // Append raw binary data seamlessly to PSRAM buffer
      if (audioBufferLen + length <= AUDIO_BUFFER_MAX_SIZE) {
        memcpy(psramAudioBuffer + audioBufferLen, payload, length);
        audioBufferLen += length;
      } else {
        Serial.println("Audio Buffer Overflow! Discarding chunk.");
      }
    } else if (length == 1024 && !isReceivingAudio) {
      memcpy(customFaceBuffer, payload, 1024);
      currentState = 100; // 100 = CUSTOM FACE STATE
      stateTimer = 99999; // Keep it on screen for a long time
      logToApp("Received Custom Face (1024 bytes)!");
    }
  } else if (type == WStype_TEXT) {
    String text((char*)payload, length);
    
    if (text == "audio_start") {
      isReceivingAudio = true;
      audioBufferLen = 0;
      logToApp("Downloading audio to PSRAM...");
      return;
    } else if (text == "audio_end") {
      isReceivingAudio = false;
      logToApp("Download complete. Playing!");
      if (audioEventGroup != NULL) {
        xEventGroupSetBits(audioEventGroup, AUDIO_PLAY_EVENT);
      }
      return;
    } else if (text == "stream_start") {
      cameraStreaming = true;
      videoClientIP = webSocket.remoteIP(num); // Capture phone's IP
      logToApp("Video Stream Started (UDP port 8889)");
      return;
    } else if (text == "stream_stop") {
      cameraStreaming = false;
      logToApp("Video Stream Stopped");
      return;
    } else if (text == "camera_off") {
      cameraEnabled = false;
      logToApp("Camera turned OFF");
      return;
    } else if (text == "camera_on") {
      cameraEnabled = true;
      logToApp("Camera turned ON");
      return;
    } else if (text == "quality_vga") {
      sensor_t * s = esp_camera_sensor_get();
      s->set_framesize(s, FRAMESIZE_VGA);
      logToApp("Camera Quality -> VGA");
      return;
    } else if (text == "quality_uxga") {
      sensor_t * s = esp_camera_sensor_get();
      s->set_framesize(s, FRAMESIZE_UXGA);
      logToApp("Camera Quality -> UXGA");
      return;
    } else if (text == "screen_off") {
      screenEnabled = false;
      u8g2.clearBuffer();
      u8g2.sendBuffer(); // Blank the OLED
      logToApp("OLED Screen OFF");
      return;
    } else if (text == "screen_on") {
      screenEnabled = true;
      logToApp("OLED Screen ON");
      return;
    }

    if (text.startsWith("cmd:")) {
      String cmd = text.substring(4);
      if (cmd == "face_happy") { currentState = 1; stateTimer = 300; logToApp("Expression -> HAPPY"); }
      else if (cmd == "face_excited") { currentState = 4; stateTimer = 300; logToApp("Expression -> EXCITED (WINK)"); }
      else if (cmd == "face_shocked") { currentState = 2; stateTimer = 300; logToApp("Expression -> WONDER/SHOCKED"); }
      else if (cmd == "face_sad") { currentState = 3; stateTimer = 300; logToApp("Expression -> SLEEPY/SAD"); }
      else if (cmd == "horn") { logToApp("Chirping Horn!"); LED_RGB.setPixelColor(0, LED_RGB.Color(255, 255, 255)); LED_RGB.show(); delay(100); }
      else if (cmd == "action_wave") { currentState = 5; stateTimer = 150; logToApp("Action -> WAVE"); }
      else if (cmd == "action_rock") { currentState = 6; stateTimer = 150; logToApp("Action -> ROCK"); }
      else if (cmd == "action_paper") { currentState = 7; stateTimer = 150; logToApp("Action -> PAPER"); }
      else if (cmd == "action_scissors") { currentState = 8; stateTimer = 150; logToApp("Action -> SCISSORS"); }
      else if (cmd == "face_angry") { currentState = 9; stateTimer = 300; logToApp("Expression -> ANGRY"); }
      else if (cmd == "face_love") { currentState = 10; stateTimer = 300; logToApp("Expression -> LOVE"); }
      else if (cmd == "face_dizzy") { currentState = 11; stateTimer = 300; logToApp("Expression -> DIZZY"); }
      else if (cmd.startsWith("drive_")) {
          int dur = 1500; // Default if not provided
          int colIdx = cmd.indexOf(':');
          String baseCmd = cmd;
          if (colIdx != -1) {
              baseCmd = cmd.substring(0, colIdx);
              dur = cmd.substring(colIdx + 1).toInt();
          }

          if (baseCmd == "drive_forward") { driveMotors(1.0, 0.0); logToApp("Driving Forward (" + String(dur) + "ms)"); }
          else if (baseCmd == "drive_backward") { driveMotors(-1.0, 0.0); logToApp("Driving Backward (" + String(dur) + "ms)"); }
          else if (baseCmd == "drive_left") { driveMotors(0.0, -1.0); logToApp("Turning Left (" + String(dur) + "ms)"); }
          else if (baseCmd == "drive_right") { driveMotors(0.0, 1.0); logToApp("Turning Right (" + String(dur) + "ms)"); }
          else if (baseCmd == "drive_stop") { driveMotors(0.0, 0.0); logToApp("Stopping"); }
      }
      else if (cmd.startsWith("servo_")) {
        int angle = cmd.substring(6).toInt();
        if (angle < 0) angle = 0;
        if (angle > 110) angle = 110; // Strict hardware limit
        headServo.write(angle);
      }
      return;
    }

    processDriveCommand(text);
  }

}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== IRA Robot Booting ===");

  // ── Initialize I2S Audio ──
  initI2S();

  // ── Initialize OLED FIRST so we can show boot status ──
  u8g2.setBusClock(400000);
  u8g2.begin(); 
  u8g2.setContrast(255);

  // ── Boot splash screen ──
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(30, 25, "IRA");
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(20, 40, "Robot Companion");
  u8g2.drawStr(25, 55, "Booting...");
  u8g2.sendBuffer();
  delay(1000);

  // ── OLED boot log helper ──
  // We'll track boot log lines and display them
  String bootLines[8]; // 8 lines fit on 128x64 OLED with small font
  int bootLineCount = 0;

  auto oledLog = [&](const char* msg) {
    Serial.println(msg);
    if (bootLineCount < 8) {
      bootLines[bootLineCount++] = String(msg);
    } else {
      // Scroll up
      for (int i = 0; i < 7; i++) bootLines[i] = bootLines[i + 1];
      bootLines[7] = String(msg);
    }
    // Render all lines
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    for (int i = 0; i < bootLineCount; i++) {
      u8g2.drawStr(0, 7 + i * 8, bootLines[i].c_str());
    }
    u8g2.sendBuffer();
  };

  oledLog("OLED: OK");

  // ── Motor Driver ──
  pinMode(MOTOR_AIN1, OUTPUT); digitalWrite(MOTOR_AIN1, LOW);
  pinMode(MOTOR_AIN2, OUTPUT); digitalWrite(MOTOR_AIN2, LOW);
  pinMode(MOTOR_BIN1, OUTPUT); digitalWrite(MOTOR_BIN1, LOW);
  pinMode(MOTOR_BIN2, OUTPUT); digitalWrite(MOTOR_BIN2, LOW);
  oledLog("Motors: OK");

  // ── Servo Initialization ──
  headServo.setPeriodHertz(50); // Standard 50Hz servo
  headServo.attach(SERVO_PIN, 500, 2500); // Attach pin and set min/max pulse widths
  headServo.write(100); // Start at default 100 deg
  oledLog("Servo: OK");

  // ── NeoPixel LED ──
  LED_RGB.begin();
  LED_RGB.setBrightness(40);
  LED_RGB.setPixelColor(0, LED_RGB.Color(0, 0, 50)); // Blue = booting
  LED_RGB.show();
  oledLog("LED: OK");

  // ── Random seed ──
  randomSeed(esp_random());

  // ── WiFi Connection (Station Mode) ──
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  char ssidMsg[40];
  snprintf(ssidMsg, sizeof(ssidMsg), "WiFi STA: %s", STA_SSID);
  oledLog(ssidMsg);
  oledLog("Connecting...");

  WiFi.begin(STA_SSID, STA_PASSWORD);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  
  if (wifiConnected) {
    WiFi.setSleep(false); // Disable WiFi power saving for maximum stability and throughput
  }
  
  // Reset boot log for next phase
  bootLineCount = 0;

  if (wifiConnected) {
    LED_RGB.setPixelColor(0, LED_RGB.Color(0, 50, 0)); // Green = connected
    LED_RGB.show();

    char ipMsg[40];
    snprintf(ipMsg, sizeof(ipMsg), "Connected!");
    oledLog(ipMsg);
    snprintf(ipMsg, sizeof(ipMsg), "IP: %s", WiFi.localIP().toString().c_str());
    oledLog(ipMsg);
    
    Serial.println("\nConnected to Hotspot!");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    LED_RGB.setPixelColor(0, LED_RGB.Color(50, 0, 0)); // Red = failed
    LED_RGB.show();

    oledLog("AP FAILED!");
    
    Serial.println("\nAP FAILED!");
    delay(3000); // Show error for 3 seconds
  }

  // ── Camera Init ──
  oledLog("Camera: init...");
  initCamera();
  oledLog("Camera: OK");

  // ── mDNS & WebSocket Server ──
  if (wifiConnected) {
    if (MDNS.begin("ira")) {
      oledLog("mDNS: ira.local");
      Serial.println("MDNS responder started: ira.local");
    } else {
      oledLog("mDNS: FAILED");
    }
    
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    udp.begin(8888);
    oledLog("WS Server: 81");

    // --- Create FreeRTOS Mutex and Task ---
    wsMutex = xSemaphoreCreateRecursiveMutex();
    
    // Allocate 450KB buffer in external SPI RAM
    psramAudioBuffer = (uint8_t*)heap_caps_malloc(AUDIO_BUFFER_MAX_SIZE, MALLOC_CAP_SPIRAM);
    if (psramAudioBuffer == NULL) {
      Serial.println("CRITICAL ERROR: Failed to allocate PSRAM audio buffer!");
    } else {
      Serial.println("Successfully allocated 450KB PSRAM audio buffer.");
    }
    
    audioEventGroup = xEventGroupCreate();
    
    xTaskCreatePinnedToCore(
      audioTask,
      "AudioTask",
      4096,
      NULL,
      1,
      NULL,
      1 // Pin to Core 1 (Application CPU) to free up WiFi core
    );

    xTaskCreatePinnedToCore(
      cameraTask,       // Task function
      "CameraTask",     // Name of task
      8192,             // Stack size
      NULL,             // Parameter
      1,                // Priority (1 is standard)
      NULL,             // Task handle
      0                 // Pin to Core 0 (Pro CPU - WiFi/BT)
    );
  } else {
    oledLog("WS Server: Skipped");
    oledLog("(No WiFi)");
  }

  oledLog("Boot complete!");
  delay(2000); // Show final status for 2 seconds
}

// --- CUSTOM DRAWING HELPERS ---

// Draws a cute pixel-art blush using a dither (checkerboard) pattern
void drawBlush(int cx, int cy) {
  u8g2.setDrawColor(1);
  for (int x = -4; x <= 4; x++) {
    for (int y = -2; y <= 2; y++) {
      // Keep it within a soft oval boundary
      if ((x*x) / 16.0 + (y*y) / 4.0 <= 1.0) {
        // Pixel-art checkboard dither pattern
        if ((x + y) % 2 == 0) {
          u8g2.drawPixel(cx + x, cy + y);
        }
      }
    }
  }
}

// Custom Eye Graphics
void drawHeartEye(int cx, int cy) {
  u8g2.setDrawColor(1);
  u8g2.drawBox(cx - 5, cy - 6, 4, 4);
  u8g2.drawBox(cx + 1, cy - 6, 4, 4);
  u8g2.drawBox(cx - 7, cy - 4, 14, 4);
  u8g2.drawBox(cx - 5, cy, 10, 2);
  u8g2.drawBox(cx - 3, cy + 2, 6, 2);
  u8g2.drawBox(cx - 1, cy + 4, 2, 2);
}

void drawDizzyEye(int cx, int cy) {
  u8g2.setDrawColor(1);
  u8g2.drawLine(cx - 6, cy - 6, cx + 6, cy + 6);
  u8g2.drawLine(cx - 5, cy - 6, cx + 7, cy + 6);
  u8g2.drawLine(cx - 6, cy + 6, cx + 6, cy - 6);
  u8g2.drawLine(cx - 5, cy + 6, cx + 7, cy - 6);
}


// Draws a cute expressive eyebrow
void drawEyebrow(int x, int y, bool isLeft, int st) {
  u8g2.setDrawColor(1);
  
  int slantL = isLeft ? -1 : 1;
  int slantR = isLeft ? 1 : -1;
  
  if (st == 1) { // HAPPY (Curved upward)
    u8g2.drawLine(x - 8, y - 13, x, y - 16);
    u8g2.drawLine(x, y - 16, x + 8, y - 13);
    u8g2.drawLine(x - 8, y - 12, x, y - 15);
    u8g2.drawLine(x, y - 15, x + 8, y - 12);
  } 
  else if (st == 2) { // WONDER (One raised higher inquisitively)
    if (isLeft) {
      u8g2.drawLine(x - 8, y - 18, x + 8, y - 16); // Raised Left
      u8g2.drawLine(x - 8, y - 17, x + 8, y - 15);
    } else {
      u8g2.drawLine(x - 8, y - 14 + slantL, x + 8, y - 14 + slantR); // Normal Right
      u8g2.drawLine(x - 8, y - 13 + slantL, x + 8, y - 13 + slantR);
    }
  } 
  else if (st == 3) { // SLEEPY (Flat and low)
    u8g2.drawLine(x - 8, y - 12, x + 8, y - 12);
  } 
  else if (st == 9) { // ANGRY (Slanted downwards)
    if (isLeft) {
      u8g2.drawLine(x - 8, y - 16, x + 8, y - 11);
      u8g2.drawLine(x - 8, y - 15, x + 8, y - 10);
    } else {
      u8g2.drawLine(x - 8, y - 11, x + 8, y - 16);
      u8g2.drawLine(x - 8, y - 10, x + 8, y - 15);
    }
  }
  else if (st == 10 || st == 11) {
    // No eyebrows for love/dizzy
  }
  else { // IDLE / DEFAULT (Gentle relaxed slant)
    u8g2.drawLine(x - 8, y - 14 + slantL, x + 8, y - 14 + slantR);
    u8g2.drawLine(x - 8, y - 13 + slantL, x + 8, y - 13 + slantR);
  }
}

// Draws a highly detailed open/happy/closed eye with pupil sparkles
void drawEye(int x, int y, bool open, bool happy, int lookX, int lookY) {
  if (!open) {
    // Cute chubby closed eye (thick horizontal rounded line)
    u8g2.drawRBox(x - 7, y - 1, 14, 3, 1);
  } 
  else if (happy) {
    // Sleek upward happy curve ( ^ )
    u8g2.drawLine(x - 8, y + 2, x, y - 5);
    u8g2.drawLine(x, y - 5, x + 8, y + 2);
    u8g2.drawLine(x - 8, y + 3, x, y - 4); // Double thickness for styling
    u8g2.drawLine(x, y - 4, x + 8, y + 3);
  } 
  else {
    // Open expressive eye
    u8g2.setDrawColor(1);
    u8g2.drawRBox(x - 7, y - 9, 14, 18, 4); // White rounded outer eye
    
    // Draw pupils / sparkles in black (0) inside the white eye
    u8g2.setDrawColor(0);
    u8g2.drawBox(x + lookX + 1, y + lookY - 6, 3, 3); // Main top-right twinkle
    u8g2.drawBox(x + lookX - 4, y + lookY + 2, 2, 2); // Secondary bottom-left twinkle
    u8g2.setDrawColor(1);
  }
}

// Draws a beautifully smooth, animated mouth based on emotional state
void drawMouth(int cx, int cy, int st, float breath) {
  u8g2.setDrawColor(1);
  
  if (st == 1) { // HAPPY: Big open laughing mouth (crescent half-circle)
    int w = 9;
    int h = 9 + (int)(breath * 2.0); // Responds to breathing!
    u8g2.drawDisc(cx, cy, w);
    u8g2.setDrawColor(0);
    u8g2.drawBox(cx - w - 1, cy - w - 1, (w + 1) * 2, w + 1); // Mask top half
    u8g2.setDrawColor(1);
  } 
  else if (st == 2) { // WONDER: Small talking "o" shape
    int r = 4 + (int)(breath * 1.0);
    u8g2.drawDisc(cx, cy, r);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(cx, cy, r - 2); // Hollow center
    u8g2.setDrawColor(1);
  } 
  else if (st == 3) { // SLEEPY: Small relaxed smile
    int r = 5;
    u8g2.drawDisc(cx, cy - 2, r);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(cx, cy - 4, r + 1); // Hollow cut
    u8g2.drawBox(cx - r - 1, cy - r - 4, (r + 1) * 2, r + 1); // Clear top
    u8g2.setDrawColor(1);
  } 
  else if (st == 4) { // WINK / SMIRK: Cute side smirk
    int r = 6;
    int smirkX = cx + 3; // Shifted horizontally
    u8g2.drawDisc(smirkX, cy, r);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(smirkX, cy - 2, r + 1);
    u8g2.drawBox(smirkX - r - 1, cy - r - 2, (r + 1) * 2, r + 1);
    u8g2.setDrawColor(1);
  } 
  else if (st == 9) { // ANGRY: small upside down crescent
    int r = 6;
    u8g2.drawDisc(cx, cy + 4, r);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(cx, cy + 6, r + 1);
    u8g2.drawBox(cx - r - 1, cy + 6, (r + 1) * 2, r + 1);
    u8g2.setDrawColor(1);
  }
  else if (st == 10) { // LOVE: small happy open mouth
    int r = 5;
    u8g2.drawDisc(cx, cy, r);
    u8g2.setDrawColor(0);
    u8g2.drawBox(cx - r - 1, cy - r - 1, (r + 1) * 2, r + 1);
    u8g2.setDrawColor(1);
  }
  else if (st == 11) { // DIZZY: small open O
    u8g2.drawCircle(cx, cy, 4);
    u8g2.drawCircle(cx, cy, 3);
  }
  else { // IDLE: Beautiful warm crescent smile
    int r = 8;
    int openHeight = 3 + (int)(breath * 2.0); // Gentle talking/breathing bounce
    u8g2.drawDisc(cx, cy, r);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(cx, cy - openHeight, r + 1);
    u8g2.drawBox(cx - r - 1, cy - r - openHeight, (r + 1) * 2, r + 1);
    u8g2.setDrawColor(1);
  }
}

// Draws a cute action graphic (waving hand, rock, paper, scissors)
void drawActionGraphic(int st, int frame) {
  u8g2.setDrawColor(1);
  int cx = 110; // Right side of the screen
  int cy = 40;  // Middle height

  if (st == 5) { // WAVE
    // Animate hand moving left and right rapidly
    int waveOffset = (int)(sin(frame * 0.6) * 7);
    cx += waveOffset;
    
    // Draw hand (palm + fingers)
    u8g2.drawRBox(cx - 5, cy - 5, 10, 10, 2); // palm
    u8g2.drawLine(cx - 4, cy - 5, cx - 4, cy - 10); // pinky
    u8g2.drawLine(cx - 1, cy - 6, cx - 1, cy - 12); // ring
    u8g2.drawLine(cx + 2, cy - 6, cx + 2, cy - 13); // middle
    u8g2.drawLine(cx + 5, cy - 5, cx + 5, cy - 11); // index
    u8g2.drawLine(cx - 7, cy - 1, cx - 10, cy + 2); // thumb
  } else if (st == 6) { // ROCK
    // Draw a fist
    u8g2.drawRBox(cx - 6, cy - 4, 12, 10, 3);
    u8g2.drawLine(cx - 5, cy - 5, cx - 2, cy - 5);
    u8g2.drawLine(cx, cy - 6, cx + 3, cy - 6);
  } else if (st == 7) { // PAPER
    // Draw open palm
    u8g2.drawRBox(cx - 6, cy - 8, 12, 14, 2);
    u8g2.drawLine(cx - 5, cy - 8, cx - 5, cy - 14);
    u8g2.drawLine(cx - 1, cy - 8, cx - 1, cy - 15);
    u8g2.drawLine(cx + 3, cy - 8, cx + 3, cy - 14);
    u8g2.drawLine(cx + 6, cy - 8, cx + 6, cy - 12);
  } else if (st == 8) { // SCISSORS
    // Draw scissors (peace sign)
    u8g2.drawRBox(cx - 5, cy - 4, 10, 10, 2); // fist base
    u8g2.drawLine(cx - 2, cy - 4, cx - 6, cy - 12); // index
    u8g2.drawLine(cx + 2, cy - 4, cx + 6, cy - 12); // middle
  }
}

void loop() {
  // Update emotions

  // ── NON-BLOCKING FRAME TIMING ──
  // Replace the old delay(30) with millis()-based timing for the OLED animation
  static unsigned long lastOledTime = 0;
  unsigned long now = millis();
  bool oledTick = (now - lastOledTime >= 66); // ~15fps for OLED animation (Saves 50% CPU!)

  // ── UDP BROADCAST (Every 2 seconds if no client connected) ──
  static unsigned long lastUdpTime = 0;
  if (!isWsConnected && now - lastUdpTime > 2000) {
    // Target the absolute broadcast address so AP clients receive it
    udp.beginPacket(IPAddress(255, 255, 255, 255), 8888);
    String ipMsg = "IRA_ROBOT_IP:" + WiFi.localIP().toString();
    udp.print(ipMsg);
    udp.endPacket();
    lastUdpTime = now;
  }

  // ── PROCESS UDP COMMANDS (Zero-Latency Driving) ──
  // Process ALL queued packets instantly to prevent bufferbloat
  while (int packetSize = udp.parsePacket()) {
    char packetBuffer[255];
    int len = udp.read(packetBuffer, 254);
    if (len > 0) {
      packetBuffer[len] = 0;
      processDriveCommand(String(packetBuffer));
    }
  }

  // (WebSocket loop is handled safely on Core 0 by cameraTask to prevent cross-core memory crashes)
  
  // ── Only update OLED + animations at ~30fps ──
  if (!oledTick) {
    delay(1); // Give the RTOS scheduler and WiFi stack a proper 1ms breathing room
    return;  // Skip the heavy OLED rendering this iteration
  }
  lastOledTime = now;

  // --- 1. MOTION MATH (Breathing & Gazing) ---
  
  // Sine wave for breathing (cycles smoothly)
  float breath = sin(frame * 0.07);
  int breathY = (int)(breath * 2.0); // gentle vertical drift (-2 to +2 pixels)

  // Smoothly interpolate the gaze offsets towards the targets (smoothing factor 0.2)
  currentLookX += (targetLookX - currentLookX) * 0.2;
  currentLookY += (targetLookY - currentLookY) * 0.2;
  
  int lookX = (int)round(currentLookX);
  int lookY = (int)round(currentLookY);

  // --- 2. NEOPIXEL SYNCED PULSATION ---
  // The LED breaths in sync with the companion's chest, color matching the mood
  float breathNormal = (breath + 1.0f) / 2.0f; // Normalize to 0.0 -> 1.0
  int r = 0, g = 0, b = 0;

  if (currentState == 1) { // HAPPY: Warm Green/Yellow pulse
    r = (int)(breathNormal * 40);
    g = (int)(breathNormal * 50);
  } else if (currentState == 2) { // WONDER: Curious Purple/Magenta pulse
    r = (int)(breathNormal * 45);
    b = (int)(breathNormal * 45);
  } else if (currentState == 3) { // SLEEPY: Dim cozy Amber/Orange pulse
    r = (int)(breathNormal * 20);
    g = (int)(breathNormal * 8);
  } else if (currentState == 4) { // WINK: Flashy Golden pulse
    r = (int)(breathNormal * 50);
    g = (int)(breathNormal * 30);
  } else if (currentState >= 5 && currentState <= 8) { // ACTIONS (Wave, RPS): Magical Pink/Purple pulse
    r = (int)(breathNormal * 60);
    b = (int)(breathNormal * 60);
  } else if (currentState == 9) { // ANGRY: Red pulse
    r = (int)(breathNormal * 80);
  } else if (currentState == 10) { // LOVE: Pink pulse
    r = (int)(breathNormal * 60);
    b = (int)(breathNormal * 20);
  } else if (currentState == 11) { // DIZZY: Yellow pulse
    r = (int)(breathNormal * 40);
    g = (int)(breathNormal * 40);
  } else { // IDLE: Calm Cyan/Teal breathing pulse
    g = (int)(breathNormal * 30);
    b = (int)(breathNormal * 50);
  }
  LED_RGB.setPixelColor(0, LED_RGB.Color(r, g, b));
  LED_RGB.show();

  // (WebSocket loop removed to prevent cross-core corruption)
  
  // --- 3. STATE & TIMER UPDATES ---
  
  // A. Emotion State Timer
  stateTimer--;
  if (stateTimer <= 0) {
    // Automatically revert to Scan / IDLE (Normal Face) after an expression finishes
    // Removed the random emotion cycling so the behavior is predictable
    currentState = 0; // 0 = IDLE (Normal scanning eyes)
    stateTimer = 99999; // Stay in normal face until a new command is received
  }

  // B. Gaze Gaze Look-Around Timer (Only looks around in IDLE or WONDER states)
  gazeTimer--;
  if (gazeTimer <= 0) {
    if (currentState == 0 || currentState == 2) {
      int randGaze = random(0, 10);
      if (randGaze < 6) { // Look Straight (60%)
        targetLookX = 0;
        targetLookY = 0;
        gazeTimer = random(60, 120);
      } else if (randGaze < 8) { // Look Left (20%)
        targetLookX = -2;
        targetLookY = 0;
        gazeTimer = random(30, 60);
      } else { // Look Right (20%)
        targetLookX = 2;
        targetLookY = 0;
        gazeTimer = random(30, 60);
      }
    } else {
      // In special states, keep gaze locked to center
      targetLookX = 0;
      targetLookY = 0;
      gazeTimer = 40;
    }
  }

  // C. Blinking Timer (Only blinks naturally in IDLE, WONDER, or SLEEPY states)
  blinkTimer--;
  if (blinkTimer <= 0) {
    isBlinking = true;
    if (blinkTimer <= -4) { // Blink lasts exactly 4 frames (approx 120ms)
      isBlinking = false;
      blinkTimer = random(100, 240); // Next blink in 3 to 8 seconds
    }
  } else {
    isBlinking = false;
  }

  // --- 4. OLED RENDERING ---
  
  if (screenEnabled) {
    u8g2.clearBuffer(); // Clear graphic canvas

    if (currentState == 100) {
      // Draw the exact 128x64 bit-packed pixel array directly to the OLED buffer
      u8g2.drawXBM(0, 0, 128, 64, customFaceBuffer);
    } else {
      // Layout Parameters (All anchored to Y-breathing offset for vertical drift)
    int leftEyeX = 36;
    int rightEyeX = 92;
    int eyeY = 27 + breathY;
    int mouthX = 64;
    int mouthY = 46 + breathY;
    int blushY = eyeY + 11;

    // Determine drawing parameters based on active state
    bool drawLeftOpen = true;
    bool drawRightOpen = true;
    bool eyesHappy = false;

    if (currentState == 1 || currentState >= 5) { // HAPPY or ACTION state
      eyesHappy = true;
    } 
    else if (currentState == 3) { // SLEEPY
      // Slow sleepy blinks (drowsy looking eyes)
      bool isDrowsyClosed = (frame % 80) < 15; // stays closed longer
      drawLeftOpen = !isDrowsyClosed;
      drawRightOpen = !isDrowsyClosed;
    } 
    else if (currentState == 4) { // WINK
      drawLeftOpen = false; // Left eye winks closed
      drawRightOpen = true; // Right eye stays open sparkling
    } 
    else { // IDLE or WONDER
      // Use natural random blink
      drawLeftOpen = !isBlinking;
      drawRightOpen = !isBlinking;
    }

    // A. Draw Eyebrows
    drawEyebrow(leftEyeX, eyeY, true, currentState);
    drawEyebrow(rightEyeX, eyeY, false, currentState);

    // B. Draw Eyes
    if (currentState == 10) {
      drawHeartEye(leftEyeX, eyeY);
      drawHeartEye(rightEyeX, eyeY);
    } else if (currentState == 11) {
      drawDizzyEye(leftEyeX, eyeY);
      drawDizzyEye(rightEyeX, eyeY);
    } else {
      drawEye(leftEyeX, eyeY, drawLeftOpen, eyesHappy, lookX, lookY);
      drawEye(rightEyeX, eyeY, drawRightOpen, eyesHappy, lookX, lookY);
    }

    // C. Draw Blush (Dithered Cheek Circles)
    // Only draw blush when happy, idle, winking, or performing actions
    if (currentState == 0 || currentState == 1 || currentState == 4 || currentState >= 5) {
      drawBlush(leftEyeX, blushY);
      drawBlush(rightEyeX, blushY);
    }

    // D. Draw Mouth
    drawMouth(mouthX, mouthY, currentState >= 5 ? 1 : currentState, breath); // Force happy mouth during actions

    // E. Draw Action Graphics
    if (currentState >= 5) {
      drawActionGraphic(currentState, frame);
    }
    }

    u8g2.sendBuffer(); // Render canvas to the physical SH1106 OLED screen
  }

  // Frame increment (no delay() — timing is handled by millis() check above)
  frame++;
}
