#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <functional>

/**
 * @brief Trạng thái kết nối của XiaozhiClient
 */
enum class XiaozhiConnectionState {
    DISCONNECTED = 0,
    CONNECTING,
    HANDSHAKING,
    CONNECTED,
    LISTENING,
    SPEAKING,
    ERROR_STATE
};

/**
 * @brief Chế độ lắng nghe giọng nói theo chuẩn giao thức xiaozhi-esp32
 */
enum class XiaozhiListeningMode {
    AUTO = 0,    // Tự động nhận diện im lặng và ngắt câu (Cloud VAD / Local VAD)
    MANUAL = 1,  // Thủ công nhấn để nói và nhấn dừng (Push-to-talk)
    REALTIME = 2 // Luồng thời gian thực hai chiều liên tục (Full Duplex)
};

/**
 * @brief Kiểu sự kiện gửi từ background thread về UI / Main thread
 */
enum class XiaozhiEventType {
    CONNECTION_STATE_CHANGED,
    USER_MESSAGE,           // STT từ người dùng
    ASSISTANT_SENTENCE,     // Câu trả lời TTS từng đoạn từ AI
    EMOTION_CHANGED,        // Cảm xúc từ LLM (happy, thinking, neutral, excited...)
    TTS_START,              // Bắt đầu phát âm thanh AI
    TTS_STOP,               // Dừng phát âm thanh AI
    ACTIVATION_INFO,        // Mã kích hoạt OTA 6 số & thông điệp ghép đôi
    ALERT_MESSAGE,          // Thông báo lỗi / cảnh báo hệ thống
    CAMERA_CAPTURE_START,   // Bắt đầu chụp ảnh nhận diện VLM
    CAMERA_CAPTURE_END      // Hoàn tất chụp ảnh & upload
};

/**
 * @brief Dữ liệu sự kiện trao đổi giữa các luồng
 */
struct XiaozhiEvent {
    XiaozhiEventType type;
    XiaozhiConnectionState state;
    char text[512];
    char extra[64];
    int code; // Close reason (1000, 1001, 1006...) hoặc error code
};

/**
 * @brief Cấu trúc gói tin âm thanh nén Opus
 */
struct XiaozhiAudioPacket {
    size_t len;
    uint8_t data[1024]; // Kích thước gói Opus tối đa hỗ trợ 1024 bytes
};

// Callbacks thông báo sự kiện cho các tầng ứng dụng (OLED, LVGL, Loa Headless)
typedef std::function<void(XiaozhiConnectionState state, int closeCode)> XiaozhiStateCallback;
typedef std::function<void(const char* role, const char* text)> XiaozhiChatCallback;
typedef std::function<void(const char* emotion)> XiaozhiEmotionCallback;
typedef std::function<void(bool speaking)> XiaozhiSpeakingCallback;
typedef std::function<void(const char* code, const char* msg)> XiaozhiActivationCallback;
typedef std::function<void(const char* message)> XiaozhiAlertCallback;
typedef std::function<void(bool capturing, const char* question)> XiaozhiCameraCaptureCallback;

// Callback âm thanh & VAD
typedef std::function<void(const uint8_t* data, size_t len)> XiaozhiAudioOutCallback;
typedef std::function<void(float level)> XiaozhiAudioLevelCallback;
typedef std::function<void(bool isSpeaking)> XiaozhiVadStateCallback;

// Callback Hardware Abstraction Layer cho Micro & Loa
typedef std::function<size_t(int16_t* buffer, size_t samples, uint32_t timeoutMs)> XiaozhiMicReadFunc;
typedef std::function<size_t(const int16_t* buffer, size_t samples, uint32_t timeoutMs)> XiaozhiSpeakerWriteFunc;

// Callback Vision/Camera MCP Tool (tùy chọn cho thiết bị có camera)
typedef std::function<bool(const char* question, uint8_t** outJpeg, size_t* outLen)> XiaozhiVisionCaptureFunc;
typedef std::function<void(uint8_t* jpegBuf)> XiaozhiVisionFreeFunc;

// Forward declaration cJSON
struct cJSON;

// Callback xử lý Custom MCP Tool (GPIO, Relay, cảm biến...)
// Tham số: args là cJSON object chứa các arguments do AI truyền xuống
// Trả về: chuỗi text phản hồi kết quả để AI đọc ra loa
typedef std::function<String(const cJSON* args)> XiaozhiToolHandler;

/**
 * @brief Định nghĩa công cụ MCP mở rộng (Model Context Protocol Tool)
 */
struct XiaozhiTool {
    std::string name;             // Tên định danh tool, vd: "self.device.control"
    std::string description;      // Mô tả công năng để AI biết khi nào nên gọi
    std::string inputSchemaJson;  // Chuỗi JSON Schema quy định tham số đầu vào
    XiaozhiToolHandler handler;   // Hàm callback xử lý
};
