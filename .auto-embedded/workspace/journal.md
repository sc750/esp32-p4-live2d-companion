
## #1  task=读取prd并完成phase-1开发  phase=EXECUTE
break-loop: BSP 重构（屏挂死根因=Kconfig 断裂误入 ILI9881C 分支）复盘完成——8 个 bug 五问沉淀至 docs/debug/2026-09-04-bsp-refactor-postmortem.md；5 条 conventions promote（Kconfig 静默失败验证/禁手工 managed_components/改 defaults 删 sdkconfig/BSP 包裹前 grep API/Windows 脚本编码）；hardware 坑 1~5 已在 R7 入 spec

## #2  task=phase2-ui-home-polish-对话状态层与主页灵魂  phase=EXECUTE
break-loop: R6~R12 八坑复盘完成——豆腐块四连击(文案与字库不同步)根因=资产管线断链，防复发=gen_nino_font.py --verify 机械门禁(已实现,扫描上屏字符串⊆CHARSET,257字PASS)；SNTP COMPLETED瞬态/WiFi无重连/event_bus无订阅者/状态机无出口/lv_obj默认pad/测试代码残留 已promote到spec/conventions(7条)；arch-check 增 PainterEngine-master vendor 排除+lv_font_*.c 生成文件豁免

## #3  task=phase2-ui-home-polish-对话状态层与主页灵魂  phase=EXECUTE
finish-work: phase2-ui-home-polish 收官——对话=主页状态层(Chat页拆除)/闲聊轮播+触摸台词/SNTP真时钟+变灰策略/WiFi开关+自动重连/自检音拆除 全部验收通过(用户确认'正常了')；check.py 三门 ALL PASS；7条spec promote+--verify字体门禁落地。回退点链: 60333c9→6eda82d→f706ea1→e993468→d9eae0b→cbffbba→384dc6c→ad205fe→4a5ab8a。下一步: 用户准备进入下一阶段(Phase 3 语音对话/M05音频+M07 AI Agent 或其他,待用户定)。

## #4  task=phase3语音对话mvp-录音asr-llm-tts播放最小链路  phase=EXECUTE
break-loop: 三玖膝盖截断=push() region 语义错(PIL crop box 当 xywh 解)+body 下界公式错,双重负负得正骗过 verify(atlas_h 碰巧等于正确值,内容只有192/336行);修复=push 语义理正+下界改H;防复发=rigpack pack() 末尾 selfcheck_body_reach 机械自检(body 内容必须触达 canvas 底部)已实现+promote conventions
