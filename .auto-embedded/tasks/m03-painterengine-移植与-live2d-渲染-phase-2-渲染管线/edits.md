# 编辑清单

| 文件 | 改动 | 验证标准 | 结果 | commit |
|---|---|---|---|---|
# 编辑清单

| 文件 | 改动 | 验证标准 | 结果 | commit |
|---|---|---|---|---|
| R1 | `tools/rigpack.py`（placeholder/pack/verify 三模式，rigbin v1 格式：32B 头 + 36B×图层表 + RGBA8888 atlas）+ `assets/art/nino/README.md` 素材规范 | placeholder 生成 + verify 读回 6 图层全部字段正确 | ✅ 1.2MB rigbin，呆毛带 physics 标志，atlas 24% 非透明 | - |
| R1b | rigpack 真实素材适配：base.{png,jpg} 大小写兼容、max_height 降采样（800px）、白底阈值可配、变体按自身 bbox 对齐 + 差分高斯去噪 + 头区限制 + parent=head；用户三玖素材打包成功（5 层 3.6MB） | 真实立绘差分切层 bbox 为脸部尺寸 | ✅ eyes_closed 193x206/mouth 202x206，verify PASS | - |
