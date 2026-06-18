# 路线图 (Roadmap)

v4.0.5 已发布 (2026-06-18)。

## v4.2 — 待拍

候选(等翔哥拍):

| 候选 | 估 | 备注 |
|---|---|---|
| 多 push 通道 (Telegram / 钉钉 / Bark) | L | |
| 短信关键词过滤 | S | |
| Web 配置导入导出 | S | |
| TLS 客户端证书 | S | 自建 CA 支持 |
| 数据用量统计 | M | 翔哥 v4.0 不要,改主意可加 |
| MQTT 集成 | M | 引 PubSubClient |
| 国际化 (中英 UI) | M | |

## 远期想法

- GSM 7-bit 解码 (兼容老短信, v4.0 已用 UCS2, 老 GSM 短信是 7-bit packed, 解码复杂)
- TLS 双向认证 (银行级推送)
- OTA 签名验证 (防伪固件)
- 多个 4G 模组负载均衡 (高可用)

## 翔哥立刻能做的

v4.0.5 长短信够用, **不用急开新版本**。如果某天:
- WiFi 挂想用 4G 推送 → 提 issue。v4.0.3 已删 RNDIS 死路径,**走 esp_eth 重做 RNDIS** 或换模组(WiFi 永断场景才能补)
- 收到 4 段以上长短信仍异常 → 提 issue, 我们查 UDH slot
- 想要其他 push 通道 / 关键词过滤 / MQTT → 提 issue, 我们加

---

**当前状态**: v4.0.5 stable (DCS 表对齐 gammu + 混合启发修泰文 DCS=0 误标 + BOOT 60s 防死循环)
**Release**: https://github.com/xiangwhy/sms-forwarder-esp32/releases/tag/v4.0.5