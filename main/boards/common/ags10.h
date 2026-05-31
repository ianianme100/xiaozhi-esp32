#pragma once
#include <driver/i2c_master.h>
#include <cstdint>

class Ags10 {
public:
    Ags10(i2c_master_bus_handle_t bus);
    // Returns TVOC concentration in ppb, or -1 on error / warmup
    int32_t ReadTVOC();

private:
    i2c_master_dev_handle_t dev_;
    uint8_t CRC8(const uint8_t* data, size_t len);
};
