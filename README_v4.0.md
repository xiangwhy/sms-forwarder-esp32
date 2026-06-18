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

## 凭据维护 (3 种改 NVS 的方法)

设备 6 个 NVS 凭据在 namespace `cfg` 下: `wifi.ssid` / `wifi.pass` / `pp.tok` / `pp.tpc` / `ota.user` / `ota.pass`。

| 方法 | 适用 | 风险 |
|---|---|---|
| **A. Web `/send` 配网** | 改一项 (e.g. pushplus token) | 最低, 标准流程 |
| **B. 长按 BOOT 5s 清 NVS → AP 配** | 全量重配 | bootCount / stat 不重置 (走 Preferences clear, 不是 NVS 全擦) |
| **C. NVS blob 灌入 (esptool)** | 改一项, 不进 web | 见下方坑 |

### 方法 C: NVS blob 灌入 (脱机改凭据)

> 用于: 板子没配过 (AP 模式死循环) / 想脚本化初始化 N 块板子 / 不方便进 web。

**坑**: `nvs_partition_gen.py` 的 CSV 第一行 **必须** 写 namespace 声明, 不然 6 个 key 全落到 namespace index 0 (ESP-IDF 系统 NS), Arduino `Preferences::begin("cfg")` 找不到, 全部回退占位符。

**步骤**:

1. 写 CSV (`/tmp/esp32-cfg.csv`):
   ```csv
   key,type,encoding,value
   cfg,namespace,,
   wifi.ssid,data,string,YOUR_WIFI_SSID
   wifi.pass,data,string,YOUR_WIFI_PASS
   pp.tok,data,string,YOUR_PUSHPLUS_TOKEN
   pp.tpc,data,string,
   ota.user,data,string,admin
   ota.pass,data,string,change-me
   ```
   **第一行 `<ns_name>,namespace,,` 不可省**。
   **警告**: CSV 含真实凭据,**绝对不能 commit 进仓库**,只放 `/tmp/` 用完即删。

2. 生成 blob:
   ```bash
   python3 /Users/xiang/.platformio/packages/framework-espidf/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py generate /tmp/esp32-cfg.csv /tmp/esp32-cfg.bin 0x6000
   ```
   0x6000 = 24KB, 与 partition table 一致。

3. 烧到 NVS 分区 (0x9000):
   ```bash
   python3 -m esptool --chip esp32s3 -p /dev/cu.usbserial-1240 write_flash 0x9000 /tmp/esp32-cfg.bin
   ```
   esptool 烧完自动 RTS 复位, 板子重启, 重新连 WiFi。

4. **副作用**: 重写整个 NVS 也会清掉 page 1 里 ESP-IDF WiFi 库自动存的 `sta.ssid`/`sta.pswd` 和 `cal_data` — 但 ESP32 boot 时会自己重建, 不影响。`bootCount` 重置回 0。

5. **恢复**: 烧之前先备份, `python3 -m esptool ... read_flash 0x9000 0x6000 /tmp/esp32-nvs-backup.bin`, 要回滚就 `write_flash 0x9000 /tmp/esp32-nvs-backup.bin`。

**验证**:

```bash
# 抓 log 看 Config 行
python3 -c "
import serial, time
ser = serial.Serial('/dev/cu.usbserial-1240', 115200, timeout=1)
ser.setRTS(False); time.sleep(0.2); ser.setRTS(True)
time.sleep(2); ser.reset_input_buffer()
end = time.time() + 8
buf = b''
while time.time() < end:
    chunk = ser.read(4096)
    if chunk: buf += chunk
print(buf.decode('utf-8', errors='replace'))
" | grep -E "Config:|WiFi connected|Web server"
```

期望看到:
```
Config: ssid=YOUR_WIFI_SSID token=YOUR_PUSHPLUS... ota_user=admin
WiFi connected, IP=192.168.1.130
Web server up (Dashboard / API / OTA)
```

## Web 页面

设备连上 WiFi 后,同网段浏览器:

| URL | 说明 |
|---|---|
| `/` | 跳转到 dashboard |
| `/dashboard` | 状态总览(30s 自动刷新) |
| `/api/status` | JSON 状态 (字段表见下) |

### `/api/status` 字段表

| 字段 | 类型 | 单位/含义 | 备注 |
|---|---|---|---|
| `boot` | int | 启动次数 (从 NVS `stat.bootCount` 累计) | 长按 GPIO0 5s 不重置 |
| `pushOk` | int | 推送成功累计 (atomic) | 推一条 pushplus 200 即 +1 |
| `pushFail` | int | 推送失败累计 (atomic) | 包含 WiFi down + HTTP 4xx/5xx |
| `qLen` | int | NVS 待推队列长度 (0–32) | pushQ 满时落 NVS,WiFi 恢复后 drain |
| `wifi` | bool | WiFi STA 是否连上 | 关联到 IP_EVENT_STA_GOT_IP |
| `wifiRssi` | int | WiFi RSSI (dBm),wifi off 时 0 | 调试信号强度用 |
| `freeHeap` | int | 当前空闲 heap (字节) | `esp_get_free_heap_size()` |
| `minFreeHeap` | int | 启动以来最小空闲 heap | `esp_get_minimum_free_heap_size()`,排查泄漏关键 |
| `lastSmsMs` | int | 上次收短信时间 (millis 上电后) | 0 表示从未收过;前端要换算成"X 秒前"需自己减 `Date.now()` |
| `lastPushOkMs` | int | 上次推送成功时间 (millis) | 同上 |
| `mlAlive` | bool | 4G 模组 (ML307) 是否 alive | setup 后置 true,watchdog 未实现 |
| `udhActive` | int | 当前活跃 UDH 长短信拼接槽 (0–4) | 长时间 >0 表示有 slot 卡死 |
| `fw` | str | 固件版本号 | 例 "v4.0.5" |

**示例**:
```json
{
  "boot": 42, "pushOk": 138, "pushFail": 2, "qLen": 0,
  "wifi": true, "wifiRssi": -58,
  "freeHeap": 87344, "minFreeHeap": 81200,
  "lastSmsMs": 3842100, "lastPushOkMs": 3842150,
  "mlAlive": true, "udhActive": 0,
  "fw": "v4.0.5"
}
```
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
| BOOT 长按 15s 清 NVS | ✅ | GPIO0 长按 15s 触发(+ 500ms debounce + 30s 启动 grace 防误触) |
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
│   ├── main.cpp           # 业务代码 (WiFi / Web / SMS / push 队列)
│   ├── pdu_codec.h        # PDU 解码函数声明 (纯 C++, host test 可跑)
│   ├── pdu_codec.cpp      # PDU 7-bit packed / UCS-2 / phone / UDH 解码
│   ├── CMakeLists.txt     # main 组件 REQUIRES
│   └── idf_component.yml  # 依赖 iot_usbh_cdc ^3.0
├── tests/
│   ├── host/              # C++ host test (PC 上跑,45 用例,绑定生产 pdu_codec.cpp)
│   └── test_pdu_parser.py # 旧版 Python 测试 (deprecated, 已被 C++ 替代)
├── v4.0.5.factory.bin     # 整包固件 (bootloader+partitions+app),1.13MB
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
