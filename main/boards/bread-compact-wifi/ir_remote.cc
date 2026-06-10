#include "ir_remote.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <nvs.h>

#include <algorithm>
#include <cstring>

static const char* TAG = "IrRemote";
static const char* kIrStorageNamespace = "ir_remote";

IrRemote::IrRemote(gpio_num_t rx_gpio, gpio_num_t tx_gpio)
    : rx_gpio_(rx_gpio), tx_gpio_(tx_gpio) {
    Initialize();
}

IrRemote::~IrRemote() {
    if (tx_channel_) {
        rmt_disable(tx_channel_);
        rmt_del_channel(tx_channel_);
    }
    if (copy_encoder_) {
        rmt_del_encoder(copy_encoder_);
    }
}

void IrRemote::Initialize() {
    if (initialized_) {
        return;
    }

    gpio_config_t rx_config = {};
    rx_config.pin_bit_mask = (1ULL << rx_gpio_);
    rx_config.mode = GPIO_MODE_INPUT;
    rx_config.pull_up_en = GPIO_PULLUP_ENABLE;
    rx_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    rx_config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&rx_config));

    rmt_tx_channel_config_t tx_config = {};
    tx_config.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_config.resolution_hz = kResolutionHz;
    tx_config.mem_block_symbols = 64;
    tx_config.trans_queue_depth = 4;
    tx_config.gpio_num = tx_gpio_;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_config, &tx_channel_));

    rmt_carrier_config_t carrier_config = {};
    carrier_config.frequency_hz = 38000;
    carrier_config.duty_cycle = 0.33f;
    carrier_config.flags.polarity_active_low = false;
    ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel_, &carrier_config));
    ESP_ERROR_CHECK(rmt_enable(tx_channel_));

    rmt_copy_encoder_config_t encoder_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&encoder_config, &copy_encoder_));

    LoadStoredCodes();
    initialized_ = true;
    ESP_LOGI(TAG, "IR remote initialized: RX GPIO %d, TX GPIO %d", rx_gpio_, tx_gpio_);
}

void IrRemote::LoadStoredCodes() {
    LoadStoredCode("ac_open", ac_open_);
    LoadStoredCode("ac_close", ac_close_);
    LoadStoredCode("tv_open", tv_open_);
    LoadStoredCode("tv_close", tv_close_);
    LoadStoredCode("fan_open", fan_open_);
    LoadStoredCode("fan_close", fan_close_);
    LoadStoredCode("fan_speed", fan_speed_);
    LoadStoredCode("fan_speed_down", fan_speed_down_);
    LoadStoredCode("light_power", light_power_);
}

bool IrRemote::LoadStoredCode(const char* key, LearnedCode& code) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kIrStorageNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open IR storage for %s: %s", key, esp_err_to_name(err));
        return false;
    }

    StoredCode stored {};
    size_t size = sizeof(stored);
    err = nvs_get_blob(handle, key, &stored, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK || size != sizeof(stored)) {
        ESP_LOGW(TAG, "Failed to load IR code %s: %s", key, esp_err_to_name(err));
        return false;
    }
    if (stored.magic != kStorageMagic || stored.version != kStorageVersion || stored.count == 0 || stored.count > kMaxSymbols) {
        ESP_LOGW(TAG, "Ignoring invalid IR code %s", key);
        return false;
    }

    std::copy(stored.symbols.begin(), stored.symbols.begin() + stored.count, code.symbols.begin());
    code.count = stored.count;
    code.learned = true;
    ESP_LOGI(TAG, "Loaded IR code %s: %u symbols", key, static_cast<unsigned>(stored.count));
    return true;
}

bool IrRemote::SaveStoredCode(const char* key, const LearnedCode& code) {
    if (!code.learned || code.count == 0 || code.count > kMaxSymbols) {
        return false;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kIrStorageNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open IR storage for writing %s: %s", key, esp_err_to_name(err));
        return false;
    }

    StoredCode stored {};
    stored.count = code.count;
    std::copy(code.symbols.begin(), code.symbols.begin() + code.count, stored.symbols.begin());

    err = nvs_set_blob(handle, key, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save IR code %s: %s", key, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Saved IR code %s: %u symbols", key, static_cast<unsigned>(code.count));
    return true;
}

std::string IrRemote::Learn(LearnedCode& code, const char* label, const char* storage_key) {
    Initialize();
    std::memset(capture_symbols_.data(), 0, capture_symbols_.size() * sizeof(rmt_symbol_word_t));

    uint32_t edges = 0;
    int last_level = gpio_get_level(rx_gpio_);
    int64_t last_change_us = esp_timer_get_time();
    int64_t last_yield_us = last_change_us;
    const int64_t start_us = last_change_us;
    bool started = false;

    auto append_interval = [&](int interval_level, int64_t duration_us) {
        if (duration_us < kNoiseFilterUs) {
            return;
        }
        const size_t symbol_index = edges / 2;
        if (symbol_index >= kMaxSymbols) {
            return;
        }

        const uint32_t duration = duration_us > kMaxDurationUs ? kMaxDurationUs : static_cast<uint32_t>(duration_us);
        const uint32_t tx_level = interval_level ? 0 : 1;
        rmt_symbol_word_t& symbol = capture_symbols_[symbol_index];
        if ((edges % 2) == 0) {
            symbol.level0 = tx_level;
            symbol.duration0 = duration;
            symbol.level1 = 0;
            symbol.duration1 = 0;
        } else {
            symbol.level1 = tx_level;
            symbol.duration1 = duration;
        }
        ++edges;
        started = true;
    };

    ESP_LOGI(TAG, "Polling IR receiver on GPIO %d for %s, idle level=%d", rx_gpio_, label, last_level);
    while ((esp_timer_get_time() - start_us) < static_cast<int64_t>(kLearnTimeoutMs) * 1000) {
        const int64_t now_us = esp_timer_get_time();
        const int level = gpio_get_level(rx_gpio_);

        if (level != last_level) {
            const int64_t duration_us = now_us - last_change_us;
            if (duration_us > kCaptureIdleUs) {
                if (started && edges >= 12) {
                    break;
                }
                edges = 0;
                started = false;
                std::memset(capture_symbols_.data(), 0, capture_symbols_.size() * sizeof(rmt_symbol_word_t));
            } else {
                append_interval(last_level, duration_us);
            }
            last_level = level;
            last_change_us = now_us;
        }

        if (started && edges >= 12 && (now_us - last_change_us) > kCaptureIdleUs) {
            break;
        }

        if ((now_us - last_yield_us) > 2000) {
            taskYIELD();
            last_yield_us = now_us;
        }
    }

    if (edges < 12) {
        const int final_level = gpio_get_level(rx_gpio_);
        ESP_LOGW(TAG, "Learning %s timed out, captured %u edges, final GPIO level=%d", label, static_cast<unsigned>(edges), final_level);
        return std::string("Learning ") + label + " timed out. GPIO " +
            std::to_string(static_cast<int>(rx_gpio_)) + " captured " + std::to_string(edges) +
            " edges; check OUT wiring, common GND, and try another RX GPIO.";
    }

    size_t count = (edges + 1) / 2;
    if (count > kMaxSymbols) {
        count = kMaxSymbols;
    }

    std::copy(capture_symbols_.begin(), capture_symbols_.begin() + count, code.symbols.begin());
    if (code.symbols[count - 1].duration1 == 0) {
        code.symbols[count - 1].level1 = 0;
        code.symbols[count - 1].duration1 = 1000;
    }
    code.count = count;
    code.learned = true;

    const bool saved = SaveStoredCode(storage_key, code);
    ESP_LOGI(TAG, "Learned %s: %u symbols from %u edges", label, static_cast<unsigned>(count), static_cast<unsigned>(edges));
    return std::string("Learned ") + label + " with " + std::to_string(count) +
        " IR symbols" + (saved ? " and saved it." : ", but saving failed.");
}

std::string IrRemote::Send(const LearnedCode& code, const char* label) {
    Initialize();
    if (!code.learned || code.count == 0) {
        return std::string(label) + " has not been learned yet.";
    }

    rmt_transmit_config_t tx_config = {};
    tx_config.loop_count = 0;

    esp_err_t err = rmt_transmit(
        tx_channel_,
        copy_encoder_,
        code.symbols.data(),
        code.count * sizeof(rmt_symbol_word_t),
        &tx_config
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to transmit %s: %s", label, esp_err_to_name(err));
        return std::string("Failed to send ") + label + ": " + esp_err_to_name(err);
    }

    err = rmt_tx_wait_all_done(tx_channel_, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        return std::string("IR send timeout for ") + label + ".";
    }

    ESP_LOGI(TAG, "Sent %s", label);
    return std::string("Sent ") + label + ".";
}

std::string IrRemote::LearnAcOpen() {
    return Learn(ac_open_, "ac open", "ac_open");
}

std::string IrRemote::LearnAcClose() {
    return Learn(ac_close_, "ac close", "ac_close");
}

std::string IrRemote::LearnTvOpen() {
    return Learn(tv_open_, "tv open", "tv_open");
}

std::string IrRemote::LearnTvClose() {
    return Learn(tv_close_, "tv close", "tv_close");
}

std::string IrRemote::LearnFanOpen() {
    return Learn(fan_open_, "fan open", "fan_open");
}

std::string IrRemote::LearnFanClose() {
    return Learn(fan_close_, "fan close", "fan_close");
}

std::string IrRemote::LearnFanSpeed() {
    return Learn(fan_speed_, "fan speed", "fan_speed");
}

std::string IrRemote::LearnFanSpeedDown() {
    return Learn(fan_speed_down_, "fan speed down", "fan_speed_down");
}

std::string IrRemote::LearnLightPower() {
    return Learn(light_power_, "light power", "light_power");
}

std::string IrRemote::SendAcOpen() {
    return Send(ac_open_, "ac open");
}

std::string IrRemote::SendAcClose() {
    return Send(ac_close_, "ac close");
}

std::string IrRemote::SendTvOpen() {
    return Send(tv_open_, "tv open");
}

std::string IrRemote::SendTvClose() {
    return Send(tv_close_, "tv close");
}

std::string IrRemote::SendFanOpen() {
    return Send(fan_open_, "fan open");
}

std::string IrRemote::SendFanClose() {
    return Send(fan_close_, "fan close");
}

std::string IrRemote::SendFanSpeed() {
    return Send(fan_speed_, "fan speed");
}

std::string IrRemote::SendFanSpeedDown() {
    return Send(fan_speed_down_, "fan speed down");
}

std::string IrRemote::SendLightPower() {
    return Send(light_power_, "light power");
}

std::string IrRemote::GetStatus() const {
    return std::string("{\"ac_open\":") + (ac_open_.learned ? "true" : "false") +
        ",\"ac_close\":" + (ac_close_.learned ? "true" : "false") +
        ",\"tv_open\":" + (tv_open_.learned ? "true" : "false") +
        ",\"tv_close\":" + (tv_close_.learned ? "true" : "false") +
        ",\"fan_open\":" + (fan_open_.learned ? "true" : "false") +
        ",\"fan_close\":" + (fan_close_.learned ? "true" : "false") +
        ",\"fan_speed\":" + (fan_speed_.learned ? "true" : "false") +
        ",\"fan_speed_down\":" + (fan_speed_down_.learned ? "true" : "false") +
        ",\"light_power\":" + (light_power_.learned ? "true" : "false") + "}";
}
