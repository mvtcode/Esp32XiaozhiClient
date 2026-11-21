/**
 * @file xiaozhi_audio.cpp
 * @brief Triển khai dịch vụ pipeline âm thanh thời gian thực Opus cho XiaoZhi AI
 *        Hỗ trợ Opus Codec (16kHz Mono 60ms frame = 960 mẫu PCM) + Voice Activity Detection (VAD)
 *        Tách biệt hoàn toàn phần cứng qua Hardware Abstraction Layer (HAL).
 */

#include "xiaozhi_audio.h"
#include <opus.h>
#include <math.h>

#if __has_include("log.h")
#include "log.h"
#else
#include "esp_log.h"
#define LOG_I(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define LOG_D(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define LOG_E(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#endif

static const char *TAG = "XiaozhiAudio";

XiaozhiAudio& XiaozhiAudio::getInstance() {
    static XiaozhiAudio instance;
    return instance;
}

XiaozhiAudio::XiaozhiAudio()
    : _audioDriver(nullptr),
      _micReadFunc(nullptr),
      _spkWriteFunc(nullptr),
      _opusEncoder(nullptr),
      _opusDecoder(nullptr),
      _recordTaskHandle(nullptr),
      _playbackTaskHandle(nullptr),
      _playbackQueue(nullptr),
      _initialized(false),
      _isRecording(false),
      _isPlaying(false),
      _audioLevel(0.0f),
      _isSpeechActive(false),
      _warmupFrames(0),
      _lastPlaybackActiveMs(0),
      _speechFrameCount(0),
      _silenceFrameCount(0),
      _audioOutCallback(nullptr),
      _audioLevelCallback(nullptr),
      _vadCallback(nullptr) {
}

XiaozhiAudio::~XiaozhiAudio() {
    stop();
}

void XiaozhiAudio::setAudioDriver(IXiaozhiAudioDriver* driver) {
    _audioDriver = driver;
}

void XiaozhiAudio::setAudioIo(XiaozhiMicReadFunc micReadFunc, XiaozhiSpeakerWriteFunc spkWriteFunc) {
    _micReadFunc = micReadFunc;
    _spkWriteFunc = spkWriteFunc;
}

bool XiaozhiAudio::begin() {
    if (_initialized) return true;

    LOG_I(TAG, "Khoi dong pipeline am thanh Opus (Fs: %u Hz, Frame: %u ms, Samples: %u)...",
          SAMPLE_RATE, FRAME_DURATION_MS, (unsigned)FRAME_SAMPLES);

    // 1. Khoi tao phan cung am thanh qua Driver neu co
    if (_audioDriver) {
        _audioDriver->init(SAMPLE_RATE);
    }

    // 2. Khoi tao Opus Encoder
    int encErr = OPUS_OK;
    _opusEncoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &encErr);
    if (encErr != OPUS_OK || !_opusEncoder) {
        LOG_E(TAG, "Loi khoi tao Opus Encoder (code: %d)!", encErr);
        stop();
        return false;
    }
    opus_encoder_ctl(_opusEncoder, OPUS_SET_BITRATE(24000));      // 24 kbps chuan voice VoIP
    opus_encoder_ctl(_opusEncoder, OPUS_SET_COMPLEXITY(0));       // 0: toi uu CPU cho vi dieu khien
    opus_encoder_ctl(_opusEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(_opusEncoder, OPUS_SET_VBR(1));              // Variable Bitrate

    // 3. Khoi tao Opus Decoder
    int decErr = OPUS_OK;
    _opusDecoder = opus_decoder_create(SAMPLE_RATE, 1, &decErr);
    if (decErr != OPUS_OK || !_opusDecoder) {
        LOG_E(TAG, "Loi khoi tao Opus Decoder (code: %d)!", decErr);
        stop();
        return false;
    }

    // 4. Hang doi audio phat (chua toi da 10 goi Opus ~600ms dem am thanh)
    if (_playbackQueue == nullptr) {
        _playbackQueue = xQueueCreate(10, sizeof(XiaozhiAudioPacket));
        if (!_playbackQueue) {
            LOG_E(TAG, "Khong the tao _playbackQueue!");
            stop();
            return false;
        }
    }

    // 5. Khoi tao cac task FreeRTOS xu ly am thanh rieng biet
    _initialized = true;

    BaseType_t resRec = xTaskCreatePinnedToCore(
        _recordTaskTrampoline,
        "xzRecTask",
        28672, // 28KB stack an toan cho Opus SILK encoder (can ~21-22KB)
        this,
        4,     // Priority 4
        &_recordTaskHandle,
        0
    );

    BaseType_t resPlay = xTaskCreatePinnedToCore(
        _playbackTaskTrampoline,
        "xzPlayTask",
        12288, // 12KB stack (Opus decoder)
        this,
        4,     // Priority 4
        &_playbackTaskHandle,
        0
    );

    if (resRec != pdPASS || resPlay != pdPASS) {
        LOG_E(TAG, "Loi khoi tao FreeRTOS audio tasks (rec=%d, play=%d)", (int)resRec, (int)resPlay);
        stop();
        return false;
    }

    LOG_I(TAG, ">>> XiaozhiAudio pipeline da khoi tao thanh cong!");
    return true;
}

void XiaozhiAudio::stop() {
    if (!_initialized && !_recordTaskHandle && !_playbackTaskHandle) {
        return;
    }

    _initialized = false;
    _isRecording = false;
    _isPlaying = false;
    _audioLevel = 0.0f;

    // Chờ tối đa 300ms cho 2 audio tasks tự giải phóng buffer và kết thúc an toàn
    int waitMs = 300;
    while ((_recordTaskHandle != nullptr || _playbackTaskHandle != nullptr) && waitMs > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waitMs -= 10;
    }

    if (_recordTaskHandle) {
        LOG_W(TAG, "xzRecTask bi ket, cuong che xoa task!");
        vTaskDelete(_recordTaskHandle);
        _recordTaskHandle = nullptr;
    }
    if (_playbackTaskHandle) {
        LOG_W(TAG, "xzPlayTask bi ket, cuong che xoa task!");
        vTaskDelete(_playbackTaskHandle);
        _playbackTaskHandle = nullptr;
    }

    if (_playbackQueue) {
        vQueueDelete(_playbackQueue);
        _playbackQueue = nullptr;
    }

    if (_opusEncoder) {
        opus_encoder_destroy(_opusEncoder);
        _opusEncoder = nullptr;
    }

    if (_opusDecoder) {
        opus_decoder_destroy(_opusDecoder);
        _opusDecoder = nullptr;
    }

    LOG_I(TAG, "Da giai phong toan bo pipeline XiaozhiAudio an toan.");
}

void XiaozhiAudio::startRecording() {
    if (!_initialized) {
        begin();
    }
    _isSpeechActive = false;
    _speechFrameCount = 0;
    _silenceFrameCount = 0;
    _warmupFrames = 6; // 6 frames * 60ms = 360ms lọc sạch xung khởi động & âm đọng
    _isRecording = true;
    LOG_I(TAG, ">>> Bat dau thu am Micro cho Voice Chat (Auto VAD)...");
}

void XiaozhiAudio::stopRecording() {
    _isRecording = false;
    _isSpeechActive = false;
    _speechFrameCount = 0;
    _silenceFrameCount = 0;
    _audioLevel = 0.0f;
    LOG_I(TAG, ">>> Da dung thu am Micro.");
}

bool XiaozhiAudio::feedPlaybackOpus(const uint8_t* data, size_t len) {
    if (!_initialized || !_playbackQueue || !data || len == 0 || len > sizeof(XiaozhiAudioPacket::data)) {
        return false;
    }

    XiaozhiAudioPacket pkt;
    pkt.len = len;
    memcpy(pkt.data, data, len);

    return (xQueueSend(_playbackQueue, &pkt, pdMS_TO_TICKS(20)) == pdTRUE);
}

void XiaozhiAudio::clearPlaybackBuffer() {
    if (_playbackQueue) {
        xQueueReset(_playbackQueue);
    }
    _isPlaying = false;
    _audioLevel = 0.0f;
    LOG_I(TAG, "Da xoa sach bo dem phat am thanh (ngat loi).");
}

void XiaozhiAudio::_recordTaskTrampoline(void* arg) {
    static_cast<XiaozhiAudio*>(arg)->_recordTask();
}

void XiaozhiAudio::_recordTask() {
    int16_t* pcmBuffer = (int16_t*)heap_caps_malloc(FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcmBuffer) {
        pcmBuffer = (int16_t*)malloc(FRAME_SAMPLES * sizeof(int16_t));
    }
    uint8_t* opusBuffer = (uint8_t*)malloc(256);

    if (!pcmBuffer || !opusBuffer) {
        LOG_E(TAG, "Khong the cap phat buffer cho record task!");
        if (pcmBuffer) free(pcmBuffer);
        if (opusBuffer) free(opusBuffer);
        vTaskDelete(NULL);
        return;
    }

    while (_initialized) {
        if (_isRecording && _opusEncoder) {
            // Half-Duplex Acoustic Protection:
            // Khi loa dang phat HOAC trong vong 500ms sau khi loa vua tat tieng:
            // Xa sach bo dem de tranh tieng loa vang vao micro
            if (_isPlaying || (millis() - _lastPlaybackActiveMs < 500)) {
                int16_t dropBuf[256];
                if (_micReadFunc) {
                    _micReadFunc(dropBuf, 256, 10);
                } else if (_audioDriver) {
                    _audioDriver->readMicrophone(dropBuf, 256, 10);
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            // 1. Doc du 960 mau tu Micro qua HAL Callback hoac Driver
            size_t readCount = 0;
            if (_micReadFunc) {
                readCount = _micReadFunc(pcmBuffer, FRAME_SAMPLES, 70);
            } else if (_audioDriver) {
                readCount = _audioDriver->readMicrophone(pcmBuffer, FRAME_SAMPLES, 70);
            }

            if (readCount == FRAME_SAMPLES) {
                // 2. Tinh toan RMS & Peak tren mau PCM goc
                int64_t sumSq = 0;
                int16_t peak = 0;
                for (size_t i = 0; i < FRAME_SAMPLES; i++) {
                    int16_t val = abs(pcmBuffer[i]);
                    if (val > peak) peak = val;
                    sumSq += (int64_t)val * val;
                }
                float rms = sqrtf((float)sumSq / (float)FRAME_SAMPLES);
                float level = (rms / 1500.0f) * 100.0f; // Scale 0..100% phù hợp độ nhạy micro ECM
                if (level > 100.0f) level = 100.0f;
                _audioLevel = level;

                if (_audioLevelCallback) {
                    _audioLevelCallback(level);
                }

                if (_warmupFrames > 0) {
                    _warmupFrames--;
                } else {
                    // 2.1. Bộ nhận diện giọng nói VAD cục bộ (Energy-based Voice Activity Detection)
                    if (level >= 6.0f) {
                        _speechFrameCount++;
                        _silenceFrameCount = 0;
                        if (_speechFrameCount >= 2 && !_isSpeechActive) {
                            _isSpeechActive = true;
                            LOG_I(TAG, "VAD: Phát hiện bắt đầu nói (Level: %.1f%%, RMS: %.0f)", level, rms);
                            if (_vadCallback) {
                                _vadCallback(true);
                            }
                        }
                    } else {
                        if (_isSpeechActive) {
                            _silenceFrameCount++;
                            // 30 frames * 60ms = 1800ms im lặng -> ngắt câu nói tự nhiên
                            if (_silenceFrameCount >= 30) {
                                _isSpeechActive = false;
                                _speechFrameCount = 0;
                                _silenceFrameCount = 0;
                                LOG_I(TAG, "VAD: Phát hiện người dùng đã ngừng nói (Im lặng > 1.8s)");
                                if (_vadCallback) {
                                    _vadCallback(false);
                                }
                            }
                        } else {
                            _speechFrameCount = 0;
                        }
                    }
                }

                // 3. Nen PCM -> Opus packet
                int opusLen = opus_encode(_opusEncoder, pcmBuffer, FRAME_SAMPLES, opusBuffer, 256);
                if (opusLen > 0) {
                    // 4. Day goi Opus qua callback toi XiaozhiClient
                    if (_audioOutCallback) {
                        _audioOutCallback(opusBuffer, (size_t)opusLen);
                    }
                } else {
                    LOG_W(TAG, "Loi ma hoa Opus: %d", opusLen);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20)); // Cho khi chua bat thu am
        }

        vTaskDelay(1); // Nhuong CPU chong watchdog starvation
    }

    free(pcmBuffer);
    free(opusBuffer);
    LOG_I(TAG, "xzRecTask da ket thuc va tu huy an toan.");
    _recordTaskHandle = nullptr;
    vTaskDelete(NULL);
}

void XiaozhiAudio::_playbackTaskTrampoline(void* arg) {
    static_cast<XiaozhiAudio*>(arg)->_playbackTask();
}

void XiaozhiAudio::_playbackTask() {
    const int MAX_OUT_SAMPLES = 2880; // Hỗ trợ frame lên đến 120ms
    int16_t* pcmOutBuffer = (int16_t*)heap_caps_malloc(MAX_OUT_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcmOutBuffer) {
        pcmOutBuffer = (int16_t*)malloc(MAX_OUT_SAMPLES * sizeof(int16_t));
    }
    if (!pcmOutBuffer) {
        LOG_E(TAG, "Khong the cap phat buffer cho playback task!");
        vTaskDelete(NULL);
        return;
    }

    XiaozhiAudioPacket pkt;
    uint32_t lastPacketTime = 0;

    while (_initialized) {
        if (xQueueReceive(_playbackQueue, &pkt, pdMS_TO_TICKS(50)) == pdTRUE) {
            _isPlaying = true;
            lastPacketTime = millis();

            if (_opusDecoder && pkt.len > 0) {
                // 1. Giai ma Opus packet -> PCM 16kHz
                int decodedSamples = opus_decode(_opusDecoder, pkt.data, pkt.len, pcmOutBuffer, MAX_OUT_SAMPLES, 0);
                if (decodedSamples > 0) {
                    // 2. Tinh toan RMS de dong bo hieu ung am thanh
                    int64_t sumSq = 0;
                    for (int i = 0; i < decodedSamples; i++) {
                        int16_t val = abs(pcmOutBuffer[i]);
                        sumSq += (int64_t)val * val;
                    }
                    float rms = sqrtf((float)sumSq / (float)decodedSamples);
                    float level = (rms / 8000.0f) * 100.0f;
                    if (level > 100.0f) level = 100.0f;
                    _audioLevel = level;

                    if (_audioLevelCallback) {
                        _audioLevelCallback(level);
                    }

                    // 3. Phat am thanh ra Loa qua HAL Callback hoac Driver
                    if (_spkWriteFunc) {
                        _spkWriteFunc(pcmOutBuffer, decodedSamples, 80);
                    } else if (_audioDriver) {
                        _audioDriver->writeSpeaker(pcmOutBuffer, decodedSamples, 80);
                    }
                    _lastPlaybackActiveMs = millis();
                } else {
                    LOG_W(TAG, "Loi giai ma Opus packet: %d", decodedSamples);
                }
            }
        } else {
            // Hang doi trong -> neu da qua 250ms khong co goi moi, danh dau da dung phat
            if (_isPlaying && (millis() - lastPacketTime >= 250)) {
                _isPlaying = false;
                _lastPlaybackActiveMs = millis();
                _audioLevel = 0.0f;
                if (_audioLevelCallback) {
                    _audioLevelCallback(0.0f);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        vTaskDelay(1); // Nhuong CPU
    }

    free(pcmOutBuffer);
    LOG_I(TAG, "xzPlayTask da ket thuc va tu huy an toan.");
    _playbackTaskHandle = nullptr;
    vTaskDelete(NULL);
}
