#include "ags10.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "AGS10"
#define AGS10_ADDR 0x1A

Ags10::Ags10(i2c_master_bus_handle_t bus) {
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AGS10_ADDR,
        .scl_speed_hz = 30 * 1000,  // AGS10 max clock is 30kHz
        .scl_wait_us = 0,
        .flags = { .disable_ack_check = 0 },
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, &dev_));
}

int32_t Ags10::ReadTVOC() {
    uint8_t cmd = 0x00;
    if (i2c_master_transmit(dev_, &cmd, 1, 100) != ESP_OK) {
        ESP_LOGE(TAG, "Write command failed");
        return -1;
    }
    // AGS10 needs 30ms to prepare measurement data
    vTaskDelay(pdMS_TO_TICKS(30));

    uint8_t buf[5] = {};
    if (i2c_master_receive(dev_, buf, 5, 100) != ESP_OK) {
        ESP_LOGE(TAG, "Read failed");
        return -1;
    }

    if (CRC8(buf, 4) != buf[4]) {
        ESP_LOGE(TAG, "CRC mismatch: got 0x%02X expected 0x%02X", buf[4], CRC8(buf, 4));
        return -1;
    }

    if (buf[0] & 0x01) {
        ESP_LOGW(TAG, "Sensor is warming up");
        return -1;
    }

    return (int32_t)((buf[1] << 8) | buf[2]);
}

uint8_t Ags10::CRC8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
        }
    }
    return crc;
}
