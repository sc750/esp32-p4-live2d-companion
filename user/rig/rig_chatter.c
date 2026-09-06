/**
 * @file    rig_chatter.c
 * @brief   三玖闲聊轮播实现（L4）
 *
 * 语料风格：三玖式傲娇+温柔（口是心非、关心藏在别扭里）。
 * 改文案须知：
 *   1. 直接改下面的语料表即可（static const，存 flash）
 *   2. 新出现的汉字必须先加进 tools/gen_nino_font.py 的 CHARSET
 *      并重新生成字体（py tools/gen_nino_font.py），否则屏幕上就是豆腐块
 *   3. 单句别超过 ~40 字（字幕区两行放不下会截断）
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#include "rig_chatter.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
#include "esp_timer.h"

#define TAG "rig_chatter"

/* ---- 节奏参数 ---- */
#define CHATTER_MIN_GAP_MS  (3 * 60 * 1000)     /* 最短间隔 3 分钟 */
#define CHATTER_MAX_GAP_MS  (8 * 60 * 1000)     /* 最长间隔 8 分钟 */
#define CHATTER_STARTUP_QUIET_MS  (60 * 1000)   /* 开机头 1 分钟静音 */

/* ---- 分时段 bucket ---- */
typedef enum {
    BUCKET_MORNING = 0,     /* 早晨 05:00~10:59 */
    BUCKET_DAY,             /* 白天 11:00~16:59 */
    BUCKET_EVENING,         /* 晚间 17:00~22:59 */
    BUCKET_NIGHT,           /* 深夜 23:00~04:59 */
    BUCKET_COUNT
} chatter_bucket_t;

/** 单句语料：text + 适用时段（1<<bucket 的位集合，方便一句多时段复用） */
typedef struct {
    const char *text;
    uint8_t     buckets;
} chatter_line_t;

#define B_MORNING   (1u << BUCKET_MORNING)
#define B_DAY       (1u << BUCKET_DAY)
#define B_EVENING   (1u << BUCKET_EVENING)
#define B_NIGHT     (1u << BUCKET_NIGHT)

/**
 * 语料表（三玖味：傲娇+温柔，关心藏在别扭里）
 * 时段边界见 chatter_bucket_from_hour()
 */
static const chatter_line_t LINES[] = {
    /* 早晨 */
    { "早安……才、才不是特意等你醒来的呢。",            B_MORNING },
    { "今天也要加油哦，我会看着你的。",                B_MORNING | B_DAY },
    { "困了就再眯一会儿，我哪儿也不去。",              B_MORNING },
    /* 白天 */
    { "工作学习要记得休息，眼睛看坏了我可是会心疼的。", B_DAY },
    { "有我在陪着呢，安心做事吧。",                    B_DAY | B_EVENING },
    { "哼，今天也算有点干劲嘛。",                      B_DAY },
    { "累了的话……摸摸我的头，也、也不是不可以啦。",    B_DAY | B_EVENING | B_NIGHT },
    { "五胞胎里面，我可是最努力的那个哦。",            B_DAY },
    /* 晚间 */
    { "晚上好。今天过得怎么样？",                      B_EVENING },
    { "夜深了，别熬太久哦。",                          B_EVENING | B_NIGHT },
    { "晚饭吃了吗？不许糊弄自己。",                    B_EVENING },
    { "最喜欢耳机了……也想让你听听我听的歌。",          B_EVENING | B_NIGHT },
    /* 深夜 */
    { "……还不睡吗，明天会没精神的哦。",                B_NIGHT },
    { "夜宵少吃点，会长胖的。",                        B_NIGHT },
    { "别一直盯着屏幕嘛……好啦，再看你一眼，就一眼。",  B_NIGHT },
    { "晚安，做个好梦。……梦里也要有我哦。",            B_NIGHT },
};

/**
 * 触摸反应语料（rig_chatter_touch 用，与时段无关）
 * on_head=true 摸头反应，false 戳身体反应
 */
static const struct { const char *text; bool on_head; } TOUCH_LINES[] = {
    { "呀……突然摸头什么的，也、也不是不可以啦。",      true },
    { "再、再摸的话，就把耳机分你一半哦？",            true },
    { "哼，摸舒服了没有？……那就再摸一会儿吧。",        true },
    { "干、干嘛戳我！",                                false },
    { "别戳了别戳了，好痒！",                          false },
    { "戳戳戳，我又不是按钮啦。",                      false },
};

/* 触摸反应冷却：连摸时 2.5s 才接一句，防止刷屏 */
#define TOUCH_COOLDOWN_MS   (2500)

/** 引擎状态（仅主循环上下文访问，无锁） */
static struct {
    rig_chatter_cb_t cb;        /* 触发回调 */
    void *ctx;
    uint32_t next_ms;           /* 下一次开腔时刻（esp_timer ms） */
    int last_line;              /* 上一句索引（防连播同一句） */
    int last_touch;             /* 上一句触摸语料索引（防连播） */
    uint32_t last_touch_ms;     /* 上次触摸触发时刻（冷却用，0=从未） */
} s;

/** 当前时段：按墙钟小时划分（24h 制；SNTP 未同步时 time() 靠拢 epoch，落深夜桶） */
static chatter_bucket_t chatter_bucket_from_time(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);         /* 时区由 time_sync 设置 TZ=CST-8 */
    int hour = tm_now.tm_hour;
    if (hour >= 5 && hour < 11) {
        return BUCKET_MORNING;
    }
    if (hour >= 11 && hour < 17) {
        return BUCKET_DAY;
    }
    if (hour >= 17 && hour < 23) {
        return BUCKET_EVENING;
    }
    return BUCKET_NIGHT;        /* 23~4 点 */
}

/** 掷出下一个随机间隔 */
static uint32_t next_gap_ms(void)
{
    return CHATTER_MIN_GAP_MS + esp_random() % (CHATTER_MAX_GAP_MS - CHATTER_MIN_GAP_MS);
}

/** 建议口型时长：3 字节/汉字 × 220ms/字，再加 800ms 起收尾，上限 4.5s */
static uint32_t estimate_speak_ms(const char *text)
{
    uint32_t chars = (uint32_t)(strlen(text) / 3);      /* UTF-8 中文≈3B/字 */
    uint32_t ms = chars * 220 + 800;
    return (ms > 4500) ? 4500 : ms;
}

esp_err_t rig_chatter_init(void)
{
    memset(&s, 0, sizeof(s));
    /* 首句节奏（R11 收紧）：60s 开机静音 + 30~90s 随机——约 1.5~2.5 分钟
     * 内见到第一句，不至于让人以为闲聊没在工作 */
    s.next_ms = (uint32_t)(esp_timer_get_time() / 1000)
                + CHATTER_STARTUP_QUIET_MS
                + 30000 + esp_random() % 60000;
    ESP_RETURN_ON_FALSE(sizeof(LINES) / sizeof(LINES[0]) >= 3,
                        ESP_ERR_INVALID_STATE, TAG, "语料太少");
    ESP_LOGI(TAG, "闲聊引擎就绪: %u 句语料, 间隔 %d~%d 分钟, 触摸反应 %u 句",
             (unsigned)(sizeof(LINES) / sizeof(LINES[0])),
             CHATTER_MIN_GAP_MS / 60000, CHATTER_MAX_GAP_MS / 60000,
             (unsigned)(sizeof(TOUCH_LINES) / sizeof(TOUCH_LINES[0])));
    return ESP_OK;
}

void rig_chatter_set_callback(rig_chatter_cb_t cb, void *ctx)
{
    s.cb = cb;
    s.ctx = ctx;
}

void rig_chatter_touch(bool on_head)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    /* 冷却：连摸 2.5s 内不接新句（表情照常，只是不刷台词） */
    if (s.cb == NULL || (s.last_touch_ms != 0 &&
                         now - s.last_touch_ms < TOUCH_COOLDOWN_MS)) {
        return;
    }
    s.last_touch_ms = now;

    /* 从对应部位语料里随机挑一句（与上句不同优先） */
    const int count = (int)(sizeof(TOUCH_LINES) / sizeof(TOUCH_LINES[0]));
    int pick = -1;
    for (int tries = 0; tries < 4; tries++) {
        int cand = (int)(esp_random() % (unsigned)count);
        if (TOUCH_LINES[cand].on_head == on_head &&
            (cand != s.last_touch || tries == 3)) {
            pick = cand;
            break;
        }
    }
    if (pick < 0) {
        return;
    }
    s.last_touch = pick;

    /* 触摸反应顶掉了即将到来的 idle 闲聊档期 → 顺延，避免话赶话 */
    s.next_ms = now + next_gap_ms();

    s.cb(TOUCH_LINES[pick].text, estimate_speak_ms(TOUCH_LINES[pick].text), s.ctx);
}

void rig_chatter_tick(void)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s.cb == NULL || now < s.next_ms) {
        return;
    }

    /* 从当前时段的语料里随机挑一句（与上句不同则收货，最多重掷 4 次） */
    const uint8_t bucket = (uint8_t)(1u << chatter_bucket_from_time());
    int pick = -1;
    for (int tries = 0; tries < 5; tries++) {
        int cand = (int)(esp_random() % (sizeof(LINES) / sizeof(LINES[0])));
        if ((LINES[cand].buckets & bucket) &&
            (cand != s.last_line || tries == 4)) {
            pick = cand;        /* 末次重掷放宽"不与上句重复"，避免稀疏桶卡死 */
            break;
        }
    }
    if (pick < 0) {
        return;                 /* 该时段没语料（不应发生），等下一轮 */
    }
    s.last_line = pick;

    s.next_ms = now + next_gap_ms();                    /* 先约下次，再开腔 */
    s.cb(LINES[pick].text, estimate_speak_ms(LINES[pick].text), s.ctx);
    ESP_LOGD(TAG, "chatter[%d]: %s", pick, LINES[pick].text);
}
