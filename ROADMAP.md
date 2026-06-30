# 路线图

v4.0.28 当前生产 (2026-06-30 烧)。

## v4.1 — 候选 (等翔哥拍)

| 候选 | 估 | 备注 |
|---|---|---|
| 多 push 通道 (Telegram / 钉钉 / Bark) | L | |
| 短信关键词过滤 | S | |
| Web 配置导入导出 | S | |
| TLS 客户端证书 | S | 自建 CA |
| MQTT 集成 | M | 引 PubSubClient |
| 国际化 (中英 UI) | M | |
| `MAX_UDH_REFS` 4→2 (再省 8.5KB) | S | 仅 min_free < 30K 时 |

## 远期想法

- TLS 双向认证 (银行级推送)
- OTA 签名验证 (防伪固件)
- 多 4G 模组负载均衡 (高可用)

## 已知限制

- **4 段以上长短信**: v4.0.23 起 silently drop (UDH 内存权衡)
- **dtac gateway 异常**: 偶发 raw UD bytes, 4 层 sniff 仍 cover 不到 (已知问题)
- **RNDIS 4G 上网**: v4.0.3 已删, WiFi 挂时无 4G 推送通道
- **并发 /api/send**: v4.0.25 加 active guard + v4.0.26 加 gen counter, 根因 TWDT detect worker stuck 仍未做

## 翔哥提 issue 场景

- WiFi 挂想用 4G 推送 → 走 esp_eth 重做 RNDIS 或换模组
- 5+ 段长短信 dtac 乱码 → v4.0.26 修了 4 段以下, 5+ 未覆盖
- min_free 持续 < 30K → 砍 `MAX_UDH_REFS` 4→2
- 想要其他 push 通道 / 关键词过滤 → 提 issue

---

**当前状态**: v4.0.28 生产 (2026-06-30 烧)。host test 326/3 PASS。
**本地 build**: `pio run` → `.pio/build/esp32-s3-devkitc-1/firmware.bin`
**GitHub Release**: https://github.com/xiangwhy/sms-forwarder-esp32/releases
