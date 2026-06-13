# SMS Forwarder v4.0

ESP32-S3 + USB 4G 模组 + WiFi 推送 pushplus。短消息已验证工作。

## 硬件

- **ESP32-S3 DevKitC-1** (8MB flash, USB Host OTG, 至少 1 个原生 USB 口给 4G 模组)
- **ML307** 4G 模组(或同类型 USB CDC + RNDIS 模组),USB 接 ESP32-S3 的 USB Host 口

## 引脚(本固件已确定)

| GPIO | 用途 |
|---|---|
| GPIO8 | 4G 模组 PWR(高电平上电) |
| GPIO7 | LED 4G 状态 |
| GPIO15 | LED WiFi 状态 |
| GPIO6 | LED NET 推送状态 |
| GPIO0 | BOOT 按钮 (内置,长按 5s 清 NVS) |

**LED 状态**:
- GPIO7: 4G 模组 AT 起来 → 常亮;等待中 → 慢闪
- GPIO15: WiFi 连上 → 常亮;未连 → 快闪;AP 模式 → 慢闪
- GPIO6: 推完一条短信 → 闪一下(短促)

## 编译

```bash
# 一次性
cd esp32-sms
./setup_components.sh   # 如果是旧仓库,拉 esp-iot-solution USB 组件

# 编译
pio run

# 烧
pio run -t upload --upload-port /dev/cu.usbserial-XXXX

# 看 log
pio device monitor -b 115200 -p /dev/cu.usbserial-XXXX
```

> **重要**:pioarduino 平台是必须的。已固定在 `platformio.ini`:
> ```
> platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
> ```
> 配套 IDF 5.5 + Arduino-ESP32 3.x。

## 首次配网

1. 烧完没配过 → 设备进入 **AP 模式**
2. 手机连 WiFi `SMS-Forwarder-XXXXXX`(无密码或 `12345678`)
3. 浏览器开 `192.168.4.1`
4. 填表:
   - WiFi SSID
   - WiFi 密码
   - pushplus token
   - pushplus topic (可空)
   - **OTA 用户名**(默认 admin)
   - **OTA 密码**(至少 4 位,防别人乱刷)
5. 保存 → 设备自动重启 → 启动后连 WiFi、连 4G、收 SMS、推送

## Web 页面

设备连上 WiFi 后,同网段浏览器:

| URL | 说明 |
|---|---|
| `/` | 跳转到 dashboard |
| `/dashboard` | 状态总览(30s 自动刷新) |
| `/api/status` | JSON 状态 (推送成功/失败/队列数) |
| `/update` | OTA 烧录 (弹窗输 OTA 用户密码) |

## 功能清单

| 功能 | 状态 | 备注 |
|---|---|---|
| USB CDC 接 4G 模组 (AT 通道) | ✅ | iot_usbh_cdc 3.x, itf=2 |
| 收 +CMT 短信(单条) | ✅ | UCS2 BE 解码,中英泰日文都支持 |
| 收 +CMT 短信(长短信 UDH 拼接) | ⚠️ | **已知不稳**: `+CMT: "True App"` 等字母数字 sender + 4 段以上 UDH 拼接可能丢段 |
| pushplus 推送 | ✅ | HTTPS,走 IDF 内置 CA bundle |
| NVS push 队列 | ✅ | 推失败落盘,30s 重发 |
| Web Dashboard | ✅ | 实时状态 |
| Web OTA | ✅ | BasicAuth,密码存 NVS |
| AP 配网 | ✅ | 无密码或默认 `12345678` |
| LED 状态 | ✅ | 3 灯 |
| BOOT 长按 5s 清 NVS | ✅ | GPIO0 长按 5s 触发 |
| RNDIS 4G 上网 | ❌ | 长期搁置,iot_eth 0.1.x 的 `stack_input` 没接好会 NULL deref panic |
| ArduinoOTA | ❌ | 用 web OTA 替代 |
| 数据用量统计 | ❌ | 不要 |

## 已知问题

1. **长短信 (UDH)**:当 sender 是字母数字 ID(`+CMT: "True App"` 这种)且长短信 total > 2 时,可能丢段。**临时方案**:用手机号 sender 测稳定,字母 sender 待修。
2. **RNDIS 4G 上网**:v4.0 禁用(编译时排除),只有 USB CDC 收短信可用。
3. **RNDIS 推送 fallback**:WiFi 挂时无 4G 推送通道,只能等 WiFi 恢复。

## 文件结构

```
esp32-sms/
├── platformio.ini          # 平台 + 库 + build flags
├── sdkconfig.defaults      # ESP-IDF 配置(FREERTOS_HZ=1000, USB Host 等)
├── partitions.csv          # 8MB flash dual-OTA 分区表
├── src/
│   ├── main.cpp           # 全部业务代码 (~1320 行,单文件)
│   ├── CMakeLists.txt     # main 组件 REQUIRES
│   └── idf_component.yml  # 依赖 iot_usbh_cdc ^3.0
├── managed_components/     # 旧版残留,新版本不再用
├── setup_components.sh     # 旧版拉 esp-iot-solution 脚本,新版本不需
├── v4.0.factory.bin       # 整包固件 (bootloader+partitions+app),1.3MB
├── README_v3.6.md          # v3.6 旧文档 (deprecated)
└── PLATFORMIO_GUIDE.md     # PIO 环境搭建指南
```

## v4.0 改动相对 v3.6.4

1. **平台从 IDF 4.4 升到 5.5** (via pioarduino 社区 platform)
2. **Arduino-ESP32 升 3.x** (旧 2.x)
3. **iot_usbh_cdc 升 3.x** API 重写(register_dev_event_cb + usbh_cdc_port_open)
4. **代码从 1640 行精简到 ~1320 行** (砍 ArduinoOTA/Dashboard/4G 统计/NVS 推队列/ping 监控等部分 v3.6.4 功能)
5. **RNDIS 4G 上网禁用** (长期搁置)
6. **短信编码从 v3.6.4 big-endian 切到 v4.0 little-endian 又回 big-endian** (调试过程产物,最终 v4.0 是 BE,与短短信工作结果一致)

## 开发

```bash
# 编译
pio run

# 烧 (需按 Ctrl+] 退 monitor)
pio run -t upload --upload-port /dev/cu.usbserial-11240

# 看实时 log
pio device monitor -b 115200 -p /dev/cu.usbserial-11240
```

## License

私有项目。
