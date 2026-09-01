# M06: 网络与 Wi-Fi

> **优先级**: P0 | **预估工时**: 1 周 | **依赖**: M01

---

## 1. 模块概述

通过 ESP-Hosted 方案使用板载 ESP32-C6 提供 Wi-Fi 连接，管理 HTTP/HTTPS 通信、TLS 连接串行化、以及网络状态管理。为 AI Agent (ASR/LLM/TTS) 和音乐在线流提供网络基础。

---

## 2. ESP-Hosted 架构

```
┌─────────────────────┐    SDIO 4-bit    ┌──────────────────────┐
│  ESP32-P4 (Host)    │ ◄══════════════► │  ESP32-C6-MINI-1     │
│                     │    40MHz          │  (Wi-Fi Slave)       │
│  应用层 HTTP/TLS    │                   │  Wi-Fi 6 射频         │
│  esp_http_client    │    RPC 控制       │  802.11 b/g/n/ax     │
│  mbedTLS            │ ──────────────── │  STA/AP 模式          │
│                     │    数据通道       │                      │
│  EMAC RMII ◄═════════════════════════► │ RJ45 以太网 PHY      │
│  (以太网, 备选)     │                   │ 10/100 Mbps          │
└─────────────────────┘                   └──────────────────────┘

网络优先级: 以太网 > Wi-Fi
- 优先使用有线以太网 (更低延迟, 更稳定)
- Wi-Fi 作为备选/补充
```

---

## 3. 功能需求

| ID | 需求 | 优先级 |
|----|------|--------|
| NET-01 | ESP-Hosted SDIO 初始化与连接 (Wi-Fi) | P0 |
| NET-02 | Wi-Fi STA 模式连接 | P0 |
| NET-03 | 以太网连接 (RJ45, 10/100M) | P1 |
| NET-04 | Wi-Fi / 以太网自动切换与冗余 | P2 |
| NET-05 | HTTP GET/POST 请求 | P0 |
| NET-06 | HTTPS (TLS 1.2+) 请求 | P0 |
| NET-07 | TLS 连接串行化 (避免 mbedTLS 并发崩溃) | P0 |
| NET-08 | WebSocket 连接 (用于流式 ASR) | P1 |
| NET-09 | 网络状态监控与自动重连 | P0 |
| NET-10 | DNS 解析缓存 | P1 |
| NET-11 | 请求超时与重试 | P0 |
| NET-12 | 网络请求队列 (串行化) | P0 |

---

## 4. TLS 串行化方案

### 4.1 问题

ESP32-P4 的 mbedTLS 使用共享的 SHA/AES 硬件加速器。当多个 TLS 连接并发时，硬件加速器访问冲突导致崩溃。

### 4.2 解决方案

```c
// tls_serialization.h

// 全局 TLS 互斥锁
static SemaphoreHandle_t tls_mutex = NULL;

// 串行化 HTTP 请求封装
typedef struct {
    char *url;
    char *method;
    char *headers;
    char *body;
    char *response;
    size_t response_size;
    int status_code;
    SemaphoreHandle_t done_sem;
} http_request_t;

// 提交请求到队列 (阻塞直到完成)
esp_err_t tls_serialized_request(http_request_t *req);

// 内部: 单独任务消费请求队列
void tls_request_consumer_task(void *arg);
```

### 4.3 请求队列架构

```
AI Agent Task ──┐
Music Task ─────┤    ┌─────────────────┐    ┌──────────────┐
Diary Task ─────┼──▶ │ Request Queue    │──▶ │ TLS Worker    │──▶ 网络
Other Tasks ────┘    │ (FreeRTOS Queue) │    │ (单一任务)    │
                     └─────────────────┘    └──────────────┘

- 所有 TLS 请求必须通过队列
- TLS Worker 一次处理一个请求
- 非 TLS (HTTP) 请求可以并行 (但通常也串行以节省资源)
```

---

## 5. HTTP 客户端封装

```c
// http_client.h

typedef struct {
    const char *url;
    const char *method;       // "GET", "POST", etc.
    const char *content_type;
    const char *auth_token;
    const uint8_t *body;
    size_t body_len;
} http_request_config_t;

typedef struct {
    int status_code;
    uint8_t *data;
    size_t data_len;
    bool is_chunked;
    // 流式回调
    void (*on_chunk)(const uint8_t *chunk, size_t len, void *ctx);
    void *chunk_ctx;
} http_response_t;

// 同步请求 (通过 TLS 串行化队列)
esp_err_t http_request(const http_request_config_t *config, 
                       http_response_t *response);

// 流式请求 (用于 ASR/TTS 流式传输)
esp_err_t http_request_streaming(const http_request_config_t *config,
                                 void (*on_data)(const uint8_t *data, size_t len, void *ctx),
                                 void *user_ctx);

// WebSocket
esp_err_t ws_connect(const char *url, ws_handle_t *handle);
esp_err_t ws_send(ws_handle_t handle, const uint8_t *data, size_t len);
esp_err_t ws_receive(ws_handle_t handle, uint8_t *buf, size_t buf_size, 
                     size_t *out_len, uint32_t timeout_ms);
void      ws_close(ws_handle_t handle);
```

---

## 6. 网络状态管理

### 6.1 状态机

```
                ┌──────────┐
                │ DISCONNECT│
                └─────┬────┘
                      │ esp_wifi_connect()
                ┌─────▼────┐
          ┌─────│ CONNECTING│─────┐
          │     └─────┬────┘     │
          │ timeout   │ success  │ fail
          │     ┌─────▼────┐    │
          │     │CONNECTED │    │
          │     └─────┬────┘    │
          │           │ lost    │
          │     ┌─────▼────┐    │
          │     │RECONNECTING│   │
          │     └─────┬────┘    │
          │           │         │
          └───────────┘  重试次数 > max
                      ┌─────▼────┐
                      │  FAILED   │  → 通知 UI
                      └──────────┘
```

### 6.2 自动重连策略

```c
#define WIFI_MAX_RETRY        5
#define WIFI_RETRY_BASE_DELAY_MS  1000   // 1s
#define WIFI_RETRY_MAX_DELAY_MS   30000  // 30s

// 指数退避: 1s, 2s, 4s, 8s, 16s
int retry_delay = WIFI_RETRY_BASE_DELAY_MS * (1 << retry_count);
if (retry_delay > WIFI_RETRY_MAX_DELAY_MS) {
    retry_delay = WIFI_RETRY_MAX_DELAY_MS;
}
```

---

## 7. 内存预算

| 用途 | 大小 | 说明 |
|------|------|------|
| mbedTLS 上下文 | ~40 KB | 单个 TLS 连接 |
| HTTP 收发缓冲 | ~16 KB | 请求/响应缓冲 |
| 接收缓冲 | ~8 KB | chunked 传输 |
| DNS 缓存 | ~1 KB | 域名解析缓存 |
| **合计** | **~65 KB** | 单连接串行化 |

---

## 8. 需要访问的云端服务

| 服务 | 协议 | 用途 |
|------|------|------|
| ASR 服务 | WebSocket/HTTPS | 流式语音识别 (如 Whisper API) |
| LLM 服务 | HTTPS (SSE) | 大模型推理 (OpenAI 兼容) |
| TTS 服务 | HTTPS | 语音合成 (如 Edge TTS) |
| 天气 API | HTTPS | 天气信息 |
| 音乐服务 | HTTPS | 在线音乐流/搜索 |

---

## 9. 对外接口

```c
// network_service.h

esp_err_t network_service_init(void);
esp_err_t network_service_connect(const char *ssid, const char *password);
bool      network_service_is_connected(void);
esp_err_t network_service_get_ip(char *buf, size_t len);

// HTTP (通过 TLS 串行化)
esp_err_t network_service_http_request(const http_request_config_t *req,
                                       http_response_t *resp);

// 流式 HTTP
esp_err_t network_service_http_stream(const http_request_config_t *req,
                                      http_chunk_cb_t on_chunk, void *ctx);
```

---

## 10. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-NET-01 | Wi-Fi 连接 | ≤10s 获取 IP |
| TST-NET-02 | HTTP GET | 正确获取响应, 状态码 200 |
| TST-NET-03 | HTTPS 请求 | TLS 握手成功, 数据正确 |
| TST-NET-04 | TLS 并发请求 | 串行化执行, 无崩溃 |
| TST-NET-05 | 断线重连 | ≤30s 自动重连 |
| TST-NET-06 | 流式接收 | 数据逐块到达, 无丢失 |
| TST-NET-07 | 长时间连接 (2h) | 无内存泄漏/断连 |
