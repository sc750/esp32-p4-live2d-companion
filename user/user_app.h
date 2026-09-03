/**
 * @file    user_app.h
 * @brief   用户应用入口接口
 *
 * 这是整个应用的"总入口"。
 * main.c 只调用 user_app_run()，所有初始化和主循环都在这里面。
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

#ifndef USER_APP_H
#define USER_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 运行用户应用
 *
 * 执行顺序：
 *   1. BSP 初始化（硬件外设）
 *   2. 核心服务初始化（事件总线、内存管理器、配置管理）
 *   3. 状态机初始化
 *   4. UI 初始化（LVGL 图形库 + 页面创建）
 *   5. 进入主循环（每秒检查一次内存使用情况）
 */
void user_app_run(void);

#ifdef __cplusplus
}
#endif

#endif /* USER_APP_H */
