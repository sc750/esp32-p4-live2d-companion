# 编辑清单

| 文件 | 改动 | 验证标准 | 结果 | commit |
|---|---|---|---|---|
| tools/gateway/gateway.py（新增） | 网关骨架：WS 服务/握手/echo/统计/断线清理 | 板连上+握手+回显 | ✅ | 待提交 |
| user/ai/gw_client.c/.h（新增） | 设备 WS 客户端：自动连接/重连/hello/收发 | gw 命令显示已连接，回环通 | ✅ | 待提交 |
| user/ai/ai_http.c | http_common_setup 支持自定义头（open 前） | 豆包 X-Api-* 生效 | ✅（上轮） | 2f5c2f9 |
| user/Kconfig | GW_URL/GW_RECONNECT_MS | 默认 ws://10.56.204.52:8765 | ✅ | 待提交 |
| user/main_app.c | 4e. gw_client_init() | 启动即连 | ✅ | 待提交 |
| user/ai/chat_console.c | gw / gw send 命令 | 回环测试通过 | ✅ | 待提交 |

## 构建证据（2026-09-12）
- 端到端：板上 gw send → 网关「设备文本: hello-gateway-step1」→ 网关 echo →
  板上「网关→: {"type":"echo",...}」；断线（1006）后 3s 自动重连握手
