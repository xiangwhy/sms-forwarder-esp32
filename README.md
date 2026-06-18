# SMS Forwarder

ESP32-S3 + USB 4G 模组 + pushplus 推送的 SMS 转发器。

[![Release](https://img.shields.io/github/v/release/xiangwhy/sms-forwarder-esp32)](https://github.com/xiangwhy/sms-forwarder-esp32/releases)
[![License](https://img.shields.io/badge/license-proprietary-red)]()
[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)]()

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
- ✅ **长短信 UDH 拼接** — 2 段拼接 (泰文/中文/英文), ref/total/seq 正确解析
- ✅ **phone 短号显示** — 纯数字短号 (如 10086910) 不再误判 UCS2 hex 乱码
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
| **v4.0.5** (最新) | [v4.0.5.factory.bin](https://github.com/xiangwhy/sms-forwarder-esp32/releases/download/v4.0.5/v4.0.5.factory.bin) |
| v4.0.2 | [v4.0.2.factory.bin](https://github.com/xiangwhy/sms-forwarder-esp32/releases/download/v4.0.2/v4.0.2.factory.bin) |
| v4.0.1 | [v4.0.1.factory.bin](https://github.com/xiangwhy/sms-forwarder-esp32/releases/download/v4.0.1/v4.0.1.factory.bin) |
| v4.0 | [v4.0.factory.bin](https://github.com/xiangwhy/sms-forwarder-esp32/releases/download/v4.0/v4.0.factory.bin) |

