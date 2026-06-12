# 路线图 (Roadmap)

v4.0 已发布,这是后续版本的计划。每项有粗略工作量估算 (S=小, M=中, L=大)。

## v4.1 — 网络与稳定性 (预计 1-2 周)

| 项 | 估 | 说明 |
|---|---|---|
| **修 RNDIS 4G 拨号** | L | v4.0 禁用。`iot_eth 0.1.x` 的 `stack_input` callback 没接好,会 NULL deref panic。备选:用 `esp_eth` 替代 `iot_eth`(API 稳) |
| **修长短信 UDH 拼接** | M | v4.0 在字母 sender + 4 段以上仍可能丢段。重点测:1) NVS 持久化 `UdhTable` 重启不丢, 2) "重传请求" `AT+CMGL=0` 拉未读 |
| **诊断 log 减负** | S | 关掉 `RAW +CMT HEAD/BODY` / `body_raw` / `body_codepoints` 等长串 log,只在 `AT TIMEOUT` 时打 |

## v4.2 — 推送能力 (预计 1-2 月)

| 项 | 估 | 说明 |
|---|---|---|
| **多 push 通道** | L | 支持 Telegram Bot / 钉钉 / Bark 替代 pushplus。配置多通道并行,主通道挂切备用 |
| **短信关键词过滤** | M | 白名单/黑名单/正则,匹配才推 |
| **数据用量统计** (可选) | M | 翔哥 v4.0 不要,改主意可加 |
| **TLS 客户端证书** | S | 自建 CA 支持 |

## v4.3 — 用户体验 (预计 2-3 月)

| 项 | 估 | 说明 |
|---|---|---|
| **MQTT 集成** | M | 家用 broker,推送到本地 |
| **Web 配置导入导出** | S | NVS JSON 备份/恢复 |
| **国际化** | M | 中英文 UI 自动切 |

## 远期想法

- **GSM 7-bit 解码** (兼容老短信)
- **TLS 双向认证** (银行级推送)
- **OTA 签名验证** (防伪固件)
- **多个 4G 模组负载均衡** (高可用)

## 翔哥立刻能做的

如果 v4.0 短短信够用,**v4.1 不急**。如果某天:
- WiFi 挂想用 4G 推送 → 开 v4.1 工作
- 收到 4 段以上长短信被乱码 → 提 issue,我们修

---

**当前状态**: v4.0 稳定
**Release**: https://github.com/xiangwhy/sms-forwarder-esp32/releases/tag/v4.0
