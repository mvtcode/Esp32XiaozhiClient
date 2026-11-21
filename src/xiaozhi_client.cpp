/**
 * @file xiaozhi_client.cpp
 * @brief Triển khai dịch vụ kết nối mạng và giao thức WebSocket với máy chủ XiaoZhi AI
 *        Tách biệt hoàn toàn khỏi UI và phần cứng ngoại vi.
 */

#include "xiaozhi_client.h"
#include <ArduinoWebsockets.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "xiaozhi_audio.h"
#include <cJSON.h>

#if __has_include("log.h")
#include "log.h"
#else
#include "esp_log.h"
#define LOG_I(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define LOG_D(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define LOG_E(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#endif

using namespace websockets;

static const char *TAG = "XiaozhiClient";
static Preferences s_prefs;

XiaozhiClient& XiaozhiClient::getInstance() {
    static XiaozhiClient instance;
    return instance;
}

XiaozhiClient::XiaozhiClient()
    : _state(XiaozhiConnectionState::DISCONNECTED),
      _listeningMode(XiaozhiListeningMode::AUTO),
      _lastCloseReason(1000),
      _deviceId(""),
      _clientId(""),
      _serverUrl("wss://api.tenclass.net/xiaozhi/v1/"),
      _serverToken(""),
      _sessionId(""),
      _activationCode(""),
      _activationMessage(""),
      _visionUrl(""),
      _visionToken(""),
      _taskHandle(nullptr),
      _eventQueue(nullptr),
      _outQueue(nullptr),
      _audioOutQueue(nullptr),
      _taskRunning(false),
      _shouldConnect(false),
      _shouldDisconnect(false),
      _fetchOtaRequested(false),
      _isActivating(false),
      _ttsServerFinished(false),
      _stateCallback(nullptr),
      _chatCallback(nullptr),
      _emotionCallback(nullptr),
      _speakingCallback(nullptr),
      _activationCallback(nullptr),
      _alertCallback(nullptr),
      _cameraCaptureCallback(nullptr),
      _wifiChecker(nullptr),
      _visionCaptureFunc(nullptr),
      _visionFreeFunc(nullptr),
      _isCapturingPhoto(false),
      _lastPingMs(0),
      _lastReconnectAttemptMs(0),
      _lastOtaPollMs(0),
      _lastActivatePollMs(0) {
}

XiaozhiClient::~XiaozhiClient() {
    stop();
}

void XiaozhiClient::setCustomMacAddress(const char* mac) {
    if (mac != nullptr && strlen(mac) > 0) {
        _deviceId = mac;
    }
}

void XiaozhiClient::setWifiConnectedChecker(std::function<bool()> checker) {
    _wifiChecker = checker;
}

void XiaozhiClient::setVisionHandler(XiaozhiVisionCaptureFunc captureFunc, XiaozhiVisionFreeFunc freeFunc) {
    _visionCaptureFunc = captureFunc;
    _visionFreeFunc = freeFunc;
}

void XiaozhiClient::registerTool(const char* name, const char* description, const char* inputSchemaJson, XiaozhiToolHandler handler) {
    if (!name || !handler) return;
    unregisterTool(name);

    XiaozhiTool tool;
    tool.name = name;
    tool.description = description ? description : "";
    tool.inputSchemaJson = inputSchemaJson ? inputSchemaJson : "{\"type\":\"object\",\"properties\":{}}";
    tool.handler = handler;
    _customTools.push_back(tool);
    LOG_I(TAG, "Da dang ky MCP Tool: '%s'", name);
}

void XiaozhiClient::unregisterTool(const char* name) {
    if (!name) return;
    for (auto it = _customTools.begin(); it != _customTools.end(); ++it) {
        if (it->name == name) {
            _customTools.erase(it);
            LOG_I(TAG, "Da huy dang ky MCP Tool: '%s'", name);
            break;
        }
    }
}

bool XiaozhiClient::_isNetworkConnected() const {
    if (_wifiChecker != nullptr) {
        return _wifiChecker();
    }
    return (WiFi.status() == WL_CONNECTED);
}

void XiaozhiClient::_loadNvsSettings() {
    s_prefs.begin("xiaozhi_cfg", false);
    if (s_prefs.isKey("url")) {
        _serverUrl = s_prefs.getString("url", "wss://api.tenclass.net/xiaozhi/v1/").c_str();
    } else {
        _serverUrl = "wss://api.tenclass.net/xiaozhi/v1/";
    }
    // Tự động chuyển domain cũ nếu có sang domain chính thức tenclass
    if (_serverUrl.empty() || _serverUrl.find("api.xiaozhi.me") != std::string::npos) {
        _serverUrl = "wss://api.tenclass.net/xiaozhi/v1/";
    }
    if (s_prefs.isKey("token")) {
        _serverToken = s_prefs.getString("token", "").c_str();
    } else {
        _serverToken = "";
    }
    if (s_prefs.isKey("uuid")) {
        _clientId = s_prefs.getString("uuid", "").c_str();
    } else {
        _clientId = "";
    }
    s_prefs.end();

    _ensureClientId();

    // Lấy MAC address từ WiFi nếu chưa được gán tùy biến
    if (_deviceId.empty() || _deviceId == "00:00:00:00:00:00") {
        String mac = WiFi.macAddress();
        if (mac.length() > 0 && mac != "00:00:00:00:00:00") {
            mac.toLowerCase();
            _deviceId = mac.c_str();
        } else {
            _deviceId = "10:bd:a3:9a:96:28";
        }
    }

    LOG_I(TAG, "Loaded config: URL='%s', Device-Id='%s', Client-Id='%s'",
          _serverUrl.c_str(), _deviceId.c_str(), _clientId.c_str());
}

void XiaozhiClient::_saveNvsSettings() {
    s_prefs.begin("xiaozhi_cfg", false);
    s_prefs.putString("url", _serverUrl.c_str());
    s_prefs.putString("token", _serverToken.c_str());
    s_prefs.putString("uuid", _clientId.c_str());
    s_prefs.end();
}

void XiaozhiClient::_ensureClientId() {
    if (_clientId.length() < 32) {
        // Sinh UUID v4: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
        char uuidBuf[37];
        snprintf(uuidBuf, sizeof(uuidBuf), "%08lx-%04lx-4%03lx-%04lx-%08lx%04lx",
                 (unsigned long)esp_random(),
                 (unsigned long)(esp_random() & 0xFFFF),
                 (unsigned long)(esp_random() & 0x0FFF),
                 (unsigned long)((esp_random() & 0x3FFF) | 0x8000),
                 (unsigned long)esp_random(),
                 (unsigned long)(esp_random() & 0xFFFF));
        _clientId = uuidBuf;
        s_prefs.begin("xiaozhi_cfg", false);
        s_prefs.putString("uuid", _clientId.c_str());
        s_prefs.end();
        LOG_I(TAG, "Sinh Client-Id UUID moi: %s", _clientId.c_str());
    }
}

void XiaozhiClient::_pushEvent(const XiaozhiEvent& ev) {
    if (_eventQueue != nullptr) {
        xQueueSend(_eventQueue, &ev, 0);
    }
}

bool XiaozhiClient::begin() {
    if (_taskRunning) return true;

    _loadNvsSettings();

    // Nếu chưa có token máy chủ, tự động yêu cầu mã kích hoạt OTA từ tenclass
    if (_serverToken.empty()) {
        _fetchOtaRequested = true;
    }

    if (_eventQueue == nullptr) {
        _eventQueue = xQueueCreate(10, sizeof(XiaozhiEvent));
    }
    if (_outQueue == nullptr) {
        _outQueue = xQueueCreate(6, 1024);
    }
    if (_audioOutQueue == nullptr) {
        _audioOutQueue = xQueueCreate(8, sizeof(XiaozhiAudioPacket));
    }

    // Khởi động pipeline âm thanh Opus và đăng ký đẩy gói Opus lên hàng đợi WebSocket
    XiaozhiAudio::getInstance().begin();
    XiaozhiAudio::getInstance().onAudioOutput([this](const uint8_t* data, size_t len) {
        if (_audioOutQueue != nullptr && len <= sizeof(XiaozhiAudioPacket::data) && isConnected()) {
            XiaozhiAudioPacket pkt;
            pkt.len = len;
            memcpy(pkt.data, data, len);
            xQueueSend(_audioOutQueue, &pkt, 0);
        }
    });

    _taskRunning = true;
    BaseType_t res = xTaskCreatePinnedToCore(
        _taskTrampoline,
        "xiaozhiTask",
        12288, // 12KB stack an toàn
        this,
        2,     // Priority 2
        &_taskHandle,
        0
    );

    if (res != pdPASS) {
        LOG_E(TAG, "Khong the tao xiaozhiTask FreeRTOS!");
        _taskRunning = false;
        return false;
    }

    LOG_I(TAG, "XiaozhiClient da khoi dong task thanh cong.");
    return true;
}

void XiaozhiClient::stop() {
    _taskRunning = false;
    _shouldConnect = false;
    _shouldDisconnect = true;

    XiaozhiAudio::getInstance().stop();

    if (_taskHandle != nullptr) {
        int waitMs = 500;
        while (_taskHandle != nullptr && waitMs > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waitMs -= 10;
        }
        if (_taskHandle != nullptr) {
            LOG_W(TAG, "xiaozhiTask khong thoat kip, cuong che xoa task!");
            vTaskDelete(_taskHandle);
            _taskHandle = nullptr;
        }
    }

    if (_eventQueue != nullptr) {
        vQueueDelete(_eventQueue);
        _eventQueue = nullptr;
    }
    if (_outQueue != nullptr) {
        vQueueDelete(_outQueue);
        _outQueue = nullptr;
    }
    if (_audioOutQueue != nullptr) {
        vQueueDelete(_audioOutQueue);
        _audioOutQueue = nullptr;
    }

    _state = XiaozhiConnectionState::DISCONNECTED;
}

void XiaozhiClient::connect() {
    _shouldDisconnect = false;
    _shouldConnect = true;
}

void XiaozhiClient::disconnect() {
    _shouldConnect = false;
    _shouldDisconnect = true;
}

void XiaozhiClient::updateServerConfig(const char* url, const char* token) {
    if (url != nullptr && strlen(url) > 0) {
        _serverUrl = url;
    }
    if (token != nullptr) {
        _serverToken = token;
    }
    _saveNvsSettings();
    LOG_I(TAG, "Da cap nhat server config: url=%s", _serverUrl.c_str());
    disconnect();
    connect();
}

bool XiaozhiClient::fetchActivationCode() {
    _fetchOtaRequested = true;
    return true;
}

void XiaozhiClient::pollEvents() {
    if (_eventQueue == nullptr) return;

    XiaozhiEvent ev;
    while (xQueueReceive(_eventQueue, &ev, 0) == pdTRUE) {
        switch (ev.type) {
            case XiaozhiEventType::CONNECTION_STATE_CHANGED:
                if (_stateCallback != nullptr) {
                    _stateCallback(ev.state, ev.code);
                }
                break;
            case XiaozhiEventType::USER_MESSAGE:
                if (_chatCallback != nullptr) {
                    _chatCallback("user", ev.text);
                }
                break;
            case XiaozhiEventType::ASSISTANT_SENTENCE:
                if (_chatCallback != nullptr) {
                    _chatCallback("assistant", ev.text);
                }
                break;
            case XiaozhiEventType::EMOTION_CHANGED:
                if (_emotionCallback != nullptr) {
                    _emotionCallback(ev.text);
                }
                break;
            case XiaozhiEventType::TTS_START:
                if (_speakingCallback != nullptr) {
                    _speakingCallback(true);
                }
                break;
            case XiaozhiEventType::TTS_STOP:
                if (_speakingCallback != nullptr) {
                    _speakingCallback(false);
                }
                break;
            case XiaozhiEventType::ACTIVATION_INFO:
                if (_activationCallback != nullptr) {
                    _activationCallback(ev.text, ev.extra);
                }
                break;
            case XiaozhiEventType::ALERT_MESSAGE:
                LOG_W(TAG, "Alert event: %s", ev.text);
                if (_alertCallback != nullptr) {
                    _alertCallback(ev.text);
                }
                break;
            case XiaozhiEventType::CAMERA_CAPTURE_START:
                if (_cameraCaptureCallback != nullptr) {
                    _cameraCaptureCallback(true, ev.text);
                }
                break;
            case XiaozhiEventType::CAMERA_CAPTURE_END:
                if (_cameraCaptureCallback != nullptr) {
                    _cameraCaptureCallback(false, ev.text);
                }
                break;
            default:
                break;
        }
    }
}

static String extractJsonField(const String& json, const char* field) {
    String search = "\"" + String(field) + "\"";
    int idx = json.indexOf(search);
    if (idx < 0) return "";
    int colon = json.indexOf(':', idx + search.length());
    if (colon < 0) return "";
    int quote1 = json.indexOf('\"', colon);
    if (quote1 < 0) return "";
    int quote2 = json.indexOf('\"', quote1 + 1);
    if (quote2 < 0) return "";
    return json.substring(quote1 + 1, quote2);
}

void XiaozhiClient::_taskTrampoline(void* arg) {
    XiaozhiClient* self = static_cast<XiaozhiClient*>(arg);
    self->_workerTask();
}

void XiaozhiClient::_workerTask() {
    WebsocketsClient client;
    client.setInsecure();

    client.onEvent([this, &client](WebsocketsEvent event, String data) {
        if (event == WebsocketsEvent::ConnectionOpened) {
            LOG_I(TAG, ">>> WebSocket Connection Opened!");
        } else if (event == WebsocketsEvent::ConnectionClosed) {
            int closeReason = (int)client.getCloseReason();
            _lastCloseReason = closeReason;
            LOG_W(TAG, ">>> WebSocket Connection Closed (Reason=%d, Msg='%s')",
                  closeReason, data.c_str());
            client.close();
            _state = XiaozhiConnectionState::DISCONNECTED;
            XiaozhiEvent ev;
            ev.type = XiaozhiEventType::CONNECTION_STATE_CHANGED;
            ev.state = _state;
            ev.code = closeReason;
            snprintf(ev.text, sizeof(ev.text), "%s", data.c_str());
            _pushEvent(ev);
        } else if (event == WebsocketsEvent::GotPing) {
            LOG_D(TAG, "Got Ping");
        } else if (event == WebsocketsEvent::GotPong) {
            LOG_D(TAG, "Got Pong");
        }
    });

    client.onMessage([this](WebsocketsMessage message) {
        if (message.isBinary()) {
            const WSString& raw = message.rawData();
            if (raw.size() > 0) {
                // Đảm bảo dừng thu âm Micro ngay khi nhận gói âm thanh phát ra (chống hú / vọng âm)
                if (_state != XiaozhiConnectionState::SPEAKING) {
                    XiaozhiAudio::getInstance().stopRecording();
                    _state = XiaozhiConnectionState::SPEAKING;
                    _ttsServerFinished = false;
                    XiaozhiEvent ev;
                    ev.type = XiaozhiEventType::TTS_START;
                    ev.state = _state;
                    _pushEvent(ev);
                }
                XiaozhiAudio::getInstance().feedPlaybackOpus((const uint8_t*)raw.data(), raw.size());
            }
        } else if (message.isText()) {
            String payload = message.data();
            LOG_D(TAG, "WS Recv: %s", payload.c_str());

            String type = extractJsonField(payload, "type");
            if (type == "hello") {
                String sessionId = extractJsonField(payload, "session_id");
                if (sessionId.length() > 0) {
                    _sessionId = sessionId.c_str();
                }
                _state = XiaozhiConnectionState::CONNECTED;
                LOG_I(TAG, "Handshake hello thanh cong! SessionId=%s", _sessionId.c_str());

                XiaozhiEvent ev;
                ev.type = XiaozhiEventType::CONNECTION_STATE_CHANGED;
                ev.state = _state;
                snprintf(ev.text, sizeof(ev.text), "%s", _sessionId.c_str());
                _pushEvent(ev);
            } else if (type == "stt") {
                String text = extractJsonField(payload, "text");
                if (text.length() > 0) {
                    XiaozhiEvent ev;
                    ev.type = XiaozhiEventType::USER_MESSAGE;
                    snprintf(ev.text, sizeof(ev.text), "%s", text.c_str());
                    _pushEvent(ev);
                }
            } else if (type == "tts") {
                String state = extractJsonField(payload, "state");
                if (state == "start") {
                    XiaozhiAudio::getInstance().stopRecording();
                    _state = XiaozhiConnectionState::SPEAKING;
                    _ttsServerFinished = false;
                    XiaozhiEvent ev;
                    ev.type = XiaozhiEventType::TTS_START;
                    ev.state = _state;
                    _pushEvent(ev);
                } else if (state == "stop") {
                    _ttsServerFinished = true;
                } else if (state == "sentence_start") {
                    String text = extractJsonField(payload, "text");
                    if (text.length() > 0) {
                        XiaozhiEvent ev;
                        ev.type = XiaozhiEventType::ASSISTANT_SENTENCE;
                        snprintf(ev.text, sizeof(ev.text), "%s", text.c_str());
                        _pushEvent(ev);
                    }
                }
            } else if (type == "llm") {
                String emotion = extractJsonField(payload, "emotion");
                if (emotion.length() > 0) {
                    XiaozhiEvent ev;
                    ev.type = XiaozhiEventType::EMOTION_CHANGED;
                    snprintf(ev.text, sizeof(ev.text), "%s", emotion.c_str());
                    _pushEvent(ev);
                }
            } else if (type == "alert") {
                String msg = extractJsonField(payload, "message");
                XiaozhiEvent ev;
                ev.type = XiaozhiEventType::ALERT_MESSAGE;
                snprintf(ev.text, sizeof(ev.text), "%s", msg.c_str());
                _pushEvent(ev);
            } else if (type == "mcp") {
                cJSON *root = cJSON_Parse(payload.c_str());
                if (root) {
                    cJSON *payloadObj = cJSON_GetObjectItem(root, "payload");
                    if (!payloadObj) payloadObj = root;

                    int idVal = 1;
                    cJSON *idItem = cJSON_GetObjectItem(payloadObj, "id");
                    if (idItem && cJSON_IsNumber(idItem)) {
                        idVal = idItem->valueint;
                    }

                    cJSON *methodItem = cJSON_GetObjectItem(payloadObj, "method");
                    const char *method = methodItem && cJSON_IsString(methodItem) ? methodItem->valuestring : "";

                    LOG_I(TAG, "MCP Request id=%d, method='%s'", idVal, method);

                    if (strcmp(method, "initialize") == 0) {
                        cJSON *params = cJSON_GetObjectItem(payloadObj, "params");
                        if (params) {
                            cJSON *caps = cJSON_GetObjectItem(params, "capabilities");
                            if (caps) {
                                cJSON *vision = cJSON_GetObjectItem(caps, "vision");
                                if (vision) {
                                    cJSON *vUrl = cJSON_GetObjectItem(vision, "url");
                                    cJSON *vTok = cJSON_GetObjectItem(vision, "token");
                                    if (vUrl && cJSON_IsString(vUrl) && strlen(vUrl->valuestring) > 0) {
                                        _visionUrl = vUrl->valuestring;
                                    }
                                    if (vTok && cJSON_IsString(vTok) && strlen(vTok->valuestring) > 0) {
                                        _visionToken = vTok->valuestring;
                                    }
                                    LOG_I(TAG, "Da nhan Vision config tu MCP: URL='%s'", _visionUrl.c_str());
                                }
                            }
                        }

                        String mcpResp = "{\"session_id\":\"" + String(_sessionId.c_str()) + "\","
                                         "\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":" + String(idVal) + ","
                                         "\"result\":{\"protocolVersion\":\"2024-11-05\","
                                         "\"capabilities\":{\"tools\":{}},"
                                         "\"serverInfo\":{\"name\":\"xiaozhi-esp32-client\",\"version\":\"1.0.0\"}}}}";
                        _sendWsText(mcpResp);
                        LOG_I(TAG, "Phan hoi MCP initialize thanh cong!");
                    } else if (strcmp(method, "tools/list") == 0) {
                        cJSON *toolsArray = cJSON_CreateArray();

                        // 1. Tool Camera nếu có Vision Handler
                        if (_visionCaptureFunc != nullptr) {
                            cJSON *camTool = cJSON_CreateObject();
                            cJSON_AddStringToObject(camTool, "name", "self.camera.take_photo");
                            cJSON_AddStringToObject(camTool, "description", "Always remember you have a camera. If the user asks you to see something, use this tool to take a photo and then explain it.\\nArgs:\\n `question`: The question that you want to ask about the photo.\\nReturn:\\n A JSON object that provides the photo information.");
                            cJSON *schema = cJSON_Parse("{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\",\"description\":\"The question that you want to ask about the photo.\"}},\"required\":[\"question\"]}");
                            if (schema) {
                                cJSON_AddItemToObject(camTool, "inputSchema", schema);
                            }
                            cJSON_AddItemToArray(toolsArray, camTool);
                        }

                        // 2. Các Custom Tools đã đăng ký (GPIO, Relay, cảm biến...)
                        for (const auto& tool : _customTools) {
                            cJSON *tObj = cJSON_CreateObject();
                            cJSON_AddStringToObject(tObj, "name", tool.name.c_str());
                            cJSON_AddStringToObject(tObj, "description", tool.description.c_str());
                            cJSON *schema = cJSON_Parse(tool.inputSchemaJson.c_str());
                            if (schema) {
                                cJSON_AddItemToObject(tObj, "inputSchema", schema);
                            } else {
                                cJSON *emptySchema = cJSON_CreateObject();
                                cJSON_AddStringToObject(emptySchema, "type", "object");
                                cJSON_AddItemToObject(tObj, "inputSchema", emptySchema);
                            }
                            cJSON_AddItemToArray(toolsArray, tObj);
                        }

                        char *toolsPrinted = cJSON_PrintUnformatted(toolsArray);
                        String toolsListJson = toolsPrinted ? String(toolsPrinted) : "[]";
                        if (toolsPrinted) free(toolsPrinted);
                        cJSON_Delete(toolsArray);

                        String mcpResp = "{\"session_id\":\"" + String(_sessionId.c_str()) + "\","
                                         "\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":" + String(idVal) + ","
                                         "\"result\":{\"tools\":" + toolsListJson + "}}}";
                        _sendWsText(mcpResp);
                        LOG_I(TAG, "Phan hoi MCP tools/list!");
                    } else if (strcmp(method, "tools/call") == 0) {
                        cJSON *params = cJSON_GetObjectItem(payloadObj, "params");
                        const char *toolName = "";
                        const char *question = "Phía trước bạn có gì?";
                        cJSON *args = nullptr;
                        if (params) {
                            cJSON *nameItem = cJSON_GetObjectItem(params, "name");
                            if (nameItem && cJSON_IsString(nameItem)) toolName = nameItem->valuestring;
                            args = cJSON_GetObjectItem(params, "arguments");
                            if (args) {
                                cJSON *qItem = cJSON_GetObjectItem(args, "question");
                                if (qItem && cJSON_IsString(qItem)) question = qItem->valuestring;
                            }
                        }
                        LOG_I(TAG, "MCP tools/call: tool='%s', question='%s'", toolName, question);
                        if (strcmp(toolName, "self.camera.take_photo") == 0 && _visionCaptureFunc != nullptr) {
                            _handleCameraTakePhotoTool(idVal, question);
                        } else {
                            bool toolHandled = false;
                            for (const auto& tool : _customTools) {
                                if (tool.name == toolName && tool.handler != nullptr) {
                                    LOG_I(TAG, "Thuc thi custom MCP tool: '%s'", toolName);
                                    String resultText = tool.handler(args);
                                    resultText.replace("\\", "\\\\");
                                    resultText.replace("\"", "\\\"");
                                    resultText.replace("\n", "\\n");
                                    resultText.replace("\r", "");

                                    String toolResp = "{\"session_id\":\"" + String(_sessionId.c_str()) + "\","
                                                      "\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":" + String(idVal) + ","
                                                      "\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + resultText + "\"}]}}}";
                                    _sendWsText(toolResp);
                                    LOG_I(TAG, "Da phan hoi custom tool '%s' id=%d!", toolName, idVal);
                                    toolHandled = true;
                                    break;
                                }
                            }

                            if (!toolHandled) {
                                String errResp = "{\"session_id\":\"" + String(_sessionId.c_str()) + "\","
                                                 "\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":" + String(idVal) + ","
                                                 "\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}}";
                                _sendWsText(errResp);
                            }
                        }
                    } else {
                        LOG_D(TAG, "Nhan MCP message: %s", method);
                    }
                    cJSON_Delete(root);
                }
            }
        }
    });

    while (_taskRunning) {
        // 1. Kiểm tra yêu cầu truy vấn OTA Activation Code
        if (_fetchOtaRequested) {
            if (!_isNetworkConnected()) {
                _fetchOtaRequested = false;
                LOG_W(TAG, "Chua co ket noi mang, khong the lay ma OTA!");
                XiaozhiEvent ev;
                ev.type = XiaozhiEventType::ACTIVATION_INFO;
                snprintf(ev.text, sizeof(ev.text), "Chưa có mạng");
                snprintf(ev.extra, sizeof(ev.extra), "Vui lòng kết nối Wi-Fi trước");
                _pushEvent(ev);
            } else {
                _fetchOtaRequested = false;
                LOG_I(TAG, "Dang truy van OTA Activation tu https://api.tenclass.net/xiaozhi/ota/ ...");
                
                WiFiClientSecure otaClient;
                otaClient.setInsecure();
                otaClient.setTimeout(8000);
                HTTPClient http;
                http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
                http.setUserAgent("XiaoZhi-ESP32-Client/1.0");

                if (http.begin(otaClient, "https://api.tenclass.net/xiaozhi/ota/")) {
                    http.addHeader("Device-Id", _deviceId.c_str());
                    http.addHeader("Client-Id", _clientId.c_str());
                    http.addHeader("Activation-Version", "1");
                    http.addHeader("Content-Type", "application/json");

                    int code = http.POST("{}");
                    if (code == 200) {
                        String resp = http.getString();
                        LOG_I(TAG, "OTA Response: %s", resp.c_str());
                        
                        cJSON *root = cJSON_Parse(resp.c_str());
                        if (root != NULL) {
                            cJSON *act = cJSON_GetObjectItem(root, "activation");
                            if (cJSON_IsObject(act)) {
                                cJSON *c = cJSON_GetObjectItem(act, "code");
                                cJSON *m = cJSON_GetObjectItem(act, "message");
                                if (cJSON_IsString(c) && strlen(c->valuestring) > 0) {
                                    _activationCode = c->valuestring;
                                }
                                if (cJSON_IsString(m) && strlen(m->valuestring) > 0) {
                                    _activationMessage = m->valuestring;
                                }

                                _isActivating = true;
                                _lastActivatePollMs = millis();
                                XiaozhiEvent ev;
                                ev.type = XiaozhiEventType::ACTIVATION_INFO;
                                snprintf(ev.text, sizeof(ev.text), "%s", _activationCode.c_str());
                                snprintf(ev.extra, sizeof(ev.extra), "%s", _activationMessage.c_str());
                                _pushEvent(ev);
                            } else {
                                // Thiết bị đã liên kết thành công trên xiaozhi.me / tenclass!
                                _isActivating = false;
                                _activationCode.clear();
                                _activationMessage.clear();
                            }

                            cJSON *ws = cJSON_GetObjectItem(root, "websocket");
                            if (cJSON_IsObject(ws)) {
                                cJSON *u = cJSON_GetObjectItem(ws, "url");
                                cJSON *t = cJSON_GetObjectItem(ws, "token");
                                if (cJSON_IsString(u) && strlen(u->valuestring) > 0) {
                                    _serverUrl = u->valuestring;
                                }
                                if (cJSON_IsString(t) && strlen(t->valuestring) > 0) {
                                    _serverToken = t->valuestring;
                                }
                                _saveNvsSettings();

                                if (!_isActivating) {
                                    LOG_I(TAG, ">>> Thiet bi da lien ket thanh cong! Token: '%s', Url: %s <<<",
                                          _serverToken.c_str(), _serverUrl.c_str());
                                    _shouldConnect = true;
                                }
                            }
                            cJSON_Delete(root);
                        }
                    } else {
                        LOG_W(TAG, "OTA HTTP Error: %d", code);
                    }
                    http.end();
                }
            }
        }

        // 1b. Thăm dò trạng thái kích hoạt từ endpoint /xiaozhi/ota/activate mỗi 3 giây
        if (_isActivating && !_activationCode.empty() && _isNetworkConnected()) {
            uint32_t nowMs = millis();
            if (nowMs - _lastActivatePollMs >= 3000) {
                _lastActivatePollMs = nowMs;
                LOG_I(TAG, "Dang tham do trang thai kich hoat tren xiaozhi.me (ma: %s)...", _activationCode.c_str());

                WiFiClientSecure actClient;
                actClient.setInsecure();
                actClient.setTimeout(5000);
                HTTPClient http;
                http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
                http.setUserAgent("XiaoZhi-ESP32-Client/1.0");

                if (http.begin(actClient, "https://api.tenclass.net/xiaozhi/ota/activate")) {
                    http.addHeader("Device-Id", _deviceId.c_str());
                    http.addHeader("Client-Id", _clientId.c_str());
                    http.addHeader("Activation-Version", "1");
                    http.addHeader("Content-Type", "application/json");

                    int httpCode = http.POST("{}");
                    if (httpCode == 200) {
                        LOG_I(TAG, ">>> KICH HOAT THANH CONG TREN XIAOZHI.ME! <<<");
                        _isActivating = false;
                        _fetchOtaRequested = true;
                    } else if (httpCode == 202) {
                        LOG_D(TAG, "Dang cho nguoi dung nhap ma tren xiaozhi.me (HTTP 202)...");
                    } else {
                        LOG_W(TAG, "Activate poll HTTP code: %d", httpCode);
                    }
                    http.end();
                }
            }
        }

        // 2. Xử lý ngắt kết nối
        if (_shouldDisconnect) {
            _shouldDisconnect = false;
            _lastCloseReason = 1000;
            if (client.available()) {
                client.close();
            }
            _state = XiaozhiConnectionState::DISCONNECTED;
            XiaozhiEvent ev;
            ev.type = XiaozhiEventType::CONNECTION_STATE_CHANGED;
            ev.state = _state;
            ev.code = 1000;
            snprintf(ev.text, sizeof(ev.text), "Disconnected");
            _pushEvent(ev);
        }

        // 3. Xử lý kết nối mới
        static uint8_t s_consecutiveWsFailCount = 0;
        if (_shouldConnect) {
            if (_deviceId.empty() || _deviceId == "00:00:00:00:00:00") {
                String realMac = WiFi.macAddress();
                if (realMac.length() > 0 && realMac != "00:00:00:00:00:00") {
                    realMac.toLowerCase();
                    _deviceId = realMac.c_str();
                }
            }

            if (_isActivating || _serverToken.empty()) {
                LOG_W(TAG, "Thiet bi chua co token tren xiaozhi.me, bat dau truy van OTA...");
                _shouldConnect = false;
                _fetchOtaRequested = true;
                _state = XiaozhiConnectionState::DISCONNECTED;
                XiaozhiEvent ev;
                ev.type = XiaozhiEventType::CONNECTION_STATE_CHANGED;
                ev.state = _state;
                snprintf(ev.text, sizeof(ev.text), "Cần kích hoạt");
                _pushEvent(ev);
            } else if (_isNetworkConnected()) {
                _shouldConnect = false;
                LOG_I(TAG, "Bat dau ket noi toi WebSocket: %s (Device: %s)", _serverUrl.c_str(), _deviceId.c_str());
                _state = XiaozhiConnectionState::CONNECTING;
                XiaozhiEvent ev;
                ev.type = XiaozhiEventType::CONNECTION_STATE_CHANGED;
                ev.state = _state;
                _pushEvent(ev);

                client.clearHeaders();
                std::string tokenHeader = _serverToken;
                if (tokenHeader.find("Bearer ") == std::string::npos) {
                    tokenHeader = "Bearer " + tokenHeader;
                }
                client.addHeader("Authorization", tokenHeader.c_str());
                client.addHeader("Protocol-Version", "1");
                client.addHeader("Device-Id", _deviceId.c_str());
                client.addHeader("Client-Id", _clientId.c_str());

                client.close();
                vTaskDelay(pdMS_TO_TICKS(50));
                bool ok = client.connect(_serverUrl.c_str());
                if (ok) {
                    s_consecutiveWsFailCount = 0;
                    _state = XiaozhiConnectionState::HANDSHAKING;
                    ev.state = _state;
                    _pushEvent(ev);

                    String helloMsg = "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
                                      "\"features\":{\"mcp\":true},"
                                      "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
                    client.send(helloMsg);
                    LOG_I(TAG, "Da gui hello handshake toi server!");
                } else {
                    client.close();
                    LOG_E(TAG, "Ket noi WebSocket that bai!");
                    s_consecutiveWsFailCount++;
                    _lastCloseReason = 1006;
                    _state = XiaozhiConnectionState::ERROR_STATE;
                    ev.state = _state;
                    ev.code = 1006;
                    snprintf(ev.text, sizeof(ev.text), "Lỗi kết nối");
                    _pushEvent(ev);

                    if (s_consecutiveWsFailCount >= 2) {
                        LOG_W(TAG, "Ket noi WebSocket that bai %d lan, yeu cau OTA cap nhat Token/URL...", s_consecutiveWsFailCount);
                        s_consecutiveWsFailCount = 0;
                        _fetchOtaRequested = true;
                    }
                }
            } else {
                uint32_t now = millis();
                if (now - _lastReconnectAttemptMs >= 5000) {
                    _lastReconnectAttemptMs = now;
                    LOG_W(TAG, "Dang cho mang san sang de ket noi XiaoZhi...");
                }
            }
        }

        // 4. Poll WebSocket khi đang kết nối & đẩy dữ liệu hàng đợi
        if (client.available()) {
            if (_outQueue != nullptr) {
                char outBuf[1024];
                while (xQueueReceive(_outQueue, outBuf, 0) == pdTRUE) {
                    client.send(outBuf);
                    LOG_I(TAG, "WS Out: %s", outBuf);
                }
            }

            if (_audioOutQueue != nullptr) {
                XiaozhiAudioPacket pkt;
                while (xQueueReceive(_audioOutQueue, &pkt, 0) == pdTRUE) {
                    client.sendBinary((const char*)pkt.data, pkt.len);
                }
            }

            client.poll();

            uint32_t now = millis();
            if (now - _lastPingMs >= 30000) {
                _lastPingMs = now;
                client.ping();
            }
        }

        // 4b. Đảm bảo loa xả sạch toàn bộ âm thanh TTS trước khi chuyển trạng thái kết thúc SPEAKING
        if (_state == XiaozhiConnectionState::SPEAKING && _ttsServerFinished && !XiaozhiAudio::getInstance().isPlaying()) {
            _ttsServerFinished = false;
            _state = XiaozhiConnectionState::CONNECTED;
            XiaozhiEvent ev;
            ev.type = XiaozhiEventType::TTS_STOP;
            ev.state = _state;
            _pushEvent(ev);
            LOG_I(TAG, "Loa da phat het am thanh TTS -> Ket thuc SPEAKING");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (client.available()) {
        client.close();
    }

    LOG_I(TAG, "xiaozhiTask da ket thuc va tu huy an toan.");
    _taskHandle = nullptr;
    vTaskDelete(NULL);
}

bool XiaozhiClient::sendTextMessage(const char* text) {
    if (!isConnected() || text == nullptr || strlen(text) == 0) return false;

    if (_state != XiaozhiConnectionState::LISTENING) {
        String startJson = "{\"session_id\":\"" + String(_sessionId.c_str()) + "\","
                           "\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}";
        if (_outQueue != nullptr) {
            char bufStart[256];
            strncpy(bufStart, startJson.c_str(), sizeof(bufStart) - 1);
            bufStart[sizeof(bufStart) - 1] = '\0';
            xQueueSend(_outQueue, bufStart, 0);
        }
    }

    String json = "{\"session_id\":\"" + String(_sessionId.c_str()) + "\","
                  "\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + String(text) + "\"}";

    LOG_I(TAG, "Queue text message: %s", text);
    if (_outQueue != nullptr) {
        char buf[512];
        strncpy(buf, json.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        return (xQueueSend(_outQueue, buf, 0) == pdTRUE);
    }
    return false;
}

bool XiaozhiClient::startVoiceListening(XiaozhiListeningMode mode) {
    if (!isConnected()) return false;

    _listeningMode = mode;
    XiaozhiAudio::getInstance().startRecording();

    String modeStr = "auto";
    if (mode == XiaozhiListeningMode::MANUAL) {
        modeStr = "manual";
    } else if (mode == XiaozhiListeningMode::REALTIME) {
        modeStr = "realtime";
    }

    String json = "{\"session_id\":\"" + String(_sessionId.c_str()) + "\","
                  "\"type\":\"listen\",\"state\":\"start\",\"mode\":\"" + modeStr + "\"}";
    LOG_I(TAG, "Queue start listening (mode=%s): %s", modeStr.c_str(), json.c_str());
    _state = XiaozhiConnectionState::LISTENING;
    XiaozhiEvent ev;
    ev.type = XiaozhiEventType::CONNECTION_STATE_CHANGED;
    ev.state = _state;
    _pushEvent(ev);

    if (_outQueue != nullptr) {
        char buf[512];
        strncpy(buf, json.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        return (xQueueSend(_outQueue, buf, 0) == pdTRUE);
    }
    return true;
}

bool XiaozhiClient::stopVoiceListening() {
    if (!isConnected()) return false;

    XiaozhiAudio::getInstance().stopRecording();

    String json = "{\"session_id\":\"" + String(_sessionId.c_str()) + "\","
                  "\"type\":\"listen\",\"state\":\"stop\"}";
    LOG_I(TAG, "Queue stop listening: %s", json.c_str());
    _state = XiaozhiConnectionState::CONNECTED;
    XiaozhiEvent ev;
    ev.type = XiaozhiEventType::CONNECTION_STATE_CHANGED;
    ev.state = _state;
    _pushEvent(ev);

    if (_outQueue != nullptr) {
        char buf[512];
        strncpy(buf, json.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        return (xQueueSend(_outQueue, buf, 0) == pdTRUE);
    }
    return true;
}

bool XiaozhiClient::abortSpeaking() {
    if (!isConnected()) return false;

    _ttsServerFinished = false;
    XiaozhiAudio::getInstance().clearPlaybackBuffer();

    String json = "{\"session_id\":\"" + String(_sessionId.c_str()) + "\","
                  "\"type\":\"abort\",\"reason\":\"user_interrupt\"}";
    LOG_I(TAG, "Queue abort speaking");
    _state = XiaozhiConnectionState::CONNECTED;
    XiaozhiEvent ev;
    ev.type = XiaozhiEventType::TTS_STOP;
    ev.state = _state;
    _pushEvent(ev);

    if (_outQueue != nullptr) {
        char buf[512];
        strncpy(buf, json.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        return (xQueueSend(_outQueue, buf, 0) == pdTRUE);
    }
    return true;
}

void XiaozhiClient::_sendWsText(const String& text) {
    if (_outQueue != nullptr) {
        char outBuf[1024];
        size_t len = text.length();
        if (len >= sizeof(outBuf)) len = sizeof(outBuf) - 1;
        memcpy(outBuf, text.c_str(), len);
        outBuf[len] = '\0';
        xQueueSend(_outQueue, outBuf, 0);
    }
}

struct VisionTaskParam {
    XiaozhiClient* client;
    int reqId;
    std::string question;
};

void XiaozhiClient::_visionTaskTrampoline(void* arg) {
    VisionTaskParam* p = static_cast<VisionTaskParam*>(arg);
    if (p && p->client) {
        p->client->_runVisionTask(p->reqId, p->question.c_str());
    }
    if (p) delete p;
    vTaskDelete(NULL);
}

void XiaozhiClient::_handleCameraTakePhotoTool(int reqId, const char* question) {
    if (_isCapturingPhoto) {
        LOG_W(TAG, "Camera dang ban chup anh, bo qua yeu cau trung lap");
        return;
    }
    _isCapturingPhoto = true;

    VisionTaskParam* param = new VisionTaskParam();
    param->client = this;
    param->reqId = reqId;
    param->question = (question && strlen(question) > 0) ? question : "Phía trước có gì?";

    BaseType_t res = xTaskCreatePinnedToCore(
        _visionTaskTrampoline,
        "xiaozhiVision",
        10240,
        param,
        1,
        nullptr,
        0
    );

    if (res != pdPASS) {
        LOG_E(TAG, "Khong the tao xiaozhiVision task!");
        delete param;
        _isCapturingPhoto = false;

        String errResp = "{\"session_id\":\"" + String(_sessionId.c_str()) + "\","
                         "\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":" + String(reqId) + ","
                         "\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"Lỗi không thể khởi tạo camera chụp ảnh\"}]}}}";
        _sendWsText(errResp);
    }
}

void XiaozhiClient::_runVisionTask(int reqId, const char* question) {
    LOG_I(TAG, "Bat dau vision task chup anh & phan tich (reqId=%d, question='%s')...", reqId, question);

    // 1. Thông báo sự kiện bắt đầu chụp ảnh
    XiaozhiEvent evStart;
    evStart.type = XiaozhiEventType::CAMERA_CAPTURE_START;
    snprintf(evStart.text, sizeof(evStart.text), "%s", question);
    _pushEvent(evStart);

    // 2. Chụp ảnh JPEG qua Vision Handler đã đăng ký
    uint8_t* jpegBuf = nullptr;
    size_t jpegSize = 0;
    bool encodeOk = false;

    if (_visionCaptureFunc != nullptr) {
        encodeOk = _visionCaptureFunc(question, &jpegBuf, &jpegSize);
    }

    String resultText = "Không thể chụp ảnh hoặc nén JPEG";

    if (encodeOk && jpegBuf && jpegSize > 0) {
        LOG_I(TAG, "Chup anh thanh cong, bat dau upload (%u bytes JPEG)...", (unsigned int)jpegSize);

        std::string uploadUrl = _visionUrl;
        if (uploadUrl.empty()) {
            uploadUrl = "https://api.tenclass.net/xiaozhi/v1/vision/explain";
        }

        std::string token = _visionToken.empty() ? _serverToken : _visionToken;
        if (!token.empty() && token.find("Bearer ") == std::string::npos) {
            token = "Bearer " + token;
        }

        WiFiClientSecure secClient;
        secClient.setInsecure();
        secClient.setTimeout(8000);

        HTTPClient http;
        http.setUserAgent("XiaoZhi-ESP32-Client/1.0");

        String boundary = "----ESP32_CAMERA_BOUNDARY";
        String headPart = "--" + boundary + "\r\n"
                          "Content-Disposition: form-data; name=\"question\"\r\n\r\n" +
                          String(question) + "\r\n--" + boundary + "\r\n"
                          "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n"
                          "Content-Type: image/jpeg\r\n\r\n";
        String tailPart = "\r\n--" + boundary + "--\r\n";

        size_t totalBodyLen = headPart.length() + jpegSize + tailPart.length();
        uint8_t* postBody = (uint8_t*)heap_caps_malloc(totalBodyLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (postBody && http.begin(secClient, uploadUrl.c_str())) {
            http.addHeader("Authorization", token.c_str());
            http.addHeader("Device-Id", _deviceId.c_str());
            http.addHeader("Client-Id", _clientId.c_str());
            http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

            memcpy(postBody, headPart.c_str(), headPart.length());
            memcpy(postBody + headPart.length(), jpegBuf, jpegSize);
            memcpy(postBody + headPart.length() + jpegSize, tailPart.c_str(), tailPart.length());

            int httpCode = http.POST(postBody, totalBodyLen);
            LOG_I(TAG, "Vision Upload HTTP Code: %d", httpCode);

            if (httpCode == 200 || httpCode == 201) {
                String resp = http.getString();
                LOG_I(TAG, "Vision Response: %s", resp.c_str());

                cJSON* respJson = cJSON_Parse(resp.c_str());
                if (respJson) {
                    cJSON* textItem = cJSON_GetObjectItem(respJson, "text");
                    if (!textItem) textItem = cJSON_GetObjectItem(respJson, "result");
                    if (!textItem) textItem = cJSON_GetObjectItem(respJson, "description");
                    if (textItem && cJSON_IsString(textItem)) {
                        resultText = textItem->valuestring;
                    } else {
                        resultText = resp;
                    }
                    cJSON_Delete(respJson);
                } else {
                    resultText = resp;
                }
            } else {
                LOG_W(TAG, "Vision Upload HTTP error: %d", httpCode);
                resultText = "Lỗi tải ảnh lên máy chủ phân tích thị giác.";
            }
            http.end();
        } else {
            LOG_E(TAG, "Khong the khoi tao HTTP POST Vision hoac het bo nho PSRAM!");
        }

        if (postBody) free(postBody);
        if (_visionFreeFunc != nullptr) {
            _visionFreeFunc(jpegBuf);
        } else {
            free(jpegBuf);
        }
    } else {
        LOG_E(TAG, "encodeJpeg hoac chup anh that bai!");
    }

    resultText.replace("\\", "\\\\");
    resultText.replace("\"", "\\\"");
    resultText.replace("\n", "\\n");
    resultText.replace("\r", "");

    String toolResp = "{\"session_id\":\"" + String(_sessionId.c_str()) + "\","
                      "\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":" + String(reqId) + ","
                      "\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + resultText + "\"}]}}}";

    _sendWsText(toolResp);
    LOG_I(TAG, "Da gui ket qua phan tich anh MCP tool call id=%d!", reqId);

    XiaozhiEvent evEnd;
    evEnd.type = XiaozhiEventType::CAMERA_CAPTURE_END;
    _pushEvent(evEnd);

    _isCapturingPhoto = false;
}
