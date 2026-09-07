# 编辑清单

| 文件 | 改动 | 验证标准 | 结果 | commit |
|---|---|---|---|---|
| M0 | LLM 文本链路：Kconfig(AI_LLM_*/AI_MIMO_*，key 经用户授权入库) + ai_http(esp_http_client+crt_bundle HTTPS, SSE 行解析) + llm_client(UTF-8 边界安全 token 流) + dialog_manager(三玖人设+10轮环形历史) + chat_console(USB-SJ 驱动直读 v3) + 字幕流式刷新+状态点 | 串口发中文 → DeepSeek 流式回复上字幕，人设在性格；整轮 ~2.4s | ✅ 实测一轮完成 问18B/答156B，回复带耳机/傲娇细节 | - |
| M1 | 装耳朵：asr_client(WAV头+分块base64+手工组包防内存翻倍) + voice_rec(2MB PSRAM 常驻缓冲,30s上限) + voice_pipeline(按住说话事件机/定时录音调试口/dialog互斥) + 字幕栏三段式麦克风按钮 | rec 4 全链路：录音256KB→ASR→LLM 177B 回复 ~3s | ✅ 管线实测贯通；识别率待用户实机验证（可能需调 ADC 增益） | - |
| M1b | 字幕栏 UI 修复（模拟器过稿）：字幕标签 984→700 定宽+WRAP+居中（原宽度+三段式布局=1296>1024 被顶出屏幕）；状态点 lv_obj_remove_style_all 剥光默认主题（顺序坑：先剥光再 set_size，反着来尺寸回退默认值）；模拟器 1024x600 复现验证布局 | 用户实测识别 OK；字幕不再溢出、状态点干净圆 | ✅ 烧录完成待用户目检 | - |
| M1c | 三玖全身修复：pack() push() 的 region 语义错（PIL crop box 被当 xywh 解，atlas_h 记了 lower 值）+ body 切割下界公式错（应为图片底部 H）——双重 bug 负负得正骗过 verify，膝盖以下内容全丢 | 合成目检：小腿袜+皮鞋完整入画 | ✅ 重打包烧录完成 | 0a1e00b |
