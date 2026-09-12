# 编辑清单

| 文件 | 改动 | 验证标准 | 结果 | commit |
|---|---|---|---|---|
| user/ui/scr_music.c/.h（新增） | 音乐页全屏面板：顶栏+播控区+列表，回调注入式分层 | 编译零警告；set_state 有比对/隐藏两道闸门 | ✅ 533 行 | 本次 |
| user/ui/ui_manager.c | 注册 UI_PAGE_MUSIC 页面（init 时建页，防 navigate 黑屏） | 页面 is_created=true | ✅ | 本次 |
| user/ui/ui_bridge.c/.h | 新增 navigate / set_music_state / set_music_volume / set_music_playlist 四个持锁桥 | 任意任务可安全调 | ✅ | 本次 |
| user/ui/scr_home.c/.h | 状态栏右侧新增音乐入口按钮（右组容器防 SPACE_BETWEEN 挤中间）+ 入口回调 | 按钮 0x2C3E50 深灰底白图标（防主题隐身坑） | ✅ | 本次 |
| user/main_app.c | 编排接线：on_music_entry/back/ctrl + music_ui_load_playlist + 主循环 1s 状态喂送 | 须在 music_service_init 之后接线 | ✅ | 本次 |
| user/ai/music_service.c/.h | 新增 music_get_volume / music_current_index；set_volume 夹紧并记值 | 滑块初值与真实音量一致 | ✅ | 本次 |
| tools/gen_nino_font.py | CHARSET 补音乐页文案 + 电台名用字（网络电台环境子舒缓声空间氛围量当无曲目） | 字体已含全部新字（已核对） | ✅ | 本次 |
| user/CMakeLists.txt | scr_music.c 加入编译 | 构建通过 | ✅ | 本次 |
| user/ui/ui_bridge.c/.h + main_app.c（EXECUTE 续轮） | 修复：music_ui_load_playlist 原直调 scr_music_set_playlist 未持 adapter 锁（渲染任务已在跑，真竞争）→ 补 ui_bridge_set_music_playlist 桥接 | 编译零警告，线程约定回归一致 | ✅ | 本次 |

## 构建证据
- `cmd /c build.bat build` exit=0，零 warning / 零 error。
- project1.bin 0x586840 B，app 分区余 39%。
