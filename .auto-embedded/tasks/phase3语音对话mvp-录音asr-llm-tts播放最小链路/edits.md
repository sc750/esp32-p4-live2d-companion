# 编辑清单

| 文件 | 改动 | 验证标准 | 结果 | commit |
|---|---|---|---|---|
| M0 | LLM 文本链路：Kconfig(AI_LLM_*/AI_MIMO_*，key 经用户授权入库) + ai_http(esp_http_client+crt_bundle HTTPS, SSE 行解析) + llm_client(UTF-8 边界安全 token 流) + dialog_manager(三玖人设+10轮环形历史) + chat_console(USB-SJ 驱动直读 v3) + 字幕流式刷新+状态点 | 串口发中文 → DeepSeek 流式回复上字幕，人设在性格；整轮 ~2.4s | ✅ 实测一轮完成 问18B/答156B，回复带耳机/傲娇细节 | - |
| M1 | 装耳朵：asr_client(WAV头+分块base64+手工组包防内存翻倍) + voice_rec(2MB PSRAM 常驻缓冲,30s上限) + voice_pipeline(按住说话事件机/定时录音调试口/dialog互斥) + 字幕栏三段式麦克风按钮 | rec 4 全链路：录音256KB→ASR→LLM 177B 回复 ~3s | ✅ 管线实测贯通；识别率待用户实机验证（可能需调 ADC 增益） | - |
| M1b | 字幕栏 UI 修复（模拟器过稿）：字幕标签 984→700 定宽+WRAP+居中（原宽度+三段式布局=1296>1024 被顶出屏幕）；状态点 lv_obj_remove_style_all 剥光默认主题（顺序坑：先剥光再 set_size，反着来尺寸回退默认值）；模拟器 1024x600 复现验证布局 | 用户实测识别 OK；字幕不再溢出、状态点干净圆 | ✅ 烧录完成待用户目检 | - |
| M1c | 三玖全身修复：pack() push() 的 region 语义错（PIL crop box 被当 xywh 解，atlas_h 记了 lower 值）+ body 切割下界公式错（应为图片底部 H）——双重 bug 负负得正骗过 verify，膝盖以下内容全丢 | 合成目检：小腿袜+皮鞋完整入画 | ✅ 重打包烧录完成 | 0a1e00b |
| M2b | Codex 提交回归测试：发现并修复 UI 创建与 LVGL 渲染任务的无锁竞态（WiFi 异步化曝光的祖传时序 bug）——ui_manager_init 全程持 adapter 锁；另修 build.bat 工具链路径失效（20241119→20260121）；清理实验构建树 | 复位后看门狗 0 次、UI 完整初始化、WiFi 拿 IP、NTP 同步、24fps | ✅ 全绿 | - |
| M3 | 延迟优化三箭头：⏱ 分段计时埋点（ASR/LLM首句/TTS首块/全程）；system prompt 首句≤15字；录音单声道化（左声道抽取，上传减半，缓冲 2MB→937KB）。量化数据：ASR 6.1s(59%) / LLM 首句 2.1s / TTS 首块 2.2s，首音延迟 ≈10.4s；播放 underflows=0；句间缝 ~1.5s（短句播快于下句合成） | 数据齐备，确认 ASR 非流式为头号瓶颈 → 换流式 ASR 优先 | ✅ 实测三轮 | - |
| M4 | 讯飞流式听写适配层：xfyun_iat.c（hmac-sha256 URL 鉴权 + WS 帧协议 1280B/40ms + 结果追加拼接，官方文档逐条核对）+ esp_websocket_client 托管组件 + Kconfig 三凭据 + asr_client 运行时后端选择（讯飞失败自动回退 MiMo）| 编译零错烧录通过；看门狗 0/系统就绪/讯飞模块加载 ✓；待 key 联调 | ✅ 代码就绪等 key | - |
| M4b | 讯飞链路三连修 + 实测打通：①事件注册改私有循环（esp_websocket_register_events，默认循环收不到事件）②wss 挂 crt_bundle（同 HTTPS 教训）③组件任务栈 4KB→8KB+解析副本走堆（2KB 栈副本+cJSON 触发栈保护崩机）④时钟健全性检查（SNTP 未同步直接回退不浪费 8s 超时）。实测：鉴权通过/识别两轮全对（你好呀/你那边天气怎么样）/无崩机/ASR 5.1s | 用户实测识别准确、无崩机、TTS 播放正常 | ✅ 讯飞流式 ASR 全链路打通 | - |
| M5 | ASR 帧间隔 40ms→10ms（4 倍速上传，发送段 2.3s→0.6s 不再是瓶颈）；实测识别无劣化（你好呀，今天天气怎么样？全对）；LLM 首句 1.9s 稳定；按句预取 underflows=0。ASR 总耗时 11s 受手机热点弱网拖累（环境变量），稳定网络下预期 2~3s | 识别无劣化 + 发送段不再主导延迟 | ✅ 实测通过 | - |
