# PRD — Phase 4 智能功能 MVP

> trace_id=T-phase4智能功能-长期记忆人设-日记-音乐播放
> 总原则（用户指令）：**MVP 优先，细节后面再打磨**。每个里程碑独立可实测、独立提交。

## 1. 范围（对照 PRD 需求做减法）

### M08 长期记忆与人设 —— MVP（本轮）
| 需求 | 处置 |
|------|------|
| MEM-01 存 SPIFFS | ✅ 做（`/spiffs/data/memories.json` 单文件整存整取） |
| MEM-02 对话摘要提取 | ✅ 做（回复播完后后台 LLM 提取，静默写入） |
| MEM-03 记忆检索注入上下文 | ✅ 做（关键词粗筛 top-N 注入 system prompt） |
| MEM-04 人设 System Prompt | ✅ 做（三玖人设从 C 迁到 `/spiffs/data/persona.json`，缺失自动生成默认） |
| MEM-05 CRUD API | ✅ 做（内部 API + 串口 mem 命令调试） |
| MEM-06 自动整理/遗忘 | ⏸ 打磨期 |
| MEM-07 Function Calling | ⏸ 打磨期（风险 R1） |
| MEM-08 语义搜索 | ⏸ 打磨期（MVP 用关键词子串匹配） |

### M09 日记 —— MVP
| 需求 | 处置 |
|------|------|
| DIARY-01 自动生成 | ✅ 做（SNTP 就绪后 22:00 esp_timer 触发 + 当日已有素材才生成） |
| DIARY-02 第一人称 | ✅ 做（复用 M08 persona） |
| DIARY-03 存 Flash | ✅ 做（`/spiffs/data/diary/YYYY-MM-DD.json`） |
| DIARY-04 浏览界面 | ⏸ 打磨期（MVP：串口 diary 命令触发/读取，字幕区滚动展示生成结果） |
| DIARY-05 TTS 朗读 | ⏸ 打磨期 |
| DIARY-06 历史查看 | ✅ 做（串口 diary list / diary read <date>） |

### M10 音乐播放 —— MVP
| 需求 | 处置 |
|------|------|
| MUS-01 SD 卡 MP3 播放 | ✅ 做（bsp_sdcard 挂载 + esp_audio_codec 解码） |
| MUS-02 播放/暂停/切歌 | ✅ 做（串口 music 命令族） |
| MUS-03 进度显示 | ✅ 做（串口 music status） |
| MUS-04 音量 | ✅ 做 |
| MUS-05 扫描列表 | ✅ 做（扫 `/sdcard/music/` 下 mp3/wav） |
| MUS-09 Live2D 跳舞 | ⏸ 打磨期 |
| MUS-10 AI 语音控制 | ⏸ 打磨期（依赖 MEM-07） |
| TTS 打断协调 | ✅ 做（MVP：按住说话即 music_stop，不自动恢复） |

## 2. 里程碑与验证标准

### MS1 — M08 记忆人设（先行）
- 轮次 1：memory_store 模块（SPIFFS 读写 + CRUD + 关键词搜索 + mutex）
- 轮次 2：persona.json 加载 + dialog_manager system prompt 重构（人设+记忆注入）
- 轮次 3：摘要提取后台任务（对话完成事件 → LLM 提取 → 去重入库）
- 轮次 4：串口 mem 命令（list/add/del）+ 实测：告诉它一件事 → 重启后仍记得
- **验收**：跨重启记忆保持；system prompt 含记忆；ARCH/HW 门禁绿

### MS2 — M09 日记
- 轮次 5：diary_service（生成/存储/读取 + 22:00 定时器 + 串口 diary 命令）
- **验收**：串口触发生成第一人称日记并落盘；重启后 diary read 可读

### MS3 — M10 音乐
- 轮次 6：SD 挂载 + esp_audio_codec 依赖引入（编译冒烟）
- 轮次 7：music_service（扫描/解码循环播放/控制命令/TTS 协调）
- **验收**：SD 卡放 MP3 → music play 出声 → 暂停/切歌/音量可用 → 按住说话音乐停

> **2026-09-10 验收标准变更（用户决策）**：用户手头无 SD 卡，选择「在线流播放」替代，
> 由我方找免费源（SomaFM Icecast 三台，明文 http 不占 mbedTLS）。
> 验收改为：`music list` 列出 3 台 → `music play 0` 出声（44100Hz/16bit/2ch）→
> play/pause/resume/next/prev/vol/stop 命令可用 → 停止复位 16000Hz →
> 播放中对话触发抢占（停音乐 + TTS 出声）。
> 详细设计见 edits.md「MS3-b 网络流音源」。

## 3. 约束

- 每轮一个改动点，编译+门禁绿才提交（本地快照，不 push）
- 所有新代码中文行尾注释 + 段注释（用户硬性要求）
- 大缓冲一律 PSRAM（SPIRAM_USE_MALLOC 已启用，>16KB 自动路由）
- 不新增引脚/DMA/IRQ；`/spiffs` 与 `/sdcard` 挂载点不与 LVGL 资源冲突
- file ≤800 行 / .h API ≤20（ARCH-5/6）

## 4. 停止条件

- MS1 实测不过（重启丢记忆）→ 停下排查持久化
- esp_audio_codec 编译失败且 WAV 回退也不可行 → 回报用户改方案
