#ifndef IDIOM_PROTOCOL_H
#define IDIOM_PROTOCOL_H


#include "protocol.h"
#include <mqtt.h>
#include <udp.h>
#include <cJSON.h>
#include <mbedtls/aes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <functional>
#include <string>
#include <map>
#include <mutex>

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 10000

#define IDIOM_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class IdiomProtocol : public Protocol {
public:
    IdiomProtocol();
    ~IdiomProtocol();

    void Start() override;
    void SendAudio(const std::vector<uint8_t>& data) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;

    std::string endpoint_;
    std::string client_id_;
    std::string username_;
    std::string password_;
    std::string publish_topic_;
    std::string subscribe_topic_;
    int mqtt_port_= 0;

    std::mutex channel_mutex_;
    Mqtt* mqtt_ = nullptr;
    Udp* udp_ = nullptr;
    mbedtls_aes_context aes_ctx_;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;

    bool StartIdiomClient(bool report_error=false);
    void ParseServerHello(const cJSON* root);
    std::string DecodeHexString(const std::string& hex_string);
    void ConvertTextToSpeech(const std::string& text);

public:
    void SendText(const std::string& text) override;
};


#endif // IDIOM_PROTOCOL_H
