# 编辑清单

| 文件 | 改动 | 验证标准 | 结果 | commit |
|---|---|---|---|---|
| `managed_components/espressif__esp32_p4_function_ev_board/esp32_p4_function_ev_board.c` | ~~待移除 ILI9881C 无超时 ID 读取~~ | COM41 启动日志无 Task WDT | **作废**：根因是 Kconfig 断裂误入 ILI9881C 分支，不做 managed_components 手术，改为重构依赖链（R1） | - |
