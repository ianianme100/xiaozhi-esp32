#pragma once

#include <driver/gpio.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>
#include <freertos/FreeRTOS.h>

#include <array>
#include <string>

class IrRemote {
public:
    IrRemote(gpio_num_t rx_gpio, gpio_num_t tx_gpio);
    ~IrRemote();

    std::string LearnFanPower();
    std::string LearnFanSpeed();
    std::string LearnFanSpeedDown();
    std::string LearnLightPower();

    std::string SendFanPower();
    std::string SendFanSpeed();
    std::string SendFanSpeedDown();
    std::string SendLightPower();

    std::string GetStatus() const;

private:
    static constexpr size_t kMaxSymbols = 256;
    static constexpr uint32_t kResolutionHz = 1000000;
    static constexpr uint32_t kLearnTimeoutMs = 10000;
    static constexpr uint32_t kCaptureIdleUs = 45000;
    static constexpr uint32_t kNoiseFilterUs = 60;
    static constexpr uint32_t kMaxDurationUs = 32767;
    static constexpr uint32_t kStorageMagic = 0x49524331;
    static constexpr uint32_t kStorageVersion = 1;

    struct LearnedCode {
        std::array<rmt_symbol_word_t, kMaxSymbols> symbols {};
        size_t count = 0;
        bool learned = false;
    };

    struct StoredCode {
        uint32_t magic = kStorageMagic;
        uint32_t version = kStorageVersion;
        uint32_t count = 0;
        std::array<rmt_symbol_word_t, kMaxSymbols> symbols {};
    };

    gpio_num_t rx_gpio_;
    gpio_num_t tx_gpio_;
    rmt_channel_handle_t tx_channel_ = nullptr;
    rmt_encoder_handle_t copy_encoder_ = nullptr;
    bool initialized_ = false;

    std::array<rmt_symbol_word_t, kMaxSymbols> capture_symbols_ {};

    LearnedCode fan_power_;
    LearnedCode fan_speed_;
    LearnedCode fan_speed_down_;
    LearnedCode light_power_;

    void Initialize();
    void LoadStoredCodes();
    bool LoadStoredCode(const char* key, LearnedCode& code);
    bool SaveStoredCode(const char* key, const LearnedCode& code);
    std::string Learn(LearnedCode& code, const char* label, const char* storage_key);
    std::string Send(const LearnedCode& code, const char* label);
};
