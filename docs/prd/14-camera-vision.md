# M14: 摄像头视觉系统 (AI之眼)

> **优先级**: P2 | **预估工时**: 2 周 | **依赖**: M06, M07

---

## 1. 模块概述

为 AI 角色赋予视觉感知能力——"眼睛"。通过板载 MIPI-CSI 摄像头采集图像，利用 ESP32-P4 硬件 JPEG 编码器压缩，结合云端 Vision 多模态大模型实现场景理解、手势识别、表情识别等视觉交互功能。同时支持简单的本地运动检测作为触发机制。

**核心思路：端侧感知 + 云端理解**

```
┌──────────────────────────────────┐        ┌──────────────────────────┐
│  ESP32-P4 (端侧)                 │        │  云端 (Vision LLM)        │
│                                   │        │                          │
│  MIPI-CSI 摄像头                  │        │  GPT-4o / Claude Vision  │
│  ↓                                │        │  / Gemini / Qwen-VL      │
│  硬件 JPEG 编码 (~10ms)           │──JPEG──▶                          │
│  ↓                                │◀─文本──│  返回场景描述/情感分析    │
│  本地运动检测 (帧差法)             │        │                          │
│  → 挥手/有人/变化 → 触发事件      │        │  "用户在挥手"            │
└──────────────────────────────────┘        └──────────────────────────┘
```

---

## 2. 硬件资源

### 2.1 摄像头参数

| 项目 | 规格 |
|------|------|
| 接口 | MIPI-CSI, FPC 15Pin (1.0K-GT-15PB) |
| 像素 | 200 万像素 (可选配件) |
| 推荐分辨率 | 720p (1280×720) 用于 AI 分析, 320×240 用于本地检测 |
| 输出格式 | YUV422 (传感器输出) → JPEG (ESP32-P4 硬件编码) |
| 帧率 | 720p@24fps, 1080p@15fps |
| ISP | ESP32-P4 内置 ISP (RAW→YUV/RGB 处理) |

### 2.2 硬件 JPEG 编码器

ESP32-P4 内置 JPEG 硬件编解码器，编码性能：

| 分辨率 | 编码时间 | 输出大小 (质量80) |
|--------|---------|-------------------|
| 320×240 | ~3ms | ~5-8 KB |
| 640×480 | ~8ms | ~15-25 KB |
| 1280×720 | ~15ms | ~30-50 KB |
| 1920×1080 | ~25ms | ~60-100 KB |

---

## 3. 功能需求

| ID | 需求 | 优先级 |
|----|------|--------|
| CAM-01 | MIPI-CSI 摄像头初始化与出图 | P0 |
| CAM-02 | 硬件 JPEG 编码 | P0 |
| CAM-03 | 本地运动检测 (帧差法) | P1 |
| CAM-04 | Vision LLM 场景理解 | P0 |
| CAM-05 | Vision LLM 表情识别 | P1 |
| CAM-06 | 手势识别 (挥手/点头) | P1 |
| CAM-07 | 视觉 MCP 工具集成到 AI Agent | P0 |
| CAM-08 | 拍照保存到 SD 卡 | P2 |
| CAM-09 | 视觉历史记录 (最近 N 张) | P2 |
| CAM-10 | 摄像头隐私指示灯/软件开关 | P1 |

---

## 4. 系统架构

### 4.1 模块结构

```
┌──────────────────────────────────────────────┐
│              Vision Service                    │
│                                               │
│  ┌──────────────┐  ┌───────────────────────┐  │
│  │ Camera HAL    │  │ Vision Agent           │  │
│  │ (MIPI-CSI +   │  │ (Vision LLM 调用)      │  │
│  │  JPEG Encode) │  │ - 场景理解             │  │
│  └──────┬───────┘  │ - 表情识别             │  │
│         │          │ - 物体识别             │  │
│         │          └───────────┬───────────┘  │
│         │                      │               │
│  ┌──────▼───────┐  ┌──────────▼───────────┐  │
│  │ Motion        │  │ Image Formatter       │  │
│  │ Detector      │  │ (JPEG → Base64 编码)  │  │
│  │ (本地帧差法)  │  └──────────────────────┘  │
│  └──────────────┘                              │
│                                               │
└──────────────────────────────────────────────┘
         │
    ┌────▼────┐
    │ AI Agent │  通过 function calling 调用
    └─────────┘
```

### 4.2 数据流

```
摄像头帧 (YUV422, 720p)
    │
    ├──▶ [本地路径] 运动检测 (320×240 降采样)
    │     ├── 检测到运动 → EVENT_MOTION_DETECTED
    │     ├── 检测到挥手 → EVENT_GESTURE_WAVE
    │     └── 周期性检查 (每 500ms)
    │
    └──▶ [云端路径] AI 场景理解
          ├── JPEG 硬件编码 (720p, ~15ms)
          ├── Base64 编码
          ├── Vision LLM API 请求
          └── 返回文本描述 + 情感标签
```

---

## 5. 本地运动检测

### 5.1 帧差法实现

```c
// motion_detector.h

#define MOTION检测_WIDTH    320
#define MOTION检测_HEIGHT   240
#define MOTION_THRESHOLD   30      // 像素差值阈值
#define MOTION_REGION_MIN  100     // 最小变化区域 (像素数)
#define MOTION_COOLDOWN_MS 3000    // 触发冷却时间

typedef struct {
    uint8_t *prev_frame;            // 上一帧灰度图 (320×240)
    uint8_t *curr_frame;            // 当前帧灰度图
    int width, height;
    uint32_t last_trigger_time;
    bool enabled;
} motion_detector_t;

// 初始化
esp_err_t motion_detector_init(motion_detector_t *det);

// 处理一帧 (从摄像头获取 YUV, 降采样到灰度, 计算帧差)
motion_event_t motion_detector_process(motion_detector_t *det, 
                                       const uint8_t *yuv_frame);

typedef enum {
    MOTION_NONE,         // 无变化
    MOTION_DETECTED,     // 有运动
    MOTION_WAVE,         // 挥手 (特定运动模式)
} motion_event_t;
```

### 5.2 简易手势识别

通过分析运动区域的方向和周期性：

| 手势 | 特征 | 检测方法 |
|------|------|---------|
| **挥手** | 水平方向周期性运动 | 水平方向运动向量周期 ≥2 次 |
| **点头** | 垂直方向小幅度运动 | 垂直方向运动 + 低幅度 |
| **摇头** | 水平方向快速往返 | 水平方向运动 + 高频率 |
| **有人出现** | 大面积像素变化 | 变化区域 > 阈值 |

### 5.3 运动检测触发策略

```
空闲状态 (STATE_IDLE)
    │
    ├── 检测到运动 (MOTION_DETECTED)
    │   └── Live2D 角色切换到 "注意" 表情, 眼睛看向摄像头方向
    │
    ├── 检测到挥手 (MOTION_WAVE)
    │   ├── Live2D 角色切换到 "happy" 表情
    │   ├── 播放问候语音: "啊，你回来啦！"
    │   └── 可选: 触发拍照 + Vision LLM 分析
    │
    └── 持续无运动 (>30s)
        └── Live2D 角色回到 idle 表情
```

---

## 6. Vision LLM 集成

### 6.1 拍照+分析流程

```c
// vision_agent.h

typedef struct {
    char *description;       // 场景描述
    char *emotion;           // 检测到的情感 ("happy"/"sad"/...)
    char *objects[10];       // 检测到的物体列表
    int   objects_count;
    float confidence;        // 置信度
} vision_analysis_t;

// 拍照并分析
esp_err_t vision_analyze_scene(vision_analysis_t *result);

// 拍照并分析用户表情
esp_err_t vision_analyze_user_expression(vision_analysis_t *result);

// 释放结果
void vision_analysis_free(vision_analysis_t *result);
```

### 6.2 Vision API 请求格式

```json
POST /v1/chat/completions
{
    "model": "gpt-4o-mini",
    "messages": [
        {
            "role": "user",
            "content": [
                {
                    "type": "text",
                    "text": "请用中文简短描述这张图片中的场景（50字以内），并判断图中人物的情绪状态（happy/sad/neutral/surprised）。返回 JSON 格式：{\"description\": \"...\", \"emotion\": \"...\", \"objects\": [\"...\"]}"
                },
                {
                    "type": "image_url",
                    "image_url": {
                        "url": "data:image/jpeg;base64,{BASE64_JPEG_DATA}"
                    }
                }
            ]
        }
    ],
    "max_tokens": 200
}
```

### 6.3 延迟优化

| 策略 | 说明 | 节省时间 |
|------|------|---------|
| 降低分辨率 | 320×240 (而非 720p) 发送给 LLM | 上传时间 -50% |
| 降低 JPEG 质量 | quality=50 (而非 80) | 文件大小 -40% |
| 缓存分析结果 | 5 秒内重复请求使用缓存 | 0ms (命中时) |
| 异步分析 | 不阻塞主对话流程 | 感知延迟 0 |

---

## 7. MCP 工具集成

### 7.1 新增 function calling 工具

```c
// Vision 相关工具定义
static const llm_tool_def_t vision_tools[] = {
    {
        .name = "see_surroundings",
        .description = "用摄像头看看用户周围的情况，了解用户在做什么",
        .parameters = "{"
            "\"type\": \"object\","
            "\"properties\": {"
                "\"focus\": {"
                    "\"type\": \"string\","
                    "\"enum\": [\"user\", \"desk\", \"screen\", \"general\"],"
                    "\"description\": \"关注重点: user=看用户, desk=看桌面, screen=看屏幕, general=整体\""
                "}"
            "}"
        "}"
    },
    {
        .name = "take_photo",
        .description = "拍一张照片保存",
        .parameters = "{"
            "\"type\": \"object\","
            "\"properties\": {"
                "\"save\": {\"type\": \"boolean\", \"description\": \"是否保存到SD卡\"}"
            "}"
        "}"
    },
    {
        .name = "check_expression",
        .description = "查看用户当前的表情/心情",
        .parameters = "{}"
    }
};
```

### 7.2 用户交互示例

```
用户: "看看我在干什么"
  → LLM 调用 see_surroundings({ focus: "general" })
  → 摄像头拍照 + Vision LLM 分析
  → 返回: "我看到你在电脑前写代码呢，好认真哦~(●'◡'●)"
  → Live2D 角色: happy 表情, 说出回复

用户: "你觉得我现在心情怎么样？"
  → LLM 调用 check_expression()
  → 摄像头拍照 + Vision LLM 分析表情
  → 返回: "看起来你有点疲惫呢，要不要休息一下？"
  → Live2D 角色: sad 关切表情

(自动检测) 用户挥手
  → 本地 motion_detector 检测到挥手
  → EVENT_GESTURE_WAVE → Live2D happy + 挥手动作
  → 语音: "欢迎回来~"
```

---

## 8. 图像格式管线

```
摄像头 Sensor (MIPI-CSI)
    │
    │  YUV422 原始帧 (1280×720×2 = ~1.8MB)
    ▼
┌─────────────┐
│ ISP 硬件     │  RAW → YUV 转换 (如果是 RAW sensor)
└──────┬──────┘
       │
       ├──▶ [路径A: 本地检测] PPA 降采样 → 320×240 灰度 → 运动检测
       │
       └──▶ [路径B: AI 分析] JPEG 硬件编码 (质量70-80)
              │
              │  JPEG 数据 (~30-50KB)
              ▼
         Base64 编码 (~40-67KB)
              │
              ▼
         HTTPS POST → Vision LLM API
              │
              ▼
         JSON 响应 → 解析 → UI/Live2D 更新
```

---

## 9. 内存预算

| 用途 | 大小 | 说明 |
|------|------|------|
| 摄像头帧缓冲 (×2) | ~3.6 MB | 720p YUV422 × 2 (双缓冲) |
| JPEG 编码输出缓冲 | ~100 KB | 硬件 JPEG 编码输出 |
| 运动检测缓冲 | ~150 KB | 320×240 灰度 × 2 |
| Base64 编码缓冲 | ~70 KB | JPEG → Base64 |
| **合计** | **~4 MB** | 从 PSRAM 碎片预留中分配 |

---

## 10. 隐私与安全

| 措施 | 说明 |
|------|------|
| 软件开关 | 用户可在设置中完全禁用摄像头 |
| 隐私指示 | 摄像头使用时 Live2D 角色显示"眼睛睁开"动画 |
| 仅按需拍照 | 不进行持续录像, 仅在用户请求或触发时拍照 |
| 不本地存储 | 照片默认不保存, 仅临时用于 API 调用 |
| API 安全 | 图片仅发送到用户配置的 Vision API, 不存储在第三方 |

---

## 11. 对外接口

```c
// vision_service.h

esp_err_t vision_service_init(void);

// 摄像头控制
esp_err_t vision_camera_start(void);
esp_err_t vision_camera_stop(void);
bool      vision_camera_is_active(void);

// 本地检测
esp_err_t vision_motion_detector_start(void);
esp_err_t vision_motion_detector_stop(void);

// AI 分析
esp_err_t vision_analyze_scene(const char *focus, vision_analysis_t *result);
esp_err_t vision_analyze_expression(vision_analysis_t *result);

// 隐私
esp_err_t vision_set_enabled(bool enabled);
bool      vision_is_enabled(void);
```

---

## 12. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-CAM-01 | 摄像头出图 | JPEG 帧正确输出, 分辨率/帧率达标 |
| TST-CAM-02 | 运动检测 | 有人进入画面时触发事件 |
| TST-CAM-03 | 挥手检测 | 水平挥手正确识别 |
| TST-CAM-04 | 场景理解 | Vision LLM 正确描述场景 |
| TST-CAM-05 | 表情识别 | 正确识别用户情绪 |
| TST-CAM-06 | function calling | "看看我在干嘛" → 正确调用工具 |
| TST-CAM-07 | 延迟测试 | 端到端 ≤5 秒 |
| TST-CAM-08 | 隐私开关 | 禁用后摄像头不采集 |
| TST-CAM-09 | 长时间运行 (2h) | 无内存泄漏 |
| TST-CAM-10 | 同时 Live2D + 摄像头 | FPS 无明显下降 |
