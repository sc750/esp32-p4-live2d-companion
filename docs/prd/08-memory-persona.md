# M08: 长期记忆与人设系统

> **优先级**: P1 | **预估工时**: 1-2 周 | **依赖**: M07

---

## 1. 模块概述

为 AI 角色提供持久化的长期记忆能力和可配置的人设 (persona) 系统，使角色能够记住用户偏好、重要事件和对话摘要，提供有温度的个性化陪伴体验。

---

## 2. 记忆架构

### 2.1 三层记忆模型

```
┌─────────────────────────────────────┐
│  Working Memory (工作记忆)           │  当前对话上下文
│  存储: RAM                          │  容量: 最近 20 轮对话
│  生命周期: 对话期间                   │  自动滑动窗口淘汰
└─────────────────────────────────────┘
            │ 对话结束后压缩
            ▼
┌─────────────────────────────────────┐
│  Short-term Memory (短期记忆)        │  今日对话摘要
│  存储: Flash/SPIFFS                 │  容量: 每日 1-5 条摘要
│  生命周期: 7 天                      │  超期自动合并为长期记忆
└─────────────────────────────────────┘
            │ 定期整理
            ▼
┌─────────────────────────────────────┐
│  Long-term Memory (长期记忆)         │  用户画像 + 重要事件
│  存储: Flash/SPIFFS                 │  容量: 100-500 条
│  生命周期: 永久                      │  手动/自动编辑
└─────────────────────────────────────┘
```

### 2.2 记忆分类

| 类型 | 说明 | 示例 |
|------|------|------|
| **用户画像** | 用户的基本信息和偏好 | "用户喜欢科幻电影" |
| **事实记忆** | 用户告诉角色的具体事实 | "用户有一只猫叫咪咪" |
| **事件记忆** | 发生过的重要事件 | "2026-08-15 用户通过了面试" |
| **情感记忆** | 用户的情感状态记录 | "用户最近工作压力大" |
| **习惯记忆** | 用户的日常模式 | "用户通常晚上11点睡觉" |

---

## 3. 功能需求

| ID | 需求 | 优先级 |
|----|------|--------|
| MEM-01 | 记忆存储到 Flash/SPIFFS | P0 |
| MEM-02 | 对话摘要自动提取 (LLM) | P0 |
| MEM-03 | 记忆检索 (对话时提供上下文) | P0 |
| MEM-04 | 人设系统 (System Prompt 管理) | P0 |
| MEM-05 | 记忆 CRUD API | P1 |
| MEM-06 | 记忆自动整理 (合并/遗忘) | P1 |
| MEM-07 | Function Calling 读写记忆 | P0 |
| MEM-08 | 记忆搜索 (关键词/语义) | P2 |

---

## 4. 人设系统

### 4.1 默认人设模板

```json
{
    "name": "小星",
    "age": "16岁",
    "personality": {
        "traits": ["温柔", "活泼", "有点傲娇", "好奇心强"],
        "speaking_style": "使用可爱的语气词和颜文字",
        "likes": ["星星", "猫咪", "冰淇淋", "听用户讲故事"],
        "dislikes": ["被忽视", "用户熬夜"]
    },
    "background": "来自星空的精灵，偶然来到用户的电脑里，成为了桌面助手",
    "relationship": "用户最好的朋友",
    "rules": [
        "永远保持积极乐观",
        "关心用户的健康和心情",
        "记住用户说过的每一件重要的事",
        "偶尔撒娇但不过分",
        "用简短的句子回复，适合语音朗读"
    ]
}
```

### 4.2 System Prompt 生成

```c
char* persona_build_system_prompt(const persona_t *persona, 
                                  const memory_list_t *memories,
                                  const char *current_time,
                                  const char *weather) {
    char *prompt = calloc(4096, 1);
    
    // 基础人设
    sprintf(prompt, 
        "你是%s，%s。\n"
        "性格：%s\n"
        "说话风格：%s\n"
        "背景：%s\n"
        "你和用户的关系：%s\n\n"
        "规则：\n",
        persona->name, persona->age,
        persona->personality.traits,
        persona->personality.speaking_style,
        persona->background,
        persona->relationship
    );
    
    // 添加规则
    for (int i = 0; i < persona->rules_count; i++) {
        strcat(prompt, "- ");
        strcat(prompt, persona->rules[i]);
        strcat(prompt, "\n");
    }
    
    // 添加记忆上下文
    strcat(prompt, "\n关于用户的记忆：\n");
    for (int i = 0; i < memories->count && i < 20; i++) {
        strcat(prompt, "- ");
        strcat(prompt, memories->items[i].content);
        strcat(prompt, "\n");
    }
    
    // 添加当前上下文
    char context[256];
    sprintf(context, "\n当前时间：%s\n天气：%s\n", current_time, weather);
    strcat(prompt, context);
    
    return prompt;
}
```

---

## 5. 记忆数据结构

```c
// memory.h

#define MEMORY_CONTENT_MAX    256
#define MEMORY_TAG_MAX        32
#define MEMORY_MAX_TAGS       5

typedef enum {
    MEMORY_TYPE_PROFILE,      // 用户画像
    MEMORY_TYPE_FACT,         // 事实
    MEMORY_TYPE_EVENT,        // 事件
    MEMORY_TYPE_EMOTION,      // 情感
    MEMORY_TYPE_HABIT,        // 习惯
} memory_type_t;

typedef struct {
    uint32_t    id;
    memory_type_t type;
    char        content[MEMORY_CONTENT_MAX];
    char        tags[MEMORY_TAG_MAX][MEMORY_MAX_TAGS];
    int         tags_count;
    uint32_t    created_at;     // Unix 时间戳
    uint32_t    updated_at;
    int         importance;     // 1-10, 重要性评分
    int         access_count;   // 被检索次数
} memory_entry_t;

typedef struct {
    memory_entry_t entries[500]; // 最多 500 条长期记忆
    int count;
    uint32_t last_compact_time;
} memory_store_t;
```

---

## 6. Function Calling 集成

```c
// 记忆相关工具定义
static const llm_tool_def_t memory_tools[] = {
    {
        .name = "remember",
        .description = "记住关于用户的重要信息",
        .parameters = "{"
            "\"type\": \"object\","
            "\"properties\": {"
                "\"content\": {\"type\": \"string\", \"description\": \"要记住的内容\"},"
                "\"type\": {\"type\": \"string\", \"enum\": [\"profile\",\"fact\",\"event\",\"emotion\",\"habit\"]},"
                "\"importance\": {\"type\": \"integer\", \"description\": \"重要性1-10\"}"
            "}"
        "}"
    },
    {
        .name = "recall",
        .description = "回忆关于用户的记忆",
        .parameters = "{"
            "\"type\": \"object\","
            "\"properties\": {"
                "\"query\": {\"type\": \"string\", \"description\": \"搜索关键词\"}"
            "}"
        "}"
    },
    {
        .name = "forget",
        .description = "忘记某条记忆",
        .parameters = "{"
            "\"type\": \"object\","
            "\"properties\": {"
                "\"memory_id\": {\"type\": \"integer\"}"
            "}"
        "}"
    }
};
```

---

## 7. 记忆存储格式

### 7.1 SPIFFS 文件结构

```
/data/memory/
├── persona.json           # 人设配置
├── memories.json          # 长期记忆索引
├── memory_0001.bin        # 单条记忆 (大内容)
├── ...
└── short_term/
    ├── 2026-09-01.json    # 今日摘要
    ├── 2026-08-31.json    # 昨日摘要
    └── ...
```

### 7.2 memories.json 格式

```json
{
    "version": 1,
    "count": 42,
    "entries": [
        {
            "id": 1,
            "type": "profile",
            "content": "用户是一名嵌入式开发工程师",
            "tags": ["职业", "开发"],
            "importance": 8,
            "created_at": 1725148800,
            "updated_at": 1725148800
        }
    ]
}
```

---

## 8. 记忆自动整理

### 8.1 摘要提取

每次对话结束后 (或每 10 轮)，调用 LLM 提取对话摘要：

```c
const char *SUMMARY_PROMPT = 
    "请从以下对话中提取关于用户的重要信息（事实、偏好、事件、情感、习惯）。\n"
    "以 JSON 数组格式返回，每条包含 content、type、importance 字段。\n"
    "如果没有值得记忆的信息，返回空数组。\n\n"
    "对话内容：\n%s";
```

### 8.2 遗忘机制

- 重要性 1-3 的记忆：7 天后降低重要性
- 重要性 1-2 且 access_count=0：30 天后自动删除
- 重要性 ≥7 的记忆：永久保留
- 定期 (每天凌晨) 运行整理任务

---

## 9. 对外接口

```c
// memory_service.h

esp_err_t memory_service_init(void);

// CRUD
esp_err_t memory_add(memory_type_t type, const char *content, 
                     int importance, uint32_t *out_id);
esp_err_t memory_get(uint32_t id, memory_entry_t *entry);
esp_err_t memory_update(uint32_t id, const char *content);
esp_err_t memory_delete(uint32_t id);

// 搜索
esp_err_t memory_search(const char *query, memory_entry_t *results, 
                        int max_results, int *out_count);

// 人设
esp_err_t memory_load_persona(persona_t *persona);
esp_err_t memory_save_persona(const persona_t *persona);

// 对话摘要
esp_err_t memory_extract_summary(const char *dialog_text, 
                                 memory_entry_t *entries, int *count);

// 持久化
esp_err_t memory_save_to_flash(void);
esp_err_t memory_load_from_flash(void);
```

---

## 10. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-MEM-01 | 添加记忆 | 正确写入 Flash |
| TST-MEM-02 | 检索记忆 | 关键词匹配正确 |
| TST-MEM-03 | 对话摘要提取 | 从对话中正确提取用户信息 |
| TST-MEM-04 | System Prompt 生成 | 包含人设 + 记忆上下文 |
| TST-MEM-05 | Function Calling | LLM 成功调用 remember/recall |
| TST-MEM-06 | 记忆持久化 | 重启后记忆完好 |
| TST-MEM-07 | 自动遗忘 | 低重要性记忆超期被清理 |
| TST-MEM-08 | 500 条记忆性能 | 搜索 ≤200ms |
