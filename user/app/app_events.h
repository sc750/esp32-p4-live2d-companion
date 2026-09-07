/**
 * @file    app_events.h
 * @brief   系统事件类型定义
 *
 * 定义所有模块间通信使用的事件类型枚举。
 * 事件通过 event_bus 发布-订阅机制分发。
 * 每个事件对应系统中的一个特定动作或状态变化。
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

#ifndef APP_EVENTS_H
#define APP_EVENTS_H

/* 标准头文件：提供 size_t 和 uint32_t 类型 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 系统事件类型枚举
 *
 * 列出系统中所有可能发生的事情。
 * 当某件事发生时，对应的事件会被发送到事件总线，
 * 所有订阅了这个事件的模块都会收到通知。
 *
 * 分类说明：
 * - 触摸事件：用户手指在屏幕上操作时产生
 * - 语音事件：AI 语音对话过程中产生
 * - 定时器事件：各种定时任务到期时产生
 * - 系统事件：系统状态变化时产生（如 Wi-Fi 连接/断开）
 * - Live2D 事件：角色动画相关事件
 * - 页面导航事件：用户切换界面时产生
 */
typedef enum {
    /* ========== 触摸事件 ========== */
    /* 当用户手指接触屏幕时触发，payload 中包含触摸坐标 */
    EVENT_TOUCH_DOWN,
    /* 当用户手指离开屏幕时触发 */
    EVENT_TOUCH_UP,
    /* 当用户手指在屏幕上移动时触发 */
    EVENT_TOUCH_MOVE,
    /* 当用户点击角色区域时触发，用于启动对话 */
    EVENT_SCREEN_TAP,
    /* 当用户长按屏幕超过 1 秒时触发，用于弹出快捷菜单 */
    EVENT_SCREEN_LONG_PRESS,

    /* ========== 语音对话事件 ========== */
    /* 语音识别的中间结果（边说边识别，文字会不断更新） */
    EVENT_ASR_PARTIAL,
    /* 语音识别的最终结果（用户说完后，识别出完整句子） */
    EVENT_ASR_FINAL,
    /* 大模型每返回一个字/词时触发（流式输出，让打字机效果） */
    EVENT_LLM_TOKEN,
    /* 大模型推理完成时触发（所有文字都返回了） */
    EVENT_LLM_DONE,
    /* 大模型要求调用外部工具时触发（如查天气、设闹钟） */
    EVENT_LLM_TOOL_CALL,
    /* TTS 合成的一小段音频数据到达时触发（用于流式播放） */
    EVENT_TTS_CHUNK,
    /* TTS 全部音频播放完毕时触发 */
    EVENT_TTS_DONE,

    /* ========== 定时器事件 ========== */
    /* 番茄钟每秒滴答一次，用于更新倒计时显示 */
    EVENT_POMODORO_TICK,
    /* 番茄钟倒计时结束（25 分钟到了） */
    EVENT_POMODORO_DONE,
    /* 到了写日记的时间（每天晚上 10 点自动触发） */
    EVENT_DIARY_TIME,
    /* 用户长时间没有操作，触发息屏休眠 */
    EVENT_SLEEP_TIMEOUT,

    /* ========== 系统事件 ========== */
    /* Wi-Fi 成功连接到路由器 */
    EVENT_WIFI_CONNECTED,
    /* Wi-Fi 断开连接（可能因为距离太远或路由器重启） */
    EVENT_WIFI_DISCONNECTED,
    /* 系统可用内存不足，需要释放非关键缓存 */
    EVENT_LOW_MEMORY,
    /* 用户切换了昼/夜主题 */
    EVENT_THEME_CHANGE,

    /* ========== Live2D 角色事件 ========== */
    /* 切换角色表情（开心、难过、思考等） */
    EVENT_L2D_EXPRESSION,
    /* 触发角色动作（点头、摇头、挥手等） */
    EVENT_L2D_MOTION,

    /* ========== 页面导航事件 ========== */
    /* 返回主界面（显示角色待机状态） */
    EVENT_NAV_HOME,
    /* 进入对话界面（开始或继续语音对话） */
    EVENT_NAV_CHAT,
    /* 进入音乐播放界面 */
    EVENT_NAV_MUSIC,
    /* 进入番茄钟界面 */
    EVENT_NAV_POMODORO,
    /* 进入日记阅读界面 */
    EVENT_NAV_DIARY,
    /* 进入设置界面 */
    EVENT_NAV_SETTINGS,

    EVENT_TYPE_COUNT  /* 哨兵值：表示事件类型的总数，用于数组大小计算 */
} app_event_type_t;

/**
 * 对话状态（M1：主页状态层与语音管线共享的中立词汇。
 * 定义在 app_events 而非 scr_home，避免 ai 层反向依赖 ui 层）
 */
typedef enum {
    DIALOG_STATE_IDLE = 0,      /* 待机：状态点隐藏 */
    DIALOG_STATE_LISTENING,     /* 听：蓝点 */
    DIALOG_STATE_THINKING,      /* 想：橙点 */
    DIALOG_STATE_SPEAKING,      /* 说：绿点 */
} dialog_state_t;

/**
 * @brief 事件载荷联合体
 *
 * 不同类型的事件携带不同的数据。
 * 用联合体节省内存：同一时间只有一种事件，所以可以共用一块内存。
 *
 * 例如：
 * - 触摸事件携带触摸坐标 (x, y)
 * - 语音事件携带识别出的文字
 * - Live2D 事件携带表情 ID
 */
typedef union {
    struct { int x; int y; } touch;         /* 触摸事件：x/y 坐标（像素） */
    struct { const char *text; } asr;       /* 语音事件：识别出的文字内容 */
    struct { const char *token; } llm;      /* LLM 事件：大模型返回的一个字/词 */
    struct { int expression_id; } l2d;      /* Live2D 事件：表情编号 */
    struct { size_t size; } memory;         /* 内存事件：请求分配的内存大小 */
    struct { int theme; } theme;            /* 主题事件：0=日间, 1=夜间 */
} app_event_payload_t;

/**
 * @brief 应用事件结构体
 *
 * 这是事件总线中传输的数据单元。
 * 每个事件都包含：是什么事（type）、什么时候发生的（timestamp）、具体数据（payload）
 */
typedef struct {
    app_event_type_t type;          /* 事件类型：是什么事情发生了 */
    uint32_t timestamp;             /* 时间戳：事情发生的时刻（毫秒） */
    app_event_payload_t payload;    /* 事件载荷：事情的具体数据 */
} app_event_t;

/**
 * @brief 获取事件类型的中文名称（调试用）
 *
 * 输入事件枚举值，返回对应的中文字符串。
 * 用于日志输出，方便调试时看懂是哪个事件。
 *
 * @param[in] type  事件类型枚举值
 * @return 事件名称字符串（如 "TOUCH_DOWN"、"ASR_FINAL" 等）
 */
const char *app_event_type_name(app_event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* APP_EVENTS_H */
