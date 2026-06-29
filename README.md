# IRA Robot Companion - ESP32 Firmware

This repository contains the firmware for the **IRA Robot Companion**, designed for the ESP32-S3 platform. The robot features camera streaming, OLED facial expressions, motorized movement, audio playback, and WebSocket-based communication with a mobile application.

## 👤 Author
**Shivam Saini**

## 🚀 Why PlatformIO?

We use **PlatformIO** (via VS Code) instead of the standard Arduino IDE for this project because it provides several critical advantages for a complex ESP32 project:

1. **Automated Dependency Management**: PlatformIO automatically installs and manages all required libraries (like `U8g2`, `WebSockets`, `ArduinoJson`, `ESP32Servo`, `esp32-camera`, etc.) directly from the `platformio.ini` file. You don't have to hunt down and manually install ZIP files.
2. **Build Configurations**: Advanced features like enabling external PSRAM (`BOARD_HAS_PSRAM`) for the ESP32-S3 or adjusting partition tables are handled seamlessly via `platformio.ini` without modifying core files.
3. **Reproducibility**: Anyone who clones this repository will have the exact same build environment and library versions, ensuring the code compiles successfully on the first try.
4. **Advanced Tooling**: PlatformIO integrates beautifully with VS Code, offering superior code autocompletion, linting, file management, and debugging.

## ⚙️ Hardware Requirements

*   **Microcontroller**: ESP32-S3 (e.g., Freenove ESP32-S3 WROOM with Camera)
*   **Display**: SH1106 OLED (128x64) via I2C (SDA: 20, SCL: 19)
*   **Motor Driver**: TB6612FNG or L298N (Pins: 1, 2, 14, 42)
*   **Servo**: Standard 50Hz Servo for head movement (Pin: 47)
*   **Audio**: I2S Audio Module (BCLK: 40, LRC: 39, DOUT: 3)
*   **LED**: WS2812 NeoPixel (Pin: 46)

## 🛠️ Installation and Setup

1. **Install VS Code**: Download and install [Visual Studio Code](https://code.visualstudio.com/).
2. **Install PlatformIO**: Open VS Code, go to the Extensions tab (`Ctrl+Shift+X`), search for "PlatformIO IDE", and install it.
3. **Open Project**: Clone or download this repository. Open the `IRA_ESP32` folder in VS Code (`File > Open Folder`). PlatformIO will automatically initialize the project and download all dependencies listed in `platformio.ini`.
4. **Configure WiFi**: Open `src/main.cpp` and update your mobile hotspot/WiFi credentials:
   ```cpp
   // Enter your Mobile Hotspot Name and Password here!
   const char* STA_SSID = "Shivam";
   const char* STA_PASSWORD = "1234567891";
   ```
5. **Build and Upload**:
   *   Connect your ESP32-S3 to your computer via USB.
   *   Click the **PlatformIO: Build** (checkmark icon) in the bottom status bar to compile the code.
   *   Click the **PlatformIO: Upload** (right arrow icon) to flash the firmware to your ESP32.

## 🎮 How to Use

1. **Power On**: Power on the IRA Robot. The built-in OLED will display a boot sequence ("Booting...").
2. **Connect**: Ensure your mobile hotspot is active. Wait for the robot to connect to your hotspot. The OLED will show "Connected!" along with the robot's local IP address, and the onboard LED will turn green.
3. **App Integration**: Launch the IRA mobile companion app (which should be connected to the same network). 
4. **Interact**: The robot will communicate with the app via WebSockets (Port 81), allowing you to:
   *   Drive the robot using the app's D-Pad.
   *   View the live UDP camera stream.
   *   Change the robot's OLED facial expressions (Happy, Shocked, Sleepy, Angry, etc.).
   *   Control head movements.
   *   Stream and play audio files.
