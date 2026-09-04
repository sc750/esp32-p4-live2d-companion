# BSP 重构 Debug 复盘（break-loop）

> 日期：2026-09-03 ~ 09-04
> 范围：BSP 全面重构（R1~R7，commit `3db7673`..`3ae3e49`）
> 方法：RIPER-5 RESEARCH→PLAN→EXECUTE→REVIEW，每轮实测验证
> 复盘目的：沉淀根因与防复发机制，打破"修了又犯"

---

## 0. 故障时间线（重构前状态 → 根治）

```
[重构前]  屏幕挂死：CPU0 卡 panel_io_dbi_rx_param()，Task WDT
          └─ 旧待办："移除 ILI9881C ID 读取"（症状手术，已作废）
[R1]      发现真正根因 = Kconfig 断裂（非 ILI9881C 驱动问题）
[R1构建]  新故障：esp_hosted 编译失败 CONFIG_WIFI_RMT_* undeclared
          └─ 根因 = 构建环境缺 ESP_IDF_VERSION（30min 排查）
[R2]      隐藏缺陷：dsi lane 速率=0（对照官方逐字段时发现）
[R2实测]  ✅ 屏幕点亮（EK79007 分支生效，挂死消失）
[R3]      命名冲突：本层 bsp_audio_init 撞 BSP 同名函数（编译期拦截）
[R4]      相机三连坑：sizeimage=0 → QBUF EINVAL(64B对齐) → 128B 修复
[R5]      Wi-Fi 一次通过（前期 R1 的 WIFI_RMT 修复铺路）
[R7]      框架门禁 arch-check.ps1 两处 PS5.1 不兼容（vendor 判定失效）
```

---

## 1. 显示挂死（本次重构的起因，最重要）

### 五问

**① 根因类别**：第三方组件管理误用（三个错误叠加）
- 手工替换 `managed_components/espressif__esp32_p4_function_ev_board/` 目录内容（1.2.0 → 5.2.3 noglib 副本）
- 且副本的 **Kconfig 被裁剪**（丢了整个显示菜单：`BSP_LCD_TYPE_*`/`BSP_LCD_DPI_BUFFER_NUMS` 等）
- `main/idf_component.yml` 里 `lock_managed_components: true` 锁死目录防覆盖

后果链：`sdkconfig.defaults` 的 `CONFIG_BSP_LCD_TYPE_1024_600=y` 找不到 Kconfig 定义 → **被静默丢弃** → `display.h` 的 `#if CONFIG_BSP_LCD_TYPE_1024_600` 编译期落空 → 走 else 分支（800×1280 ILI9881C）→ 对不在场的 ILI9881C 发 DBI ID 寄存器读取（**该驱动读取无超时**）→ CPU0 死等 → Task WDT。

**② 为什么之前没发现**：
- Kconfig 丢弃 unknown symbol 只打一行 warning，淹没在构建输出里；**编译 100% 通过**（`#if` 为假是合法路径）
- "能用的固件"是在 VSCode 扩展完整激活环境下构建的，环境差异掩盖了问题
- 审查时只盯着 C 代码（vendor_config 约定、初始化顺序），**没有核对"编译进的是哪个面板分支"**——这个验证动作（grep sdkconfig）从没做过

**③ 真正的修复**（对比症状手术）：
- 症状手术（作废）：给 managed_components 打补丁移除 ID 读取——下次换组件又复发
- 根治（R1，commit `0cac84a`）：显式声明 `espressif/esp32_p4_function_ev_board_noglib: "5.2.*"`、删除手工组件、解除锁定、删 sdkconfig 重新生成
- 证据：`grep CONFIG_BSP_LCD_TYPE_1024_600=y sdkconfig` 命中；实机日志 `Install EK79007 LCD control panel` + `Display initialized`（[R2 实测](../../.auto-embedded/tasks/读取prd并完成phase-1开发/edits.md)）

**④ 防复发机制**：
- 约定（已 promote）：禁止手工放置 managed_components；目录内容与 dependencies.lock 不一致 = 红色警报
- 约定（已 promote）：改 Kconfig/defaults 后必须 grep sdkconfig 验证符号生效（Kconfig 失败全是静默的）
- check.py 的 SPEC 门禁已含 spec 完整性；Kconfig 符号断言可日后加进 check.py

**⑤ 同类排查**：其余 4 个 `CONFIG_BSP_*` 显示符号（DPI_BUFFER_NUMS/COLOR_FORMAT/USE_DMA2D/BRIGHTNESS_LEDC_CH）——R1 后已逐一 grep 确认生效 ✅

---

## 2. 构建环境 ESP_IDF_VERSION 缺失（R1 新故障，耗 30min）

**① 根因类别**：构建环境不完整 × Kconfig 静默失败（复合）
- 本机是 Windows 安装器布局（工具在 `C:\Espressif`），`export.bat` 永远激活失败（报 gdb tools not installed）
- 项目因此用 build.bat 手工拼 PATH——但漏了 export.bat 会导出的 `ESP_IDF_VERSION`
- `esp_wifi_remote` 的 Kconfig 用 `orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"` 按版本加载符号，变量为空 → **整个版本文件静默跳过** → `CONFIG_WIFI_RMT_*` 36 个符号消失 → esp_hosted 的 `eh_host_wifi.c` 引用 undeclared 报错

**② 为什么没发现**：reconfigure "成功"（orsource 失败不报错）；此前能编译是因为 VSCode 扩展的激活环境带全变量。

**③ 真正修复**：build.bat/build.ps1 补 `ESP_IDF_VERSION=5.5` + `ESP_ROM_ELF_DIR`（顺带消除了每次构建的 gdbinit 警告）。证据：修后 sdkconfig 命中 36 个 WIFI_RMT 符号。

**④ 防复发**：已写入 build.bat 头部注释 + 会话记忆（esp-idf-build-env-project1）+ spec 沉淀。任何 idf.py 调用必须走 build.bat 封装。

**⑤ 同类排查**：grep 全部 defaults 符号在 sdkconfig 中的存在性——R1 重建时已顺带完成（旧 LV_* 警告是 LVGL8→9 改名，无害，列入遗留清理）。

---

## 3. dsi lane 速率 = 0（R2，隐藏第二层缺陷）

**① 类别**：结构体零初始化遗漏字段。`bsp_display_config_t cfg = {};`——`phy_clk_src=0` 恰好与官方行为一致（官方也不设），但 `lane_bit_rate_mbps=0` 是致命的。
**② 为什么没发现**：零初始化合法编译；对照官方 main.cpp 逐字段 diff 才暴露（Kconfig 修复后它就是下一个炸弹）。
**③ 真正修复**：显式 `.dsi_bus.lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS`。
**④ 防复发**：约定——**照抄官方初始化时必须逐字段 diff，不凭"看起来一样"**（已沉淀 hardware spec 坑 3）。
**⑤ 同类排查**：触摸/音频/相机的 config 传参——均已逐字段对照过官方（bsp_touch_new 传 NULL 走默认、camera 用 BSP bsp_camera_start）✅

---

## 4. 音频命名冲突（R3，编译期拦截，成本最低）

**① 类别**：命名空间冲突。本层 `bsp_audio_init(void)` 撞 BSP 头文件的 `bsp_audio_init(const i2s_std_config_t*)`。
**② 为什么没发现**：写头文件时没先 grep BSP 头的既有 API；靠编译器拦截（这正是分层的好处）。
**③ 修复**：本层改名 `bsp_audio_open`（先 grep BSP 头确认无其它冲突再改）。
**④ 防复发**：约定——**在官方 BSP 之上包一层前，先 grep BSP 公开头文件的同名前缀 API**。写入 conventions。
**⑤ 同类排查**：`bsp_camera_*`/`bsp_wifi_*`/`bsp_sdcard_*`——均 grep 过 ✅（BSP 只有 `bsp_camera_start`/`bsp_sdcard_mount` 等下划线后缀名）

---

## 5. 相机三连坑（R4，同类都是"第三方驱动契约假设错误"）

### 5a. sizeimage=0
**① 类别**：V4L2 ioctl 契约假设错误——假设 G_FMT 会回填 sizeimage；esp_video 不回填。格式已相同而跳过 S_FMT 使其暴露。
**③ 修复**：`sizeimage ? sizeimage : w*h*2`（官方 Camera.cpp 即手算 RGB565 2B/px）。

### 5b. QBUF EINVAL（errno=22）
**① 类别**：缓冲区对齐假设错误。照"cache line 对齐"注释写了 64B——本工程 `CONFIG_CACHE_L2_CACHE_LINE_128B=y`，esp_video 校验 `userptr % align_size`，64B 对齐地址 `%128=64` → EINVAL。
**② 为什么没发现**：官方示例也写 64B（但其 sdkconfig 用默认 64B cache line！**抄代码连配置一起抄才有效**）。
**③ 修复**：`esp_cache_get_alignment(MALLOC_CAP_SPIRAM)` 运行时查询（128）。证据：诊断日志 `userptr=0x484c4bc0`，0x4bc0%128=64。

### 5c. 缓冲长度以 QUERYBUF 为准
**① 类别**：同 5a——USERPTR 的 QBUF 长度契约。官方把 QUERYBUF 返回的 `buf.length` 直接透传给 QBUF，我在中间替换成了自己的计算值。
**③ 修复**：REQBUFS → QUERYBUF 探明驱动要求长度 → 按它分配 → 透传。

**④ 防复发（5a/5b/5c 合并）**：调用 V4L2/驱动 ioctl：**输出字段不假设有值、长度/对齐用运行时查询、透传结构体不重填字段**。已沉淀 hardware spec 坑 1/2。
**⑤ 同类排查**：音频 PCM 缓冲由 esp_codec_dev 内部管理（无手写 DMA 分配）；DSI 帧缓冲由 BSP 管理 ✅ 全项目无其它手写 USERPTR。

---

## 6. sdkconfig 粘性压住 defaults（R4，隐蔽配置陷阱）

**① 类别**：kconfgen 语义误解。以为 defaults 里 `CONFIG_X=y` 会覆盖旧 sdkconfig 的 `# CONFIG_X is not set`——**不会**：sdkconfig 已有条目（含显式 not set）优先，defaults 只对"新"符号生效。
**② 为什么没发现**：改 defaults 后直接 build，没有验证符号。现象是 CAMERA_SC2336 仍 not set 而 kconfig 无任何警告。
**③ 修复**：删 sdkconfig 重新生成。
**④ 防复发**：约定（已 promote）——**改 defaults 必删 sdkconfig 重建**（或用 menuconfig 改运行态）。
**⑤ 同类排查**：R1 时删除 sdkconfig 恰好侥幸规避了同一问题；此后每次改 defaults（R4 摄像头段）都先删再生成 ✅

---

## 7. 框架门禁工具失效（R7，顺带发现）

**① 类别**：工具链环境假设错误 ×2。
- `arch-check.ps1` 无 BOM：Windows PowerShell 5.1 按 GBK 解析 UTF-8 → 语法解析错误
- 用了 `[System.IO.Path]::GetRelativePath`（.NET Core 2.0+/PS7 only）→ PS5.1 抛 MethodNotFound → **Get-RelPath 恒返回空 → vendor 目录判定全失效**（managed_components 里 lvgl 的 20-include 头文件被当成自家代码报违规；真实的自家代码反而漏检）
**② 为什么没发现**：门禁工具的失败被当成"项目违规"阅读，没人怀疑工具本身；且此前从未在 PS5.1 下完整跑通过它。
**③ 修复**：加 BOM；Get-RelPath 回退 `Resolve-Path -Relative`（注意返回值是字符串不能再取 .Path——第一次修复又栽了一次）。VENDOR_DIRS 补 `managed_components`/`build`。
**④ 防复发**：PowerShell 脚本必须 UTF-8 带 BOM；跨版本兼容 API（或头部 `#requires -Version 7`）。**门禁工具自己也要过门禁**：vendor 判定失效时它"全报违规"而不是"报错退出"，这种 fail-open 设计要警惕。
**⑤ 同类排查**：check.py 其余子检查（HW/SPEC）为纯 python，无此问题 ✅

---

## 8. 元教训（流程层，比单个 bug 更值钱）

1. **静默失败是嵌入式配置的最大敌人**：Kconfig unknown symbol、orsource 跳过、sdkconfig 粘性、`#if` 落空——全都不报错、全都能编译。**每个配置改动都必须有"符号级验证"动作**（grep sdkconfig / 看编译分支日志）。
2. **手工绕过包管理器 = 延迟爆炸**：手工放置 managed_components 当时省 5 分钟，之后花了整个 R1+R2 才根治。
3. **抄官方示例必须连配置一起抄**：官方 64B 对齐在其 64B cache line 配置下正确，到 128B 配置下就是炸弹。环境/配置等价性要显式验证，不能"代码一样就行"。
4. **症状手术要挂起待办而不是执行**："移除 ID 读取"如果当时直接做了，ILI9881C 分支会继续潜伏（触摸坐标 800×1280 错乱、背光脚错误 GPIO23），且掩盖根因。RIPER-5 的 RESEARCH 门禁强制"先查根因再动手"挡住了它。
5. **门禁工具会腐化**：框架的检查脚本自身也需要验证（BOM/PS 版本/API 兼容），它的"通过/失败"语义必须 fail-closed。

## 9. 防复发沉淀索引

| 沉淀位置 | 内容 |
|---|---|
| spec/hardware/index.md 坑 1~5 | 128B cache 对齐 / sizeimage 契约 / lane 速率 / sdkconfig 粘性 / C6 固件版本 |
| spec/conventions（promote） | Kconfig 符号验证 / 禁手工 managed_components / 改 defaults 删 sdkconfig / BSP 包裹前 grep API / Windows 脚本编码纪律 |
| 会话记忆 esp-idf-build-env-project1 | 构建环境配方（export.bat 不可用、ESP_IDF_VERSION 必设） |
| build.bat/build.ps1 头注释 | 环境变量原因说明（就地文档） |
| 本文件 | 全量复盘 + 证据索引 |

## 10. 遗留待办（复盘产出）

- [ ] LVGL8 时代的 `LV_*` Kconfig 条目清理（sdkconfig.defaults，当前无害仅警告）
- [ ] C6 协处理器固件 OTA 升级（esp_hosted OTA coprocessor from host）
- [ ] R6 SD 卡模块（等 SD 卡到货，15 分钟工作量）
- [ ] check.py 增加"BSP Kconfig 符号断言"门禁（验证点：CONFIG_BSP_LCD_TYPE_1024_600 等基线符号必须在 sdkconfig 中为 y）
