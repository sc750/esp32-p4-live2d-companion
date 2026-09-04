# 研究发现

| 关键词 | 来源 | 摘要 | 可信度 | 状态 |
|---|---|---|---|---|
# 研究发现

| 关键词 | 来源 | 摘要 | 可信度 | 状态 |
|---|---|---|---|---|
| PE Live2D = 自有格式 | deepwiki ×PX_LiveFramework 源码问答 | PX_LiveFramework.h/.c（kernel/）+ PX_Object_Live2D 实现自研 Live2D 框架；模型二进制头为 "PainterEngineLiveDBinary"（24B 头 + 属性 + RGBA 纹理 + 图层[Delaunay 三角形/顶点/关键点] + 动画帧[payload 含平移/拉伸/旋转/冲量]）；**不解析官方 .moc3** | 高（代码佐证） | 已确认 |
| 无 moc3 转换工具 | deepwiki + zread 搜索 + WebSearch | 仓库无转换器、无示例模型文件（README 有 Live2D 截图但无模型）；PX_LiveFrameworkExport/Import 是仅有的进出接口；社区无转换教程 | 高 | 已确认 |
| 无 ESP32 移植 | zread /platform 全目录枚举 | 平台=android/fpga_gpu/harmonyos/linux/linux_embedded/linux_standalone/macos/standalone/uefi/visualos/webassembly/windows/windows_gdi；无任何 ESP32 | 高（目录枚举） | 已确认 |
| 裸机平台层参考 | platform/standalone/px_main.c + modules/ | 需实现：px_file/px_time/px_display/px_input/px_audio/px_thread/px_socket(可选)；内存走 PE 自带内存池；linux_embedded 的 framebuffer 显示层可作参考 | 高 | 可行（PRD 已计划自研） |
| 许可 BSD-3 | deepwiki（kernel/PX_Kernel.h） | 宽松，可商用可修改 | 高 | 无障碍 |
| Cubism 官方 SDK 不可行 | 官方文档 + WebSearch | Cubism Core 为闭源二进制，官方仅 Windows/Mac/Linux/Android/iOS/Web，无 ESP32/RISC-V；moc/moc3 不可互转（需 cmo3 工程） | 高 | 死路排除 |
| PRD 假设错误 | 对照 PRD 03-painterengine-live2d.md | PRD 假设"加载 moc3 + physics3.json + motion3.json + exp3.json"在 PainterEngine 下**不成立**；PRD 标注"已验证的嵌入式 Live2D 方案"与事实不符 | 高 | 需用户决策 |
| 路线决策 | 用户确认（AskUserQuestion） | 选定 **方案 C：类 Live2D**——分层 PNG + 自研轻量 rig（呼吸/眨眼/转头/口型/物理摆动），放弃 moc3；后期可升级。PainterEngine 整体排除（不认 moc3/无 ESP32 移植/无模型资产，3 项全否决）。 | 高 | 已决策 |
| INNOVATE 子决策 | 方案对比 | 自研 rig vs DragonBones C runtime（无维护良好的 C 运行时，移植成本>自研）vs Spine（商业授权）→ **自研轻量 rig**。美术资产先用程序生成 placeholder 验证管线，用户后续提供真实立绘可无缝替换 | 中 | 建议待 PLAN 确认 |
