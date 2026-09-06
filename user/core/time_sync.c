/**
 * @file    time_sync.c
 * @brief   SNTP 时间同步实现（L3）
 *
 * 用法（编排层 main_app）：
 *   主循环里 `if (bsp_wifi_is_connected()) time_sync_start();`——
 *   幂等，联网后首次调用即启动，之后调用是空操作。
 *
 * 时区：CST-8（中国标准时间，UTC+8），localtime_r 全家桶直接可用；
 * rig_chatter 的分时段语料也依赖这个 TZ。
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#include "time_sync.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"

#define TAG "time_sync"

static bool s_started = false;      /* SNTP 是否已启动（幂等闸门） */
static volatile bool s_synced = false;  /* 至少同步成功过一次（回调置位） */
static uint32_t s_start_ms;         /* 启动时刻（算"多久没同步"用） */
static bool s_warned;               /* 超时未同步告警只发一次 */

/**
 * SNTP 同步结果回调：成功才触发（失败靠超时推断）
 *
 * 教训（R11 实测）：esp_sntp_get_sync_status() 的 COMPLETED 是瞬态
 * （下次请求开始前就复位），1s 轮询会错过 → 状态栏永远 "--:--"。
 * 所以"是否同步过"以本回调置位的标志为准，一次性、不回落。
 */
static void on_sntp_sync(struct timeval *tv)
{
    s_synced = true;
    time_t t = tv->tv_sec;
    struct tm tm_now;
    localtime_r(&t, &tm_now);
    ESP_LOGI(TAG, "时间已同步: %04d-%02d-%02d %02d:%02d:%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
}

void time_sync_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;

    /* 时区必须在取 localtime 前设置：CST-8 = UTC+8，无夏令时 */
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_set_time_sync_notification_cb(on_sntp_sync);
    /* 双服务器容灾：国内阿里云为主，国际池为备 */
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    s_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "SNTP 启动 (TZ=CST-8, ntp.aliyun.com + pool.ntp.org)");
}

bool time_sync_is_synced(void)
{
    return s_synced;    /* 回调置位后永久有效（SNTP 周期校时维持精度） */
}

bool time_sync_get_hhmm(char *buf, size_t len)
{
    if (buf == NULL || len < 6 || !time_sync_is_synced()) {
        /* 诊断：启动 30s 还没同步成功就告警一次（多为网络拦 UDP/123） */
        if (s_started && !s_warned && !time_sync_is_synced() &&
            (uint32_t)(esp_timer_get_time() / 1000) - s_start_ms > 30000) {
            s_warned = true;
            time_t t = time(NULL);
            ESP_LOGW(TAG, "SNTP 30s 未同步（网络可能拦 UDP/123），系统时钟当前=%llds（同步前≈开机秒数）",
                     (long long)t);
        }
        return false;
    }
    time_t now = time(NULL);            /* SNTP 同步后系统时钟即墙钟 */
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    snprintf(buf, len, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    return true;
}
