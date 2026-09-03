/**
 * @file    event_bus.c
 * @brief   事件总线实现
 *
 * 实现发布-订阅模式的事件通信机制。
 * 支持同步分发（直接调用回调）和异步分发（通过队列延迟处理）。
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

/* 1. 自身公开头 */
#include "event_bus.h"

/* 2. C 标准库 */
#include <string.h>

/* 3. 项目级 */

/* 4. 平台/厂商头 */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "event_bus";

/* 异步事件队列最多容纳 32 个待处理事件 */
#define ASYNC_QUEUE_SIZE  32

/* 订阅者表：最多 32 个订阅者，每个槽位记录一个订阅者的信息 */
static event_subscriber_t s_subscribers[EVENT_BUS_MAX_SUBSCRIBERS];

/* 当前已注册的订阅者数量（用于统计，实际判断用 is_active） */
static int s_subscriber_count = 0;

/* FreeRTOS 队列句柄：用于异步事件的暂存 */
static QueueHandle_t s_async_queue = NULL;

/* 消费者任务句柄：负责从队列中取出异步事件并分发 */
static TaskHandle_t s_consumer_task = NULL;

/**
 * @brief 异步事件消费者任务
 *
 * 这是一个 FreeRTOS 后台任务，持续运行。
 * 它的工作流程：
 *   1. 从队列中取出一个事件（如果没有事件就等待）
 *   2. 遍历所有订阅者，找到订阅了这个事件类型的
 *   3. 调用订阅者的回调函数，把事件传给它
 *   4. 回到第 1 步，继续等待下一个事件
 */
static void async_consumer_task(void *arg)
{
    (void)arg;  /* 未使用参数，消除编译警告 */
    app_event_t event;  /* 用于接收队列中的事件 */

    while (1) {
        /* 阻塞等待队列中的事件，最长等待时间：永久等待 */
        if (xQueueReceive(s_async_queue, &event, portMAX_DELAY) == pdTRUE) {
            /* 遍历所有订阅者槽位 */
            for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
                /* 跳过未使用的槽位 */
                if (!s_subscribers[i].is_active) {
                    continue;
                }
                /* 如果订阅者关心这个事件类型，或者订阅者订阅了所有事件（type==0） */
                if (s_subscribers[i].event_type == event.type ||
                    s_subscribers[i].event_type == 0) {
                    /* 调用订阅者的回调函数，把事件和上下文传给它 */
                    s_subscribers[i].handler(&event, s_subscribers[i].ctx);
                }
            }
        }
    }
}

/**
 * @brief 初始化事件总线
 *
 * 做三件事：
 *   1. 清空订阅者表（把所有槽位标记为未使用）
 *   2. 创建 FreeRTOS 队列（用于异步事件暂存）
 *   3. 创建消费者任务（后台处理异步事件）
 */
esp_err_t event_bus_init(void)
{
    /* 第一步：清空订阅者表 */
    memset(s_subscribers, 0, sizeof(s_subscribers));
    s_subscriber_count = 0;

    /* 第二步：创建异步事件队列，每个元素是一个 app_event_t 大小 */
    s_async_queue = xQueueCreate(ASYNC_QUEUE_SIZE, sizeof(app_event_t));
    if (s_async_queue == NULL) {
        ESP_LOGE(TAG, "异步事件队列创建失败");
        return ESP_ERR_NO_MEM;
    }

    /* 第三步：创建消费者任务，栈大小 4KB，优先级 5 */
    BaseType_t ret = xTaskCreate(async_consumer_task, "evt_bus", 4096, NULL, 5,
                                 &s_consumer_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "消费者任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "事件总线初始化完成");
    return ESP_OK;
}

/**
 * @brief 订阅事件
 *
 * 在订阅者表中找一个空槽位，把回调函数和事件类型记录进去。
 * 之后当该类型的事件发生时，回调函数就会被自动调用。
 */
esp_err_t event_bus_subscribe(app_event_type_t type,
                              event_handler_fn_t handler,
                              void *ctx,
                              int *sub_id)
{
    /* 参数检查：回调函数不能为空 */
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 遍历订阅者表，找第一个空闲槽位 */
    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        if (!s_subscribers[i].is_active) {
            /* 找到空槽位，填入订阅信息 */
            s_subscribers[i].handler = handler;          /* 回调函数 */
            s_subscribers[i].ctx = ctx;                  /* 用户上下文 */
            s_subscribers[i].event_type = type;          /* 订阅的事件类型 */
            s_subscribers[i].is_active = true;           /* 标记为已使用 */
            s_subscriber_count++;                        /* 订阅者数量 +1 */

            if (sub_id != NULL) {
                *sub_id = i;  /* 返回槽位索引，供取消订阅时使用 */
            }
            ESP_LOGD(TAG, "订阅成功: 槽位 %d, 事件 %d", i, type);
            return ESP_OK;
        }
    }

    /* 所有槽位都满了，无法订阅 */
    ESP_LOGE(TAG, "订阅者已满（最多 %d 个）", EVENT_BUS_MAX_SUBSCRIBERS);
    return ESP_ERR_NO_MEM;
}

/**
 * @brief 取消订阅
 *
 * 根据订阅时返回的 ID，把对应槽位标记为未使用。
 */
esp_err_t event_bus_unsubscribe(int sub_id)
{
    /* 检查 ID 是否在有效范围内 */
    if (sub_id < 0 || sub_id >= EVENT_BUS_MAX_SUBSCRIBERS) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 如果该槽位确实在使用中，就取消它 */
    if (s_subscribers[sub_id].is_active) {
        s_subscribers[sub_id].is_active = false;  /* 标记为未使用 */
        s_subscriber_count--;                      /* 订阅者数量 -1 */
        ESP_LOGD(TAG, "取消订阅: 槽位 %d", sub_id);
    }
    return ESP_OK;
}

/**
 * @brief 发布事件（同步）
 *
 * 构造一个事件对象，然后立即遍历所有订阅者，
 * 找到关心这个事件的订阅者，直接调用它的回调函数。
 *
 * 注意：回调函数在当前任务上下文中执行，如果回调耗时较长会阻塞发送者。
 */
esp_err_t event_bus_post(app_event_type_t type,
                         const app_event_payload_t *payload)
{
    /* 构造事件对象 */
    app_event_t event = {
        .type = type,                                          /* 事件类型 */
        .timestamp = (uint32_t)(esp_timer_get_time() / 1000), /* 当前时间（毫秒） */
    };
    /* 如果有载荷数据，就拷贝过来 */
    if (payload != NULL) {
        event.payload = *payload;
    }

    /* 遍历所有订阅者，分发事件 */
    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        if (!s_subscribers[i].is_active) {
            continue;  /* 跳过未使用的槽位 */
        }
        /* 如果订阅者关心这个事件类型，或者订阅了所有事件 */
        if (s_subscribers[i].event_type == type ||
            s_subscribers[i].event_type == 0) {
            /* 直接调用回调函数 */
            s_subscribers[i].handler(&event, s_subscribers[i].ctx);
        }
    }
    return ESP_OK;
}

/**
 * @brief 发布事件（异步）
 *
 * 把事件放入 FreeRTOS 队列，由后台消费者任务稍后处理。
 * 这个函数不会阻塞，立即返回。
 * 适合在 ISR 或需要快速返回的场景中使用。
 */
esp_err_t event_bus_post_async(app_event_type_t type,
                               const app_event_payload_t *payload)
{
    /* 构造事件对象 */
    app_event_t event = {
        .type = type,
        .timestamp = (uint32_t)(esp_timer_get_time() / 1000),
    };
    if (payload != NULL) {
        event.payload = *payload;
    }

    /* 检查队列是否已初始化 */
    if (s_async_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 尝试把事件放入队列（非阻塞，放不下就丢弃） */
    if (xQueueSend(s_async_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "异步队列已满，事件 %d 被丢弃", type);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
