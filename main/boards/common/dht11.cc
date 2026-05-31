#include "dht11.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rom/ets_sys.h>

#define TAG "DHT11"

Dht11::Dht11(gpio_num_t pin) : pin_(pin) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin_, 1);
}

int Dht11::WaitLevel(int expected_level, int timeout_us) {
    int elapsed = 0;
    while (gpio_get_level(pin_) != expected_level) {
        if (elapsed >= timeout_us) return -1;
        ets_delay_us(1);
        elapsed++;
    }
    return elapsed;
}

bool Dht11::Read(float* temperature, float* humidity) {
    uint8_t data[5] = {};

    // Send start signal: pull low 18ms then release
    gpio_set_direction(pin_, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin_, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(pin_, 1);
    ets_delay_us(30);
    gpio_set_direction(pin_, GPIO_MODE_INPUT);

    // Wait for DHT11 response: ~80us low then ~80us high
    if (WaitLevel(0, 100) < 0) { ESP_LOGE(TAG, "No response low");  return false; }
    if (WaitLevel(1, 100) < 0) { ESP_LOGE(TAG, "No response high"); return false; }
    if (WaitLevel(0, 100) < 0) { ESP_LOGE(TAG, "No data start");    return false; }

    // Read 40 bits
    for (int i = 0; i < 40; i++) {
        // Each bit: 50us low then 26-28us (0) or 70us (1) high
        if (WaitLevel(1, 100) < 0) return false;
        ets_delay_us(40);  // Sample at 40us: still high → '1', already low → '0'
        data[i / 8] = (data[i / 8] << 1) | gpio_get_level(pin_);
        if (WaitLevel(0, 100) < 0) return false;
    }

    // Verify checksum
    if (data[4] != (uint8_t)(data[0] + data[1] + data[2] + data[3])) {
        ESP_LOGE(TAG, "CRC error: %02X != %02X", data[4],
                 (uint8_t)(data[0] + data[1] + data[2] + data[3]));
        return false;
    }

    *humidity    = data[0] + data[1] * 0.1f;
    *temperature = data[2] + data[3] * 0.1f;
    return true;
}
