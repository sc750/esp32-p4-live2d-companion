# M04: PPA 图层融合

> **优先级**: P0 | **预估工时**: 1-2 周 | **依赖**: M02, M03

---

## 1. 模块概述

利用 ESP32-P4 的 PPA (Pixel Processing Accelerator) 硬件加速器，将 PainterEngine 输出的 ARGB8888 Live2D overlay 与 LVGL 输出的 RGB565 UI 背景实时融合，输出到 MIPI-DSI 帧缓冲。目标是零 CPU 开销的图层合成。

---

## 2. PPA Blend 架构

### 2.1 数据流

```
┌─────────────────┐
│ LVGL 渲染完成     │ ──▶ RGB565 帧缓冲 (BG Layer)
│ (RGB565)         │
└─────────────────┘
                         │
                    ┌────▼────┐
                    │ PPA Blend│ ──▶ 融合后帧缓冲 → MIPI-DSI DMA
                    │ 硬件融合  │
                    └────┬────┘
                         │
┌─────────────────┐      │
│ Live2D 渲染完成   │ ──▶ ARGB8888 overlay (FG Layer)
│ (ARGB8888)       │
└─────────────────┘
```

### 2.2 色彩模式

| 层 | 格式 | 说明 |
|----|------|------|
| BG (背景/UI) | RGB565 | LVGL 渲染输出 |
| FG (前景/Live2D) | ARGB8888 | PainterEngine 渲染输出, alpha 通道控制透明度 |
| Output | RGB565 | 融合后输出到 MIPI-DSI |

---

## 3. 功能需求

| ID | 需求 | 优先级 |
|----|------|--------|
| PPA-01 | PPA Blend 客户端注册 | P0 |
| PPA-02 | ARGB8888 → RGB565 背景融合 | P0 |
| PPA-03 | VSync 同步 (避免撕裂) | P0 |
| PPA-04 | 非阻塞异步融合 (DMA) | P0 |
| PPA-05 | 支持全屏融合 (1024×600) | P0 |
| PPA-06 | 融合延迟 ≤16ms (60fps) | P1 |
| PPA-07 | Live2D 区域脏更新 (非全屏) | P2 |

---

## 4. 实现方案

### 4.1 PPA Blend 初始化

```c
// ppa_blend_manager.h

typedef struct {
    ppa_client_handle_t client;
    ppa_blend_block_t   block;
    void               *fg_buf;       // ARGB8888 Live2D overlay
    void               *bg_buf;       // RGB565 LVGL 帧缓冲
    void               *out_buf;      // 融合输出
    SemaphoreHandle_t   done_sem;
    volatile bool       busy;
} ppa_blend_ctx_t;

esp_err_t ppa_blend_init(ppa_blend_ctx_t *ctx);
esp_err_t ppa_blend_start(ppa_blend_ctx_t *ctx);  // 启动异步融合
bool      ppa_blend_is_done(ppa_blend_ctx_t *ctx); // 检查是否完成
```

### 4.2 融合配置

```c
// PPA Blend 前景层配置 (ARGB8888 Live2D)
ppa_blend_in_layer_t fg_layer = {
    .buffer          = live2d_overlay_a,  // ARGB8888
    .w               = 1024,
    .h               = 600,
    .color_mode      = PPA_BLEND_COLOR_MODE_ARGB8888,
    .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
};

// PPA Blend 融合块配置
ppa_blend_block_t blend_block = {
    .bg_offset_x  = 0,
    .bg_offset_y  = 0,
    .fg_offset_x  = 0,
    .fg_offset_y  = 0,
    .out_offset_x = 0,
    .out_offset_y = 0,
    .w            = 1024,
    .h            = 600,
    .update_type  = PPA_BLEND_UPDATE_FG,
};
```

### 4.3 VSync 同步流程

```
VSync 中断触发
  │
  ├── 1. 检查 Live2D 渲染是否完成
  │     └── 如果完成, swap Live2D slot
  │
  ├── 2. 启动 PPA Blend (ARGB8888 overlay → 融合到输出缓冲)
  │
  ├── 3. 等待 PPA Blend 完成 (中断/信号量)
  │
  └── 4. 通知 LVGL: 可以开始渲染下一帧
        └── lv_display_flush_ready()
```

### 4.4 关键时序

```
帧时间 = 16.67ms (60Hz VSync)

├── 0.00ms  VSync 中断
├── 0.05ms  swap slot, 启动 PPA Blend
├── ~3ms    PPA Blend 完成 (1024×600 全屏)
├── 3-16ms  LVGL 在新 buffer 上渲染下一帧
└── 16.67ms 下一个 VSync

Live2D 渲染并行:
├── 与 LVGL 渲染同时进行 (不同 Core)
├── 在下一个 VSync 前必须完成
└── 目标: ≤10ms 完成一帧 Live2D
```

---

## 5. Buffer 管理

### 5.1 四缓冲方案

```
Buffer 分配 (PSRAM, 需 DMA 对齐):

LVGL Buffers (RGB565):
  buf_lvgl_a: 1024 × 600 × 2 = 1,228,800 bytes
  buf_lvgl_b: 1024 × 600 × 2 = 1,228,800 bytes

Live2D Overlay Buffers (ARGB8888):
  buf_live2d_a: 1024 × 600 × 4 = 2,457,600 bytes
  buf_live2d_b: 1024 × 600 × 4 = 2,457,600 bytes

Output Buffer (RGB565):
  buf_output: 1024 × 600 × 2 = 1,228,800 bytes (由 MIPI-DSI DMA 使用)

总计: ~8.6 MB
```

### 5.2 DMA 内存对齐

```c
// PPA 要求输入输出缓冲区按 L1+L2 cache line 对齐
esp_dma_mem_info_t dma_mem_info = {
    .extra_heap_caps = MALLOC_CAP_SPIRAM,
};

// 分配时确保对齐
esp_dma_capable_calloc(1, buf_size, &dma_mem_info, &aligned_buf);
```

---

## 6. 性能考量

| 操作 | 耗时估算 | 优化策略 |
|------|---------|---------|
| PPA Blend 全屏 (1024×600) | ~2-3ms | 硬件 DMA, 无需 CPU |
| Cache 刷新 | ~0.5ms | L2 cache line 128B 优化 |
| LVGL 渲染 (典型帧) | ~8-12ms | 仅渲染脏区 |
| Live2D 渲染 | ~5-10ms | 双核并行, 纹理压缩 |
| **帧总时间** | **~16ms** | 满足 60fps VSync |

---

## 7. 已知问题与规避

| 问题 | 条件 | 规避 |
|------|------|------|
| PPA 卡死 | P4 + TRIPLE_PARTIAL + 屏幕旋转 90°/270° | 应用 ESP LVGL Adapter 补丁 |
| 带宽不足 underrun | PSRAM 时钟 <200MHz | 确保 `SPIRAM_SPEED_200M=y` |
| Alpha 边缘锯齿 | Live2D overlay 边缘 | PainterEngine 侧做预乘 alpha |

---

## 8. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-PPA-01 | 纯 LVGL (无 Live2D) | 正常显示, α=0 全透明 overlay |
| TST-PPA-02 | LVGL + Live2D 融合 | 角色叠加在 UI 上, 半透明字幕可见 |
| TST-PPA-03 | 帧率测试 | 稳定 60fps VSync, Live2D ≥20fps |
| TST-PPA-04 | 无撕裂验证 | 快速滚动/动画无水平撕裂线 |
| TST-PPA-05 | CPU 占用 | PPA 融合期间 Core 0 CPU <10% |
| TST-PPA-06 | 长时间运行 (4h) | 无冻结/卡死 |
