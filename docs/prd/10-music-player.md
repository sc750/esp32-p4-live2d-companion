# M10: 音乐播放器

> **优先级**: P1 | **预估工时**: 1 周 | **依赖**: M05, M06

---

## 1. 模块概述

实现 MP3 音乐播放功能，支持 SD 卡本地播放和在线流式播放，包含播放控制、歌曲信息显示、与 AI 对话的交互协调。

---

## 2. 功能需求

| ID | 需求 | 优先级 |
|----|------|--------|
| MUS-01 | SD 卡 MP3 本地播放 | P0 |
| MUS-02 | 播放/暂停/上一首/下一首 | P0 |
| MUS-03 | 播放进度显示 | P0 |
| MUS-04 | 音量调节 | P0 |
| MUS-05 | 播放列表管理 | P1 |
| MUS-06 | 随机播放/循环播放 | P1 |
| MUS-07 | 在线音乐流 (可选) | P2 |
| MUS-08 | 歌词显示 (可选) | P2 |
| MUS-09 | Live2D 角色听音乐跳舞 | P1 |
| MUS-10 | AI 语音控制音乐 | P1 |

---

## 3. 架构

```
┌─────────────────────────────────┐
│         Music Service            │
│  ┌────────────┐ ┌────────────┐  │
│  │ Playlist    │ │ Player     │  │
│  │ Manager     │ │ Controller │  │
│  └──────┬─────┘ └──────┬─────┘  │
│         │               │        │
│  ┌──────▼───────────────▼─────┐  │
│  │      Audio Pipeline         │  │
│  │  File → MP3 Decode →       │  │
│  │  Resample → Volume →       │  │
│  │  Mixer → I2S TX            │  │
│  └────────────────────────────┘  │
└─────────────────────────────────┘
```

---

## 4. 数据结构

```c
typedef struct {
    char title[64];
    char artist[64];
    char file_path[128];
    int  duration_ms;
} song_info_t;

typedef enum {
    PLAY_MODE_SINGLE,    // 单曲循环
    PLAY_MODE_LIST,      // 列表顺序
    PLAY_MODE_RANDOM,    // 随机
} play_mode_t;

typedef struct {
    song_info_t songs[100];    // 最多 100 首
    int count;
    int current_index;
    play_mode_t mode;
    bool is_playing;
    int  position_ms;
} playlist_t;
```

---

## 5. AI 语音控制

通过 LLM Function Calling 集成：

```c
// 用户说 "播放周杰伦的晴天"
// LLM → tool_call: play_music({ query: "周杰伦 晴天" })
// → 本地搜索 SD 卡匹配歌曲
// → 开始播放

// 用户说 "下一首"
// LLM → tool_call: next_song()
// → 切换到下一首

// 用户说 "声音小一点"
// LLM → tool_call: adjust_volume({ level: current - 20 })
```

---

## 6. Live2D 联动

| 播放状态 | 角色表情 | 角色动作 |
|---------|---------|---------|
| 播放中 | happy | 随音乐轻轻摇摆 (dance) |
| 暂停 | normal | idle |
| 无音乐 | normal | idle |
| 切歌 | surprised | 短暂惊讶后恢复 |

---

## 7. 音频流协调

```
音乐播放优先级: 1 (低于 TTS 优先级 3)

当 TTS 开始播放时:
  ├── 方案 A: 音乐暂停, TTS 结束后恢复
  └── 方案 B: 音乐降低音量 (ducking), TTS 结束后恢复

推荐方案 A (简单可靠)
```

---

## 8. 对外接口

```c
// music_service.h

esp_err_t music_service_init(void);

// 播放控制
esp_err_t music_play(const char *file_path);
esp_err_t music_play_song(int index);
esp_err_t music_pause(void);
esp_err_t music_resume(void);
esp_err_t music_stop(void);
esp_err_t music_next(void);
esp_err_t music_prev(void);

// 音量
esp_err_t music_set_volume(int volume);

// 播放列表
esp_err_t music_scan_sd_card(const char *dir_path);
esp_err_t music_set_play_mode(play_mode_t mode);

// 状态查询
bool      music_is_playing(void);
int       music_get_position_ms(void);
int       music_get_duration_ms(void);
song_info_t* music_get_current_song(void);
```

---

## 9. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-MUS-01 | 扫描 SD 卡 | 发现所有 MP3 文件 |
| TST-MUS-02 | 播放 MP3 | 音质正常, 无杂音 |
| TST-MUS-03 | 播放控制 | 暂停/恢复/切歌正常 |
| TST-MUS-04 | 播放列表 | 顺序/随机/循环正确 |
| TST-MUS-05 | AI 语音控制 | "播放音乐" → 正确响应 |
| TST-MUS-06 | TTS 打断 | 音乐暂停 → TTS 播放 → 音乐恢复 |
| TST-MUS-07 | Live2D 联动 | 播放时角色跳舞 |
