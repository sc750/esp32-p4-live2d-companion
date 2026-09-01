# M12: 电源管理与昼夜主题

> **优先级**: P2 | **预估工时**: 1 周 | **依赖**: M02

---

## 1. 模块概述

管理系统的电源状态（正常运行/息屏休眠/唤醒），以及基于时间的昼夜主题自动切换。

---

## 2. 功能需求

### 2.1 电源管理

| ID | 需求 | 优先级 |
|----|------|--------|
| PWR-01 | 无操作超时息屏 (可配置, 默认5分钟) | P0 |
| PWR-02 | 触摸唤醒 | P0 |
| PWR-03 | 语音唤醒 (可选) | P2 |
| PWR-04 | 息屏时降低 CPU 频率 | P1 |
| PWR-05 | 息屏时暂停 Live2D 渲染 | P0 |
| PWR-06 | 息屏时暂停网络活动 (可选) | P2 |
| PWR-07 | 息屏时保持 AI 后台 (可选) | P2 |

### 2.2 昼夜主题

| ID | 需求 | 优先级 |
|----|------|--------|
| THR-01 | 手动切换昼/夜主题 | P1 |
| PWR-02 | 自动切换 (日出/日落时间) | P2 |
| THR-03 | 主题切换动画 (渐变过渡) | P2 |
| THR-04 | Live2D 背景色跟随主题 | P1 |
| THR-05 | LVGL 控件样式跟随主题 | P1 |

---

## 3. 电源状态机

```
┌──────────────┐
│   RUNNING     │  正常运行
│  (全速运行)   │
└──────┬───────┘
       │ 超时无操作
  ┌────▼─────┐
  │ DIMMING    │  屏幕渐暗 (5秒过渡)
  └────┬─────┘
       │ 渐暗完成
  ┌────▼─────┐
  │ SLEEPING   │  息屏休眠
  │ (低功耗)   │
  └────┬─────┘
       │ 触摸/按键
  ┌────▼─────┐
  │ WAKING     │  唤醒过渡
  └────┬─────┘
       │ 唤醒完成
  ┌────▼─────┐
  │ RUNNING    │
  └──────────┘
```

### 3.1 息屏实现

```c
typedef enum {
    POWER_STATE_RUNNING,
    POWER_STATE_DIMMING,
    POWER_STATE_SLEEPING,
    POWER_STATE_WAKING,
} power_state_t;

// 息屏序列
void power_enter_sleep(void) {
    // 1. 降低背光亮度 (渐变)
    for (int brightness = 255; brightness >= 0; brightness -= 5) {
        display_set_brightness(brightness);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    
    // 2. 关闭 MIPI-DSI 输出
    esp_lcd_panel_disp_off(panel_handle, true);
    
    // 3. 暂停 Live2D 渲染
    live2d_renderer_pause();
    
    // 4. 降低 CPU 频率
    esp_clk_tree_source_set_freqMHz(CLK_SRC_CPU, 80);  // 从 400MHz 降到 80MHz
    
    // 5. 暂停非必要定时器
    
    g_power_state = POWER_STATE_SLEEPING;
}

// 唤醒序列
void power_wake_up(void) {
    // 1. 恢复 CPU 频率
    esp_clk_tree_source_set_freqMHz(CLK_SRC_CPU, 400);
    
    // 2. 开启 MIPI-DSI 输出
    esp_lcd_panel_disp_off(panel_handle, false);
    
    // 3. 恢复 Live2D 渲染
    live2d_renderer_resume();
    
    // 4. 渐亮背光
    for (int brightness = 0; brightness <= 255; brightness += 5) {
        display_set_brightness(brightness);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    
    g_power_state = POWER_STATE_RUNNING;
    
    // 5. Live2D 显示"醒来"动画
    live2d_set_expression(EXPRESSION_HAPPY);
    live2d_set_motion(MOTION_GREETING);
}
```

---

## 4. 昼夜主题

### 4.1 自动切换规则

```c
typedef struct {
    int sunrise_hour;    // 日出时间 (默认 6)
    int sunset_hour;     // 日落时间 (默认 18)
    bool auto_switch;    // 是否自动切换
} theme_config_t;

theme_type_t get_auto_theme(int current_hour, const theme_config_t *config) {
    if (!config->auto_switch) return THEME_DAY;  // 由用户手动选择
    
    if (current_hour >= config->sunrise_hour && 
        current_hour < config->sunset_hour) {
        return THEME_DAY;
    } else {
        return THEME_NIGHT;
    }
}
```

### 4.2 主题切换动画

```c
// 渐变过渡: 0.5 秒内所有颜色线性插值
void theme_transition(theme_type_t from, theme_type_t to, int duration_ms) {
    int steps = duration_ms / 20;  // 每 20ms 一步
    
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / steps;
        
        lv_color_t bg = color_lerp(from.bg_color, to.bg_color, t);
        lv_color_t text = color_lerp(from.text_color, to.text_color, t);
        lv_color_t primary = color_lerp(from.primary_color, to.primary_color, t);
        
        theme_apply(bg, text, primary);
        lv_refr_now(NULL);  // 强制刷新
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
```

---

## 5. 对外接口

```c
// power_service.h
esp_err_t power_service_init(void);
esp_err_t power_enter_sleep(void);
esp_err_t power_wake_up(void);
power_state_t power_get_state(void);
esp_err_t power_set_sleep_timeout(int seconds);

// theme_service.h
esp_err_t theme_service_init(void);
esp_err_t theme_set(theme_type_t theme);
esp_err_t theme_set_auto_switch(bool enabled, int sunrise_hour, int sunset_hour);
theme_type_t theme_get_current(void);
```

---

## 6. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-PWR-01 | 超时息屏 | 5分钟无操作后自动息屏 |
| TST-PWR-02 | 触摸唤醒 | 触摸后 ≤1s 恢复显示 |
| TST-PWR-03 | 息屏功耗 | 明显低于运行状态 |
| TST-PWR-04 | 主题切换 | 昼/夜颜色正确过渡 |
| TST-PWR-05 | 自动主题 | 18:00 自动切夜间 |
