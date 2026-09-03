/**
 * @file    main.c
 * @brief   ESP32-P4 Live2D 桌面 AI 陪伴系统 — 应用入口
 *
 * 这是整个固件的最外层入口，由 ESP-IDF 的 FreeRTOS 调用。
 * 它只做一件事：调用 user_app_run()，把控制权交给用户应用。
 *
 * 这种设计的好处：
 *   - main.c 只有 5 行代码，不会变成"屎山"
 *   - 所有业务逻辑都在 user/ 组件中，职责清晰
 *   - 未来如果要换框架，只需要改 main.c
 *
 * @date    2026-09-01
 * @version 1.0.0
 * @hw      ESP32-P4-Function-EV-Board
 */

/* 包含用户应用入口头文件 */
#include "user_app.h"

/**
 * @brief ESP-IDF 应用入口
 *
 * 这是 FreeRTOS 启动后调用的第一个函数。
 * 它的签名是固定的：void app_main(void)
 */
void app_main(void)
{
    /* 调用用户应用的主入口，所有初始化和主循环都在里面 */
    user_app_run();
}
