# M11: 番茄钟

> **优先级**: P2 | **预估工时**: 0.5 周 | **依赖**: M02

---

## 1. 模块概述

提供番茄钟专注辅助功能，包含计时、休息提醒、完成统计，与 Live2D 角色互动。

---

## 2. 功能需求

| ID | 需求 | 优先级 |
|----|------|--------|
| POM-01 | 标准番茄钟 (25分钟工作 + 5分钟休息) | P0 |
| POM-02 | 自定义时长 | P1 |
| POM-03 | 倒计时 UI (圆环/数字) | P0 |
| POM-04 | 开始/暂停/重置 | P0 |
| POM-05 | 完成提示音 | P0 |
| POM-06 | 今日完成统计 | P1 |
| POM-07 | AI 语音控制 ("开始番茄钟") | P1 |
| POM-08 | Live2D 角色互动 | P1 |
| POM-09 | 长休息 (每4个番茄后15分钟) | P2 |

---

## 3. 番茄钟状态机

```
         ┌──────────┐
         │  IDLE     │  等待开始
         └─────┬────┘
               │ start
         ┌─────▼────┐
    ┌────│ FOCUSING  │────┐
    │    └─────┬────┘    │
    │  pause   │ complete │
    │    ┌─────▼────┐    │
    │    │ PAUSED    │    │
    │    └─────┬────┘    │
    │  resume  │          │
    └──────────┘    ┌─────▼────┐
                    │ BREAK     │  休息时间
                    └─────┬────┘
                    complete │
                    ┌─────▼────┐
                    │ COMPLETED │  番茄完成
                    └──────────┘
```

---

## 4. Live2D 联动

| 番茄钟状态 | 角色表情 | 角色行为 |
|-----------|---------|---------|
| FOCUSING | normal | 安静陪伴, 偶尔看用户 |
| PAUSED | surprised | "诶？暂停了吗？" |
| BREAK | happy | 放松, 伸懒腰 |
| COMPLETED | happy | "太棒了！完成一个番茄！" |
| 4连番茄 | happy+excited | "主人好厉害！休息一下吧~" |

---

## 5. 数据结构

```c
typedef struct {
    int focus_duration_s;       // 工作时长 (默认 1500 = 25min)
    int break_duration_s;       // 短休息 (默认 300 = 5min)
    int long_break_duration_s;  // 长休息 (默认 900 = 15min)
    int pomodoros_before_long;  // 长休息间隔 (默认 4)
} pomodoro_config_t;

typedef struct {
    pomodoro_config_t config;
    int remaining_seconds;
    int completed_today;
    bool is_running;
    bool is_paused;
    bool on_break;
} pomodoro_state_t;
```

---

## 6. UI 设计

```
┌────────────────────────────┐
│       🍅 番茄钟             │
│                            │
│         ╭─────────╮        │
│        │           │       │
│        │  24:32    │       │
│        │  专注中... │       │
│        │           │       │
│         ╰─────────╯        │
│                            │
│    [▶ 开始]  [⟳ 重置]      │
│                            │
│   今日完成: 🍅🍅🍅  3/4     │
│                            │
│   ┌──────────────────────┐ │
│   │ [Live2D 角色安静陪伴] │ │
│   └──────────────────────┘ │
└────────────────────────────┘
```

---

## 7. 对外接口

```c
// pomodoro_service.h

esp_err_t pomodoro_init(void);
esp_err_t pomodoro_start(void);
esp_err_t pomodoro_pause(void);
esp_err_t pomodoro_resume(void);
esp_err_t pomodoro_reset(void);
esp_err_t pomodoro_set_config(const pomodoro_config_t *config);

pomodoro_state_t pomodoro_get_state(void);
int              pomodoro_get_completed_today(void);
```

---

## 8. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-POM-01 | 基本计时 | 25:00 → 00:00 准确 |
| TST-POM-02 | 暂停/恢复 | 暂停后时间冻结 |
| TST-POM-03 | 完成提示 | 闹铃声 + 角色祝贺 |
| TST-POM-04 | AI 控制 | "开始番茄钟" → 启动 |
| TST-POM-05 | 统计 | 今日完成数正确 |
