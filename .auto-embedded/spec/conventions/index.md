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
