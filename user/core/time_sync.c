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

#include "esp_log.h"
#include "esp_sntp.h"

#define TAG "time_sync"

static bool s_started = false;      /* SNTP 是否已启动（幂等闸门） */

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
    /* 双服务器容灾：国内阿里云为主，国际池为备 */
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP 启动 (TZ=CST-8, ntp.aliyun.com + pool.ntp.org)");
}

bool time_sync_is_synced(void)
{
    return esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
}

bool time_sync_get_hhmm(char *buf, size_t len)
{
    if (buf == NULL || len < 6 || !time_sync_is_synced()) {
        return false;
    }
    time_t now = time(NULL);            /* SNTP 同步后系统时钟即墙钟 */
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    snprintf(buf, len, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    return true;
}
