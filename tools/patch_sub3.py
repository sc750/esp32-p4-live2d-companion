# -*- coding: utf-8 -*-
"""字幕同步排障补丁：网关 ensure_ascii=False + 设备接收缓冲扩容。"""
p = "tools/gateway/gateway.py"
s = open(p, encoding="utf-8").read()
old = "        data = json.dumps(obj) if isinstance(obj, (dict, list)) else obj"
new = ("        # ensure_ascii=False：中文原样传输（默认 unicode 转义膨胀 6 倍，\n"
       "        # 会撑爆设备端 512B 接收缓冲导致 JSON 截断解析失败——2026-09-12 实测）\n"
       "        data = json.dumps(obj, ensure_ascii=False) if isinstance(obj, (dict, list)) else obj")
assert old in s, "g"
s = s.replace(old, new, 1)
open(p, "w", encoding="utf-8", newline="\n").write(s)
print("gateway ok")

p = "user/ai/gw_client.c"
s = open(p, encoding="utf-8").read()
old = """        int len = ev->data_len;
        if (len > 512) {
            len = 512;                          /* 步骤 1 消息都很小，截断防御 */
        }
        char buf[513];                          /* 栈上够 */"""
new = """        int len = ev->data_len;
        if (len > 2047) {
            len = 2047;                         /* 长回复全文（reply_done）可达 KB 级 */
        }
        static char rbuf[2048];                 /* static：不占 WS 任务栈（原 512B 不够） */
        char *buf = rbuf;"""
assert old in s, "d"
s = s.replace(old, new, 1)
open(p, "w", encoding="utf-8", newline="\n").write(s)
print("device ok")
