#pragma once

#include "xiaozhi_types.h"

/**
 * @interface IXiaozhiAudioDriver
 * @brief Lớp trừu tượng Hardware Abstraction Layer (HAL) cho phần cứng âm thanh.
 *        Cho phép thư viện hoạt động linh hoạt với mọi loại DAC/ADC:
 *        - INMP441 + MAX98357A (chuẩn I2S rời)
 *        - ES8388 (ESP32-Audio-Kit)
 *        - ES8311 + NS4150 (JC1060P470C tablet)
 *        - Driver I2S tiêu chuẩn của ESP-IDF / Arduino
 */
class IXiaozhiAudioDriver {
public:
    virtual ~IXiaozhiAudioDriver() = default;

    /**
     * @brief Khởi tạo phần cứng âm thanh (I2S, codec DAC/ADC)
     * @param sampleRate Tần số lấy mẫu (thường 16000 Hz)
     * @return true nếu thành công
     */
    virtual bool init(uint32_t sampleRate) = 0;

    /**
     * @brief Đọc mẫu âm thanh PCM 16-bit Mono từ Microphone
     * @param buffer Mảng chứa mẫu PCM đầu ra
     * @param samples Số lượng mẫu cần đọc (thường 960 mẫu ~ 60ms)
     * @param timeoutMs Thời gian chờ tối đa (ms)
     * @return Số lượng mẫu thực tế đọc được
     */
    virtual size_t readMicrophone(int16_t* buffer, size_t samples, uint32_t timeoutMs) = 0;

    /**
     * @brief Ghi mẫu âm thanh PCM 16-bit Mono ra Loa (Speaker)
     * @param buffer Mảng chứa mẫu PCM cần phát
     * @param samples Số lượng mẫu cần phát
     * @param timeoutMs Thời gian chờ tối đa (ms)
     * @return Số lượng mẫu thực tế ghi được
     */
    virtual size_t writeSpeaker(const int16_t* buffer, size_t samples, uint32_t timeoutMs) = 0;

    /**
     * @brief Cài đặt âm lượng phần cứng (0 - 100)
     */
    virtual void setVolume(uint8_t volume) { (void)volume; }
};
