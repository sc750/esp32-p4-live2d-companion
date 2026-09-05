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
| R6a | 黑边根治：extract() 边缘带 α 下限 64/255（颜色反解除数 clamp ≥0.25，数值不再爆炸） | 烧板后角色边缘无黑边 | ✅ 用户视觉确认黑边消失 | 0be4b11 前后 |
| R6b | 触摸表情系统：`rig_rig.c/.h` 删头部弹簧跟随 + 新增 `rig_rig_trigger(rig_expr_t)` 表情状态机（5 种反应表 EXPR_DEFS：害羞/蹭头/惊讶/大笑/嘟嘴，补丁层 parent==head 泛化默认隐藏）；`rig_lvgl.c` touch_scan 重写为手势识别（命中测试/单击/双击/按住，img 设 CLICKABLE 屏蔽角色区对话触发）；`rigpack.py` 变体清单扩到 7 个；`assets/art/nino/` 新增 eyes_smile/eyes_wide/mouth_pout/blush 四图 + README 手势表；Codex 代码（blit alpha 数学/每帧清屏/BSP indev getter）补中文注释 | 编译零错 + 9 层加载绑定正确 + PC 云验收（expr_preview.png）六表情合成无补丁痕 + FPS≥20 | ✅ 烧录成功 24fps，等用户实机手势验证 V3~V8 | - |
| R7 | 对话触发链路修复：scr_home 的 event_bus_post 全工程无订阅者（祖传断线）→ 改直调 `app_state_machine_send_event`（scr_chat 同款先例）；状态机 LISTENING 补 SCREEN_TAP→IDLE / NAV_HOME→IDLE 两条逃生转移（否则无语音管线时困死对话页） | 实机点角色外空白 → IDLE→LISTENING 切对话页；再点/返回按钮 → 回主页 | ✅ 编译烧录通过，等用户实机验证双向切换 | - |
| R8 | 对话页重设计（lvgl-simulator 模拟器验证布局）：`scr_chat.c/.h` 全重写（48px 状态栏+386px 角色区 flex_grow+110px 字幕+56px 底栏，pad 归零修 lv_obj 默认 pad=20 撑爆栏的坑）；`tools/gen_nino_font.py` + `user/ui/fonts/`（lv_font_conv 生成 nino_cjk_16 中文自定义字库 132 字——内置 source_han 字库实测缺 玖/听/说/开 等 20 字）；`rig_lvgl_set_parent()` 角色常驻（ui_manager 状态切换时角色在主页↔对话页无缝搬家）；`scr_home.c` 状态栏 pad 修正 + 中文字体；`ui_manager.c` 状态→页面+角色+状态点（蓝听/橙思/绿说）三联动；模拟器修复：解压 npm 包内未解压的预编译 zip + LV_FONT_SOURCE_HAN_SANS_SC_16_CJK 开启 | 编译零错；字体覆盖全部所需字形（verify 227 字形 0 缺失）；实机对话页中文正常 + 三玖在场 + 布局无空洞 | ✅ 编译烧录通过，启动干净，等用户实机目检 | - |
