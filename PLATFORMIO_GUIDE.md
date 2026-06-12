# VSCode + PlatformIO 编译/烧录指南 — SMS Forwarder v3.6.4

> 翔哥：这是用 VSCode + PlatformIO 编译 v3.6.4 的完整步骤。
> 不需要装 ESP-IDF，不需要装 Arduino IDE。

---

## 1. 环境准备（一次性）

### 1.1 装 VSCode

到 https://code.visualstudio.com/ 下载 macOS 版，安装。

### 1.2 装 PlatformIO 扩展

VSCode → 左侧扩展图标 → 搜索 `PlatformIO IDE` → 安装。

装完后 VSCode 左下角出现 PlatformIO 的图标（蚂蚁头）。

### 1.3 装 USB 转串口驱动（如果用外置 USB-Serial 板）

大多数 ESP32-S3 DevKitC-1 用 **CH340** 或 **CP210x**：

- **CH340**: https://www.wch-ic.com/downloads/CH341SER_MAC_ZIP.html
- **CP210x**: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers

装完插板子 → 终端跑 `ls /dev/cu.usb*` 应该看到 `/dev/cu.usbserial-XXXX` 或 `/dev/cu.SLAB_USBtoUART`。

如果板子是 ESP32-S3 **原生 USB 口**（无外置 Serial 芯片），免驱。

---

## 2. 拉 USB Host 组件（一次性）

v3.6.4 用了 ESP-IDF 的 `iot_usbh_rndis`（4G 模组 USB 接口）和 `iot_usbh_cdc`（AT 命令通道）——这些不在 Arduino-ESP32 标准库里，需要从 espressif/esp-iot-solution 拉。

打开 VSCode 终端（`Ctrl+`` 或菜单 Terminal → New Terminal），跑：

```bash
cd esp32-sms/                  # 项目根目录
./setup_components.sh          # 自动 clone esp-iot-solution 到 components/
```

脚本会自动：
- clone espressif/esp-iot-solution v2.0.1（锁定版本）
- 拷贝 usb/ 子树到 components/
- 检查必需头文件是否齐

完成后应该看到：

```
[OK] iot_usbh_rndis.h -> components/usb/usb_host/include/iot_usbh_rndis.h
[OK] iot_usbh_cdc.h   -> components/usb/usb_host/include/iot_usbh_cdc.h
[OK] iot_eth.h        -> components/usb/...
[OK] iot_eth_netif_glue.h -> components/usb/...
```

如果有 `[MISS]`，告诉我，我换路径。

---

## 3. 打开项目 + 编译

### 3.1 VSCode 打开项目

VSCode → File → Open Folder → 选 `esp32-sms/` 文件夹。

VSCode 状态栏（左下角）会显示 `PlatformIO: esp32-s3-devkitc-1`，PIO 自动识别 platformio.ini。

### 3.2 第一次编译

终端跑：

```bash
pio run
```

或者 VSCode 左侧蚂蚁头 → PROJECT TASKS → `esp32-s3-devkitc-1` → **Build**。

第一次编译需要 5-15 分钟（拉 espressif32 平台 + Arduino-ESP32 框架 + 所有 lib_deps 库）。后续增量编译 < 30 秒。

### 3.3 烧录 + 监视

```bash
pio run -t upload -t monitor
```

或者 VSCode → PlatformIO → **Upload and Monitor**。

烧录完会自动开串口监视器（115200 baud）。看到启动 log：

```
[INFO ] === SMS Forwarder v3.6.4 (USB + RNDIS) ===
[INFO ] Config: ssid=你的WiFi token=abcd1234... ping=1
[INFO ] 4G USB CDC init OK, handle=0x3fcd1234
[INFO ] 4G module AT up
[INFO ] SIM ready
[INFO ] SMS engine configured
```

就 OK 了。

---

## 4. 常见编译错误

### 4.1 `fatal error: iot_usbh_rndis.h: No such file`

没跑 `./setup_components.sh`，USB Host 头文件没拉。

解决：

```bash
./setup_components.sh
pio run -t clean        # 清 build 缓存
pio run
```

### 4.2 `ArduinoJson.h: No such file` 或 `StaticJsonDocument` 找不到

`lib_deps` 没生效。检查：

```bash
pio pkg list                    # 看已装库
pio lib install "bblanchon/ArduinoJson@^7.0.0"   # 手动装
```

### 4.3 `undefined reference to app_main`

Arduino 框架不该有 `app_main` —— 但 v3.6.4.ino L1500 有 `extern "C" void app_main(void)`。

解决：项目根新建 `platformio_override.ini` 加：

```ini
[env:esp32-s3-devkitc-1]
build_flags =
    ${env:esp32-s3-devkitc-1.build_flags}
    -DARDUINO_MAIN_LOOP_DISABLE=0   ; 启用 Arduino 风格 setup/loop
```

或者直接把 `extern "C" void app_main` 改成 `void setup()` + `while(1) loop()`——但代码已经是 FreeRTOS 任务架构，不能用 setup/loop。

**真正解决**：Arduino-ESP32 框架 v2.x 起**支持 app_main**——只要不在 setup/loop 跑就行。当前代码 OK，不需要改。

如果还报错，可能是 Arduino-ESP32 版本太老。改 platformio.ini：

```ini
platform = espressif32 @ ~6.5.0
```

### 4.4 `Update.h` 找不到

`Update.h` 是 Arduino 内置 OTA 库，不需要 lib_deps。如果找不到说明 framework = arduino 没生效，检查 platformio.ini 的 framework 行。

### 4.5 烧录失败：`A fatal error occurred: Failed to connect to ESP32-S3`

板子没进烧录模式。**按住 BOOT 按钮 → 按一下 RESET → 松开 BOOT**——这时板子进下载模式，再点 Upload。

或者改 platformio.ini：

```ini
upload_protocol = esptool
upload_flags =
    --before default_reset
    --after hard_reset
```

---

## 5. 常用命令速查

```bash
# 只编译 (不烧录)
pio run

# 清 build 缓存重新编译
pio run -t clean && pio run

# 烧录
pio run -t upload

# 烧录 + 串口监视器
pio run -t upload -t monitor

# 只开串口监视器
pio device monitor -b 115200

# 退出监视器: Ctrl + ]

# 列出已装库
pio pkg list

# 更新库
pio pkg update

# 查 ESP-IDF 编译日志
pio run -v
```

---

## 6. 首次烧入后的配网

1. 烧入后第一次启动，NVS 没 SSID/token → **自动进 AP 模式**
2. 手机 WiFi 列表找 `SMS-Forwarder-XXXXXX`（**无密码**，5 分钟内配完）
3. 浏览器开 `192.168.4.1`
4. 填 WiFi SSID + 密码 + pushplus token → 保存
5. 设备**自动重启**，连 WiFi，推送开机卡到 pushplus

---

## 7. 烧录前必改 4 个常量

`src/main.cpp` L43-46：

```cpp
const char* WIFI_SSID     = "你的WiFi名称";
const char* WIFI_PASSWORD = "你的WiFi密码";
const char* PUSHPLUS_TOKEN = "你的pushplus_token";   // pushplus.plus 个人中心拿
const char* PUSHPLUS_TOPIC = "";                     // 留空 = 单点推送
```

或者保留默认，烧入后走 AP 模式配网页填。

---

## 8. 翔哥操作清单

按这个顺序跑一遍：

1. ☐ 装 VSCode + PlatformIO 扩展
2. ☐ 装 USB-Serial 驱动（CH340 / CP210x）
3. ☐ 打开 `esp32-sms/` 文件夹
4. ☐ 终端跑 `./setup_components.sh`
5. ☐ 终端跑 `pio run`（首次 5-15 分钟）
6. ☐ 把编译报错截屏给我
7. ☐ 编译通过后改 4 个常量 → `pio run -t upload`

翔哥跑第 5 步如果报错，把报错贴给我——大概率是 USB Host 组件路径问题或 ArduinoJson 版本问题。

---

## 附录：项目目录结构

```
esp32-sms/
├── platformio.ini              # ★ PlatformIO 配置 (新加)
├── setup_components.sh         # ★ USB Host 组件 bootstrap (新加)
├── README_v3.6.md              # 旧版说明
├── README_v3.6.md.bak          # 备份
├── CODE_REVIEW_v3.6.4.md       # Code Review 报告
├── src/
│   └── main.cpp                # ★ v3.6.4 主代码 (从 .ino 改名 + 复制)
├── components/                 # ★ setup_components.sh 生成
│   └── usb/                    #   iot_usbh_rndis/cdc/eth 等
├── sms_forwarder_v3.6.ino      # 旧版原文件
├── sms_forwarder_v3.6.ino.bak  # 备份
└── sms_forwarder_v3.6.4.ino    # 旧位置保留 (新版在 src/main.cpp)
```

翔哥装好 PlatformIO 后先跑第 4 步（setup_components.sh），完事贴一下输出。