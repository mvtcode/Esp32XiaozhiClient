#pragma once

#include <Arduino.h>
#include <vector>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "xiaozhi_types.h"
#include "xiaozhi_audio_hal.h"

// Forward declarations từ libopus
struct OpusEncoder;
struct OpusDecoder;

/**
 * @class XiaozhiAudio
 * @brief Dịch vụ quản lý luồng âm thanh thời gian thực cho XiaoZhi AI
 *        Tích hợp Opus Codec (16kHz Mono 60ms frame = 960 mẫu PCM) + Voice Activity Detection (VAD)
 *        Hoàn toàn tách biệt khỏi phần cứng cụ thể thông qua HAL (IXiaozhiAudioDriver hoặc Audio IO Callbacks).
 */
class XiaozhiAudio {
public:
    static XiaozhiAudio& getInstance();

    XiaozhiAudio();
    ~XiaozhiAudio();

    /**
     * @brief Thiết lập driver âm thanh HAL (Tùy chọn 1)
     */
    void setAudioDriver(IXiaozhiAudioDriver* driver);

    /**
     * @brief Thiết lập hàm đọc Mic & ghi Loa trực tiếp qua Lambda/Callback (Tùy chọn 2 - rất tiện lợi)
     * @param micReadFunc Hàm đọc PCM từ Micro: size_t(int16_t* buf, size_t samples, uint32_t timeoutMs)
     * @param spkWriteFunc Hàm ghi PCM ra Loa: size_t(const int16_t* buf, size_t samples, uint32_t timeoutMs)
     */
    void setAudioIo(XiaozhiMicReadFunc micReadFunc, XiaozhiSpeakerWriteFunc spkWriteFunc);

    /**
     * @brief Khởi động dịch vụ âm thanh, khởi tạo Opus Encoder và Decoder
     * @return true nếu khởi tạo thành công
     */
    bool begin();

    /**
     * @brief Dừng dịch vụ và giải phóng toàn bộ tài nguyên FreeRTOS & Opus
     */
    void stop();

    /**
     * @brief Bắt đầu thu âm từ Micro và đẩy gói Opus ra callback onAudioOutput
     */
    void startRecording();

    /**
     * @brief Dừng thu âm từ Micro
     */
    void stopRecording();

    /**
     * @brief Đẩy một gói Opus nhận từ WebSocket vào hàng đợi phát loa
     * @param data Dữ liệu Opus nhị phân
     * @param len Độ dài byte
     */
    bool feedPlaybackOpus(const uint8_t* data, size_t len);

    /**
     * @brief Xóa sạch hàng đợi phát âm thanh (ngắt lời ngay lập tức khi người dùng cướp lời)
     */
    void clearPlaybackBuffer();

    /**
     * @brief Kiểm tra xem Micro có đang thu âm hay không
     */
    bool isRecording() const { return _isRecording; }

    /**
     * @brief Kiểm tra xem Loa có đang phát âm thanh TTS hay không
     */
    bool isPlaying() const { return _isPlaying; }

    /**
     * @brief Lấy mức biên độ âm thanh thời gian thực (0.0f .. 100.0f) cho VU meter, OLED, LED, hoặc Avatar
     */
    float getAudioLevel() const { return _audioLevel; }

    /**
     * @brief Kiểm tra trạng thái phát hiện tiếng nói VAD
     */
    bool isSpeechActive() const { return _isSpeechActive; }

    // Đăng ký Callbacks
    void onAudioOutput(XiaozhiAudioOutCallback cb) { _audioOutCallback = cb; }
    void onAudioLevel(XiaozhiAudioLevelCallback cb) { _audioLevelCallback = cb; }
    void onVadStateChange(XiaozhiVadStateCallback cb) { _vadCallback = cb; }

    static constexpr uint32_t SAMPLE_RATE = 16000;
    static constexpr uint32_t FRAME_DURATION_MS = 60;
    static constexpr size_t   FRAME_SAMPLES = (SAMPLE_RATE * FRAME_DURATION_MS) / 1000; // 960 samples

private:
    void _recordTask();
    void _playbackTask();
    static void _recordTaskTrampoline(void* arg);
    static void _playbackTaskTrampoline(void* arg);

    IXiaozhiAudioDriver*     _audioDriver;
    XiaozhiMicReadFunc       _micReadFunc;
    XiaozhiSpeakerWriteFunc  _spkWriteFunc;

    OpusEncoder* _opusEncoder;
    OpusDecoder* _opusDecoder;

    TaskHandle_t  _recordTaskHandle;
    TaskHandle_t  _playbackTaskHandle;
    QueueHandle_t _playbackQueue;

    volatile bool _initialized;
    volatile bool _isRecording;
    volatile bool _isPlaying;
    volatile float _audioLevel;
    volatile bool _isSpeechActive;
    volatile int  _warmupFrames;
    volatile uint32_t _lastPlaybackActiveMs;
    uint32_t      _speechFrameCount;
    uint32_t      _silenceFrameCount;

    XiaozhiAudioOutCallback   _audioOutCallback;
    XiaozhiAudioLevelCallback _audioLevelCallback;
    XiaozhiVadStateCallback   _vadCallback;
};
