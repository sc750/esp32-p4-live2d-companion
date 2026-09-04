/**
 * @file    rig_lvgl.c
 * @brief   rig → LVGL 桥接实现（L5）：surface + lv_image + 渲染任务 + 触摸跟随
 *
 * 渲染循环（独立任务，Core 1）：rig_rig_tick 取姿态 → rig_render_pose
 * 写共享 surface → adapter 锁内 invalidate。LVGL 重绘 image 时直接读
 * 同一块 ARGB8888 缓冲（无逐帧拷贝）。
 *
 * @date    2026-09-04
 * @version 2.0.0
 */

#include "rig_lvgl.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rig_mem.h"
#include "rig_render.h"
#include "rig_rig.h"
#include "lvgl_adapter_init.h"

#define TAG "rig_lvgl"

#define RIG_TASK_STACK      (6 * 1024)
#define RIG_TASK_PRIO       (4)
#define RIG_TASK_CORE       (1)     /* PRD：Live2D 渲染固定 Core 1 */
#define RIG_FPS_LOG_WIN_MS  5000

typedef struct {
    rig_surface_t   surface;
    lv_image_dsc_t  dsc;
    lv_obj_t       *img;
    const rig_model_t *model;
    TaskHandle_t    task;
    volatile bool   running;
    uint32_t        period_ms;
    volatile uint32_t last_render_us;
} rig_lvgl_ctx_t;

static rig_lvgl_ctx_t s_ctx;

/* ---- 触摸跟随（PRD L2D-05）：渲染任务轮询 indev 状态 ----
 * v9 的 lv_indev_add_event_cb 不广播触摸按压事件（R4 实测教训），
 * 改为在 adapter 锁内直接轮询 lv_indev_get_state/get_point。 */
static lv_indev_t *s_touch_indev = NULL;
static volatile bool s_pressed = false;
static int32_t s_scr_half_w = 512, s_scr_half_h = 300;

static void touch_scan(void)
{
    if (!s_touch_indev) {
        return;
    }
    if (lv_indev_get_state(s_touch_indev) != LV_INDEV_STATE_PRESSED) {
        if (s_pressed) {
            s_pressed = false;
            rig_rig_set_touch(0, 0, false);
        }
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(s_touch_indev, &pt);
    int16_t tdx = (int16_t)((pt.x - s_scr_half_w) * 12 / s_scr_half_w);
    int16_t tdy = (int16_t)((pt.y - s_scr_half_h) * 10 / s_scr_half_h);
    if (!s_pressed) {
        ESP_LOGD(TAG, "touch down: screen=(%ld,%ld) target=(%d,%d)",
                 (long)pt.x, (long)pt.y, tdx, tdy);
    }
    s_pressed = true;
    rig_rig_set_touch(tdx, tdy, true);
}

static void render_task(void *arg)
{
    uint32_t frames = 0;
    int64_t win_start = esp_timer_get_time();

    while (s_ctx.running) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* 姿态 → 渲染（渲染缓冲仅本任务写，LVGL 读；脏矩形整块 invalidate） */
        rig_pose_t pose;
        rig_rig_tick(now_ms, &pose);
        int64_t t0 = esp_timer_get_time();
        rig_surface_clear(&s_ctx.surface);
        rig_render_pose(&s_ctx.surface, s_ctx.model, &pose);
        int64_t render_us = esp_timer_get_time() - t0;
        s_ctx.last_render_us = render_us;

        esp_lv_adapter_lock(-1);
        touch_scan();                       /* 锁内读 indev 点位（LVGL 数据） */
        lv_obj_invalidate(s_ctx.img);
        esp_lv_adapter_unlock();

        frames++;
        int64_t now_us = esp_timer_get_time();
        if (now_us - win_start >= RIG_FPS_LOG_WIN_MS * 1000) {
            ESP_LOGI(TAG, "渲染 FPS: %lu (单帧渲染 %luus)",
                     (unsigned long)(frames * 1000000 / (now_us - win_start)),
                     (unsigned long)(s_ctx.last_render_us));
            frames = 0;
            win_start = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(s_ctx.period_ms));
    }
    vTaskDelete(NULL);
}

lv_obj_t *rig_lvgl_create(lv_obj_t *parent, const rig_model_t *m, int fit_h)
{
    ESP_RETURN_ON_FALSE(parent && m && m->loaded, NULL, TAG, "bad arg");

    /* LVGL UI 互斥：adapter 任务在跑，创建需持锁 */
    esp_lv_adapter_lock(-1);

    if (rig_surface_init(&s_ctx.surface, m->canvas_w, m->canvas_h) != ESP_OK) {
        esp_lv_adapter_unlock();
        return NULL;
    }
    rig_render_model(&s_ctx.surface, m);    /* 首帧立即呈现 */

    memset(&s_ctx.dsc, 0, sizeof(s_ctx.dsc));
    s_ctx.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_ctx.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s_ctx.dsc.header.w = (uint32_t)m->canvas_w;
    s_ctx.dsc.header.h = (uint32_t)m->canvas_h;
    s_ctx.dsc.data = s_ctx.surface.buf;
    s_ctx.dsc.data_size = (uint32_t)m->canvas_w * m->canvas_h * 4;

    s_ctx.img = lv_image_create(parent);
    if (!s_ctx.img) {
        rig_surface_deinit(&s_ctx.surface);
        esp_lv_adapter_unlock();
        return NULL;
    }
    lv_image_set_src(s_ctx.img, &s_ctx.dsc);

    /* 对象盒 = 可用高度，内容 CONTAIN 等比适配（自动缩放+居中）。
     * 注意：lv_image_set_scale 不改变对象布局尺寸，直接 scale+align
     * 会导致顶部溢出屏幕（R3 实测教训）。 */
    lv_obj_set_size(s_ctx.img, m->canvas_w, fit_h);
    lv_image_set_inner_align(s_ctx.img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(s_ctx.img, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* 触摸跟随（PRD L2D-05）：遍历输入设备，取触摸（pointer）设备供渲染任务轮询。
     * 必须过滤 type==LV_INDEV_TYPE_POINTER，否则 BSP 注册的 encoder 等会被误选。
     * scr_home 的容器滚动已在 UI 层禁用（拖动不再被滚动消费）。 */
    s_touch_indev = lvgl_adapter_get_touch_indev();
    if (s_touch_indev) {
        ESP_LOGI(TAG, "触摸设备已绑定（pointer indev）");
    } else {
        ESP_LOGW(TAG, "未找到 pointer 类型输入设备，触摸跟随不可用");
    }

    esp_lv_adapter_unlock();

    s_ctx.model = m;
    ESP_LOGI(TAG, "角色 lv_image 就绪 (%dx%d → fit_h=%d)",
             m->canvas_w, m->canvas_h, fit_h);
    return s_ctx.img;
}

esp_err_t rig_lvgl_start(int fps)
{
    ESP_RETURN_ON_FALSE(s_ctx.img && !s_ctx.task, ESP_ERR_INVALID_STATE, TAG, "not ready/running");
    int period_ms = (fps > 0 && fps <= 60) ? 1000 / fps : 33;
    s_ctx.period_ms = (uint32_t)period_ms;

    s_ctx.running = true;
    if (xTaskCreatePinnedToCore(render_task, "rig_render", RIG_TASK_STACK,
                                NULL, RIG_TASK_PRIO, &s_ctx.task,
                                RIG_TASK_CORE) != pdPASS) {
        s_ctx.running = false;
        ESP_LOGE(TAG, "创建渲染任务失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "渲染任务启动 (Core %d, ~%dms 节拍)", RIG_TASK_CORE, period_ms);
    return ESP_OK;
}
