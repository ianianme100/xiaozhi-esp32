#include "mcp_server.h"
#include <esp_log.h>

#define TAG "紅燈事件："

class Red_Lamp  {          // 1. 類名改成 Red_Lamp 
private:
    bool power_ = false;
    gpio_num_t gpio_num_;

public:
    explicit Red_Lamp (gpio_num_t gpio_num) : gpio_num_(gpio_num) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(gpio_num_, 0);       //初始化為低電平

        /* 2. 把 MCP 工具名改成紅燈相關 */
        
        auto& server = McpServer::GetInstance();
        server.AddTool("紅燈.獲取開關狀態", "返回紅燈的開/關狀態",      // 工具名稱   , 工具描述    
                       PropertyList(), [this](const PropertyList&) {

                           ESP_LOGW(TAG, "獲取到了紅燈的當前狀態，當前狀態為%s", power_ ? "開" : "關");     //日誌記錄
                           return power_ ? "{\"燈光狀態：\":紅燈是開著的！}" : "{\"燈光狀態：\":紅燈是關著的！}";       //返回狀態
                       
                       });
   
        server.AddTool("紅燈.打開", "打開紅燈",     // 工具名稱   , 工具描述
                       PropertyList(), [this](const PropertyList&) {
                           power_ = true;
                           gpio_set_level(gpio_num_, 1);    //設置為高電平
                           ESP_LOGW(TAG, "已打開紅燈！");   //日誌記錄
                           return true;     //返回告訴小智執行成功！
                       });

        server.AddTool("紅燈.關閉", "關閉紅燈",     // 工具名稱   , 工具描述
                       PropertyList(), [this](const PropertyList&) {
                           power_ = false;
                           gpio_set_level(gpio_num_, 0);    //設置為低電平
                           ESP_LOGW(TAG, "已關閉紅燈！");    //日誌記錄
                           return true;     //返回告訴小智執行成功！
                       });
    }

    void TurnOn() {
        power_ = true;  // 確保狀態同步
        gpio_set_level(gpio_num_, 1);
        ESP_LOGW(TAG, "情境模式：已被同步打開！");
    }

    void TurnOff() {
        power_ = false; // 確保狀態同步
        gpio_set_level(gpio_num_, 0);
        ESP_LOGW(TAG, "情境模式：已被同步關閉！");
    }
};