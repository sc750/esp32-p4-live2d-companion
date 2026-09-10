# RESEARCH — Phase 4 智能功能（M08 记忆人设 / M09 日记 / M10 音乐）

> trace_id=T-phase4智能功能-长期记忆人设-日记-音乐播放
> 日期：2026-09-09

## 1. 芯片/平台确认

- ESP32-P4-Function-EV-Board，双核 RISC-V @400MHz，32MB PSRAM，16MB Flash
- IDF v5.5.4，LVGL v9.4，现有 AI 链路（ASR/LLM/TTS）已实测通过（Phase 3）

## 2. 现状盘点（证据）

### 2.1 存储（M08/M09 落点）✅ 现成
- 分区表 `partitions.csv`：`storage, data, spiffs, 4MB` 已存在
- `user/bsp/bsp_init.cpp:90` 启动即 `bsp_spiffs_mount()`，挂载点 **`/spiffs`**
  （`CONFIG_BSP_SPIFFS_MOUNT_POINT="/spiffs"`，`PARTITION_LABEL="storage"`，挂载失败自动格式化）
- SPIFFS `OBJ_NAME_LEN=32`：日记文件名 `2026-09-01.json`(15B) / `memories.json`(13B) 均可
- **结论：M08/M09 无需动分区表，直接读写 `/spiffs/data/` 子目录**

### 2.2 对话链路（M08 挂接点）✅ 结构清晰
- `user/ai/dialog_manager.c`：三玖人设**硬编码在 C 里** + 10 轮 RAM 环形历史 + utf8_sanitize
- `user/ai/llm_client.c`：纯流式 chat completions，**无 tools/function calling**
- `main_app.c:192-194` 初始化链：dialog_manager → llm_client → voice_pipeline
- **结论：M08-MVP 不动 llm_client（function calling 延后），走"被动记忆"路线：
  对话后台异步摘要提取 → 存 SPIFFS → 组 system prompt 时注入记忆上下文**

### 2.3 SD 卡（M10）✅ BSP 现成
- `esp32_p4_function_ev_board.h:302-381`：`bsp/esp32_p4_function_ev_board.h` 提供
  `bsp_sdcard_mount()`（推定名，头文件含 sdmmc_host/slot 获取 + handle 函数族）
- FATFS 已配置：`SECTOR_4096`、`LFN heap`（长文件名 OK）、`VOLUME_COUNT=2`
- **结论：M10 直接调 BSP 挂载，挂载点 `/sdcard`**

### 2.4 MP3 解码（M10）⚠️ 唯一缺口
- managed_components **无任何 MP3 解码器**
- 官方组件 `espressif/esp_audio_codec`（v2.6.2）提供 `esp_audio_simple_decoder`
  统一接口（MP3/AAC/AMR/G711/ADPCM…），纯软解不依赖 ADF
- 添加：`idf.py add-dependency "espressif/esp_audio_codec"`
- 音频输出复用 `bsp_audio_play()` + `bsp_audio_set_fs()`（Phase 3 已验证 16bit PCM 路径）

### 2.5 触发器/时钟（M09）
- `user/core/time_sync.c`：SNTP 真时钟已就绪（sticky 同步标志）→ 22:00 定时触发可信
- esp_timer 已在 bsp_wifi 重连中活跃使用

## 3. 风险清单

| # | 风险 | 缓解 |
|---|------|------|
| R1 | DeepSeek `deepseek-v4-flash` 是否支持 tools 未实测 | MVP 不走 function calling（被动记忆），无此依赖 |
| R2 | SPIFFS 4MB 容量：500 条记忆×256B≈128KB + 日记 365×2KB≈730KB，够用但须防碎片 | 记忆单文件 JSON 整存整取；日记按日期一文件 |
| R3 | esp_audio_codec 组件与 P4 工具链兼容性 | 第一轮先编译冒烟，失败则回退 WAV 直播方案 |
| R4 | 音乐与 TTS 抢 codec | MVP：对话开始时先 music_stop()（PRD 方案 A 简化版） |
| R5 | LLM 摘要调用占用网络时机（与 TTS 播放并发） | 摘要在回复播放完后后台任务执行，与下轮录音天然错峰 |

## 4. 相关性选择

- builder ← `spec/architecture/index.md`（分层约束）、`spec/conventions/index.md`（注释/PSRAM/门禁）
- verifier ← `spec/architecture/index.md`、`spec/hardware/index.md`（无新引脚，HW 门禁核对）
- Scout 阶段已完成（本文档），不另派

## 5. 引脚/硬件资源变更

**无新增引脚/DMA/IRQ/timer** —— SDMMC 用 BSP 预定义 slot（board 固定走线），
SPIFFS/esp_timer 均为已注册资源。hw-lock.yaml 无需改动。
