/**
 * @file    music_source.h
 * @brief   音乐音源抽象（L4）——本地文件 / 网络流统一只读接口
 *
 * 引入原因：M10 的曲目来源不止 SD 卡——用户可能没有 SD 卡（本工程即如此），
 * 网络电台（Icecast 流）要能当"曲目"播。播放任务只面对一个 read 接口，
 * 不需要知道数据来自卡还是网络。
 *
 * 设计约束（重要）：
 *   - **不进 ai_http 的全局网络锁**。音乐流是长连接（播歌期间持续读），
 *     持锁会把 LLM/TTS 请求卡到超时。语音抢占由 music_service 先停音乐
 *     释放连接来保证。
 *   - **默认频道用明文 http 协议**：明文不占用 mbedTLS 硬件加速器，
 *     与 LLM/TTS 的 HTTPS 天然不冲突（PRD 2.2 的"TLS 并发崩"只约束 TLS）。
 *     https 音源可用，但仅在语音空闲时可靠。
 *
 * @date    2026-09-10
 * @version 1.0.0
 */

#ifndef MUSIC_SOURCE_H
#define MUSIC_SOURCE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_URI_MAX   (192)       /* 音源 uri（路径或 URL）上限 */

/**
 * @brief 打开音源（按 uri 前缀自动判别后端）
 *
 * @param uri  本地路径（如 /sdcard/music/a.mp3）或网络地址（http:// / https://）
 * @return 句柄；NULL = 打开失败（文件不存在/网络不可达/非 200）
 */
struct music_source *music_source_open(const char *uri);

/**
 * @brief 读一块压缩数据（阻塞，直到有数据/流结束/超时）
 *
 * @param buf        落点
 * @param len        期望字节数
 * @param timeout_ms 单次读超时（网络源用；文件源忽略）
 * @return >0 实读字节数；0 = 流正常结束；<0 = 读错误（超时/连接断）
 */
int music_source_read(struct music_source *src, void *buf, size_t len,
                      int timeout_ms);

/** 关闭并释放（幂等，NULL 安全） */
void music_source_close(struct music_source *src);

/** 是否网络源（播放侧据此放宽读超时/决定是否可重连） */
bool music_source_is_network(const struct music_source *src);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_SOURCE_H */
