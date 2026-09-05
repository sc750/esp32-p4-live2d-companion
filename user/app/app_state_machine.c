/**
 * @file    app_state_machine.c
 * @brief   状态机实现
 *
 * 核心是"状态转移表"：一个静态数组，列出所有合法的状态切换规则。
 * 收到事件时，遍历转移表找到匹配项，执行状态切换。
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

/* 1. 自身公开头 */
#include "app_state_machine.h"

/* 2. C 标准库 */
#include <string.h>

/* 3. 项目级 */
#include "event_bus.h"

/* 4. 平台/厂商头 */
#include "esp_log.h"

static const char *TAG = "state_machine";

/* 当前状态，初始为空闲状态 */
static app_state_t s_current_state = STATE_IDLE;

/* 状态变化回调函数（只有一个，新的会覆盖旧的） */
static state_change_cb_t s_change_cb = NULL;
static void *s_change_ctx = NULL;  /* 回调函数的上下文数据 */

/**
 * @brief 状态转移表（编译期静态定义）
 *
 * 这是状态机的"规则手册"，列出了所有合法的状态切换。
 * 格式：{ 当前状态, 触发事件, 目标状态 }
 *
 * 例如第一行的含义：
 *   在 STATE_IDLE（空闲）状态下，如果收到 EVENT_SCREEN_TAP（点击屏幕）事件，
 *   就切换到 STATE_LISTENING（监听）状态。
 */
static const state_transition_t s_transitions[] = {
    /* ===== IDLE（空闲）状态下的转移规则 ===== */
    { STATE_IDLE,      EVENT_SCREEN_TAP,       STATE_LISTENING }, /* 点击屏幕 → 开始监听语音 */
    { STATE_IDLE,      EVENT_NAV_MUSIC,        STATE_MUSIC },     /* 导航事件 → 进入音乐页 */
    { STATE_IDLE,      EVENT_NAV_POMODORO,     STATE_POMODORO },  /* 导航事件 → 进入番茄钟页 */
    { STATE_IDLE,      EVENT_NAV_DIARY,        STATE_DIARY },     /* 导航事件 → 进入日记页 */
    { STATE_IDLE,      EVENT_NAV_SETTINGS,     STATE_MEMORY },    /* 导航事件 → 进入设置页 */
    { STATE_IDLE,      EVENT_SLEEP_TIMEOUT,    STATE_SLEEP },     /* 超时未操作 → 进入休眠 */
    { STATE_IDLE,      EVENT_DIARY_TIME,       STATE_DIARY },     /* 到了写日记时间 → 进入日记页 */

    /* ===== LISTENING（监听）状态下的转移规则 ===== */
    { STATE_LISTENING, EVENT_ASR_FINAL,        STATE_THINKING },  /* 语音识别完成 → 进入思考 */
    { STATE_LISTENING, EVENT_TOUCH_UP,         STATE_IDLE },      /* 松手取消 → 返回空闲 */
    /* Phase 2 还没有语音管线，进 LISTENING 后 ASR/Touch_Up 永远不会来，
     * 不加这两条用户会被困在对话页出不去（R7 实测教训）：
     *   - 再点一下屏幕 → 回主页（scr_chat 的 Live2D 区域点击发 SCREEN_TAP） */
    { STATE_LISTENING, EVENT_SCREEN_TAP,       STATE_IDLE },      /* 点击取消监听 → 返回空闲 */
    { STATE_LISTENING, EVENT_NAV_HOME,         STATE_IDLE },      /* 对话页返回按钮 → 回主页 */

    /* ===== THINKING（思考）状态下的转移规则 ===== */
    { STATE_THINKING,  EVENT_LLM_TOKEN,        STATE_SPEAKING },  /* 收到AI回复 → 开始播放 */
    { STATE_THINKING,  EVENT_LLM_DONE,         STATE_IDLE },      /* AI回复完毕 → 返回空闲 */

    /* ===== SPEAKING（说话）状态下的转移规则 ===== */
    { STATE_SPEAKING,  EVENT_TTS_DONE,         STATE_IDLE },      /* 语音播放完 → 返回空闲 */
    { STATE_SPEAKING,  EVENT_SCREEN_TAP,       STATE_IDLE },      /* 点击打断 → 返回空闲 */

    /* ===== MUSIC（音乐）状态下的转移规则 ===== */
    { STATE_MUSIC,     EVENT_NAV_HOME,         STATE_IDLE },      /* 返回主页 */

    /* ===== POMODORO（番茄钟）状态下的转移规则 ===== */
    { STATE_POMODORO,  EVENT_NAV_HOME,         STATE_IDLE },      /* 返回主页 */

    /* ===== DIARY（日记）状态下的转移规则 ===== */
    { STATE_DIARY,     EVENT_NAV_HOME,         STATE_IDLE },      /* 返回主页 */

    /* ===== MEMORY（设置）状态下的转移规则 ===== */
    { STATE_MEMORY,    EVENT_NAV_HOME,         STATE_IDLE },      /* 返回主页 */

    /* ===== SLEEP（休眠）状态下的转移规则 ===== */
    { STATE_SLEEP,     EVENT_SCREEN_TAP,       STATE_IDLE },      /* 触摸唤醒 */
    { STATE_SLEEP,     EVENT_ASR_PARTIAL,      STATE_IDLE },      /* 检测到声音唤醒 */
};

/* 计算转移表中有多少条规则 */
#define TRANSITION_COUNT (sizeof(s_transitions) / sizeof(s_transitions[0]))

/**
 * @brief 在转移表中查找匹配的规则
 *
 * 遍历转移表，找到"当前状态 + 事件"都匹配的那条规则。
 *
 * @return 匹配的规则索引，没找到返回 -1
 */
static int find_transition(app_state_t from, app_event_type_t event)
{
    for (int i = 0; i < (int)TRANSITION_COUNT; i++) {
        if (s_transitions[i].from == from && s_transitions[i].event == event) {
            return i;  /* 找到了匹配的转移规则 */
        }
    }
    return -1;  /* 没有找到匹配的规则 */
}

/**
 * @brief 执行状态切换
 *
 * 完成三件事：
 *   1. 打印日志（方便调试）
 *   2. 更新当前状态
 *   3. 通知状态变化回调函数
 */
static void do_transition(app_state_t new_state, const char *reason)
{
    app_state_t old_state = s_current_state;

    /* 如果新旧状态相同，就不需要切换 */
    if (old_state == new_state) {
        return;
    }

    /* 状态名称表：用于日志输出，把枚举值转换成可读字符串 */
    static const char *state_names[] = {
        "IDLE",       /* 空闲 */
        "LISTENING",  /* 监听 */
        "THINKING",   /* 思考 */
        "SPEAKING",   /* 说话 */
        "MUSIC",      /* 音乐 */
        "POMODORO",   /* 番茄钟 */
        "DIARY",      /* 日记 */
        "MEMORY",     /* 记忆/设置 */
        "SLEEP",      /* 休眠 */
    };

    /* 打印状态切换日志：从哪个状态 → 到哪个状态，原因是什么 */
    ESP_LOGI(TAG, "状态切换: %s → %s (%s)",
             (old_state < STATE_COUNT) ? state_names[old_state] : "?",
             (new_state < STATE_COUNT) ? state_names[new_state] : "?",
             reason);

    /* 更新当前状态 */
    s_current_state = new_state;

    /* 如果注册了状态变化回调，就调用它 */
    if (s_change_cb != NULL) {
        s_change_cb(old_state, new_state, s_change_ctx);
    }
}

/**
 * @brief 初始化状态机
 */
esp_err_t app_state_machine_init(void)
{
    s_current_state = STATE_IDLE;  /* 初始状态：空闲 */
    s_change_cb = NULL;            /* 清空回调 */
    s_change_ctx = NULL;

    ESP_LOGI(TAG, "状态机初始化完成，初始状态: IDLE");
    return ESP_OK;
}

app_state_t app_state_machine_get_current(void)
{
    return s_current_state;
}

/**
 * @brief 发送事件到状态机
 *
 * 查找转移表，如果找到匹配的规则就执行状态切换。
 */
esp_err_t app_state_machine_send_event(app_event_type_t event_type,
                                       const app_event_payload_t *payload)
{
    (void)payload;  /* 预留参数，暂时未使用 */

    /* 在转移表中查找匹配的规则 */
    int idx = find_transition(s_current_state, event_type);
    if (idx < 0) {
        /* 没有找到匹配的规则，忽略这个事件 */
        ESP_LOGD(TAG, "无匹配转移: 状态=%d, 事件=%d",
                 s_current_state, event_type);
        return ESP_ERR_NOT_FOUND;
    }

    /* 找到匹配的规则，执行状态切换 */
    do_transition(s_transitions[idx].to, "table");
    return ESP_OK;
}

void app_state_machine_force_state(app_state_t state)
{
    do_transition(state, "forced");  /* 强制切换，不受转移表约束 */
}

void app_state_machine_on_change(state_change_cb_t cb, void *ctx)
{
    s_change_cb = cb;      /* 注册回调函数 */
    s_change_ctx = ctx;    /* 保存上下文数据 */
}
