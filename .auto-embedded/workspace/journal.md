
## #1  task=读取prd并完成phase-1开发  phase=EXECUTE
break-loop: BSP 重构（屏挂死根因=Kconfig 断裂误入 ILI9881C 分支）复盘完成——8 个 bug 五问沉淀至 docs/debug/2026-09-04-bsp-refactor-postmortem.md；5 条 conventions promote（Kconfig 静默失败验证/禁手工 managed_components/改 defaults 删 sdkconfig/BSP 包裹前 grep API/Windows 脚本编码）；hardware 坑 1~5 已在 R7 入 spec

## #2  task=phase2-ui-home-polish-对话状态层与主页灵魂  phase=EXECUTE
break-loop: R6~R12 八坑复盘完成——豆腐块四连击(文案与字库不同步)根因=资产管线断链，防复发=gen_nino_font.py --verify 机械门禁(已实现,扫描上屏字符串⊆CHARSET,257字PASS)；SNTP COMPLETED瞬态/WiFi无重连/event_bus无订阅者/状态机无出口/lv_obj默认pad/测试代码残留 已promote到spec/conventions(7条)；arch-check 增 PainterEngine-master vendor 排除+lv_font_*.c 生成文件豁免
