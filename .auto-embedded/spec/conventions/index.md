# 编码与协作约定 spec（conventions）

> 本层是"怎么写才安全"的可执行约定。EXECUTE/REVIEW 必读。

## 证据优先（铁律）

没有以下任一证据，**禁止**宣称"已修好/应该没问题"：
代码位置 · 编译输出 · 测试结果 · 串口日志 · 数据手册依据 · 网表依据 · 实测波形/寄存器状态。
完成声明必须在当前回复内附"命令 + 输出 + 与验证标准的对照"。

## 复用优先

本地离线索引 → 官方文档/Context7 → 开源驱动 → 最后才自己写。不凭记忆猜接口。

## 代码规范

- 模块化：`.c` + `.h` 成对；公共 API 走 `.h`，内部函数 `static`。
- 命名前缀按层（halport_/bsp_/drv_/mw_/svc_/app_）。
- `volatile`：ISR 与主循环共享变量、MMIO 必须 `volatile`；不滥用。
- 临界区：多字节共享状态读写用临界区/原子；Cortex-M0+ 无 LDREX 注意。
- ISR 纪律：ISR 体 ≤ 20 行，只置标志/搬数据，重活交主循环；ISR 内禁 `printf`/阻塞/动态内存。
- 魔数写回注释来源（`@datasheet p.XX` / `@netlist`）。

## Git 快照

EXECUTE 每完成一个清单项 + 用户确认 → 本地 `git add <具体文件>`（**不用 -A**）→ commit；
**绝不自动 push**。敏感文件（.env/.key/.pem/*secret*/*token*/id_rsa*）暂停存档。

## 沉淀（promote 回流）
> REVIEW 阶段把每次的设计决策/约定/坑/gotcha 沉淀于此，下次自动注入。
- [坑/gotcha] Kconfig 失败全是静默的：unknown symbol 丢弃、$VAR 展开失败的 orsource 跳过、sdkconfig 旧条目压住新 defaults——每次改 sdkconfig.defaults 或换组件后必须 grep sdkconfig 验证目标符号真实生效（如 CONFIG_BSP_LCD_TYPE_1024_600=y）
- [坑/gotcha] 禁止手工放置/修改 managed_components：依赖必须由组件管理器从注册表解析；目录内容与 dependencies.lock 版本不一致即为红色警报（BSP 挂死根因）
- [约定] 改 sdkconfig.defaults 后必须删除 sdkconfig 重新生成（kconfgen：sdkconfig 已有条目含 is not set 优先于 defaults）；运行态调整用 menuconfig
- [约定] 在官方 BSP 之上包同名前缀函数前，先 grep BSP 公开头文件确认无同名 API（bsp_audio_init 撞车教训）；照抄官方初始化代码必须逐字段 diff 配置结构体（dsi lane 速率=0 教训），且连 sdkconfig 配置一起对齐（cache line 64B→128B 教训）
- [坑/gotcha] Windows 脚本编码纪律：.bat 注释只能 ASCII（cmd 按 GBK 解析，UTF-8 尾字节可吞 \r 导致解析错乱）；.ps1 必须 UTF-8 带 BOM（PS5.1 按 ANSI 读无 BOM 文件）且不得使用 .NET Core-only API（如 Path.GetRelativePath，PS5.1 无）
- [坑/gotcha] LVGL 定高容器必须显式 lv_obj_set_style_pad_all(0)：lv_obj 默认主题自带 pad=20，会把 40~56px 的状态栏/底栏撑爆（内容上溢到栏外）。已两次踩（chat/home 状态栏）。
- [坑/gotcha] 中文字符串改动必须同步 tools/gen_nino_font.py 的 CHARSET 并重跑生成字体：LVGL 内置 CJK 字库缺字不可信（缺听说开玖等），漏字=屏上豆腐块且编译不报错。跑 py tools/gen_nino_font.py --verify 可机械校验全工程字符串全覆盖。
- [坑/gotcha] esp_sntp_get_sync_status() 的 COMPLETED 是瞬态（下次请求前复位），轮询会漏判成永久未同步。判断是否同步过必须用 esp_sntp_set_time_sync_notification_cb 回调置 sticky 标志（见 core/time_sync.c）。
- [可复用模式] 网络类资源（WiFi/MQTT/HTTP）三件套范式（参考 bsp_wifi.c）：①三态状态机（DISCONNECTED/CONNECTING/CONNECTED，事件驱动更新+随时可查）②断线 esp_timer 指数退避重连（2s×n 封顶，禁止事件回调里硬重试）③用户手动开关（先关自动重连闸门再断开，防重连风暴）。
- [坑/gotcha] event_bus_post() 发出的事件没有任何 event_bus_subscribe() 订阅者时会静默蒸发（编译期无感知）。发布前必须 grep 确认订阅者存在；UI 触发状态机一律直调 app_state_machine_send_event（event_bus 留给未来的语音/系统异步事件）。
- [坑/gotcha] 状态机转移表每新增一个'状态入口'规则，必须同轮审计该状态的所有出口（失败/超时/用户取消路径）。教训：IDLE+SCREEN_TAP→LISTENING 上线时 LISTENING 没有任何可用出口（无语音管线），用户被困死。理想路径之外的路径才是出事的地方。
- [坑/gotcha] bring-up 阶段的测试代码（开机自检音、帧计数日志、周期打印）不得默认执行进产品路径：要么挂 Kconfig/编译宏开关，要么验证完成后删除。教训：0.5s 1kHz 开机提示音+相机每秒帧计数日志被用户投诉。同理 lvgl-simulator（LVGL 9.5）验证的布局代码移植回设备（LVGL 9.4）时，所用 API 必须先 grep managed_components 头文件确认存在（pad_gap 仅 9.5 有）。
- [坑/gotcha] PIL 的 crop box 是 (left,upper,right,lower) 四元组——包一层封装时严禁当 (x,y,w,h) 解（宽高必须 right-left/lower-upper 推导）。教训：rigpack push() 语义错 + body 切割下界错双重负负得正，atlas 尺寸'碰巧'正常骗过 verify，三玖膝盖以下全丢。资产/生成类工具产出后必须做端到端合成自检（渲染最终消费形态比对关键特征），容器结构校验骗得过、合成骗不过——rigpack 的 selfcheck_body_reach 是范本。
- [坑/gotcha] LVGL 对象的创建/销毁/属性修改必须持 esp_lv_adapter_lock（递归锁）。教训：ui_manager_init 无锁创建全部 UI 对象——过去靠 bsp_wifi 同步初始化阻塞 2s 的时序掩护才没炸；WiFi 初始化改异步后 UI 创建与 LVGL 渲染任务毫秒级撞车，main 在 lv_inv_area 死循环（IDLE0 饿死、看门狗每 5s 告警、板面假死）。诊断手段：task_wdt 回溯的 MEPC 用 riscv32-esp-elf-addr2line -e project1.elf 直接定位。
- [坑/gotcha] esp_websocket_client v1.x 创建私有事件循环（client->event_handle）且 dispatch 只 run 自己的循环——esp_event_handler_register 注册到默认循环的事件永远收不到（症状：connected()=1 但 EVT_CONNECTED 等待超时）。必须用组件公开 API esp_websocket_register_events() 注册。三连修警示：私有循环 + wss 证书 + 组件栈 4KB→8KB（解析回调在组件任务里跑，栈副本+cJSON 会爆栈）三者缺一连调都过不去。
- [坑/gotcha] ESP-IDF 默认 malloc() 不走 PSRAM（CONFIG_SPIRAM_USE_MALLOC 未启用时内部 SRAM 仅 ~280KB）——cJSON 解析含大字符串的 JSON（如 TTS hex 音频 ~700KB）时字符串堆分配必然失败返回 NULL。修复：sdkconfig 启用 CONFIG_SPIRAM_USE_MALLOC=y（>16KB 自动路由 PSRAM）。TTS hex 解码注意与 base64 区分（hex=2字符/字节，base64=4字符/3字节）。
- [坑/gotcha] esp_websocket_client / esp_http_client 的 TLS 配置是各自独立的——给 HTTP 挂了 crt_bundle_attach 不代表 WS 也配了。每个用 TLS 的组件都要单独挂 .crt_bundle_attach = esp_crt_bundle_attach，否则 ESP_ERR_MBEDTLS_SSL_SETUP_FAILED 拒连。组件任务栈同理：esp_websocket_client 默认 4KB，解析回调会爆栈（.task_stack = 8*1024 扩容）。
- [坑/gotcha] 事件组残留位 bug 模式：事件生产者在消费前置位（如按钮松手沿），消费者在条件检查被拒后未清除 → 位残留 → 下次消费者秒触发。修复：消费者入口处先清除所有可能残留的位（rec_until_stop_or 开头清 EVT_HOLD_STOP）。串口日志/测试产物测完立即删除，不留在项目根目录（用户明确要求）。
- [坑/gotcha] 「配置写了不等于生效」——LVGL 从 v8 到 v9 删了一批 Kconfig 符号（如 LV_MEM_CUSTOM），写了既不报错也不警告，生成的 sdkconfig 里根本没有该符号；本工程 CONFIG_LV_MEM_CUSTOM=y 因此静默失效，LVGL 退回内建 TLSF，64KB 池固化成内部 SRAM 的 .bss.work_mem_int.0，挤垮 esp_hosted SDIO 缓冲 → 联网一会儿后全部网络请求挂掉（getaddrinfo 202）而 WiFi 仍显示已连接。防复发：改完 sdkconfig.defaults 必须 grep 生成的 sdkconfig 确认符号存在；对已存在的 sdkconfig，CMake 不覆盖已有 choice 设置，需 reconfigure 或删 sdkconfig 重新生成。v9 正确写法 CONFIG_LV_USE_CLIB_MALLOC=y。
- [坑/gotcha] 「网络故障先查本地缓冲」——三条日志全都长得像网络问题（DNS 失败 getaddrinfo 202 / ESP_ERR_HTTP_CONNECT / 响应非 JSON），根因却全在本机：(a) 内部 SRAM 被 LVGL 池占掉 64KB 挤垮 esp_hosted SDIO 收发缓冲，驱动打 "dma_alloc(4608) failed; dropping read" 后数据面永久失联，只有复位能救；(b) TTS 环形缓冲 512KB 只够 8 秒而一条回复合成 400~795KB PCM，成片 "ring write timeout" 丢样；(c) MiniMax 响应缓冲 768KB 不够 hex 2 倍膨胀（795KB PCM → 1.59MB hex），收满被静默截断（日志 "响应非 JSON（共 767KB）"，767=768-1 就是截断标志）。判据：出现 "WiFi 已关联但全网络失败" 或 "响应非 JSON 且长度恰好等于缓冲容量-1"，先查本地内存与缓冲余量，别改网络代码。
- [坑/gotcha] 嵌入式「栈上放大数组」是高危模式——ESP-IDF 各任务栈默认 3~8KB，任何 >1KB 的局部数组都应显式走堆/static。本工程连踩两次：memory_entry_t out[32]（≈8.7KB）放在 8KB 控制台任务栈上，执行 mem list 即 panic；main 任务栈默认 3584B，SD/FATFS 挂载 + 长中文日志即越界（Guru Meditation Stack protection fault）。防复发：新增串口命令/服务时重新核对宿主任务栈预算；大快照一律 static 或 heap_caps_malloc(MALLOC_CAP_SPIRAM)。
- [可复用模式] 「缓冲容量按实测数据定，不按直觉」——TTS 环形缓冲与 HTTP 响应缓冲都因容量小于真实数据量而静默丢样/截断。定容量前先量真实峰值：本工程一条中文回复可合成 400~795KB PCM、hex 编码后 1.59MB。方案：SPK_RING_SIZE 2MB（≈32s 音频）、MiniMax resp_cap 2MB；并在接收循环出口加「缓冲是否已满」告警，把静默截断变成显式日志。
- [坑/gotcha] esp_audio_codec 的 simple_dec 必须显式注册解码器——只 include 头文件不够，漏调 esp_audio_dec_register_default() 与 esp_audio_simple_dec_register_default() 时，运行时才报 "AUDIO_DEC: Decoder MP3 not registered" / "Fail to open decoder MP3 ret -7"，编译期毫无提示。防复发：凡是用 esp_audio_simple_dec_open 的模块，init 里必须成对调用这两个注册函数并检查返回。
- [可复用模式] 网络音频流异常中断不自动重连——断网时若自动重连会无限重试，持续占用 esp_hosted SDIO 带宽，把并行的 LLM/TTS 请求一起拖死。方案：设 net_broken 标志区分「正常播完」与「异常中断」，异常中断只置标志不重连，由用户显式重新 play；同时 ok = !abort_cur && !net_broken，避免被当作正常结束而自动跳下一台。
- [坑/gotcha] 队列「回插队首」必须配 break——取出命令→xQueueSendToFront 回插→继续轮询，会在同一轮询里把同一条命令反复取出又回插，队首永远取不完；该循环内若无 vTaskDelay，任务从此不让出 CPU，表现为 task_wdt: CPU n: <任务名> 每 5 秒刷屏 + 状态冻结（本工程 music next 实测：音乐任务死循环，music stop 都救不回来，只有复位）。防复发：凡是「取出→放回队首」的写法，同一轮询里必须跳出（break），剩余命令留给外层阻塞接收处理。
- [可复用模式] 状态谓词别把多种状态压成一个 bool——music_is_playing() 曾把「暂停」并入 false，导致串口 status 只有「播放中/已停止」两支，pause 之后显示「已停止」与实际不符。方案：拆独立谓词（music_is_paused() = playing && paused），保留 is_playing 既有语义不动，显示层再按多个谓词分支。适用于任何「一个 bool 表达多种状态」的 API。
