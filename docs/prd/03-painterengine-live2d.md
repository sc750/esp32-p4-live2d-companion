# M03: PainterEngine Live2D 渲染引擎

> **优先级**: P0 | **预估工时**: 2-3 周 | **依赖**: M01

---

## 1. 模块概述

将 PainterEngine 引擎移植到 ESP32-P4 平台，利用其 Live2D 框架实现二次元角色的渲染与动画。输出 ARGB8888 格式的 overlay 缓冲，供 PPA 与 LVGL UI 背景融合。

---

## 2. PainterEngine 架构分析

### 2.1 核心子系统

PainterEngine 是跨平台 C 语言图形引擎，关键子系统：

| 子系统 | 功能 | 本项目用途 |
|--------|------|-----------|
| Object System | 组件化对象模型 | 角色对象管理 |
| Rendering System | Surface/Texture 管理 | Live2D 渲染输出 |
| Animation System | 关键帧/骨骼动画 | Live2D 物理模拟 |
| Audio System | 音频播放/合成 | 辅助音频处理 |
| UI System | 按钮/列表/滑块 | (不使用, 用 LVGL 替代) |
| Platform Layer | 平台抽象 | ESP32-P4 适配 |

### 2.2 Live2D 渲染流水线

```
Model JSON → Parser → Mesh/Deformer → Canvas → Surface (ARGB8888)
                                                    │
Texture Atlas ──────────────────────────────┘       │
                                                    ▼
                                            双槽 Overlay Buffer
                                                    │
                                                    ▼
                                              PPA Blend 融合
```

---

## 3. 功能需求

| ID | 需求 | 优先级 |
|----|------|--------|
| L2D-01 | Live2D 模型加载 (moc3 + texture) | P0 |
| L2D-02 | Live2D 物理模拟 (头发/衣服物理) | P0 |
| L2D-03 | 表情系统 (至少 8 种表情) | P0 |
| L2D-04 | 动作系统 (闲置/对话/开心/伤心等) | P0 |
| L2D-05 | 触摸跟踪 (角色眼睛跟随手指) | P1 |
| L2D-06 | 口型同步 (配合 TTS 音量) | P1 |
| L2D-07 | 背景透明 (ARGB, alpha 融合) | P0 |
| L2D-08 | 稳定 20 FPS 渲染 | P0 |
| L2D-09 | 多角色支持 (预留) | P2 |

---

## 4. 表情与动作系统

### 4.1 表情定义

| 表情 ID | 名称 | 用途 |
|---------|------|------|
| 0 | normal | 默认待机 |
| 1 | happy | 开心/问候 |
| 2 | sad | 伤感/安慰 |
| 3 | angry | 生气 (撒娇) |
| 4 | surprised | 惊讶 |
| 5 | thinking | AI 推理中 |
| 6 | listening | 正在听用户说话 |
| 7 | speaking | TTS 播报中 |
| 8 | shy | 害羞 |
| 9 | sleep | 休眠/打瞌睡 |

### 4.2 动作定义

| 动作 ID | 名称 | 说明 |
|---------|------|------|
| 0 | idle_01 | 正常待机呼吸 |
| 0 | idle_02 | 左右看 |
| 1 | greeting | 打招呼挥手 |
| 2 | nod | 点头 |
| 3 | shake_head | 摇头 |
| 4 | wave | 挥手 |
| 5 | dance | 跳舞 (音乐播放时) |
| 6 | stretch | 伸懒腰 (长时间无互动) |

### 4.3 表情-状态映射

```c
static const struct {
    app_state_t state;
    int expression_id;
    int motion_id;
} state_to_animation[] = {
    { STATE_IDLE,     0 /*normal*/,    0 /*idle_01*/ },
    { STATE_LISTENING, 6 /*listening*/, -1 /*无特定动作*/ },
    { STATE_THINKING,  5 /*thinking*/,  -1 },
    { STATE_SPEAKING,  7 /*speaking*/,  2 /*nod*/ },
    { STATE_SLEEP,     9 /*sleep*/,     -1 },
};

// LLM 返回情感标签时:
// "happy" → expression_id = 1
// "sad"   → expression_id = 2
// "shy"   → expression_id = 8
```

---

## 5. ESP32-P4 平台适配

### 5.1 Platform Layer 需要实现的接口

```c
// pe_platform_esp32p4.h

// 内存管理
void* pe_platform_malloc(size_t size);
void* pe_platform_realloc(void *ptr, size_t new_size);
void  pe_platform_free(void *ptr);

// 文件 I/O
pe_file_handle_t pe_platform_file_open(const char *path, const char *mode);
size_t  pe_platform_file_read(pe_file_handle_t h, void *buf, size_t size);
size_t  pe_platform_file_write(pe_file_handle_t h, const void *buf, size_t size);
void    pe_platform_file_close(pe_file_handle_t h);
size_t  pe_platform_file_size(const char *path);

// 时间
uint32_t pe_platform_tick_ms(void);

// 渲染目标
void pe_platform_surface_lock(pe_surface_t *surface);
void pe_platform_surface_unlock(pe_surface_t *surface);
uint8_t* pe_platform_surface_get_buffer(pe_surface_t *surface);
```

### 5.2 双槽 Producer/Consumer 渲染管线

```c
// live2d_renderer.h

typedef struct {
    uint8_t *buffer[2];     // ARGB8888 双缓冲
    int      w, h;
    int      active_slot;   // 当前活跃缓冲索引
    SemaphoreHandle_t mutex;
} live2d_overlay_t;

// 初始化 overlay 缓冲
esp_err_t live2d_overlay_init(live2d_overlay_t *overlay, int w, int h);

// 渲染一帧到非活跃 slot
esp_err_t live2d_render_frame(live2d_overlay_t *overlay);

// 交换 slot (由 VSync 回调触发)
void live2d_swap_slots(live2d_overlay_t *overlay);

// 获取当前显示用的 slot (供 PPA Blend 读取)
uint8_t* live2d_get_display_buffer(live2d_overlay_t *overlay);
```

### 5.3 内存布局

```
PSRAM 布局:
┌──────────────────────────────────────────┐
│  LVGL Frame Buffer A (RGB565)           │ ~1.2 MB
├──────────────────────────────────────────┤
│  LVGL Frame Buffer B (RGB565)           │ ~1.2 MB
├──────────────────────────────────────────┤
│  Live2D Overlay A (ARGB8888)            │ ~2.4 MB
├──────────────────────────────────────────┤
│  Live2D Overlay B (ARGB8888)            │ ~2.4 MB
├──────────────────────────────────────────┤
│  Live2D Model Data + Textures           │ ~8 MB
├──────────────────────────────────────────┤
│  Live2D Temp Rendering Buffers          │ ~2 MB
├──────────────────────────────────────────┤
│  ... (音频/网络/其他)                    │
└──────────────────────────────────────────┘
```

---

## 6. Live2D 模型资源规范

### 6.1 模型文件结构

```
/models/character_01/
├── character.moc3         # Live2D 模型二进制
├── character.model3.json  # 模型配置
├── character.physics3.json # 物理模拟配置
├── character.pose3.json   # 姿态配置
├── texture_00.png         # 纹理图集 (需转为 RGB565/RGBA4444 以节省内存)
├── motions/
│   ├── idle_01.motion3.json
│   ├── greeting.motion3.json
│   └── ...
└── expressions/
    ├── normal.exp3.json
    ├── happy.exp3.json
    └── ...
```

### 6.2 纹理优化

- 原始 PNG 纹理可能较大 (4096×4096)
- **策略**: 缩小到 2048×2048 或 1024×1024
- **格式**: RGBA4444 (减少 50% 内存) 或 ARGB8888 (质量优先)
- **预估**: 2048×2048 RGBA4444 ≈ 16 MB (太大!) → 必须缩小到 1024×1024 ≈ 4 MB

---

## 7. 性能优化策略

| 策略 | 说明 | 预期效果 |
|------|------|---------|
| 双槽无锁切换 | VSync 时原子 swap 指针 | 消除渲染撕裂 |
| 纹理 atlas 压缩 | 使用 ETC1/RGBA4444 | 减少 50% 纹理内存 |
| 物理模拟降频 | 复杂物理 30FPS, 简单物理 60FPS | 节省 CPU |
| 矩形脏区更新 | 仅重绘变化区域 | 减少像素填充 |
| 模型 LOD | 远距离简化 mesh | 减少顶点处理 |
| IRAM 缓存关键函数 | 热路径放入 IRAM | 减少 cache miss |

---

## 8. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-L2D-01 | 模型加载 | 成功加载 moc3 + 纹理, 无内存泄漏 |
| TST-L2D-02 | 基本渲染 | 角色正确显示, 透明背景 |
| TST-L2D-03 | 帧率测试 | 持续 ≥20 FPS |
| TST-L2D-04 | 表情切换 | 10 种表情正确切换, 过渡平滑 |
| TST-L2D-05 | 物理模拟 | 头发/衣物自然摆动 |
| TST-L2D-06 | 触摸跟踪 | 眼睛跟随手指, 延迟 ≤50ms |
| TST-L2D-07 | 长时间运行 (4h) | 无堆损坏/内存泄漏 |
| TST-L2D-08 | CPU 占用 | ≤40% Core 1 |
