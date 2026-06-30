# SMS Forwarder

ESP32-S3 + USB 4G 模组 (ML307) + pushplus 推送的 SMS 转发器。

[![Release](https://img.shields.io/github/v/release/xiangwhy/sms-forwarder-esp32)](https://github.com/xiangwhy/sms-forwarder-esp32/releases)
[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)]()

**当前生产**: v4.0.28 (2026-06-30 烧)

4G 卡收到 SMS → 自动推送到 pushplus 微信 / 公众号。

## 快速开始

```bash
git clone https://github.com/xiangwhy/sms-forwarder-esp32.git
cd sms-forwarder-esp32

# PIO 编译 + 烧录
pio run -t upload --upload-port /dev/cu.usbserial-XXXX

# 看 log
pio device monitor -b 115200 -p /dev/cu.usbserial-XXXX
```

## 首次配网

1. 没配过 → 设备自动进 **AP 模式** (`SMS-Forwarder-XXXXXX`, 密码 `12345678`)
2. 手机连 WiFi → 浏览器开 `192.168.4.1`
3. 填表: WiFi SSID/密码 + pushplus token + 可选 topic + **OTA 用户名密码**
4. 保存 → 设备自动重启 → 收到开机上线通知

## 硬件

| GPIO | 用途 |
|---|---|
| GPIO8 | 4G 模组 PWR (高电平上电) |
| GPIO7 | LED 4G 状态 |
| GPIO15 | LED WiFi 状态 |
| GPIO6 | LED NET 推送状态 |
| GPIO0 | BOOT 按钮 (长按 15s 清 NVS) |

- **ESP32-S3 DevKitC-1** (8MB flash)
- **ML307** 4G 模组 (USB CDC, 仅 SMS 接收)
- USB 线一条

**LED 状态**:
- GPIO7 (4G): AT 起来 → 常亮; 等待中 → 慢闪
- GPIO15 (WiFi): 连上 → 常亮; 未连 → 快闪; AP 模式 → 慢闪
- GPIO6 (NET): 推完一条 → 闪一下

## 功能

- ✅ **短短信** (≤ 70 字) — UCS-2 解码, 中英泰日文
- ✅ **长短信 UDH 拼接** — 2-4 段 concat, v4.0.28 timeout 120s
- ✅ **pushplus 推送** — HTTPS, IDF 内置 CA
- ✅ **NVS 推送队列** — 失败落盘, 30s 重发, 8 条缓冲
- ✅ **Web Dashboard** — `/dashboard` 状态总览, 30s 自动刷新
- ✅ **双向短信** — `/send` 页面发短信, 32 条历史
- ✅ **Web OTA** — `/update` 整包烧录 (BasicAuth)
- ✅ **开机上线通知** — 推一条到 pushplus
- ✅ **NTP 时间同步** — ntp.aliyun.com, dashboard 显示真实时间
- ✅ **3 LED 状态** — 4G / WiFi / NET
- ✅ **WiFi 扫描** — 配网页下拉选择, 隐藏 SSID 手动输入
- ✅ **凭据零硬编码** — NVS 缺失 → 强制 AP 配网
- ❌ **RNDIS 4G 上网** — v4.0.3 已移除 (SDK 栈不稳), 4G 仅收 SMS

## Web 页面

| URL | 说明 | 鉴权 |
|---|---|---|
| `/` | 跳转 dashboard | 无 |
| `/dashboard` | 状态总览 (运行/推送/系统卡) | 无 |
| `/send` | 双向短信 (32 条历史 + 清空) | OTA 鉴权 |
| `/update` | OTA 烧录 | OTA 鉴权 |
| `/config` | 高级配置 + 恢复出厂 | OTA 鉴权 |
| `/api/status` | JSON 状态 | 无 |
| `/api/scan` | WiFi 扫描 | 无 |

### `/api/status` 字段

| 字段 | 类型 | 含义 |
|---|---|---|
| `boot` | int | 启动次数 |
| `pushOk` / `pushFail` | int | 推送成功/失败累计 |
| `qLen` | int | NVS 待推队列长度 |
| `wifi` | bool | WiFi 是否连上 |
| `wifiRssi` | int | WiFi RSSI (dBm) |
| `freeHeap` / `minFreeHeap` | int | 当前/最小空闲 heap |
| `heapTotal` / `heapUsed` | int | 总/已用 heap |
| `lastSmsUtc` / `lastPushOkUtc` | int | 上次收/推 epoch ms |
| `deviceTimeMs` | int | 设备 epoch ms (NTP 同步后有效) |
| `timeSynced` | bool | SNTP 是否同步 |
| `mlAlive` | bool | 4G 模组是否 alive |
| `udhActive` | int | 当前活跃 UDH 拼接槽 |
| `fw` | str | 固件版本 |

## 凭据维护

设备 NVS namespace `cfg` 下: `wifi.ssid` / `wifi.pass` / `pp.tok` / `pp.tpc` / `ota.user` / `ota.pass`

| 方法 | 适用 |
|---|---|
| Web `/send` 配网 | 改单项 (推荐) |
| 长按 BOOT 15s 清 NVS → AP 配 | 全量重配 |
| NVS blob 灌入 (esptool) | 脚本化批量初始化 |

### NVS blob 灌入

```csv
# /tmp/esp32-cfg.csv — 第一行 namespace 声明不可省!
key,type,encoding,value
cfg,namespace,,
wifi.ssid,data,string,YOUR_SSID
wifi.pass,data,string,YOUR_PASS
pp.tok,data,string,YOUR_TOKEN
pp.tpc,data,string,
ota.user,data,string,admin
ota.pass,data,string,change-me
```

```bash
# 生成 + 烧入
python3 $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
  generate /tmp/esp32-cfg.csv /tmp/esp32-cfg.bin 0x6000
esptool.py --chip esp32s3 -p /dev/cu.usbserial-XXXX write_flash 0x9000 /tmp/esp32-cfg.bin
```

> **警告**: CSV 含真实凭据, 绝对不能 commit, 只放 `/tmp/` 用完即删。

## 开发

```bash
# 编译
pio run

# 烧录
pio run -t upload --upload-port /dev/cu.usbserial-XXXX

# 串口监视器 (退出: Ctrl+])
pio device monitor -b 115200 -p /dev/cu.usbserial-XXXX

# host test (PC 上跑, 绑定生产 pdu_codec.cpp)
cd tests/host && cmake --build build && ./build/test_pdu_codec
```

> **平台**: pioarduino (`platformio.ini` 已锁定), IDF 5.5 + Arduino-ESP32 3.x。

## 文件结构

```
esp32-sms/
├── platformio.ini              # PIO 配置
├── sdkconfig.defaults          # ESP-IDF 配置
├── partitions.csv              # 8MB flash dual-OTA 分区表
├── src/
│   ├── main.cpp                # 业务代码 (WiFi/Web/SMS/push/STK)
│   ├── pdu_codec.h/cpp         # PDU 解码 (7-bit/UCS-2/UDH/phone)
│   ├── stk_validate.h          # STK SELECT 验证
│   ├── web/
│   │   ├── app.h               # 导航框架 HTML
│   │   ├── dashboard.h         # Dashboard HTML
│   │   ├── send.h              # 短信发送页 HTML
│   │   ├── config.h            # 配置页 HTML
│   │   ├── ota.h               # OTA 页 HTML
│   │   └── stk.h               # STK 页 HTML
│   └── idf_component.yml       # 依赖 iot_usbh_cdc ^3.0
├── tests/
│   ├── host/
│   │   ├── CMakeLists.txt      # host test build
│   │   └── test_pdu_codec.cpp  # 326 用例 (绑定生产 pdu_codec.cpp)
│   └── test_pdu_parser.py      # 旧版 Python 测试 (deprecated)
└── docs/
    └── sms-pdu-references.md   # PDU 发送技术参考
```

## 版本历史

| 版本 | 日期 | 要点 |
|---|---|---|
| **v4.0.28** | 2026-06-28 | UDH timeout 60s→120s, dtac 2 段 concat 修复 |
| v4.0.27 | 2026-06-26 | 14 finding 全修 + 2 patch, 真机验证 |
| v4.0.25 | 2026-06-25 | caller sniff + 7-bit dataHex + cmgs cancel guard |
| v4.0.24.1 | 2026-06-23 | UCS-2/8-bit UDH skip + stripped-UDHL guard |
| v4.0.23 | 2026-06-22 | 内存优化 (-40.7KB), rx_log 截断, 凭据清理 |
| v4.0.5 | 2026-06-18 | 泰文 DCS=0 修复, BOOT 15s |
| v4.0 | 2026-06-12 | 首版: 短短信 + pushplus + OTA |

完整 changelog 见 [GitHub Releases](https://github.com/xiangwhy/sms-forwarder-esp32/releases)。

## 路线图

见 [ROADMAP.md](ROADMAP.md)。

## License

私有项目。
