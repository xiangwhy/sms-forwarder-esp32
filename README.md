# SMS Forwarder

ESP32-S3 + USB 4G 模组 + pushplus 推送的 SMS 转发器。

[![Release](https://img.shields.io/github/v/release/xiangwhy/sms-forwarder-esp32)](https://github.com/xiangwhy/sms-forwarder-esp32/releases)
[![License](https://img.shields.io/badge/license-proprietary-red)]()
[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)]()

**当前生产**: v4.0.25 (2026-06-25 烧, Boot #146)
**代码 HEAD**: **v4.0.25** (2026-06-25, 已烧 = 当前生产)
- **v4.0.25 (review fix batch 后续 + defense-in-depth)**: P0-1 caller sniff `udHex` → `dataHex` (skip UDH IE 字节污染 sniff) + P0-2 7-bit 主 decode `udHex` → `dataHex` (修 v4.0.24 UDH skip batch 漏修主路径,7-bit concat + 8-bit IE case garbage prefix) + P0-3 cmgs cancel-on-overwrite guard (`volatile bool g_cmgsJobActive` 防并发 /api/send handler 覆写 g_cmgsJob 单例 → UAF),host test **315/3 PASS** (新 fixture G/H/H.5)
- **v4.0.24 + v4.0.24.1**: UCS-2/8-bit decode 路径 skip UDH concat IE 头 (修 dtac gateway 9 段 concat 推送乱码 `Ԁλँ` prefix) + UDH skip 加 udhi gate (防单条 SMS 首字节误判 UDHL) + ML307 stripped-UDHL guard (8-bit IEI=0x00 + 16-bit IEI=0x08 strict total/seq 验证, 防 port IE 误判) + 7-bit fallback 改 dataHex + 删 dead `dataByteLen` + 加 ESP_LOGW + 加 fixture J/K stripped-UDHL TDD red, host test **305/3 PASS**
- **v4.0.23 内存优化**: 4 缓冲 -40.7KB (STK_LOG 256→64 / UDH 8→4 / RX_LOG 32×512→16×320 / pushQ 16→8), heap 占用 85% → 66%, min_free 42K → 88K
- **v4.0.23 rx_log 截断**: 4 段以上长短信 dashboard 末尾追加 `+N more` 提示
- **v4.0.23 隐藏 SSID filter**: 隐藏 AP 不进 dropdown, 用户走 c_ssid 手动输入
- **v4.0.23 凭据清理**: 删硬编码 WiFi 凭据 (NVS 缺失时空默认 → 强制 AP 模式配网)

4G 卡收到 SMS → 自动推送到 pushplus 微信 / 公众号。

## 快速开始

```bash
git clone https://github.com/xiangwhy/sms-forwarder-esp32.git
cd sms-forwarder-esp32

# 烧整包 (bootloader + app)
esptool.py --chip esp32s3 -p /dev/cu.usbserial-XXXX \
  write_flash 0x0 v4.0.5.factory.bin
```

或者用 PIO:

```bash
pio run -t upload --upload-port /dev/cu.usbserial-XXXX
```

## 首次配网

1. 没配过 → 设备自动进 **AP 模式** (`SMS-Forwarder-XXXXXX`, 密码 `12345678`)
2. 手机连 WiFi → 浏览器开 `192.168.4.1`
3. 填表: WiFi SSID/密码 + pushplus token + 可选 topic + **OTA 用户名密码** (防别人乱刷)
4. 保存 → 设备自动重启 → 收到开机上线通知

## 功能

- ✅ **短短信** (≤ 70 字符) — UCS2 解码,中英泰日文都支持
- ✅ **pushplus 推送** — HTTPS,走 IDF 内置 CA
- ✅ **NVS 推送队列** — 失败落盘,30s 重发
- ✅ **Web Dashboard** — `/dashboard` 状态总览
- ✅ **Web OTA** — `/update` 整包烧录
- ✅ **开机上线通知** — 推一条到 pushplus
- ✅ **3 LED 状态** — 4G / WiFi / NET
- ✅ **长短信 UDH 拼接** — 2-4 段拼接 (泰文/中文/英文), ref/total/seq 正确解析 (v4.0.23: 8→4 段上限, 5+ 段 silently drop)
- ✅ **phone 短号显示** — 纯数字短号 (如 10086910) 不再误判 UCS2 hex 乱码
- ✅ **rx_log 截断提示** — v4.0.23: 4 段以上长短信 dashboard 末尾追加 `+N more` 提示用户查 push
- ✅ **WiFi 扫描隐藏 SSID** — v4.0.23: 隐藏 AP 不进 dropdown, 鼓励手动输 SSID
- ✅ **凭据零硬编码** — v4.0.23: NVS 缺失时 ssid/pass 空, 强制走 AP 模式配网, 防止代码泄露真实 WiFi
- ❌ **RNDIS 4G 上网** — v4.0.3 已移除死代码(SDK 栈不稳,iot_eth 0.1.x stack_input NULL deref)。4G 仍接 USB CDC 用于收 SMS,推送只走 WiFi
- ❌ **ArduinoOTA** — 用 web OTA 替代

## 文档

- **[README_v4.0.md](README_v4.0.md)** — 详细使用 + 引脚 + 编译步骤 + 已知问题
- **[ROADMAP.md](ROADMAP.md)** — v4.2+ 后续版本规划
- **[PLATFORMIO_GUIDE.md](PLATFORMIO_GUIDE.md)** — PIO 环境搭建

## 硬件

- ESP32-S3 DevKitC-1 (8MB flash)
- ML307 4G 模组 (USB CDC,仅 SMS 接收)
- USB 线一条

## 烧录下载

| Release | 下载 |
|---|---|
| **v4.0.25** (当前生产, 2026-06-25 烧, Boot #146) | 待打包 (本地 build: `pio run` 产物 `.pio/build/esp32-s3-devkitc-1/firmware.bin`) |
| **v4.0.24.1** (review fix batch, 已被 v4.0.25 替换) | [v4.0.24.1 GitHub Release](https://github.com/xiangwhy/sms-forwarder-esp32/releases/tag/v4.0.24.1) |
| **v4.0.23** (内存优化, 已被 v4.0.24.1 替换, Boot #135) | 待打包 (代码已就绪) |
| v4.0.5 (GitHub Release 最新) | [v4.0.5.factory.bin](https://github.com/xiangwhy/sms-forwarder-esp32/releases/download/v4.0.5/v4.0.5.factory.bin) |
| v4.0.2 | [v4.0.2.factory.bin](https://github.com/xiangwhy/sms-forwarder-esp32/releases/download/v4.0.2/v4.0.2.factory.bin) |
| v4.0.1 | [v4.0.1.factory.bin](https://github.com/xiangwhy/sms-forwarder-esp32/releases/download/v4.0.1/v4.0.1.factory.bin) |
| v4.0 | [v4.0.factory.bin](https://github.com/xiangwhy/sms-forwarder-esp32/releases/download/v4.0/v4.0.factory.bin) |

> **注**: v4.0.25 工厂镜像 .factory.bin 暂无, OTA 升级走 `pio run` 产物 `firmware.bin` 通过 `/update` 端点。当前生产 = 代码 HEAD, 无未烧 commit。

