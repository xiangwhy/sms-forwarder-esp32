# 路线图 (Roadmap)

v4.0.2 hotfix 已发布 (commit `adc0dce`)。这是后续版本的计划。每项有粗略工作量估算 (S=小, M=中, L=大)。

## v4.1 — 重新设计 (远期, 待 v4.0.2 收尾后开)

**v4.1 PDU 重写思路已搁置 (2026-06-13)**。原计划项需要重评:
- ~~修 RNDIS 4G 拨号~~ — 远期, 翔哥 v4.0 主动禁用, 重评必要性
- ~~修长短信 UDH 拼接~~ — **v4.0.2 已修** (parse_udh 8-bit 分支 + udhSkip + strstr 锚点 + clear_udh_ref slot 复用, 见 commit `adc0dce`)
- ~~诊断 log 减负~~ — 待 v4.0.2 跑稳后重评

v4.1 方向待定: 可能继续 PDU 模式 (v4.1 working 1607 行) 重启, 可能切其他方向。

## v4.2+ — 远期 (重评)

| 项 | 估 | 状态 |
|---|---|---|
| 多 push 通道 (Telegram / 钉钉 / Bark) | L | 待 v4.1 拍 |
| 短信关键词过滤 | M | 待 v4.1 拍 |
| 数据用量统计 | M | 翔哥 v4.0 不要, 改主意可加 |
| TLS 客户端证书 | S | 自建 CA 支持 |
| MQTT 集成 | M | 家用 broker, 推本地 |
| Web 配置导入导出 | S | NVS JSON 备份/恢复 |
| 国际化 (中英 UI) | M | 待 v4.1 拍 |

## 远期想法

- GSM 7-bit 解码 (兼容老短信, v4.0 已用 UCS2, 老 GSM 短信是 7-bit packed, 解码复杂)
- TLS 双向认证 (银行级推送)
- OTA 签名验证 (防伪固件)
- 多个 4G 模组负载均衡 (高可用)

## 翔哥立刻能做的

如果 v4.0.2 长短信够用, **v4.1 不急**。如果某天:
- WiFi 挂想用 4G 推送 → 提 issue, 开 v4.1 工作
- 收到 4 段以上长短信仍异常 → 提 issue, 我们查 UDH slot
- 想要其他 push 通道 → 提 issue, 我们加

---

**当前状态**: v4.0.2 stable (UDH 长短信拼接修)
**Release**: https://github.com/xiangwhy/sms-forwarder-esp32/releases/tag/v4.0
**Last commit**: `adc0dce` fix(UDH): 长短信拼接 4 处 bug
