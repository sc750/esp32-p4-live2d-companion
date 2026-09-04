/**
 * @file    main_app.c
 * @brief   用户应用入口实现
 *
 * Phase 1: 验证显示通路正常
 *
 * @date    2026-09-02
 * @version 1.0.0
 */

#include "user_app.h"

#include <stdio.h>
#include "bsp_init.h"
#include "event_bus.h"
#include "memory_manager.h"
#include "app_config.h"
#include "app_state_machine.h"
#include "ui_manager.h"
#include "rig_model.h"
#include "rig_lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

void user_app_run(void)
{
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "  ESP32-P4 Live2D AI Companion");
    ESP_LOGI(TAG, "  Phase 2: 渲染管线");
    ESP_LOGI(TAG, "==========================================");

    /* 1. BSP 初始化 */
    ESP_ERROR_CHECK(bsp_init_all());

    /* 2. 核心服务初始化 */
    ESP_ERROR_CHECK(event_bus_init());
    ESP_ERROR_CHECK(mem_manager_init());
    ESP_ERROR_CHECK(app_config_init());

    /* 3. 状态机初始化 */
    ESP_ERROR_CHECK(app_state_machine_init());

    /* 4. UI 初始化 */
    ESP_ERROR_CHECK(ui_manager_init());

    /* 5. 角色模型加载 + 静态渲染上屏（M03 R3） */
    static rig_model_t s_model;
    if (rig_model_load_default(&s_model) == ESP_OK) {
        rig_lvgl_create(lv_layer_top(), &s_model, 600);
    } else {
        ESP_LOGW(TAG, "角色模型加载失败，继续启动");
    }

    /* 6. 内存报告 */
    mem_print_report();

    ESP_LOGI(TAG, "系统就绪！进入主循环...");

    /* 主循环 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* 每 60 秒打印内存报告 */
        static int tick_count = 0;
        tick_count++;
        if (tick_count >= 60) {
            tick_count = 0;
            mem_print_report();
        }
    }
}
