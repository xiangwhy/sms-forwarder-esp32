# 路线图 (Roadmap)

v4.0.23 代码已就绪 (2026-06-22, 待烧 — 翔哥拍板)。v4.0.5 → v4.0.23 期间改动:

- **内存优化** (4 缓冲 -40.7KB, heap 占用 85% → 66%, min_free 42K → 88K):
  - `STK_LOG_CAP` 256→64 (省 15.4KB, 单点最大)
  - `MAX_UDH_PARTS` 8→4 (省 8.4KB, >4 段长短信 silently drop)
  - `RX_LOG_CAP` 32→16 + `body[512]`→`body[320]` (省 8.6KB, 4+ 段拼接 dashboard 截断)
  - `g_pushQ` 16→8 (省 8.3KB, pushplus 推送慢, 8 缓冲够)
- **rx_log 截断提示**: body 截断时末尾追加 `+N more` 提示 (避免静默丢内容)
- **隐藏 SSID 扫描**: 隐藏 AP 不进 dropdown, 走 c_ssid 手动输入
- **安全清理**: 删硬编码 WiFi 凭据 (NVS 缺失时空默认 → 强制 AP 模式配网)
- **build 配套**: setup() 末尾加 `ESP_LOGW("MEM", heap free + min_free)` 水位监控

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
| v4.0.23 factory.bin 打包 + GitHub Release | S | 配合 v4.0.23 工厂首次烧录场景 |
| `MAX_UDH_REFS` 4→2 (再省 8.5KB) | S | 仅在 min_free 持续 < 30K 时再砍 |

## 远期想法

- GSM 7-bit 解码 (兼容老短信, v4.0 已用 UCS2, 老 GSM 短信是 7-bit packed, 解码复杂)
- TLS 双向认证 (银行级推送)
- OTA 签名验证 (防伪固件)
- 多个 4G 模组负载均衡 (高可用)

## 翔哥立刻能做的

v4.0.23 内存稳定, **不用急开新版本**。如果某天:
- WiFi 挂想用 4G 推送 → 提 issue。v4.0.3 已删 RNDIS 死路径,**走 esp_eth 重做 RNDIS** 或换模组(WiFi 永断场景才能补)
- 收到 4 段以上长短信仍异常 → 提 issue, 4 段以上 silently drop 是 v4.0.23 设计权衡 (UDH 内存省)
- 想要其他 push 通道 / 关键词过滤 / MQTT → 提 issue, 我们加
- min_free 持续 < 30K → 提 issue, 我们砍 `MAX_UDH_REFS` 4→2 再省 8.5KB

---

**当前状态**: v4.0.22 仍生产 (Boot #128, 2026-06-22 烧)。v4.0.23 代码 HEAD 已就绪, 烧板等拍板。
**本地 build**: `pio run` 产物 `.pio/build/esp32-s3-devkitc-1/firmware.bin`
**GitHub Release**: https://github.com/xiangwhy/sms-forwarder-esp32/releases/tag/v4.0.5 (v4.0.23 工厂镜像待打包)