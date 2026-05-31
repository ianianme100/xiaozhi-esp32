#pragma once
#include <driver/gpio.h>

class Dht11 {
public:
    explicit Dht11(gpio_num_t pin);
    // Returns true on success. temperature in °C, humidity in %.
    bool Read(float* temperature, float* humidity);

private:
    gpio_num_t pin_;
    int WaitLevel(int expected_level, int timeout_us);
};
