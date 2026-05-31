#pragma once
#include <driver/i2c_master.h>
#include <cstdint>

// Lightweight SSD1306 driver for a dedicated sensor data display.
// Bypasses LVGL entirely — renders text directly via I2C in page mode.
class SensorDisplay {
public:
    SensorDisplay(i2c_master_bus_handle_t bus, uint8_t i2c_addr = 0x3C);
    void ShowAirQuality(int32_t tvoc_ppb);  // tvoc_ppb < 0 → warming up
    void ShowAll(int32_t tvoc_ppb, float temperature, float humidity, bool dht_ok);

private:
    i2c_master_dev_handle_t dev_;
    uint8_t pages_[8][128];  // 128 cols × 8 pages (64 rows)

    void SendCommands(const uint8_t* cmds, size_t len);
    void Initialize();
    void Clear();
    void PutChar(int col, int page, char c);
    void PutStr(int col, int page, const char* str);
    void Flush();
};
