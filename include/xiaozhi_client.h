#pragma once

#include <Arduino.h>
#include <vector>
#include <string>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "xiaozhi_types.h"

class XiaozhiAudio;

/**
 * @class XiaozhiClient
 * @brief Dịch vụ kết nối và quản lý giao thức WebSocket hai chiều với máy chủ XiaoZhi AI
 *        Tự động quản lý luồng nền FreeRTOS, mã định danh MAC/UUID và cơ chế tự phục hồi.
 *        Hoàn toàn độc lập với UI - hoạt động với Loa không màn hình, Màn hình OLED đơn sắc hoặc LVGL.
 */
class XiaozhiClient {
public:
    static XiaozhiClient& getInstance();

    XiaozhiClient();
    ~XiaozhiClient();

    /**
     * @brief Khởi động dịch vụ nền XiaoZhi
     */
    bool begin();

    /**
     * @brief Dừng dịch vụ và ngắt kết nối
     */
    void stop();

    /**
     * @brief Hàm kiểm tra và xử lý sự kiện trong vòng lặp chính của ứng dụng (non-blocking)
     *        Gọi đều đặn trong loop() của Arduino hoặc FreeRTOS Main Task.
     */
    void pollEvents();

    /**
     * @brief Kết nối tới máy chủ WebSocket
     */
    void connect();

    /**
     * @brief Ngắt kết nối máy chủ WebSocket
     */
    void disconnect();

    /**
     * @brief Gửi câu hỏi văn bản lên máy chủ XiaoZhi
     * @param text Nội dung câu hỏi
     */
    bool sendTextMessage(const char* text);

    /**
     * @brief Bắt đầu ghi âm và đẩy luồng giọng nói lên máy chủ
     * @param mode Chế độ nghe: AUTO (tự động nhận diện im lặng) hoặc MANUAL (Push-to-talk)
     */
    bool startVoiceListening(XiaozhiListeningMode mode = XiaozhiListeningMode::AUTO);

    /**
     * @brief Dừng ghi âm giọng nói
     */
    bool stopVoiceListening();

    /**
     * @brief Cấu hình chế độ lắng nghe
     */
    void setListeningMode(XiaozhiListeningMode mode) { _listeningMode = mode; }
    XiaozhiListeningMode getListeningMode() const { return _listeningMode; }

    /**
     * @brief Ngắt ngang phản hồi đang phát của trợ lý AI
     */
    bool abortSpeaking();

    /**
     * @brief Yêu cầu máy chủ OTA phát mã kích hoạt 6 chữ số gán thiết bị vào xiaozhi.me / tenclass
     */
    bool fetchActivationCode();

    /**
     * @brief Cập nhật cấu hình kết nối máy chủ và lưu vào NVS
     */
    void updateServerConfig(const char* url, const char* token);

    /**
     * @brief Tùy biến địa chỉ MAC của thiết bị (mặc định lấy từ WiFi.macAddress())
     */
    void setCustomMacAddress(const char* mac);

    /**
     * @brief Đăng ký hàm kiểm tra trạng thái Wi-Fi tùy biến (nếu dùng kiến trúc ngoại vi như ESP32-C6)
     */
    void setWifiConnectedChecker(std::function<bool()> checker);

    /**
     * @brief Đăng ký handler Vision / Camera MCP Tool (tùy chọn - chỉ dùng khi thiết bị có camera)
     */
    void setVisionHandler(XiaozhiVisionCaptureFunc captureFunc, XiaozhiVisionFreeFunc freeFunc);

    /**
     * @brief Đăng ký công cụ MCP tùy biến (Bật/tắt GPIO, đọc cảm biến, gửi MQTT...)
     * @param name Tên định danh công cụ (ví dụ: "self.device.control")
     * @param description Mô tả công cụ bằng ngôn ngữ tự nhiên để AI biết khi nào cần gọi
     * @param inputSchemaJson Chuỗi JSON Schema định nghĩa tham số đầu vào
     * @param handler Hàm callback thực thi và trả về chuỗi text kết quả
     */
    void registerTool(const char* name, const char* description, const char* inputSchemaJson, XiaozhiToolHandler handler);

    /**
     * @brief Hủy đăng ký một công cụ MCP
     */
    void unregisterTool(const char* name);

    // Getters trạng thái & thông tin thiết bị
    XiaozhiConnectionState getState() const { return _state; }
    bool isConnected() const { 
        return (_state == XiaozhiConnectionState::CONNECTED || 
                _state == XiaozhiConnectionState::LISTENING || 
                _state == XiaozhiConnectionState::SPEAKING); 
    }
    const std::string& getDeviceId() const { return _deviceId; }
    const std::string& getClientId() const { return _clientId; }
    const std::string& getServerUrl() const { return _serverUrl; }
    const std::string& getServerToken() const { return _serverToken; }
    const std::string& getActivationCode() const { return _activationCode; }
    const std::string& getActivationMessage() const { return _activationMessage; }
    int getLastCloseReason() const { return _lastCloseReason; }
    bool isIntentionalClose() const { return (_lastCloseReason == 1000 || _lastCloseReason == 1001); }

    // Callbacks đăng ký cho UI (OLED, LVGL, Loa Headless)
    void onStateChanged(XiaozhiStateCallback cb) { _stateCallback = cb; }
    void onChatMessage(XiaozhiChatCallback cb) { _chatCallback = cb; }
    void onEmotionChanged(XiaozhiEmotionCallback cb) { _emotionCallback = cb; }
    void onSpeakingChanged(XiaozhiSpeakingCallback cb) { _speakingCallback = cb; }
    void onActivationInfo(XiaozhiActivationCallback cb) { _activationCallback = cb; }
    void onAlertMessage(XiaozhiAlertCallback cb) { _alertCallback = cb; }
    void onCameraCapture(XiaozhiCameraCaptureCallback cb) { _cameraCaptureCallback = cb; }

private:
    void _loadNvsSettings();
    void _saveNvsSettings();
    void _ensureClientId();
    void _pushEvent(const XiaozhiEvent& ev);
    void _workerTask();
    static void _taskTrampoline(void* arg);
    void _sendWsText(const String& text);
    void _handleCameraTakePhotoTool(int reqId, const char* question);
    void _runVisionTask(int reqId, const char* question);
    static void _visionTaskTrampoline(void* arg);
    bool _isNetworkConnected() const;

    XiaozhiConnectionState _state;
    XiaozhiListeningMode   _listeningMode;
    int                    _lastCloseReason;
    std::string _deviceId;          // MAC Address Wi-Fi
    std::string _clientId;          // Software UUID
    std::string _serverUrl;         // WebSocket Server URL (e.g. wss://api.tenclass.net/xiaozhi/v1/)
    std::string _serverToken;       // Bearer Token
    std::string _sessionId;         // Session ID từ server
    std::string _activationCode;    // Mã ghép đôi 6 số
    std::string _activationMessage; // Hướng dẫn ghép đôi
    std::string _visionUrl;         // Vision endpoint từ MCP initialize
    std::string _visionToken;       // Vision token từ MCP initialize

    TaskHandle_t   _taskHandle;
    QueueHandle_t  _eventQueue;
    QueueHandle_t  _outQueue;
    QueueHandle_t  _audioOutQueue;
    volatile bool  _taskRunning;
    volatile bool  _shouldConnect;
    volatile bool  _shouldDisconnect;
    volatile bool  _fetchOtaRequested;
    volatile bool  _isActivating;
    volatile bool  _ttsServerFinished;

    XiaozhiStateCallback         _stateCallback;
    XiaozhiChatCallback          _chatCallback;
    XiaozhiEmotionCallback       _emotionCallback;
    XiaozhiSpeakingCallback      _speakingCallback;
    XiaozhiActivationCallback    _activationCallback;
    XiaozhiAlertCallback         _alertCallback;
    XiaozhiCameraCaptureCallback _cameraCaptureCallback;

    std::function<bool()>        _wifiChecker;
    XiaozhiVisionCaptureFunc     _visionCaptureFunc;
    XiaozhiVisionFreeFunc        _visionFreeFunc;
    std::vector<XiaozhiTool>     _customTools;

    volatile bool  _isCapturingPhoto;
    uint32_t       _lastPingMs;
    uint32_t       _lastReconnectAttemptMs;
    uint32_t       _lastOtaPollMs;
    uint32_t       _lastActivatePollMs;
};
