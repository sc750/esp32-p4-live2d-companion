/**
 * @file    app_state_machine.h
 * @brief   应用状态机管理
 *
 * 状态机是整个应用的"大脑"，决定系统当前处于什么状态，
 * 以及收到某个事件后应该切换到什么状态。
 *
 * 例如：
 *   - 当前是"空闲"状态，用户点击了屏幕 → 切换到"监听"状态
 *   - 当前是"监听"状态，语音识别完成 → 切换到"思考"状态
 *   - 当前是"思考"状态，大模型返回结果 → 切换到"说话"状态
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

#ifndef APP_STATE_MACHINE_H
#define APP_STATE_MACHINE_H

#include "app_events.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 应用状态枚举
 *
 * 系统在任意时刻只能处于以下某一个状态。
 * 状态之间的切换由事件驱动（通过状态转移表）。
 */
typedef enum {
    STATE_IDLE,         /* 空闲状态：显示角色待机画面，等待用户操作 */
    STATE_LISTENING,    /* 监听状态：麦克风正在采集语音，等待用户说完 */
    STATE_THINKING,     /* 思考状态：AI 正在处理语音，等待大模型回复 */
    STATE_SPEAKING,     /* 说话状态：TTS 正在播放 AI 的回复语音 */
    STATE_MUSIC,        /* 音乐状态：正在播放音乐 */
    STATE_POMODORO,     /* 番茄钟状态：正在倒计时 */
    STATE_DIARY,        /* 日记状态：正在显示/朗读日记 */
    STATE_MEMORY,       /* 记忆状态：正在查看/编辑 AI 的记忆 */
    STATE_SLEEP,        /* 休眠状态：息屏，等待唤醒 */
    STATE_COUNT         /* 状态总数（哨兵值，用于数组大小计算） */
} app_state_t;

/**
 * @brief 状态转移表项
 *
 * 定义一条转移规则：当处于 from 状态时，如果收到 event 事件，
 * 就切换到 to 状态。
 *
 * 例如：{ STATE_IDLE, EVENT_SCREEN_TAP, STATE_LISTENING }
 * 含义：空闲状态下点击屏幕 → 进入监听状态
 */
typedef struct {
    app_state_t from;           /* 源状态：当前处于什么状态 */
    app_event_type_t event;     /* 触发事件：收到了什么事件 */
    app_state_t to;             /* 目标状态：应该切换到什么状态 */
} state_transition_t;

/**
 * @brief 状态变化回调函数类型
 *
 * 当状态发生变化时，这个函数会被调用。
 * 可以用来更新 UI、启动/停止某些功能等。
 *
 * @param[in] old_state  变化前的状态
 * @param[in] new_state  变化后的状态
 * @param[in] ctx        用户上下文
 */
typedef void (*state_change_cb_t)(app_state_t old_state,
                                  app_state_t new_state, void *ctx);

/**
 * @brief 初始化状态机
 *
 * 把当前状态设置为 IDLE（空闲），清空回调函数。
 * 必须在使用其他状态机函数之前调用。
 *
 * @return ESP_OK 成功
 */
esp_err_t app_state_machine_init(void);

/**
 * @brief 获取当前状态
 *
 * @return 当前状态枚举值（如 STATE_IDLE、STATE_LISTENING 等）
 */
app_state_t app_state_machine_get_current(void);

/**
 * @brief 发送事件到状态机
 *
 * 根据状态转移表，检查当前状态 + 事件 是否有匹配的转移规则。
 * 如果有，就执行状态切换；如果没有，就忽略这个事件。
 *
 * @param[in] event_type  事件类型
 * @param[in] payload     事件载荷（目前未使用，预留）
 * @return ESP_OK 事件已处理（状态已切换），ESP_ERR_NOT_FOUND 没有匹配的转移规则
 */
esp_err_t app_state_machine_send_event(app_event_type_t event_type,
                                       const app_event_payload_t *payload);

/**
 * @brief 强制设置状态（跳过转移表检查）
 *
 * 用于特殊场景，比如系统启动时直接进入某个状态。
 *
 * @param[in] state  目标状态
 */
void app_state_machine_force_state(app_state_t state);

/**
 * @brief 注册状态变化回调
 *
 * 当状态发生变化时，指定的函数会被自动调用。
 * 只能注册一个回调（新的会覆盖旧的）。
 *
 * @param[in] cb   回调函数
 * @param[in] ctx  传给回调函数的上下文数据
 */
void app_state_machine_on_change(state_change_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATE_MACHINE_H */
