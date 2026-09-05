# 编辑清单

| 文件 | 改动 | 验证标准 | 结果 | commit |
|---|---|---|---|---|
| P0 | 字体修复：gen_nino_font.py CHARSET 补 角/色/触/摸（主页字幕豆腐块根因=语料字集不全家桶） | 主页字幕完整中文 | ✅ 已烧板 | 60333c9 |
| P1 | 拆除独立 Chat 页：scr_chat.c/.h 删除；ui_manager 只留 HOME；状态机删 IDLE→LISTENING 入口（对话=主页状态层，brainstorm 决策）；点空白留白 | 编译零错 + grep 无 scr_chat 残留 + 点空白无反应 | ✅ 编译过 | 6eda82d |
| P2 | 闲聊轮播：rig_chatter.c/.h（L4，16 句三玖语料分 4 时段桶，3~8min 随机）；rig_rig_speak() 口型按需串；ui_bridge.c/.h（L5 锁内 UI 更新薄桥）；main_app 编排接线 | 语料字库全覆盖（127 字复核 0 缺失）+ 启动日志引擎就绪 | ✅ 编译烧录通过 | 本轮 |
| P3 | SNTP 真时钟：core/time_sync.c/.h（TZ=CST-8 双 NTP 容灾）+ 主循环分钟变化刷状态栏（断网 --:--） | 启动 8.9s 日志 SNTP 启动（IP 后 0.8s）；状态栏真时间待目检 | ✅ 日志证实 | 本轮 |
