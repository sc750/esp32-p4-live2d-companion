# edits — Phase 4 智能功能

> trace_id=T-phase4智能功能-长期记忆人设-日记-音乐播放

## MS1 — M08 长期记忆与人设

| 轮次 | commit | 改动 | 验证 |
|------|--------|------|------|
| R1 | 4e6d1ef | memory_store.c/.h 新增（200 条 PSRAM 常驻 + SPIFFS 原子落盘 + CRUD + 关键词搜索 + 去重） | 编译绿+门禁绿 |
| R2 | fc65922 | persona.c/.h 新增（persona.json 外置+默认三玖兜底）；dialog_manager 动态 system prompt（人设+记忆 top-8+时间） | 编译绿+门禁绿 |
| R3 | a1875da | memory_extract.c/.h 新增（攒 4 轮就地 LLM 提取；对话空隙同步执行规避 mbedTLS 并发） | 编译绿+门禁绿 |
| R4 | 4b913d2 | 串口 mem 命令族（list/add/del/clear）+ memory_store_clear() | 编译绿+门禁绿 |

## MS2 — M09 日记

| 轮次 | commit | 改动 | 验证 |
|------|--------|------|------|
| R5 | fc8086f | diary_service.c/.h（素材=当日记忆+轮数 → LLM 第一人称 → SPIFFS + index.json；22:00 每小时巡检触发）；ai_http 全局网络互斥锁；串口 diary now/list/read | 编译绿+门禁绿 |

## MS3 — M10 音乐

| 轮次 | commit | 改动 | 验证 |
|------|--------|------|------|
| R6-7 | 7fd8bd1 | music_service.c/.h（SD 扫描 + esp_audio_simple_dec feed 解码 + 单播放任务命令队列 + 按文件采样率切 I2S + 停止复位 16k）；bsp_init 封装 bsp_sd_mount；串口 music 命令族；TTS/按住说话抢占音乐 | 编译绿+门禁绿 |

## MS3-b — 网络流音源（用户无 SD 卡，2026-09-10 决策）

**背景**：用户手头没有 SD 卡，M10 需要替代音源。用户选择「在线流播放」+ 由我方找免费源。

**已选音源（curl 实测 200 OK，Icecast 2.4.0）**：
```
http://ice1.somafm.com/groovesalad-128-mp3    # 环境电子
http://ice1.somafm.com/lush-128-mp3           # 人声氛围
http://ice1.somafm.com/dronezone-128-mp3      # 空间氛围
```

**关键设计决策**：
1. **网络流走独立 esp_http_client，不进 ai_http 全局网络锁** ——
   音乐流是长期持有连接（播歌期间持续读），若持锁会把 LLM/TTS 卡到超时。
2. **默认频道用明文 http://** —— 明文不占用 mbedTLS 硬件加速器，
   与 LLM/TTS 的 HTTPS 天然不冲突（PRD 2.2 那条"TLS 并发崩"约束只约束 TLS 握手）。
   https:// 音源仍支持，但注释注明"仅在语音空闲时使用"。
3. **曲目表统一抽象**：本地文件（SD）+ 网络频道同列一张表，
   `music play <n>` 即可选台；网络无限流永不 EOF，`play_one` 内部循环直到 stop/切歌，
   与既有「播完自动下一首」逻辑自然兼容（电台永不触发"下一首"）。

**验证标准**：`music list` 显示 3 个电台 → `music play 0` 出声 → `music stop` 停 →
按住说话音乐立停且 TTS 正常出声（抢占 + 采样率复位）。

**停止条件**：电台流在板子上连不上（DNS/明文被拦）→ 回退到「内嵌固件」方案。

---

## 上板实测（COM41，2026-09-10）

### 第一轮（基础链路）

| # | 项 | 结果 |
|---|-----|------|
| 1 | 启动全链路（SPIFFS→记忆→日记→音乐→人设→对话） | ✅ 通过 |
| 2 | persona.json 外置生效 | ✅ 日志「人设加载: 中野三玖（907 字节）」 |
| 3 | `mem add` → `mem list` | ✅ 入库 #1 [事实] 用户喜欢科幻电影 |
| 4 | **记忆跨重启持久化** | ✅ 烧录重启后 `mem list` 仍读到 #1 |
| 5 | `music list` / `diary list` | ✅ 空列表正常返回（无卡/无日记） |
| 6 | SD 卡挂载 | ⏳ 用户手头无 SD 卡（`sdmmc_init_ocr ... 0x107` 超时），走网络电台替代方案 |
| 7 | WiFi / 摘要提取 / 日记生成 | ⏳ 首轮因热点未开失败，第二轮已全通（见下） |

### 第二轮（网络恢复后，commit 3dae0e7 修复前）

| # | 项 | 结果 |
|---|-----|------|
| 8 | **M08 摘要提取** | ✅ `提取完成: 4 轮素材 → 新增 4 条记忆（4336 ms）`；入库 #2 画像"用户叫小陈陈"、#3 机器人项目、#4 喜欢吃拉面、#5 养猫叫咪咪咪 |
| 9 | **M09 日记生成** | ✅ `/spiffs/data/diary/2026-09-10.json (862 字节)`，三玖第一人称、带 `(｡･ω･｡)`；`diary list` / `diary read` 均正常 |
| 10 | **M10 网络电台** | ✅ `网络音源已连接: groovesalad-128-mp3` → `采样参数: 44100Hz/16bit/2ch` → `music stop` → `16000Hz` 复位 |
| 11 | TTS 播放 | ⚠️ 出声但成片丢帧（`send_timeouts=20`）→ 见 B2' |
| 12 | MiniMax TTS | ⚠️ 每次都失败回退 MiMo（`响应非 JSON（共 767KB）`）→ 见 B3' |

### 第三轮（修复后，commit 3dae0e7）

| # | 项 | 结果 |
|---|-----|------|
| 13 | **音乐播放中对话抢占** | ✅ `music: 语音会话开始，音乐已停` + 采样率复位 16000 + TTS 出声 + `music status` → 已停止 |
| 14 | **TTS 丢帧消除** | ✅ `buffered=1014992B`，`underflows=0, send_timeouts=0, max_feed_gap=0ms`（改前 send_timeouts=20 / gap=5698ms） |
| 15 | **MiniMax 走通** | ✅ `POST 响应: HTTP 200, 收 1522839B` → `MiniMax 合成: 380623 样本`，不再回退 MiMo |
| 16 | 长时稳定性 | ✅ 连续 3 轮联网对话 + 播歌，无 `dma_alloc failed`、无 `getaddrinfo 202` |

### 第四轮（M10 全命令覆盖，commit cd7a40e 修复后）

| 命令 | 结果 |
|------|------|
| `music play 0` | ✅ 播放中，44100Hz/16bit/2ch |
| `music pause` | ✅ 已暂停 |
| `music status`（暂停态） | ✅ `已暂停: 网络电台1 环境电子 \| 2s`（B5' 修复前打的是"已停止"） |
| `music resume` | ✅ 继续播放 |
| `music next` | ✅ 切至 电台2 舒缓人声，**全程无 task_wdt**（B4' 修复前死循环） |
| `music prev` | ✅ 切回 电台3 → 电台2（idx_prev 正确） |
| `music vol 40` | ✅ 音量已设 |
| `music stop` | ✅ 已停止 + 采样率复位 16000Hz |
| `music status` | ✅ `已停止（上次: 网络电台2 舒缓人声）\| 共 3 首` |

## 上板实测暴露的 Bug

### B1~B3（commit 7053d88）

| # | 现象 | 根因 | 修复 |
|---|------|------|------|
| B1 | 启动至 music 初始化即 `Guru Meditation (Stack protection fault)` | main 任务栈默认 3584B，SD/FATFS 挂载 + 长中文日志推进栈占用越界 | `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`（进 sdkconfig.defaults） |
| B2 | 执行 `mem list` 即 panic | `memory_entry_t out[32]`（≈8.7KB）放在 8KB 控制台任务栈上 | 快照改 PSRAM 堆惰性分配；控制台栈提到 12KB；其余记忆快照（dialog/extract/diary）一律 static |
| B3 | `mem add` 回执恒 `#0` | `memory_store_add` 新增路径漏回填 `*out_id`（去重分支有、新增分支无） | 新增后回填 `*out_id = e->id` |

### B1'~B3'（commit 3dae0e7）

| # | 现象 | 根因 | 修复 |
|---|------|------|------|
| **B1'** | **联网跑一会儿后所有网络请求全挂**（`getaddrinfo() returns 202` / `ESP_ERR_HTTP_CONNECT`），WiFi 却仍显示"已关联"，只有复位能救；同刻日志有 `eh_sdio: dma_alloc(4608) failed; dropping read` | `sdkconfig.defaults` 里的 `CONFIG_LV_MEM_CUSTOM` 是 LVGL **v8** 的开关，v9 已删除 → 写了等于没写 → LVGL 退回内建 TLSF，64KB 池固化成内部 SRAM 的 `.bss.work_mem_int.0`（map 实测 0x10000=65536B），把 esp_hosted 的 SDIO 收发缓冲挤没 | 改 v9 写法 `CONFIG_LV_USE_CLIB_MALLOC=y`（LVGL 内存走 IDF 堆）。证据：`.dram0.bss` **110.4KB → 46.4KB**，`work_mem_int` 从 map 消失 |
| B2' | TTS 播报成片 `TTS ring write timeout: 20480/20480B dropped`，`send_timeouts=20 / max_feed_gap=5698ms` | 环形缓冲 512KB 只够 8 秒，而一条回复就合成 400~795KB PCM，生产端远快于实时播放，一眨眼填满 | `SPK_RING_SIZE` 512KB → **2MB**（≈32 秒）。证据：`buffered=1014992B`，`send_timeouts=0 / max_feed_gap=0ms` |
| B3' | MiniMax TTS 每次必失败（`响应非 JSON（共 767KB）`），白白回退 MiMo | hex 是 2 倍膨胀，795KB PCM → hex 1.59MB，`resp_cap` 给 768KB 收满被**静默截断**（767 = 768-1 正是截断标志），cJSON 解析必败 | `resp_cap` 768KB → **2MB**。证据：`POST 收 1522839B` → `MiniMax 合成: 380623 样本` |

**附带改进**：
- `ai_http.c` 加"响应缓冲已满，JSON 很可能被截断，请调大调用方 resp_cap"告警——把"猜"变成"一眼看到"
- `memory_manager.c` 内存报告加「内部DMA堆 / PSRAM-DMA堆 空闲+最大连续块」两行（B1' 的定位刚需）

### B4'~B5'（commit cd7a40e）

| # | 现象 | 根因 | 修复 |
|---|------|------|------|
| **B4'** | **`music next` 后 `task_wdt: CPU 1: music` 每 5 秒刷屏、永久不停**；状态永远停在"播放中 … \| 2s"，`music stop` 也只回执不改状态，只有复位能救 | 播放循环里的命令轮询 `while (xQueueReceive(...))` **没有 break**：切歌类命令走到 else 分支做"回插队首"(`xQueueSendToFront`) + 置 `abort_cur`，但循环继续 —— 下一次 `xQueueReceive` 立刻又取出刚回插的同一条命令、再回插，队首命令永远取不完；该循环内没有任何 `vTaskDelay`，任务从此不让出 CPU | 两个分支都 `break`（STOP 也跳出，剩余命令留给外层 `xQueueReceive(portMAX_DELAY)`）。证据：`next` → 电台2 / `prev` → 电台2，全程无 `task_wdt` |
| B5' | `music pause` 后 `music status` 打"已停止（上次: …）"，与实际状态不符 | `music_is_playing()` 把暂停并入 `false`，而 status 只有"播放中 / 已停止"两支 | 新增 `music_is_paused()` 谓词（`playing && paused`，不动既有 `is_playing` 语义）+ status 加"已暂停"分支。证据：`已暂停: 网络电台1 环境电子 \| 2s` |

**教训（待 promote）**：
1. **嵌入式下"栈上放大数组"是高危模式**——ESP-IDF 各任务栈默认 3~8KB，
   任何 >1KB 的局部数组都应显式走堆/static，新增命令/服务时要重新核对宿主任务栈预算。
2. **"配置写了但没生效"要验证到 map/运行时，不能只看 defaults 文件**——
   LVGL 从 v8 到 v9 删掉了一批 Kconfig 符号，写了既不报错也不警告，
   生成的 sdkconfig 里根本没有该符号。**判据：改完 defaults 必须 grep 生成的 sdkconfig 确认符号存在。**
   另注：对**已存在**的 sdkconfig，CMake 不会覆盖已有 choice 项的设置，
   需 `idf.py reconfigure` 或删除 sdkconfig 重新生成；本工程当前 sdkconfig 已手工同步。
3. **"网络故障"要先看本地缓冲**——B1'/B2'/B3' 三条日志全都长得像网络问题
   （DNS 失败 / 超时 / 非 JSON），根因却分别是 SRAM 被占、环形缓冲太小、
   接收缓冲太小。判据：出现"WiFi 已关联但全网络失败"，先查本地内存与缓冲余量。
4. **队列"回插队首"必须配 `break`**——回插 + 继续轮询 = 同一条命令取出→回插
   无限循环；该循环内若无 `vTaskDelay`，任务从此不让出 CPU，表现为
   `task_wdt: CPU n: <任务名>` 刷屏 + 状态冻结。判据：凡是"取出→放回队首"
   的写法，同一轮询里必须跳出。

## 打磨期待办（MVP 之外，本轮不做）

- [ ] **网络电台暂停久了会被判成"播完"**：Icecast 服务端在暂停期间断流，
      `music_source_read` 返回 0（EOF）而非 <0，被 `play_one_track` 当作正常结束
      → 外层自动连播下一台。现象：`music pause` 30s 后 `resume`，曲子已换成下一台。
      修法：网络源的 `rd == 0` 也要区分"服务端断流"与"真的播完"（电台理论上永不 EOF）。
- [ ] 暂停期间应停止读取（当前只在暂停时 `continue`，连接会被服务端回收），
      可选：暂停超过 N 秒主动保活或允许 resume 时重连回原台。
- [ ] `music next/prev` 对网络电台=换台，对本地文件=切歌，行为差异未在帮助里说明。
- [ ] MUS-09 Live2D 跳舞 / MUS-10 AI 语音控制（依赖 MEM-07 Function Calling）。
