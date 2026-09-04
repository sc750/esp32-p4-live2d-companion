# M03 PainterEngine 移植与 Live2D 渲染（Phase 2 渲染管线）

## 需求 / 验收标准

### R1 — rigbin 资产格式 + 打包工具
- [x] rigbin v1 二进制格式定义：32B 头 + 36B×图层表 + RGBA8888 atlas
- [x] `tools/rigpack.py` 三模式：placeholder（程序合成）、pack（真实素材）、verify（校验打印）
- [x] placeholder 生成 6 层角色（三玖风格），verify 回读全部字段正确

### R1b — 真实素材打包
- [x] 白底抠图：flood-fill 从四角 → morphological opening → edge-band matting + 颜色反解
- [x] 变体差分切层：base 与变体像素差分 → 高斯去噪 → 掩膜外扩 → 只保留真变化区域
- [x] 头/身在 neck_ratio 处横切，head 层底部羽化防接缝
- [x] 用户三玖素材（Base.png + eyes_closed/mouth_half/mouth_open）打包成功

### R2 — rigbin 加载器
- [x] `rig_model_load_default()` 从 EMBED_FILES 加载，全链校验（magic/ver/total/layer_count/atlas_size）
- [x] atlas 分配到 PSRAM（rig_mem），alignment 按 `esp_cache_get_alignment` 运行时查询
- [x] idx_head 按名索引 "head" 层

### R3 — 渲染引擎
- [x] `rig_render_pose()` z 升序图层 blit，每像素 src-over alpha（a==0 跳/a>=255 直拷/中间定点混合）
- [x] 输出 = LVGL ARGB8888 内存布局（小端 B,G,R,A），零二次转换
- [x] 边界裁剪：图层超出 surface 范围时正确裁剪不越界

### R4 — 参数动画引擎
- [x] 呼吸：身体/头部相位差正弦位移（周期 3.4s，头部滞后 0.9s）
- [x] 眨眼：随机间隔 2.6~5.2s，闭眼补丁显示 130ms
- [x] 口型：内部演示串（4-step closed→half→open→half，150ms 步进）+ 外部驱动优先
- [x] 头部跟随：二阶弹簧（K=0.045, D=0.82），触摸时跟随，无触摸时随机游走

### R5 — LVGL 桥接
- [x] `rig_lvgl_create()` 创建 lv_image + ARGB8888 surface，首帧渲染
- [x] `rig_lvgl_start()` 启动渲染任务（Core 1，~30fps）
- [x] 触摸跟随：adapter 锁内轮询 `lv_indev_get_state/get_point`（v9 事件回调不可靠）
- [x] FPS 日志（每 5 秒打印）

### R5b — 集成 home screen
- [x] `scr_home_get_live2d_area()` 暴露 live2d_area 容器
- [x] 容器和 live2d_area 禁用滚动（拖动手势留给头部跟随）
- [x] 角色渲染在 live2d_area 内（非 lv_layer_top）

### R5c — rigpack 抠图优化
- [x] 硬阈值 d<14→α=0 清除 JPEG 振铃形成的背景环
- [x] 变体掩膜：差分 → 高斯模糊 → 阈值 → MaxFilter 外扩 → alpha 乘积

### 跨切面验收
- [ ] 编译通过（`idf.py build`，ESP-IDF v5.5.4 + noglib 5.2.*）
- [ ] 运行时角色渲染到屏幕，呼吸/眨眼/口型动画循环正常
- [ ] 触摸屏幕角色头部跟随移动，松开回到随机游走
- [ ] 渲染帧率 ≥20fps（PSRAM 250MHz，480p 画布）
- [ ] PSRAM 占用可控（atlas ≤4MB + surface ≤1.5MB）

## 约束
- ESP32-P4 双核 RISC-V @400MHz，32MB PSRAM
- LVGL v9.4，adapter 锁机制
- rig 模块（L4）零平台依赖（只用 rig_types.h + 标准 C）
- EMBED_FILES 分发 rigbin（SPIFFS 容量不足）
- Core 0 = BSP/LVGL/UI，Core 1 = rig 渲染任务
- 触摸轮询在 adapter 锁内（v9 事件回调不可靠，R4 实测教训）
