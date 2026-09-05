/**
 * @file    time_sync.h
 * @brief   SNTP 时间同步（L3）——状态栏真时钟的时基
 *
 * Wi-Fi 就绪后调用 time_sync_start()（幂等，主循环轮询式触发即可），
 * 内部设 TZ=中国标准时间(UTC+8) 并向 阿里云 NTP + pool.ntp.org 发起同步。
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动 SNTP（幂等：重复调用只在首次生效）
 * 内部流程：设 TZ=CST-8 → esp_sntp_init（双服务器容灾）
 * 需在 Wi-Fi 拿到 IP 之后调用（未联网时启动也会在联网后自行同步）
 */
void time_sync_start(void);

/** SNTP 是否已完成至少一次同步 */
bool time_sync_is_synced(void);

/**
 * @brief 取格式化时间 "HH:MM"
 * @param[out] buf    输出缓冲
 * @param[in]  len    缓冲长度（≥6）
 * @return true=已同步且写入成功；false=未同步（buf 内容不变，调用方可
 *         预填 "--:--"）
 */
bool time_sync_get_hhmm(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TIME_SYNC_H */
