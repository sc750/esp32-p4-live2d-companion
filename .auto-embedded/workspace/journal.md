
## #1  task=读取prd并完成phase-1开发  phase=EXECUTE
break-loop: BSP 重构（屏挂死根因=Kconfig 断裂误入 ILI9881C 分支）复盘完成——8 个 bug 五问沉淀至 docs/debug/2026-09-04-bsp-refactor-postmortem.md；5 条 conventions promote（Kconfig 静默失败验证/禁手工 managed_components/改 defaults 删 sdkconfig/BSP 包裹前 grep API/Windows 脚本编码）；hardware 坑 1~5 已在 R7 入 spec

## #2  task=phase2-ui-home-polish-对话状态层与主页灵魂  phase=EXECUTE
break-loop: R6~R12 八坑复盘完成——豆腐块四连击(文案与字库不同步)根因=资产管线断链，防复发=gen_nino_font.py --verify 机械门禁(已实现,扫描上屏字符串⊆CHARSET,257字PASS)；SNTP COMPLETED瞬态/WiFi无重连/event_bus无订阅者/状态机无出口/lv_obj默认pad/测试代码残留 已promote到spec/conventions(7条)；arch-check 增 PainterEngine-master vendor 排除+lv_font_*.c 生成文件豁免

## #3  task=phase2-ui-home-polish-对话状态层与主页灵魂  phase=EXECUTE
finish-work: phase2-ui-home-polish 收官——对话=主页状态层(Chat页拆除)/闲聊轮播+触摸台词/SNTP真时钟+变灰策略/WiFi开关+自动重连/自检音拆除 全部验收通过(用户确认'正常了')；check.py 三门 ALL PASS；7条spec promote+--verify字体门禁落地。回退点链: 60333c9→6eda82d→f706ea1→e993468→d9eae0b→cbffbba→384dc6c→ad205fe→4a5ab8a。下一步: 用户准备进入下一阶段(Phase 3 语音对话/M05音频+M07 AI Agent 或其他,待用户定)。

## #4  task=phase3语音对话mvp-录音asr-llm-tts播放最小链路  phase=EXECUTE
break-loop: 三玖膝盖截断=push() region 语义错(PIL crop box 当 xywh 解)+body 下界公式错,双重负负得正骗过 verify(atlas_h 碰巧等于正确值,内容只有192/336行);修复=push 语义理正+下界改H;防复发=rigpack pack() 末尾 selfcheck_body_reach 机械自检(body 内容必须触达 canvas 底部)已实现+promote conventions

## #5  task=phase3语音对话mvp-录音asr-llm-tts播放最小链路  phase=EXECUTE
break-loop: Phase3 语音收官八坑复盘——①esp_websocket_client 私有事件循环（注册到默认循环永远收不到）②wss 每组件独立挂 crt_bundle ③组件默认栈 4KB 爆栈（task_stack 8KB + 解析副本走堆）④CONFIG_SPIRAM_USE_MALLOC 未启用（cJSON 720KB 响应内部堆放不下）⑤MiniMax hex≠base64 ⑥空白句穿透 ⑦事件组残留松手沿 ⑧USB 接触不良掉枚举。前三条+④已 promote 到 spec/conventions；机械门禁超时 120→300s + VENDOR_DIRS build_* 通配

## #6  task=phase3语音对话mvp-录音asr-llm-tts播放最小链路  phase=EXECUTE
finish-work: Phase3语音对话MVP收官——三后端全链路实测通过（讯飞流式ASR识别全对/DeepSeek流式回复人设在线/MiniMax TTS 184K样本播报正常/口型同步OK/按句预取零欠载）；首音延迟热点环境约10s（ASR 6s为主导，稳定网络预期2~3s）；六条spec promote + gen_nino_font --verify 机械门禁 + arch-check build_*通配。提交链：55a29f2→0f9651f→12f7588→3e70147→55a29f2→76a9921→2e01371→ab55977→fc1f2dd→58d9bc7→当前HEAD。下一步：换学校网/稳定路由器再测基线，或进MiniMax情绪联动/触摸表情+TTS联动等体验优化

## #7  task=phase4智能功能-长期记忆人设-日记-音乐播放  phase=EXECUTE
break-loop: Phase4 上板三坑复盘——①联网一会儿后全部网络请求挂掉(getaddrinfo 202)而 WiFi 仍显示已连接,根因=sdkconfig.defaults 里 CONFIG_LV_MEM_CUSTOM 是 LVGL v8 符号,v9 已删除→静默失效→LVGL 退回内建 TLSF,64KB 池固化进内部 SRAM(.bss.work_mem_int.0=0x10000)挤垮 esp_hosted SDIO 缓冲(dma_alloc(4608) failed→数据面永久失联,只有复位能救);修复=CONFIG_LV_USE_CLIB_MALLOC=y,.dram0.bss 110.4KB→46.4KB ②TTS ring 512KB 只够 8 秒而一条回复合成 400~795KB PCM→成片 ring write timeout;修复=2MB,send_timeouts 20→0,max_feed_gap 5698ms→0ms ③MiniMax resp_cap 768KB 不够 hex 2 倍膨胀(795KB PCM→1.59MB)→静默截断(767=768-1 是截断标志);修复=2MB,POST 收 1522839B→合成 380623 样本不再回退 MiMo。另:CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM 实测证伪(P4 的 PSRAM 堆 caps 不含 MALLOC_CAP_DMA,板子打印 PSRAM-DMA堆 空闲=0KB)。四条已 promote 到 spec/conventions。上板实测全通:记忆提取 4轮→新增4条、日记 862 字节三玖第一人称、网络电台 44100Hz 出声+停止复位 16000、音乐播放中对话抢占(TTS 出声+音乐已停)。提交 3dae0e7。下一步:/aemb:finish-work 归档。

## #8  task=phase4智能功能-长期记忆人设-日记-音乐播放  phase=EXECUTE
break-loop: Phase4 第二轮上板两坑——①music next/prev 后 task_wdt: CPU 1: music 每 5 秒刷屏、状态永远停在「播放中|2s」、music stop 也救不回(只有复位):根因=播放循环的命令轮询 while(xQueueReceive) 没 break,切歌类命令「回插队首+置 abort_cur」后循环继续,下一次 receive 立刻又取出刚回插的同一条命令再回插→队首永远取不完且循环内无 vTaskDelay→任务不让出 CPU;修复=两个分支都 break ②music pause 后 status 打「已停止」:根因=music_is_playing() 把暂停并入 false,status 只有播放中/已停止两支;修复=新增 music_is_paused() 谓词+status 三分支。两条已 promote 到 spec/conventions。M10 全命令实测通过:play/pause(已暂停)/resume/next(电台2)/prev/vol 40/stop(复位16000)/status,无 task_wdt 无 panic。提交 cd7a40e。遗留打磨项:电台暂停久了服务端断流被当成「播完」→自动换台。下一步:/aemb:finish-work 归档。
