/**
 * @file    rig_lvgl.c
 * @brief   rig → LVGL 桥接实现（L5）：surface + lv_image + 渲染任务 + 触摸表情
 *
 * 渲染循环（独立任务，Core 1）：rig_rig_tick 取姿态 → rig_render_pose
 * 写共享 surface → adapter 锁内 invalidate。LVGL 重绘 image 时直接读
 * 同一块 ARGB8888 缓冲（无逐帧拷贝）。
 *
 * 触摸系统（R6 重构）：
 *   旧版"头部弹簧跟随触摸点"已删——2D 分层 rig 平移头部模拟转头效果太假。
 *   现在改为 手势识别：渲染任务每帧轮询 indev（v9 事件回调不可靠，R4 教训），
 *   在 adapter 锁内做命中测试 + 单击/双击/按住判定，点播 rig 表情：
 *     单击头部     → 害羞低头
 *     按住头部     → 蹭头（摸头杀）
 *     单击身体     → 惊讶
 *     双击角色     → 大笑
 *     按住身体     → 嘟嘴闹脾气
 *   角色图 lv_image 设为 CLICKABLE：按在角色上的触摸被它消费，
 *   不会冒泡到 live2d_area 触发"开始对话"；点角色外区域仍走对话流程。
 *
 * @date    2026-09-04
 * @version 3.0.0
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
#include "rig_chatter.h"    /* R11：触摸台词（摸头/戳身体立刻接一句） */
#include "lvgl_adapter_init.h"

#define TAG "rig_lvgl"

#define RIG_TASK_STACK      (6 * 1024)
#define RIG_TASK_PRIO       (4)
#define RIG_TASK_CORE       (1)     /* PRD：Live2D 渲染固定 Core 1 */
#define RIG_FPS_LOG_WIN_MS  5000

/* ---- 手势判定阈值 ----
 * 注意渲染任务 ~30fps（33ms/帧），阈值别低于 2~3 帧否则采样不到 */
#define HOLD_PAT_MS         600     /* 头部按住多久算"摸头" */
#define HOLD_POUT_MS        1200    /* 身体按住多久算"闹脾气" */
#define DOUBLE_TAP_MS       400     /* 两次单击间隔小于此 = 双击 */
#define HEAD_RATIO_PCT      35      /* 头区 = 角色顶部高度占比（与打包 neck_ratio=0.35 一致） */

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

/* ---- 手势识别状态（仅渲染任务上下文访问，无锁） ---- */
typedef struct {
    bool     down;          /* 当前帧是否处于按住状态 */
    uint32_t t_down_ms;     /* 按下时刻（算按住时长） */
    bool     on_char;       /* 按下点落在角色图上 */
    bool     on_head;       /* 按下点落在头区（y < 35% 角色高） */
    bool     hold_fired;    /* 本次按压已触发过"按住"手势（松手时不再算单击） */
    uint32_t last_tap_ms;   /* 上一次单击松手时刻（双击窗口判定） */
} gesture_t;

static gesture_t s_g;
static lv_indev_t *s_touch_indev = NULL;

/**
 * 命中测试：屏幕触点 → 是否点在角色上 + 是否在头区
 *
 * 原理：lv_image 内容以 CONTAIN 等比缩放居中在对象盒内，
 * 用整数千分比算出缩放系数和内容偏移，把触点换算回画布坐标。
 * 头区判定 = 画布 y < 35%（与 rigpack 打包时的 neck_ratio 对齐，
 * 颈线以上归头部，以下归身体）。
 *
 * @return true=点在角色上（*on_head 有效）；false=点在角色外
 */
static bool gesture_hit_test(const lv_point_t *pt, bool *on_head)
{
    *on_head = false;
    if (!s_ctx.img || !s_ctx.model) {
        return false;
    }

    /* 对象盒屏幕坐标（必须在 adapter 锁内调用，LVGL 数据） */
    lv_area_t coords;
    lv_obj_get_coords(s_ctx.img, &coords);
    int box_w = coords.x2 - coords.x1 + 1;
    int box_h = coords.y2 - coords.y1 + 1;
    if (box_w <= 0 || box_h <= 0) {
        return false;
    }

    /* CONTAIN 缩放系数（千分比整数运算，避免浮点）：
     * sp = min(盒宽/画布宽, 盒高/画布高)，内容按 sp 等比缩放 */
    const rig_model_t *m = s_ctx.model;
    int sp_w = box_w * 1000 / m->canvas_w;
    int sp_h = box_h * 1000 / m->canvas_h;
    int sp = (sp_w < sp_h) ? sp_w : sp_h;
    int content_w = m->canvas_w * sp / 1000;    /* 实际显示尺寸 */
    int content_h = m->canvas_h * sp / 1000;

    /* 内容在盒内居中（CONTAIN 语义），换算触点到画布相对坐标 */
    int off_x = (box_w - content_w) / 2;
    int off_y = (box_h - content_h) / 2;
    int rel_x = (int)pt->x - coords.x1 - off_x;
    int rel_y = (int)pt->y - coords.y1 - off_y;

    if (rel_x < 0 || rel_x >= content_w || rel_y < 0 || rel_y >= content_h) {
        return false;                           /* 点在角色图外 */
    }
    *on_head = (rel_y < content_h * HEAD_RATIO_PCT / 100);
    return true;
}

/**
 * 手势扫描：每帧在 adapter 锁内调用
 *
 * 状态机沿（按下沿/松手沿）+ 按住计时：
 *   按下沿  → 记录时刻和命中区，等待后续判定
 *   按住中  → 超过阈值触发 摸头(PAT)/闹脾气(POUT)，并每帧重复触发续期
 *             （表情时长只有 300ms，靠连续触发无缝续命）
 *   松手沿  → 按住触发过就什么都不做；否则算单击：
 *             400ms 内第二次单击 = 双击 → 大笑；
 *             第一次单击按区域分发：头=害羞，身体=惊讶
 */
static void touch_scan(uint32_t now_ms)
{
    if (!s_touch_indev) {
        return;
    }
    bool pressed = (lv_indev_get_state(s_touch_indev) == LV_INDEV_STATE_PRESSED);

    /* ---- 松手沿 ---- */
    if (!pressed) {
        if (s_g.down) {
            s_g.down = false;
            /* 按住类手势（摸头/闹脾气）松手：停止续期即可，表情自行过期 */
            if (!s_g.hold_fired && s_g.on_char) {
                /* 纯单击：先看是不是双击（400ms 内的第二下） */
                if (s_g.last_tap_ms != 0 &&
                    now_ms - s_g.last_tap_ms < DOUBLE_TAP_MS) {
                    rig_rig_trigger(RIG_EXPR_LAUGH);    /* 双击 → 大笑 */
                    s_g.last_tap_ms = 0;                /* 消费掉，防三击连触 */
                } else {
                    s_g.last_tap_ms = now_ms;
                    /* 单击按区域分发：摸头一下=害羞，戳身体=惊讶 */
                    rig_rig_trigger(s_g.on_head ? RIG_EXPR_SHY : RIG_EXPR_SURPRISE);
                }
                /* R11：被摸/被戳立刻接一句台词（chatter 内部 2.5s 冷却防刷屏） */
                rig_chatter_touch(s_g.on_head);
            }
        }
        return;
    }

    /* ---- 按住中：读触点做命中测试 ---- */
    lv_point_t pt;
    lv_indev_get_point(s_touch_indev, &pt);
    bool on_head = false;
    bool on_char = gesture_hit_test(&pt, &on_head);

    /* ---- 按下沿：记录初始状态 ---- */
    if (!s_g.down) {
        s_g.down = true;
        s_g.t_down_ms = now_ms;
        s_g.on_char = on_char;
        s_g.on_head = on_head;
        s_g.hold_fired = false;
        ESP_LOGD(TAG, "touch down: (%ld,%ld) char=%d head=%d",
                 (long)pt.x, (long)pt.y, on_char, on_head);
        return;
    }

    /* ---- 长按判定：只在"按在角色上"时生效 ----
     * 触点滑出角色就当没按着（防拖动误触发）。
     * 每帧重复 trigger = 给 PAT/POUT（300ms 短时长）续期。 */
    if (s_g.on_char && on_char && now_ms - s_g.t_down_ms >=
            (s_g.on_head ? HOLD_PAT_MS : HOLD_POUT_MS)) {
        s_g.hold_fired = true;
        rig_rig_trigger(s_g.on_head ? RIG_EXPR_PAT : RIG_EXPR_POUT);
    }
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
        /* 每帧先整面清透明再重画：头部会带着补丁层平移，不清屏的话
         * 上一帧像素会残留在新位置旁边（拖影）。（Codex R5 修复，注释补注） */
        rig_surface_clear(&s_ctx.surface);
        rig_render_pose(&s_ctx.surface, s_ctx.model, &pose);
        int64_t render_us = esp_timer_get_time() - t0;
        s_ctx.last_render_us = render_us;

        esp_lv_adapter_lock(-1);
        touch_scan(now_ms);                 /* 锁内读 indev + 做手势判定 */
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

    /* R6 新增：角色可点击。LVGL 命中规则——按在可点击子对象上的触摸
     * 由子对象消费，父容器收不到 CLICKED。效果：
     *   摸三玖本体      → 表情反应（手势由 touch_scan 轮询处理）
     *   点角色外空区域  → live2d_area 的 CLICKED 照常触发"开始对话"
     * 两个功能互不干扰。 */
    lv_obj_add_flag(s_ctx.img, LV_OBJ_FLAG_CLICKABLE);

    /* 触摸输入设备：直接从 BSP 拿注册好的 pointer indev
     * （Codex R5 改法，比遍历 lv_indev_get_next 更可靠，注释补注） */
    s_touch_indev = lvgl_adapter_get_touch_indev();
    if (s_touch_indev) {
        ESP_LOGI(TAG, "触摸设备已绑定（pointer indev）");
    } else {
        ESP_LOGW(TAG, "未找到 pointer 类型输入设备，触摸表情不可用");
    }

    esp_lv_adapter_unlock();

    memset(&s_g, 0, sizeof(s_g));           /* 手势状态归零 */
    s_ctx.model = m;
    ESP_LOGI(TAG, "角色 lv_image 就绪 (%dx%d → fit_h=%d)",
             m->canvas_w, m->canvas_h, fit_h);
    return s_ctx.img;
}

esp_err_t rig_lvgl_set_parent(lv_obj_t *parent, int fit_h)
{
    ESP_RETURN_ON_FALSE(parent && s_ctx.img && s_ctx.model,
                        ESP_ERR_INVALID_STATE, TAG, "not created");

    /* 角色搬家全程持锁：渲染任务的 invalidate 与 LVGL 布局互斥。
     * 注意本函数可能从 LVGL 任务上下文被调（状态机回调），adapter 锁
     * 是递归锁，LVGL 任务内重复加锁安全。 */
    esp_lv_adapter_lock(-1);
    lv_obj_set_parent(s_ctx.img, parent);
    /* 新容器高度不同 → 重新设盒高，CONTAIN 自动等比缩放内容并底部居中 */
    lv_obj_set_size(s_ctx.img, s_ctx.model->canvas_w, fit_h);
    lv_obj_align(s_ctx.img, LV_ALIGN_BOTTOM_MID, 0, 0);
    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "角色搬家: 新容器高度 fit_h=%d", fit_h);
    return ESP_OK;
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
