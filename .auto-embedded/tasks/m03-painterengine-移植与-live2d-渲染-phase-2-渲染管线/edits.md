# 编辑清单

| 文件 | 改动 | 验证标准 | 结果 | commit |
|---|---|---|---|---|
| R1 | `tools/rigpack.py`（placeholder/pack/verify 三模式，rigbin v1 格式：32B 头 + 36B×图层表 + RGBA8888 atlas）+ `assets/art/nino/README.md` 素材规范 | placeholder 生成 + verify 读回 6 图层全部字段正确 | ✅ 1.2MB rigbin，呆毛带 physics 标志，atlas 24% 非透明 | - |
| R1b | rigpack 真实素材适配：base.{png,jpg} 大小写兼容、max_height 降采样（800px）、白底阈值可配、变体按自身 bbox 对齐 + 差分高斯去噪 + 头区限制 + parent=head；用户三玖素材打包成功（5 层 3.6MB） | 真实立绘差分切层 bbox 为脸部尺寸 | ✅ eyes_closed 193x206/mouth 202x206，verify PASS | - |
| R2 | `user/rig/rig_model.c/.h`（L4：rigbin 加载器）+ `rig_mem.c/.h`（L3：PSRAM 堆管理）+ `user/CMakeLists.txt`（EMBED_FILES） | EMBED_FILES 加载 + magic/ver/total 全链校验 + PSRAM atlas 分配 | ✅ HDR_OFF_TOTAL=22 修正后 verify PASS | - |
| R3 | `user/rig/rig_render.c/.h`（L4：z 序 blit + 每·通道 alpha，LVGL ARGB8888 直出布局）+ `rig_lvgl.c/.h`（L5：lv_image 桥接）+ main_app 接入 | 屏幕显示完整拼合角色 | ✅ 用户视觉确认（首版 scale+align 顶部溢出，改 LV_IMAGE_ALIGN_CONTAIN 修复） | - |
| R4 | `user/rig/rig_rig.c/.h`（L4：参数动画引擎——呼吸/眨眼/口型/头部弹簧跟随） | 呼吸 3.4s 周期 + 眨眼 2.6~5.2s 随机 + 口型 4-step 循环 + 头部二阶弹簧 | ✅ 编译通过，动画逻辑完整 | - |
| R5 | `user/rig/rig_lvgl.c/.h` 增强：触摸跟随（adapter 锁内轮询 indev）、FPS 日志（5s 窗口）、渲染任务 Core 1 @~30fps | 触摸头部跟随 + FPS 日志输出 | ✅ lv_indev_get_state/get_point 方案（v9 事件回调不可靠） | - |
| R5b | `user/ui/scr_home.c/.h`（暴露 live2d_area + 禁用滚动）+ `main_app.c`（角色入住 live2d_area）+ `user/CMakeLists.txt`（rig_rig.c 加入 SRCS） | 角色在 home screen 的 live2d_area 内渲染，拖动不触发页面滚动 | ✅ container + live2d_area 双禁 SCROLLABLE | - |
| R5c | `tools/rigpack.py` extract() 重写：flood-fill 四角 → 开运算 → 边缘带 matting（硬阈值 d<14 + 饱和度比例）→ 颜色反解 F=(P-(1-α)W)/α；变体掩膜差分去背景残留 | 三玖素材打包后无白边、无背景残留 | ✅ rigbin 3.6MB→2.0MB（更紧凑），verify PASS | - |
