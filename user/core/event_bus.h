/**
 * @file    event_bus.h
 * @brief   发布-订阅事件总线
 *
 * 事件总线是整个系统的"通信枢纽"。
 * 所有模块之间不直接调用对方的函数，而是通过事件总线发送/接收消息。
 *
 * 工作原理：
 *   1. 模块 A 调用 event_bus_subscribe() 订阅感兴趣的事件
 *   2. 模块 B 调用 event_bus_post() 发送一个事件
 *   3. 事件总线自动把事件通知给所有订阅了这个事件的模块
 *
 * 好处：模块之间完全解耦，A 不需要知道 B 的存在，B 也不需要知道 A 的存在。
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdbool.h>
#include "esp_err.h"
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 最大同时订阅者数量
 * 系统中最多允许 32 个模块同时订阅事件。
 * 超过这个数量会返回 ESP_ERR_NO_MEM 错误。
 */
#define EVENT_BUS_MAX_SUBSCRIBERS  32

/**
 * @brief 事件处理回调函数类型
 *
 * 当事件发生时，事件总线会调用这个函数来通知你。
 * 你可以把这个函数理解为"事件来了就执行这段代码"。
 *
 * @param[in] event  事件数据（包含事件类型、时间戳、载荷数据）
 * @param[in] ctx    用户上下文指针（你可以传入任何自己需要的数据）
 */
typedef void (*event_handler_fn_t)(const app_event_t *event, void *ctx);

/**
 * @brief 订阅者信息结构体
 *
 * 记录了一个订阅者的信息：它关心什么事件、收到事件后调用哪个函数。
 */
typedef struct {
    event_handler_fn_t handler;     /* 收到事件后要调用的回调函数 */
    void *ctx;                      /* 传给回调函数的自定义数据（可为 NULL） */
    app_event_type_t event_type;    /* 订阅的事件类型（传 0 表示订阅所有事件） */
    bool is_active;                 /* 这个订阅槽位是否在使用中 */
} event_subscriber_t;

/**
 * @brief 初始化事件总线
 *
 * 清空所有订阅者，创建异步事件队列。
 * 必须在使用其他 event_bus 函数之前调用。
 *
 * @return ESP_OK 成功
 */
esp_err_t event_bus_init(void);

/**
 * @brief 订阅指定事件
 *
 * 注册一个回调函数，当指定类型的事件发生时，这个函数会被自动调用。
 *
 * 用法示例：
 *   int sub_id;
 *   event_bus_subscribe(EVENT_TOUCH_DOWN, my_touch_handler, NULL, &sub_id);
 *   // 之后每当有 EVENT_TOUCH_DOWN 事件，my_touch_handler 就会被调用
 *
 * @param[in]  type     要订阅的事件类型（如 EVENT_TOUCH_DOWN）
 * @param[in]  handler  收到事件时要调用的函数
 * @param[in]  ctx      传给 handler 的自定义数据（可为 NULL）
 * @param[out] sub_id   返回订阅 ID（之后取消订阅时需要用到）
 *
 * @return ESP_OK 成功，ESP_ERR_NO_MEM 订阅者已满（最多32个）
 */
esp_err_t event_bus_subscribe(app_event_type_t type,
                              event_handler_fn_t handler,
                              void *ctx,
                              int *sub_id);

/**
 * @brief 取消订阅
 *
 * 不再接收某类事件的通知。用 subscribe 时返回的 sub_id 来取消。
 *
 * @param[in] sub_id  订阅时获得的 ID
 * @return ESP_OK 成功
 */
esp_err_t event_bus_unsubscribe(int sub_id);

/**
 * @brief 发布事件（同步）
 *
 * 发送一个事件，事件总线会立刻调用所有匹配的订阅者的回调函数。
 * 注意：回调函数是在当前任务中执行的，如果回调函数耗时较长，会阻塞发送者。
 *
 * @param[in] type     事件类型（如 EVENT_TOUCH_DOWN）
 * @param[in] payload  事件载荷数据（可为 NULL，表示不携带额外数据）
 * @return ESP_OK 成功
 */
esp_err_t event_bus_post(app_event_type_t type,
                         const app_event_payload_t *payload);

/**
 * @brief 发布事件（异步）
 *
 * 把事件放入队列，由后台任务稍后处理。不会阻塞发送者。
 * 适合在 ISR 或时间紧迫的场景中使用。
 *
 * @param[in] type     事件类型
 * @param[in] payload  事件载荷数据
 * @return ESP_OK 成功，ESP_ERR_NO_MEM 队列已满
 */
esp_err_t event_bus_post_async(app_event_type_t type,
                               const app_event_payload_t *payload);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_H */
