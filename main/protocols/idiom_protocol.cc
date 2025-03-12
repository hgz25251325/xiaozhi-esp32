#include "idiom_protocol.h"
#include "board.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>
#include <ml307_mqtt.h>
#include <ml307_udp.h>
#include <cstring>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "IDIOM"

IdiomProtocol::IdiomProtocol() {
    event_group_handle_ = xEventGroupCreate();
    if (event_group_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create event group");
    } else {
        ESP_LOGI(TAG, "Event group created successfully");
    }
}

IdiomProtocol::~IdiomProtocol() {
    ESP_LOGI(TAG, "IdiomProtocol deinit");
    if (udp_ != nullptr) {
        delete udp_;
    }
    if (mqtt_ != nullptr) {
        delete mqtt_;
    }
    vEventGroupDelete(event_group_handle_);
}

void IdiomProtocol::Start() {
    StartIdiomClient(false);
}

bool IdiomProtocol::StartIdiomClient(bool report_error) {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Idiom client already started");
        delete mqtt_;
    }

    // Settings settings("idiom", false);
    // endpoint_ = settings.GetString("endpoint");
    // client_id_ = settings.GetString("client_id");
    // username_ = settings.GetString("username");
    // password_ = settings.GetString("password");
    endpoint_ = "192.168.99.2";
    client_id_ = "";
    username_ = "";
    password_ = "";
    mqtt_port_ = 1883;
    publish_topic_ = "idiom_topic_pc";//settings.GetString("idiom_topic");
    subscribe_topic_ = "idiom_topic_esp32";

    if (endpoint_.empty()) {
        ESP_LOGW(TAG, "Idiom endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    mqtt_ = Board::GetInstance().CreateMqtt();
    mqtt_->SetKeepAlive(90);

    mqtt_->OnConnected([this]() {
        ESP_LOGI(TAG, "Connected to endpoint");
        mqtt_->Subscribe(subscribe_topic_,0); //最多交付一次，不保证消息一定到达订阅者。
        mqtt_->Subscribe(subscribe_topic_+"/audio",0); //最多交付一次，不保证消息一定到达订阅者。

    });
    mqtt_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Disconnected from endpoint");
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        if(topic == subscribe_topic_+"/audio")
        {
            // 处理音频数据
            ESP_LOGE(TAG, "Received audio data");
            std::vector<uint8_t> audio_data(payload.begin(), payload.end());
            auto& app = Application::GetInstance();
            Protocol* protocol_ = app.GetMqttProtocol();
            if(protocol_ != nullptr)
            {
                protocol_->SendAudio(audio_data);
            } 
        }
        else
        {
            cJSON* root = cJSON_Parse(payload.c_str());
            if (root == nullptr) {
                ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
                return;
            }
            cJSON* type = cJSON_GetObjectItem(root, "type");
            if (type == nullptr) {
                ESP_LOGE(TAG, "Message type is not specified");
                cJSON_Delete(root);
                return;
            }

            if (strcmp(type->valuestring, "hello") == 0) {
                ParseServerHello(root);
            } else if (strcmp(type->valuestring, "goodbye") == 0) {
                auto session_id = cJSON_GetObjectItem(root, "session_id");
                ESP_LOGI(TAG, "Received goodbye message, session_id: %s", session_id ? session_id->valuestring : "null");
                if (session_id == nullptr || session_id_ == session_id->valuestring) {
                    Application::GetInstance().Schedule([this]() {
                        CloseAudioChannel();
                    });
                }
            } 
            else if(strcmp(type->valuestring, "image_analysis"))    //
            {   
                // # 发布 OCR 结果到 MQTT 主题
                // mqtt_payload = {
                //     "type": "image_analysis",
                //     "timestamp": datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
                //     "ocr_type": ocr_type,
                //     "text": text.strip(),
                //     "success": True,
                //     "idiom": FindIdiom.strip()
                // }
                cJSON* success_ = cJSON_GetObjectItem(root, "success");
                cJSON* idiom_ = cJSON_GetObjectItem(root, "idiom");
                if(success_->type == cJSON_True && idiom_ != nullptr)
                {
                    std::string text_str = idiom_->valuestring;
                    auto& app = Application::GetInstance();
                    Protocol* protocol_ = app.GetMqttProtocol();
                    if(protocol_ != nullptr)
                    {
                        protocol_->SendText(text_str);
                    }
                    //ConvertTextToSpeech(text_str);
                }
                

            }
            else if (on_incoming_json_ != nullptr) {
                on_incoming_json_(root);
            }
            cJSON_Delete(root);
        }
        
        
        
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ESP_LOGI(TAG, "Connecting to endpoint %s", endpoint_.c_str());
    if (!mqtt_->Connect(endpoint_, mqtt_port_, client_id_, username_, password_)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }    

    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

void IdiomProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty()) {
        return;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
    }
}

void IdiomProtocol::SendAudio(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr) {
        return;
    }

    std::string nonce(aes_nonce_);
    *(uint16_t*)&nonce[2] = htons(data.size());
    *(uint32_t*)&nonce[12] = htonl(++local_sequence_);

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + data.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&aes_ctx_, data.size(), &nc_off, (uint8_t*)nonce.c_str(), stream_block,
        (uint8_t*)data.data(), (uint8_t*)&encrypted[nonce.size()]) != 0) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return;
    }
    udp_->Send(encrypted);
}

void IdiomProtocol::CloseAudioChannel() {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (udp_ != nullptr) {
            delete udp_;
            udp_ = nullptr;
        }
    }

    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";
    message += "\"type\":\"goodbye\"";
    message += "}";
    SendText(message);

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool IdiomProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "Idiom is not connected, try to connect now");
        if (!StartIdiomClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, IDIOM_PROTOCOL_SERVER_HELLO_EVENT);

    // 发送 hello 消息申请 UDP 通道
    std::string message = "{";
    message += "\"type\":\"hello\",";
    message += "\"version\": 3,";
    message += "\"transport\":\"udp\",";
    message += "\"audio_params\":{";
    message += "\"format\":\"opus\", \"sample_rate\":16000, \"channels\":1, \"frame_duration\":" + std::to_string(OPUS_FRAME_DURATION_MS);
    message += "}}";
    SendText(message);

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, IDIOM_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & IDIOM_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ != nullptr) {
        delete udp_;
    }
    udp_ = Board::GetInstance().CreateUdp();
    udp_->OnMessage([this](const std::string& data) {
        if (data.size() < sizeof(aes_nonce_)) {
            ESP_LOGE(TAG, "Invalid audio packet size: %zu", data.size());
            return;
        }
        if (data[0] != 0x01) {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", data[0]);
            return;
        }
        uint32_t sequence = ntohl(*(uint32_t*)&data[12]);
        if (sequence < remote_sequence_) {
            ESP_LOGW(TAG, "Received audio packet with old sequence: %lu, expected: %lu", sequence, remote_sequence_);
            return;
        }
        if (sequence != remote_sequence_ + 1) {
            ESP_LOGW(TAG, "Received audio packet with wrong sequence: %lu, expected: %lu", sequence, remote_sequence_ + 1);
        }

        std::vector<uint8_t> decrypted;
        size_t decrypted_size = data.size() - aes_nonce_.size();
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        decrypted.resize(decrypted_size);
        auto nonce = (uint8_t*)data.data();
        auto encrypted = (uint8_t*)data.data() + aes_nonce_.size();
        int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, decrypted_size, &nc_off, nonce, stream_block, encrypted, (uint8_t*)decrypted.data());
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to decrypt audio data, ret: %d", ret);
            return;
        }
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(decrypted));
        }
        remote_sequence_ = sequence;
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    udp_->Connect(udp_server_, udp_port_);

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void IdiomProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (session_id != nullptr) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // Get sample rate from hello message
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (audio_params != NULL) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (sample_rate != NULL) {
            server_sample_rate_ = sample_rate->valueint;
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (udp == nullptr) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;
    udp_port_ = cJSON_GetObjectItem(udp, "port")->valueint;
    auto key = cJSON_GetObjectItem(udp, "key")->valuestring;
    auto nonce = cJSON_GetObjectItem(udp, "nonce")->valuestring;

    // auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    // ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_, encryption);
    aes_nonce_ = DecodeHexString(nonce);
    mbedtls_aes_init(&aes_ctx_);
    mbedtls_aes_setkey_enc(&aes_ctx_, (const unsigned char*)DecodeHexString(key).c_str(), 128);
    local_sequence_ = 0;
    remote_sequence_ = 0;
    xEventGroupSetBits(event_group_handle_, IDIOM_PROTOCOL_SERVER_HELLO_EVENT);
}

// void IdiomProtocol::ConvertTextToSpeech(const std::string& text) {
//     std::vector<uint8_t> audio_data;
//     if (tts_library::TextToSpeech(text, audio_data)) {
//         auto& app = Application::GetInstance();
//         Protocol* protocol_ = app.GetMqttProtocol();
//         if(protocol_ != nullptr)
//         {
//             protocol_->SendAudio(audio_data);
//         }        
        
//     } else {
//         ESP_LOGE(TAG, "Failed to convert text to speech");
//     }
// }

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

std::string IdiomProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

bool IdiomProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}