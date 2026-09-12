# -*- coding: utf-8 -*-
"""冒烟测试：stdio 拉起 lvgl-mcp-server，握手 + tools/list。"""
import subprocess, json, time

npx = "C:\\Program Files\\nodejs\\npx.cmd"
proc = subprocess.Popen(
    [npx, "lvgl-mcp-server"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True, encoding="utf-8")
init = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
    "protocolVersion": "2024-11-05",
    "capabilities": {},
    "clientInfo": {"name": "smoke-test", "version": "0.0.1"}}}
proc.stdin.write(json.dumps(init) + "\n")
proc.stdin.flush()
time.sleep(6)
proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
proc.stdin.flush()
time.sleep(1)
proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}) + "\n")
proc.stdin.flush()
time.sleep(2)
proc.kill()
out, err = proc.communicate()
got_init = got_tools = False
for line in out.splitlines():
    try:
        msg = json.loads(line)
    except Exception:
        continue
    if msg.get("id") == 1 and "result" in msg:
        got_init = True
        print("initialize OK:", msg["result"].get("serverInfo"))
    if msg.get("id") == 2 and "result" in msg:
        got_tools = True
        print("tools:", [t["name"] for t in msg["result"]["tools"]])
if err:
    print("stderr:", err[:300])
print("RESULT:", "PASS" if (got_init and got_tools) else "FAIL")
