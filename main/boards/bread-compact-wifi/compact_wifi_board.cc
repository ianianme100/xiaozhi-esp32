#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#include "lamp_B.h"
#include "lamp_G.h"
#include "lamp_R.h"
#include "ags10.h"
#include "dht11.h"
#include "sensor_display.h"
#include "ir_remote.h"

#include <esp_timer.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

class CompactWifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    i2c_master_bus_handle_t sensor_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    bool emergency_alarm_active_ = false;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSensorI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = GPIO_NUM_8,
            .scl_io_num = GPIO_NUM_9,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = { .enable_internal_pullup = 1 },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &sensor_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        touch_button_.OnClick([this]() {
            SetEmergencyAlarm(!emergency_alarm_active_, true, true);
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void SetEmergencyAlarm(bool active, bool show_notification = true, bool announce_help = false) {
        emergency_alarm_active_ = active;
        gpio_set_level(BUZZER_GPIO, active ? BUZZER_ACTIVE_LEVEL : BUZZER_INACTIVE_LEVEL);
        ESP_LOGW(TAG, "Emergency alarm %s", active ? "ON" : "OFF");
        if (show_notification && display_ != nullptr) {
            display_->ShowNotification(active ? "需要幫助" : "緊急呼叫已取消");
        }
        if (announce_help) {
            auto& app = Application::GetInstance();
            if (active) {
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.Schedule([this]() {
                    if (display_ != nullptr) {
                        display_->SetChatMessage("system", "需要幫助");
                        display_->ShowNotification("需要幫助", 10000);
                    }
                    Application::GetInstance().Alert("緊急呼叫", "需要幫助", "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                });
            } else {
                app.Schedule([this]() {
                    auto& app = Application::GetInstance();
                    auto state = app.GetDeviceState();
                    if (state == kDeviceStateSpeaking) {
                        app.AbortSpeaking(kAbortReasonNone);
                    } else if (state == kDeviceStateListening) {
                        app.StopListening();
                    }
                    app.SetDeviceState(kDeviceStateIdle);
                    app.GetAudioService().EnableVoiceProcessing(false);
                    app.GetAudioService().EnableWakeWordDetection(true);
                    // Force the audio channel state to reset so the next wake word
                    // goes through the normal reconnect path instead of being
                    // silently dropped (channel may look "open" after the abort).
                    app.CloseAudioChannel();
                    app.DismissAlert();
                    if (display_ != nullptr) {
                        display_->SetStatus(Lang::Strings::STANDBY);
                        display_->SetEmotion("neutral");
                        display_->SetChatMessage("system", "");
                        display_->ShowNotification("緊急呼叫已取消", 3000);
                    }
                });
            }
        }
    }

    void InitializeBuzzer() {
        gpio_config_t buzzer_config = {
            .pin_bit_mask = (1ULL << BUZZER_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&buzzer_config));
        SetEmergencyAlarm(false, false);
    }

    // 物联网初始化，逐步迁移到 MCP 协议
    void InitializeTools() {
        //static LampController lamp(LAMP_GPIO);
        // 1. 實例化你的三盞燈
        static Green_Lamp lamp_G(GPIO_NUM_17);
        static Red_Lamp lamp_R(GPIO_NUM_18);
        static Blue_Lamp lamp_B(GPIO_NUM_10);

        // 2. 獲取 MCP 伺服器
        auto& server = McpServer::GetInstance();

        // 3. 註冊「全部打開」巨集工具
        server.AddTool("所有灯.打开", "一次性打开红灯、绿灯和蓝灯", PropertyList(), [](const PropertyList&) {
        lamp_G.TurnOn();
        lamp_R.TurnOn();
        lamp_B.TurnOn();
        return true;
        });

        // 4. 註冊「全部關閉」巨集工具
        server.AddTool("所有灯.关闭", "一次性关闭红灯、绿灯和蓝灯", PropertyList(), [](const PropertyList&) {
        lamp_G.TurnOff();
        lamp_R.TurnOff();
        lamp_B.TurnOff();
        return true;
        });

        // 5. AGS10 空氣品質感測器
        static Ags10 ags10(display_i2c_bus_);

        // 6. DHT11 溫溼度感測器（GPIO2）
        static Dht11 dht11(DHT11_DATA_PIN);
        static float s_temperature = 0.0f;
        static float s_humidity    = 0.0f;
        static bool  s_dht_ok      = false;

        // 7. 第二塊 OLED 專門顯示感測器數據
        static SensorDisplay sensor_disp(sensor_i2c_bus_);

        // 語音查詢工具：空氣品質
        server.AddTool("空氣品質.讀取", "讀取目前室內 TVOC 空氣品質濃度（單位 ppb，數值越低越好）",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                int32_t tvoc = ags10.ReadTVOC();
                sensor_disp.ShowAll(tvoc, s_temperature, s_humidity, s_dht_ok);
                if (tvoc < 0) {
                    return std::string("空氣感測器正在預熱或讀取失敗，請稍後再試。");
                }
                std::string level;
                if (tvoc < 220)       level = "良好";
                else if (tvoc < 660)  level = "一般";
                else if (tvoc < 2200) level = "較差";
                else                  level = "非常差";
                return std::string("目前 TVOC 濃度為 ") + std::to_string(tvoc) + " ppb，空氣品質" + level + "。";
            });

        // 語音查詢工具：溫溼度
        server.AddTool("溫溼度.讀取", "讀取目前室內溫度（°C）與濕度（%）",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                float temp, humi;
                bool ok = dht11.Read(&temp, &humi);
                if (ok) {
                    s_temperature = temp;
                    s_humidity    = humi;
                    s_dht_ok      = true;
                    sensor_disp.ShowAll(ags10.ReadTVOC(), temp, humi, true);
                    char buf[80];
                    snprintf(buf, sizeof(buf), "目前室內溫度 %.1f °C，濕度 %.1f%%。", temp, humi);
                    return std::string(buf);
                } else {
                    s_dht_ok = false;
                    return std::string("溫溼度感測器讀取失敗，請確認接線。");
                }
            });

        // 定時每 2 秒自動更新 OLED（DHT11 最短間隔 1s，留餘裕用 2s）
        static esp_timer_handle_t sensor_timer;
        esp_timer_create_args_t timer_args = {
            .callback = [](void*) {
                int32_t tvoc = ags10.ReadTVOC();
                float temp, humi;
                bool ok = dht11.Read(&temp, &humi);
                if (ok) {
                    s_temperature = temp;
                    s_humidity    = humi;
                    s_dht_ok      = true;
                }
                sensor_disp.ShowAll(tvoc, s_temperature, s_humidity, s_dht_ok);
            },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "sensor_update",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&timer_args, &sensor_timer);
        esp_timer_start_periodic(sensor_timer, 2ULL * 1000 * 1000);  // 2s

        // 8. 紅外線遙控學習與發射
        static IrRemote ir_remote(IR_RX_GPIO, IR_TX_GPIO);

        server.AddTool("self.ir.learn_fan_power", "學習風扇電源鍵的紅外線訊號", PropertyList(), [](const PropertyList&) -> ReturnValue {
            return ir_remote.LearnFanPower();
        });
        server.AddTool("self.ir.learn_fan_speed", "學習風扇風速鍵的紅外線訊號", PropertyList(), [](const PropertyList&) -> ReturnValue {
            return ir_remote.LearnFanSpeed();
        });
        server.AddTool("self.ir.learn_fan_speed_down", "學習風扇降低風速鍵的紅外線訊號", PropertyList(), [](const PropertyList&) -> ReturnValue {
            return ir_remote.LearnFanSpeedDown();
        });
        server.AddTool("self.ir.learn_light_power", "學習燈具電源鍵的紅外線訊號", PropertyList(), [](const PropertyList&) -> ReturnValue {
            return ir_remote.LearnLightPower();
        });
        server.AddTool("self.ir.send_fan_power", "發送風扇電源鍵的紅外線訊號", PropertyList(), [](const PropertyList&) -> ReturnValue {
            return ir_remote.SendFanPower();
        });
        server.AddTool("self.ir.send_fan_speed", "發送風扇風速鍵的紅外線訊號", PropertyList(), [](const PropertyList&) -> ReturnValue {
            return ir_remote.SendFanSpeed();
        });
        server.AddTool("self.ir.send_fan_speed_down", "發送風扇降低風速鍵的紅外線訊號", PropertyList(), [](const PropertyList&) -> ReturnValue {
            return ir_remote.SendFanSpeedDown();
        });
        server.AddTool("self.ir.send_light_power", "發送燈具電源鍵的紅外線訊號", PropertyList(), [](const PropertyList&) -> ReturnValue {
            return ir_remote.SendLightPower();
        });
        server.AddTool("self.ir.get_status", "取得紅外線遙控各按鍵的學習狀態（JSON 格式）", PropertyList(), [](const PropertyList&) -> ReturnValue {
            return ir_remote.GetStatus();
        });

        // 9. 緊急呼叫工具
        server.AddTool("self.nurse_call.trigger", "觸發緊急呼叫警報並通知需要幫助", PropertyList(), [this](const PropertyList&) -> ReturnValue {
            SetEmergencyAlarm(true, true, true);
            return true;
        });
        server.AddTool("self.nurse_call.cancel", "取消目前的緊急呼叫警報", PropertyList(), [this](const PropertyList&) -> ReturnValue {
            SetEmergencyAlarm(false, true, true);
            return true;
        });
        server.AddTool("self.nurse_call.get_status", "查詢目前緊急呼叫警報是否正在響", PropertyList(), [this](const PropertyList&) -> ReturnValue {
            return std::string("{\"alarm_active\":") + (emergency_alarm_active_ ? "true" : "false") + "}";
        });
    }

public:
    CompactWifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSensorI2c();
        InitializeSsd1306Display();
        InitializeBuzzer();
        InitializeButtons();
        InitializeTools();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CompactWifiBoard);
