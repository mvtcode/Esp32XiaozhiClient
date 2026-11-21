# Esp32XiaozhiClient

Thư viện C++ độc lập cho giao thức **XiaoZhi AI (小智 AI)** trên nền tảng vi điều khiển ESP32 (ESP32, ESP32-S3, ESP32-P4, v.v.).

Thư viện được thiết kế theo kiến trúc **UI-Agnostic (Hoàn toàn phi giao diện)** và **HAL-Decoupled (Tách biệt phần cứng)**:
- **Không dính dáng đến bất kỳ thư viện UI nào**: Chạy mượt mà trên **Loa thông minh không màn hình (Headless)**, **Màn hình OLED đơn sắc (SSD1306 / SH1106 qua U8g2/Adafruit)**, hoặc **Màn hình màu cảm ứng với LVGL**.
- **Hardware Abstraction Layer (HAL)**: Hỗ trợ mọi cấu hình âm thanh I2S (INMP441 + MAX98357A, ES8388, ES8311, PDM) thông qua callback hoặc driver trừu tượng.
- **Opus Codec thời gian thực**: Nén/giải mã Opus 16kHz Mono 60ms (960 mẫu PCM) tích hợp sẵn Voice Activity Detection (VAD) nhận diện người dùng ngưng nói.
- **Tự động kích hoạt OTA**: Tự động truy vấn mã 6 chữ số gán thiết bị vào `xiaozhi.me` / `tenclass.net`.
- **MCP Vision Extensibility**: Cho phép đăng ký camera tool tùy chọn nếu phần cứng có camera.

---

## 1. Cài đặt (Installation)

### PlatformIO (`platformio.ini`)
Thêm URL Git của thư viện vào mục `lib_deps`:
```ini
lib_deps =
    https://github.com/mvtcode/Esp32XiaozhiClient.git
    gilmaimon/ArduinoWebsockets@^0.5.4
    sh123/esp32_opus@^1.0.3
```

### Arduino IDE
Tải file ZIP của repository hoặc clone vào thư mục `libraries/` của Arduino:
```bash
git clone https://github.com/mvtcode/Esp32XiaozhiClient.git
```

---

## 2. Cấu trúc thư viện

```
Esp32XiaozhiClient/
├── .gitignore
├── library.json             # Manifest PlatformIO Library
├── README.md                # Hướng dẫn chi tiết & Code mẫu
├── include/
│   ├── xiaozhi_client.h     # Client kết nối WebSocket, OTA activation, MCP protocol
│   ├── xiaozhi_audio.h      # Pipeline Opus + VAD
│   ├── xiaozhi_types.h      # Định nghĩa Enum (State, Mode), Struct (Event), Callbacks
│   └── xiaozhi_audio_hal.h  # Interface trừu tượng phần cứng âm thanh (HAL)
└── src/
    ├── xiaozhi_client.cpp
    └── xiaozhi_audio.cpp
```

---

## 3. Hướng dẫn sử dụng theo 3 kịch bản hiển thị

### Kịch bản 1: Không có màn hình (Headless Loa Thông Minh AI)
Dành cho loa thông minh chỉ gồm nút bấm, micro, loa và đèn LED trạng thái.

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include "xiaozhi_client.h"
#include "xiaozhi_audio.h"

#define PIN_BUTTON  0   // Nút nhấn để nói
#define PIN_LED_RGB 2   // LED báo trạng thái

void setup() {
    Serial.begin(115200);
    WiFi.begin("SSID", "PASSWORD");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    // 1. Gán phần cứng âm thanh I2S của bạn (ví dụ INMP441 & MAX98357A)
    XiaozhiAudio::getInstance().setAudioIo(
        [](int16_t* buf, size_t samples, uint32_t timeoutMs) -> size_t {
            return myI2sReadMic(buf, samples, timeoutMs);
        },
        [](const int16_t* buf, size_t samples, uint32_t timeoutMs) -> size_t {
            return myI2sWriteSpeaker(buf, samples, timeoutMs);
        }
    );

    // 2. Khởi tạo Client & lắng nghe sự kiện
    XiaozhiClient& client = XiaozhiClient::getInstance();
    client.begin();

    client.onStateChanged([](XiaozhiConnectionState state, int code) {
        if (state == XiaozhiConnectionState::CONNECTED) {
            Serial.println(">>> Đã kết nối XiaoZhi AI!");
        } else if (state == XiaozhiConnectionState::SPEAKING) {
            Serial.println(">>> AI đang trả lời...");
        }
    });

    client.onActivationInfo([](const char* code, const char* msg) {
        Serial.printf(">>> MÃ KÍCH HOẠT OTA: %s - Nhập trên xiaozhi.me\n", code);
    });

    client.onChatMessage([](const char* role, const char* text) {
        Serial.printf("[%s]: %s\n", role, text);
    });

    client.connect();
}

void loop() {
    // Luôn gọi pollEvents() trong vòng lặp chính
    XiaozhiClient::getInstance().pollEvents();

    // Nhấn nút để nói (Push-to-talk hoặc Auto VAD)
    if (digitalRead(PIN_BUTTON) == LOW) {
        if (!XiaozhiAudio::getInstance().isRecording()) {
            XiaozhiClient::getInstance().startVoiceListening(XiaozhiListeningMode::AUTO);
        }
    }
    delay(10);
}
```

---

### Kịch bản 2: Màn hình OLED đơn sắc (SSD1306 128x64 qua U8g2 / Adafruit)
Hiển thị trạng thái, icon biểu cảm, mã kích hoạt 6 số và mức âm lượng visualizer.

```cpp
#include <Arduino.h>
#include <U8g2lib.h>
#include "xiaozhi_client.h"
#include "xiaozhi_audio.h"

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);
String g_statusStr = "Connecting...";
String g_aiText = "";

void setup() {
    u8g2.begin();
    // Khởi tạo Wi-Fi & Audio HAL...

    XiaozhiClient& client = XiaozhiClient::getInstance();
    client.begin();

    client.onStateChanged([](XiaozhiConnectionState state, int code) {
        switch (state) {
            case XiaozhiConnectionState::CONNECTING:   g_statusStr = "Dang ket noi..."; break;
            case XiaozhiConnectionState::CONNECTED:    g_statusStr = "San sang!"; break;
            case XiaozhiConnectionState::LISTENING:    g_statusStr = "Dang nghe..."; break;
            case XiaozhiConnectionState::SPEAKING:     g_statusStr = "Dang noi..."; break;
            case XiaozhiConnectionState::ERROR_STATE:  g_statusStr = "Loi mang!"; break;
            default: break;
        }
    });

    client.onActivationInfo([](const char* code, const char* msg) {
        g_statusStr = String("Ma: ") + code;
    });

    client.onChatMessage([](const char* role, const char* text) {
        if (strcmp(role, "assistant") == 0) {
            g_aiText = text;
        }
    });

    client.connect();
}

void loop() {
    XiaozhiClient::getInstance().pollEvents();

    // Vẽ giao diện lên màn hình OLED
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 12, g_statusStr.c_str());
    
    // Vẽ thanh VU Meter biên độ âm thanh thời gian thực
    int vuWidth = (int)(XiaozhiAudio::getInstance().getAudioLevel() * 1.28f);
    u8g2.drawBox(0, 20, vuWidth, 4);

    u8g2.drawStr(0, 40, g_aiText.substring(0, 20).c_str());
    u8g2.sendBuffer();

    delay(20);
}
```

---

### Kịch bản 3: Màn hình cảm ứng màu sắc với LVGL
Đăng ký các callback để cập nhật nhãn trạng thái, bong bóng chat, hoặc avatar biểu cảm AI trong LVGL:

```cpp
#include <lvgl.h>
#include "xiaozhi_client.h"
#include "xiaozhi_audio.h"

void initXiaozhiForLvgl() {
    XiaozhiClient& client = XiaozhiClient::getInstance();
    client.begin();

    client.onStateChanged([](XiaozhiConnectionState state, int code) {
        // Cập nhật trạng thái lên nhãn/status bar LVGL
    });

    client.onChatMessage([](const char* role, const char* text) {
        // Thêm bong bóng chat (role: "user" hoặc "assistant") vào container danh sách LVGL
    });

    client.onEmotionChanged([](const char* emotion) {
        // Thay đổi avatar / biểu cảm LVGL (ví dụ: "happy", "thinking", "neutral")
    });

    // VAD nhận diện dứt lời để dừng nghe và chờ phản hồi
    XiaozhiAudio::getInstance().onVadStateChange([](bool isSpeaking) {
        if (!isSpeaking) {
            XiaozhiClient::getInstance().stopVoiceListening();
        }
    });

    client.connect();
}

void loop() {
    // Gọi trong vòng lặp chính
    XiaozhiClient::getInstance().pollEvents();
    lv_timer_handler();
    delay(5);
}
```

---

## 4. Đăng ký MCP Tool điều khiển GPIO / Thiết bị thông minh

Thư viện hỗ trợ chuẩn **Model Context Protocol (MCP)** cho phép LLM tự động gọi hàm trên ESP32 để điều khiển chân GPIO (bật/tắt đèn, quạt, relay), đọc cảm biến nhiệt độ hoặc kích hoạt lệnh ngoại vi khi người dùng nói chuyện bằng ngôn ngữ tự nhiên.

```cpp
#include "xiaozhi_client.h"
#include <cJSON.h>

#define PIN_LIGHT_RELAY 12
#define PIN_FAN_RELAY   13

void setup() {
    pinMode(PIN_LIGHT_RELAY, OUTPUT);
    pinMode(PIN_FAN_RELAY, OUTPUT);

    XiaozhiClient& client = XiaozhiClient::getInstance();

    // Đăng ký Tool điều khiển thiết bị qua MCP
    client.registerTool(
        "self.device.control",
        "Điều khiển bật hoặc tắt các thiết bị điện như đèn, quạt, relay qua chân GPIO. Gọi công cụ này khi người dùng yêu cầu bật/tắt thiết bị.",
        "{\"type\":\"object\",\"properties\":{\"device\":{\"type\":\"string\",\"description\":\"Tên thiết bị (đèn, quạt, relay)\"},\"state\":{\"type\":\"boolean\",\"description\":\"true để BẬT, false để TẮT\"}},\"required\":[\"device\",\"state\"]}",
        [](const cJSON* args) -> String {
            if (!args) return "Thiếu tham số điều khiển.";

            cJSON* devItem = cJSON_GetObjectItem(args, "device");
            cJSON* stateItem = cJSON_GetObjectItem(args, "state");
            const char* dev = devItem && cJSON_IsString(devItem) ? devItem->valuestring : "";
            bool state = stateItem && cJSON_IsTrue(stateItem);

            String devStr = String(dev);
            devStr.toLowerCase();

            if (devStr.indexOf("đèn") >= 0 || devStr.indexOf("light") >= 0) {
                digitalWrite(PIN_LIGHT_RELAY, state ? HIGH : LOW);
                return state ? "Đã bật đèn thành công." : "Đã tắt đèn.";
            } else if (devStr.indexOf("quạt") >= 0 || devStr.indexOf("fan") >= 0) {
                digitalWrite(PIN_FAN_RELAY, state ? HIGH : LOW);
                return state ? "Đã bật quạt thành công." : "Đã tắt quạt.";
            }

            return "Không tìm thấy thiết bị yêu cầu.";
        }
    );

    client.begin();
    client.connect();
}
```

---

## 5. Đăng ký Camera / Vision MCP Tool (Tùy chọn)

Nếu phần cứng của bạn có camera (như ESP32-CAM, ESP32-S3 Eye, hoặc ESP32-P4), bạn có thể đăng ký Vision handler để AI có thể "nhìn thấy" qua camera:

```cpp
XiaozhiClient::getInstance().setVisionHandler(
    [](const char* question, uint8_t** outJpeg, size_t* outLen) -> bool {
        // Chụp ảnh từ camera của bạn và nén JPEG
        return myCamCaptureJpeg(outJpeg, outLen);
    },
    [](uint8_t* jpegBuf) {
        // Giải phóng buffer JPEG sau khi upload xong
        free(jpegBuf);
    }
);
```
Nếu không gọi `setVisionHandler()`, thư viện sẽ tự động bỏ qua công cụ camera, tiết kiệm tối đa bộ nhớ và không phát sinh bất kỳ lỗi nào trên thiết bị không có camera!
