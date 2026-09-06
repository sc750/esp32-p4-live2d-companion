# M02: LVGL v9 UI 框架

> **优先级**: P0 | **预估工时**: 2 周 | **依赖**: M01

---

## 1. 模块概述

基于 LVGL v9 搭建 UI 框架，实现所有用户界面页面、控件和交互逻辑。LVGL 负责渲染非 Live2D 部分（按钮、文本、图标等），输出 RGB565 帧缓冲供 PPA 融合。

---

## 2. UI 页面规划

### 2.1 页面层级

```
App Root
├── Home Screen (STATE_IDLE / 全应用唯一常驻页面, R9 修订)
│   ├── Live2D 角色 (rig 引擎渲染, 唯一交互目标: 五种触摸表情)
│   ├── 顶部状态栏 (Wi-Fi 开关滑块+三态文案, 时间[断线变灰])
│   ├── 底部对话字幕区 (闲聊轮播 / 对话字幕 / 状态层挂载点)
│   │   └── 对话状态层 (STATE_LISTENING/THINKING/SPEAKING 时叠加:
│   │        字幕升高 + 波形条 + 状态点; Phase 3 语音链路就绪时实现)
│   └── 触摸交互 (摸角色=表情+台词; 点空白=无动作, Phase 3 起=开始说话)
│
├── [已拆除] Chat Screen (原独立对话页, 2026-09-06 brainstorm 决策:
│   无语音链路时独立对话页=假监听死胡同, 对话改为 Home 的状态层)
│
├── Music Screen (STATE_MUSIC)
│   ├── Live2D 角色 (听音乐表情)
│   ├── 歌曲信息 (标题, 进度条)
│   ├── 播放控制 (播放/暂停/上一首/下一首)
│   └── 音量滑块
│
├── Pomodoro Screen (STATE_POMODORO)
│   ├── 番茄钟圆环/数字倒计时
│   ├── 开始/暂停/重置按钮
│   ├── 已完成番茄数
│   └── Live2D 角色 (专注/休息表情)
│
├── Diary Screen (STATE_DIARY)
│   ├── 日记文本滚动区
│   ├── 日期选择器
│   ├── 朗读按钮 (TTS 朗读日记)
│   └── Live2D 角色 (温柔表情)
│
├── Settings Screen
│   ├── Wi-Fi 配置
│   ├── 音量调节
│   ├── 主题切换 (昼/夜)
│   ├── 语言设置 (中/日)
│   ├── 记忆查看/编辑
│   └── 关于信息
│
└── Sleep Screen (STATE_SLEEP)
    └── 黑屏 (触摸/语音唤醒)
```

### 2.2 控件设计规范

| 控件 | 尺寸 | 字体 | 颜色 |
|------|------|------|------|
| 标题文本 | 28px | NotoSansSC Bold | 主题色 |
| 正文/字幕 | 20px | NotoSansSC Regular | 主题文本色 |
| 字幕背景 | 半透明 | — | ARGB(180, 0, 0, 0) |
| 按钮 | 48px 高, 圆角 8px | 18px | Primary / Secondary |
| 滑块 | 4px 轨道 + 20px 拇指 | — | Primary 色 |
| 状态图标 | 24×24px | — | 白色 |
| 进度圆环 | 线宽 8px, R=80px | — | Primary 色 |

---

## 3. 功能需求

| ID | 需求 | 优先级 |
|----|------|--------|
| UI-01 | 多页面管理 (带切换动画) | P0 |
| UI-02 | 流式字幕显示 (逐字出现) | P0 |
| UI-03 | 语音波形动画 (VAD 可视化) | P1 |
| UI-04 | 触摸事件传递到 Live2D 引擎 | P0 |
| UI-05 | 昼夜主题切换 (热切换) | P1 |
| UI-06 | 中文字体 (NotoSansSC) 嵌入 | P0 |
| UI-07 | 日文字体 (NotoSansJP) 嵌入 (可选) | P2 |
| UI-08 | 响应式布局 (适配 1024×600) | P0 |
| UI-09 | 优雅的页面过渡动画 | P2 |
| UI-10 | 状态栏 (Wi-Fi/时间/电量) | P1 |

---

## 4. 主题系统

### 4.1 昼夜主题

```c
// 日间主题
static const lv_style_prop_t day_theme[] = {
    .bg_color      = lv_color_hex(0xF5F5F5),  // 浅灰白
    .text_color    = lv_color_hex(0x333333),  // 深灰
    .primary_color = lv_color_hex(0x4A90D9),  // 天蓝
    .accent_color  = lv_color_hex(0xFF6B9D),  // 粉红
    .caption_bg    = lv_color_hex(0x000000),  // 字幕背景(黑半透)
};

// 夜间主题
static const lv_style_prop_t night_theme[] = {
    .bg_color      = lv_color_hex(0x1A1A2E),  // 深蓝黑
    .text_color    = lv_color_hex(0xE0E0E0),  // 浅白
    .primary_color = lv_color_hex(0x7B68EE),  // 紫蓝
    .accent_color  = lv_color_hex(0xFF6B9D),  // 粉红 (不变)
    .caption_bg    = lv_color_hex(0x000000),  // 字幕背景(黑半透)
};
```

### 4.2 主题切换流程

```
用户触发切换 → 更新 NVS 持久化 → 通知 EVENT_THEME_CHANGE
→ event_bus 广播 → LVGL 重新设置样式 → Live2D 背景色更新 → PPA 重新融合
```

---

## 5. LVGL 配置要点

### 5.1 渲染模式

基于 ESP LVGL Adapter，推荐配置：
- **防撕裂模式**: `TRIPLE_PARTIAL` (三缓冲部分刷新)
- **颜色格式**: 根据模式切换
  - ARGB8888 模式 (与 PPA Blend 配合)
  - RGB565 模式 (纯 LVGL 直出)
- **LVGL Draw Unit**: `LV_DRAW_SW_DRAW_UNIT_CNT = 1` (配合 PPA 加速时)

### 5.2 任务配置

```c
#define LVGL_TASK_STACK_SIZE    (64 * 1024)   // 64KB 栈
#define LVGL_TASK_PRIORITY      5
#define LVGL_TASK_CORE          0             // Core 0
#define LVGL_TICK_PERIOD_MS     2
#define LVGL_TASK_MIN_DELAY_MS  5
#define LVGL_TASK_MAX_DELAY_MS  500
```

### 5.3 缓冲区策略

```c
// PPA Blend 模式下使用 ARGB8888
// 缓冲区大小: 1024 × 600 × 4 bytes = ~2.4 MB
// 分配在 PSRAM, 需 L1+L2 cache line 对齐

size_t buf_size = sizeof(lv_color32_t) * LCD_H_RES * LCD_V_RES;
void *fg_buf[2];  // 双缓冲
esp_dma_capable_calloc(2, buf_size, &dma_mem_info, fg_buf);
```

---

## 6. 字体管理

### 6.1 中文字体子集化

全量 NotoSansSC 约 10MB+, 需要子集化：

- **策略**: 使用 LVGL 的 `lv_font_conv` 工具生成子集字体
- **字符集**: 
  - 一级: GB2312 常用字 (~3755 字) + ASCII
  - 二级: 按需从 Flash 动态加载
- **字号**: 20px (正文), 24px (标题), 16px (小字)
- **预估大小**: 20px 子集约 200-400 KB

### 6.2 字体加载

```c
// 固定字体 (编译进固件)
LV_FONT_DECLARE(noto_sans_sc_20);
LV_FONT_DECLARE(noto_sans_sc_28);

// 动态字体 (从 SPIFFS 加载, 按需)
lv_font_t *load_dynamic_font(const char *path, int size);
```

---

## 7. UI Bridge 接口

UI Bridge 层负责将各 Service 的状态同步到 LVGL widget：

```c
// ui_bridge.h

// 字幕更新
void ui_bridge_set_subtitle(const char *text, bool is_japanese);

// 对话状态
void ui_bridge_set_chat_state(chat_state_t state);

// 语音波形
void ui_bridge_update_waveform(const int16_t *samples, size_t count);

// 音乐信息
void ui_bridge_set_music_info(const char *title, const char *artist, int progress_ms, int duration_ms);

// 番茄钟
void ui_bridge_update_pomodoro(int remaining_seconds, int total_seconds, int completed_count);

// 日记
void ui_bridge_set_diary_content(const char *content, const char *date);

// 状态栏
void ui_bridge_update_status_bar(bool wifi_connected, int battery_pct, const char *time_str);

// 主题
void ui_bridge_set_theme(theme_type_t theme);
```

---

## 8. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-UI-01 | 各页面切换 | 无闪烁, ≤300ms 过渡 |
| TST-UI-02 | 流式字幕 | 逐字出现, 无卡顿 |
| TST-UI-03 | 触摸按钮 | 响应 ≤50ms |
| TST-UI-04 | 主题切换 | 所有控件颜色正确更新 |
| TST-UI-05 | 中文显示 | 所有常用字正确渲染 |
| TST-UI-06 | 长时间运行 (2h) | 无内存泄漏, FPS 稳定 |
