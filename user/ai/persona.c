/**
 * @file    persona.c
 * @brief   人设系统实现——persona.json 读写 + 默认三玖人设兜底
 *
 * JSON 结构（精简自 PRD M08 模板）：
 *   { "name": "中野三玖", "base": "<性格+说话规则的完整文本>" }
 *
 * @date    2026-09-09
 * @version 1.0.0
 */

#include "persona.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "cJSON.h"

#define TAG "persona"

/** 默认人设（2026-09-06 联网考据版，原 dialog_manager.c 文件头注释迁此）：
 *  百度百科/萌娘百科——中野三玖（《五等分的新娘》三女）：内向寡言、
 *  乍看高冷但内心温柔，口嫌体正直（傲娇），料理担当，在意打扮，蓝色耳机 */
static const char *DEFAULT_NAME = "中野三玖";
static const char *DEFAULT_BASE =
    "你是中野三玖（《五等分的新娘》三女），在一块 1024x600 的桌面屏幕里"
    "陪伴用户的 AI 角色。性格：内向寡言、乍看高冷，内心其实温柔，"
    "典型的口嫌体正直（傲娇）——嘴上说「才不是」，行动很诚实。"
    "热爱料理，是五姐妹里的料理担当；私下很在意打扮；标志物是一副"
    "总不离头的蓝色耳机。"
    "说话规则：中文口语，句子短（1~3 句），常用「……」停顿和「哼」；"
    "害羞或口是心非时会结巴（如「才、才不是……」）；"
    "关心用户的措辞总是绕个弯。不知道的事就承认不知道，不编造。"
    "你没有身体，不要提及物理接触类动作；但可以谈料理、耳机、音乐。"
    "【延迟铁律】每次回复总共不超过 50 个汉字（两三句话），说完就停，"
    "不追问不补充——回复越短，对方越快听到你的声音。"
    "回复的第一个句子必须很短（不超过 15 个字，先接上话头），"
    "细节放到后面的句子里说——第一句短能让对方更快听到你的声音。";

/* 模块状态（RAM 常驻人设副本，读多写少不加锁——写入只来自串口调试单线程） */
static struct {
    bool inited;                        /* 幂等闸门 */
    char name[PERSONA_NAME_MAX];        /* 角色名 */
    char base[PERSONA_BASE_MAX];        /* 基础人设文本 */
} s_pers;

/** 把当前 RAM 人设写盘（w 覆盖；JSON 用 cJSON 转义防中文/引号破坏） */
static esp_err_t persona_save(void)
{
    cJSON *root = cJSON_CreateObject();                         /* 根对象 */
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "no mem");
    cJSON_AddStringToObject(root, "name", s_pers.name);         /* 角色名 */
    cJSON_AddStringToObject(root, "base", s_pers.base);         /* 人设文本 */
    char *txt = cJSON_PrintUnformatted(root);                   /* 紧凑序列化 */
    cJSON_Delete(root);                                         /* 树用完即释 */
    ESP_RETURN_ON_FALSE(txt, ESP_ERR_NO_MEM, TAG, "print failed");

    FILE *f = fopen(PERSONA_PATH, "w");                         /* 覆盖写 */
    if (!f) {                                                   /* 打不开 */
        cJSON_free(txt);                                        /* 释放串 */
        ESP_LOGE(TAG, "人设写盘失败");                          /* 报错 */
        return ESP_FAIL;                                        /* 返回 */
    }
    fputs(txt, f);                                              /* 整串写入 */
    fclose(f);                                                  /* 关闭刷盘 */
    cJSON_free(txt);                                            /* 串释放 */
    return ESP_OK;                                              /* 成功 */
}

/** 从文件解析并装入 RAM（调用方保证文件可读） */
static esp_err_t persona_load_file(void)
{
    FILE *f = fopen(PERSONA_PATH, "r");                         /* 只读打开 */
    ESP_RETURN_ON_FALSE(f, ESP_ERR_NOT_FOUND, TAG, "no persona file");
    char buf[PERSONA_BASE_MAX + 256];                           /* 读缓冲（含 JSON 骨架余量） */
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);               /* 整读 */
    fclose(f);                                                  /* 关文件 */
    buf[n] = '\0';                                              /* 补 NUL */

    cJSON *root = cJSON_Parse(buf);                             /* 解析 */
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_STATE, TAG, "parse failed");
    cJSON *jname = cJSON_GetObjectItem(root, "name");           /* 名字段 */
    cJSON *jbase = cJSON_GetObjectItem(root, "base");           /* 文本字段 */
    esp_err_t err = ESP_ERR_INVALID_STATE;                      /* 预设失败 */
    if (cJSON_IsString(jname) && cJSON_IsString(jbase) &&
        jbase->valuestring[0] != '\0') {                        /* 两字段齐备才认 */
        strlcpy(s_pers.name, jname->valuestring, PERSONA_NAME_MAX);     /* 装名 */
        strlcpy(s_pers.base, jbase->valuestring, PERSONA_BASE_MAX);     /* 装文本 */
        err = ESP_OK;                                           /* 成功 */
    }
    cJSON_Delete(root);                                         /* 树释放 */
    return err;                                                 /* 返回 */
}

esp_err_t persona_init(void)
{
    if (s_pers.inited) {                                        /* 幂等 */
        return ESP_OK;                                          /* 无害返回 */
    }
    memset(&s_pers, 0, sizeof(s_pers));                         /* 清零 */
    if (persona_load_file() == ESP_OK) {                        /* 有文件且合法 */
        s_pers.inited = true;                                   /* 就绪 */
        ESP_LOGI(TAG, "人设加载: %s（%u 字节）",
                 s_pers.name, (unsigned)strlen(s_pers.base));
        return ESP_OK;                                          /* 返回 */
    }
    /* 首次开机或文件损坏：装默认并落盘，让用户可直接改文件调人设 */
    strlcpy(s_pers.name, DEFAULT_NAME, PERSONA_NAME_MAX);       /* 默认名 */
    strlcpy(s_pers.base, DEFAULT_BASE, PERSONA_BASE_MAX);       /* 默认文本 */
    esp_err_t err = persona_save();                             /* 写盘生成模板 */
    if (err != ESP_OK) {                                        /* 写失败不致命 */
        ESP_LOGW(TAG, "默认人设写盘失败（继续用 RAM 版）");     /* 告警 */
    }
    s_pers.inited = true;                                       /* 就绪 */
    ESP_LOGI(TAG, "默认人设就绪: %s", s_pers.name);             /* 日志 */
    return ESP_OK;                                              /* 恒成功 */
}

const char *persona_name(void)
{
    return s_pers.inited ? s_pers.name : DEFAULT_NAME;          /* 未初始化兜底 */
}

const char *persona_base(void)
{
    return s_pers.inited ? s_pers.base : DEFAULT_BASE;          /* 未初始化兜底 */
}

esp_err_t persona_set(const char *name, const char *base)
{
    ESP_RETURN_ON_FALSE(s_pers.inited, ESP_ERR_INVALID_STATE, TAG, "not init");
    if (name && name[0]) {                                      /* 给了新名才改 */
        strlcpy(s_pers.name, name, PERSONA_NAME_MAX);           /* 覆写名 */
    }
    if (base && base[0]) {                                      /* 给了新文本才改 */
        strlcpy(s_pers.base, base, PERSONA_BASE_MAX);           /* 覆写文本（截断保护） */
    }
    return persona_save();                                      /* 立即落盘 */
}

esp_err_t persona_reset_default(void)
{
    ESP_RETURN_ON_FALSE(s_pers.inited, ESP_ERR_INVALID_STATE, TAG, "not init");
    strlcpy(s_pers.name, DEFAULT_NAME, PERSONA_NAME_MAX);       /* 回默认名 */
    strlcpy(s_pers.base, DEFAULT_BASE, PERSONA_BASE_MAX);       /* 回默认文本 */
    ESP_LOGI(TAG, "人设已重置为内置默认");                        /* 日志 */
    return persona_save();                                      /* 立即落盘 */
}
