/*
 * ============================================================================
 *  SMS Forwarder Pro v3.6  (ESP32-S3 + USB 4G Modem + RNDIS + pushplus)
 *  架构重构: 4 个 FreeRTOS 任务 + LED 独立任务
 *  编译: ESP-IDF 5.x 或 PlatformIO (esp32-s3 + arduino)
 *  与 v3.5.2 区别:
 *    - 4G 模组走 USB CDC (不是 UART)
 *    - 4G 网络走 RNDIS 拨号 (不是 4G PPP)
 *    - NET LED 用 esp_ping 真检测
 *    - 4 个 task 清晰分工
 *  注意: 编译框架从 Arduino 切到 ESP-IDF 风格, 但保留 ArduinoJson 等库
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
// (esp_task_wdt removed in v3.6.3 — unused)
#include <Preferences.h>
// (mbedtls removed in v3.6.3 — unused)
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <esp_ping.h>
#include <esp_http_client.h>
// (ping/ping_sock.h removed in v3.6.3 — esp_ping.h has it)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// USB Host CDC + RNDIS (需要 ESP-IDF 组件: iot_usbh_rndis, iot_usbh_cdc)
// 编译时需要在 CMakeLists.txt 或 platformio.ini 里 REQUIRES 这些组件
#include "iot_usbh_rndis.h"
#include "iot_usbh_cdc.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"

// =================== 配置 ===================
static const char* TAG = "SMSFWD";

const char* WIFI_SSID     = "你的WiFi名称";
const char* WIFI_PASSWORD = "你的WiFi密码";
const char* PUSHPLUS_TOKEN = "你的pushplus_token";
const char* PUSHPLUS_TOPIC = "";    // 留空 = 单点
const char* PUSHPLUS_TEMPLATE = "html";

#define G4_PWR_GPIO        8
#define LED_4G             7     // 按你 DEMO 接口定义
#define LED_WIFI           15
#define LED_NET            6
#define LED_ACTIVE_LEVEL   HIGH
// (LED_BLINK_MS 移除 v3.6.3 — led_update 硬编码周期, 宏没引用)
#define LED_4G_GOOD_CSQ    5

#define BOOT_BUTTON_PIN    0
#define BOOT_HOLD_MS       5000
#define AP_SSID_PREFIX     "SMS-Forwarder-"
#define FW_VERSION         "v3.6"

// 4G 模组硬复位
// (USE_HW_RESET_FOR_4G 移除 v3.6.3 — USB 模组内部管理, 不需要软件复位)
#define PIN_4G_PWRKEY        8   // G4_PWR_GPIO 同义
// (ML307_PWRKEY_LOW_MS / ML307_BOOT_WAIT_MS 移除 v3.6.3 — 不用 PWRKEY 复位)

// =================== 队列与同步 ===================
#define SMS_QUEUE_LEN      32
#define PUSH_QUEUE_LEN     32

typedef struct {
  char     phone[24];
  char     content[512];
  uint32_t timestamp;
} SmsMsg;

// v3.6.3 P0 修复: Queue 元素改固定 char 数组, 不用 String 引用
// 之前 sizeof(String) 拷贝后 SmsTask 析构局部 String -> NetTask use-after-free
typedef struct {
  char data[1024];  // pushplus payload JSON
  uint16_t len;     // 实际长度
} PushItem;

QueueHandle_t g_smsQ;
QueueHandle_t g_pushQ;
QueueHandle_t g_urcQ;          // v3.6.2: URC 队列
// v3.6.4 P0 修复: URC 队列 8 -> 32 (4G 模组 SIM 注册时短时 burst 十几条 URC, 8 满后丢老会丢 +CEREG)
#define URC_QUEUE_LEN 32
SemaphoreHandle_t g_atMutex;
// (g_configMutex removed in v3.6.3 — unused)

// =================== 4G 模组状态 ===================
typedef struct {
  bool    alive;        // 初始化过
  bool    simReady;     // AT+CPIN? READY
  uint8_t csq;
  uint8_t failRun;      // 连续失败
  bool    degraded;     // 熔断 DEGRADED
  bool    down;         // 熔断 DOWN
  uint32_t resetCount;
  uint32_t lastFailMs;
  uint32_t lastResetMs;
  uint32_t lastProbeMs;
  // v3.6.2: 健康监控改状态标记, 避免 monitor4GHealth 递归调 send_atcmd 死锁
  volatile bool needSoftReinit;   // 标记: 下次 AtTask 主循环应软 reinit
  volatile bool needHardReset;    // 标记: 下次 AtTask 主循环应硬复位
} Ml307State;
Ml307State g_ml = {false, false, 0, 0, false, false, 0, 0, 0, 0, false, false};

// =================== 网络状态 ===================
typedef struct {
  bool   wifiUp;
  bool   rndisUp;
  bool   rndisDown;       // v3.6.1: 4G 熔断标记
  // (wifiRssi removed in v3.6.3 — never written)
  uint32_t lastPingOkMs;
  uint32_t rndisFailRun;
  bool   pingInFlight;
  bool   lastPushVia4G;
  uint32_t lastPushMs;
  // v3.7: 4G 流量统计
  uint32_t today4gBytes;
  uint32_t month4gBytes;
  uint16_t todayDate;      // YYYYMMDD, 跨日清零
  uint16_t monthTag;       // YYYYMM, 跨月清零
  // v3.7 仪表盘: 推送统计 + 24h 历史
  uint32_t totalPushOk;    // 累计推送成功
  uint32_t totalPushFail;  // 累计推送失败
  uint32_t total4gPushOk;  // 累计 4G 推送成功
  uint32_t total4gPushFail;// 累计 4G 推送失败
  uint32_t totalSmsRecv;   // 累计收 SMS
  uint32_t bootCount;      // 启动次数
  uint8_t pushHistory[24]; // 24h 推送次数, 每小时一格 (index = hour)
  uint8_t smsHistory[24];
} NetState;
// 初始化 19 个字段 (v3.7 仪表盘加 7 个统计字段)
NetState g_net = {false, false, false, 0, 0, false, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}};

// =================== LED 状态 (DEMO 风格) ===================
typedef enum { LED_OFF = 0, LED_ON, LED_BLINK_SLOW, LED_BLINK_FAST } led_state_t;
// (led_action_t 移除 v3.6.3 — 事件触发用 LedCtrl.flashTrig bool 代替)

typedef struct {
  volatile led_state_t status;
  volatile bool flashTrig;  // 业务写, LedTask 读
} LedCtrl;

static LedCtrl g_led4g  = {LED_OFF, false};
static LedCtrl g_ledWifi = {LED_OFF, false};
static LedCtrl g_ledNet = {LED_OFF, false};

// =================== 配置 (NVS) ===================
static const char* NVS_CFG_NS = "cfg";
typedef struct {
  char ssid[64];
  char pass[64];
  char token[64];
  char topic[64];
  char tpl[16];
  bool pingEnabled;   // v3.6.3: NET 灯 ping 开关 (默认开)
} Config;
Config g_cfg;

void loadConfig() {
  Preferences p; p.begin(NVS_CFG_NS, true);
  String s = p.getString("wifi.ssid", "");
  strncpy(g_cfg.ssid, s.c_str(), sizeof(g_cfg.ssid)-1); g_cfg.ssid[sizeof(g_cfg.ssid)-1] = 0;
  s = p.getString("wifi.pass", "");
  strncpy(g_cfg.pass, s.c_str(), sizeof(g_cfg.pass)-1); g_cfg.pass[sizeof(g_cfg.pass)-1] = 0;
  s = p.getString("pp.tok", "");
  // (错拷行已删除)
  strncpy(g_cfg.token, s.c_str(), sizeof(g_cfg.token)-1); g_cfg.token[sizeof(g_cfg.token)-1] = 0;
  s = p.getString("pp.tpc", "");
  strncpy(g_cfg.topic, s.c_str(), sizeof(g_cfg.topic)-1); g_cfg.topic[sizeof(g_cfg.topic)-1] = 0;
  s = p.getString("pp.tpl", "html");
  strncpy(g_cfg.tpl, s.c_str(), sizeof(g_cfg.tpl)-1); g_cfg.tpl[sizeof(g_cfg.tpl)-1] = 0;
  g_cfg.pingEnabled = p.getBool("pp.ping", true);
  p.end();
  ESP_LOGI(TAG, "Config: ssid=%s token=%.8s... ping=%d", g_cfg.ssid, g_cfg.token, g_cfg.pingEnabled);
}

void saveConfig(const Config& c) {
  Preferences p; p.begin(NVS_CFG_NS, false);
  p.putString("wifi.ssid", c.ssid);
  p.putString("wifi.pass", c.pass);
  p.putString("pp.tok", c.token);
  p.putString("pp.tpc", c.topic);
  p.putString("pp.tpl", c.tpl);
  p.putBool("pp.ping", c.pingEnabled);
  p.end();
}

bool isConfigValid() {
  return strlen(g_cfg.ssid) > 0 && strlen(g_cfg.token) >= 16;
}

// =================== USB CDC AT 命令 ===================
usbh_cdc_handle_t cdc_at_handle = 0;
char cdc_at_reply[512];

typedef struct {
  char*    replyBuf;
  size_t   replyBufLen;
  size_t   replyLen;
  SemaphoreHandle_t done;
  int      result;
} at_cmd_ctx_t;

// v3.6.4 P0 修复: ctx 改静态双缓冲, 避免 send_atcmd 返回后 ctx 栈帧销毁,
// handle_at_line 异步回写时 hit 已释放栈地址 (典型 C++ 异步 UAF)
// 用法: send_atcmd 进入取 idx^1, 退出前 atomic 切 idx; handle_at_line 只读 g_currentCmd
static volatile at_cmd_ctx_t* g_currentCmd = NULL;
static at_cmd_ctx_t s_cmdCtx[2];           // 双缓冲
static volatile uint8_t s_cmdCtxIdx = 0;
// v3.6.2: g_urcLine/g_haveUrc 改用 g_urcQ 队列
// 短信 +CMT: 头配对状态 — 头一行进来存 g_cmtHeader, 下一行 (body) 进来合成一条 SmsMsg 入队
static char   g_cmtHeader[256] = {0};
static volatile bool g_waitingCmtBody = false;  // usb_rx_task 写, SmsTask 间接读

static void handle_at_line(const char* line) {
  // 短信 +CMT: 配对逻辑
  if (g_waitingCmtBody) {
    // v3.6.3 P1 修复: 若本行是 + 开头 (URC 抢断), 入 URC 队, 保留 waitingCmtBody
    // 否则下一行被 body 解析吃掉, 短信错配
    if (line[0] == '+') {
      xQueueSend(g_urcQ, line, 0);
      return;
    }
    // 上一行是 +CMT: 头, 这次收到的就是 body
    g_waitingCmtBody = false;
    // 解析 header 拿号码 hex (引号对之间)
    char phoneHex[64] = {0};
    char* p1 = strchr(g_cmtHeader, '"');
    if (p1) {
      char* p2 = strchr(p1+1, '"');
      if (p2) {
        size_t n = p2 - p1 - 1;
        if (n >= sizeof(phoneHex)) n = sizeof(phoneHex)-1;
        memcpy(phoneHex, p1+1, n);
        phoneHex[n] = 0;
      }
    }
    SmsMsg msg = {0};
    strncpy(msg.phone, phoneHex, sizeof(msg.phone)-1); msg.phone[sizeof(msg.phone)-1] = 0;
    strncpy(msg.content, line, sizeof(msg.content)-1); msg.content[sizeof(msg.content)-1] = 0;  // body 暂存 hex, SmsTask 解码
    msg.timestamp = time(nullptr);
    if (xQueueSend(g_smsQ, &msg, 0) != pdTRUE) {
      ESP_LOGW(TAG, "SmsQueue full, drop SMS from %s", phoneHex);
    } else {
      ESP_LOGI(TAG, "SMS queued: phoneHex=%s bodyLen=%u", phoneHex, (unsigned)strlen(line));
    }
    return;
  }
  if (strncmp(line, "+CMT:", 5) == 0) {
    // 短信 header — 缓存, 等下一行 body
    strncpy(g_cmtHeader, line, sizeof(g_cmtHeader)-1); g_cmtHeader[sizeof(g_cmtHeader)-1] = 0;
    g_waitingCmtBody = true;
    return;
  }
  // 其它 URC: +CEREG / +RING / +CUSD 等都以 + 开头
  if (line[0] == '+') {
    // v3.6.4 P0 修复: 满队列策略改进 — 先尝试入队, 满则强制踢一条最老的非 +CEREG 留空间
    // (CEREG 网络注册态是关键信号, NetTask 路由决策靠它; 丢后 4G 灯状态错)
    if (xQueueSend(g_urcQ, line, 0) != pdTRUE) {
      char dummy[256];
      // 优先踢非 CEREG
      for (int i = 0; i < URC_QUEUE_LEN; i++) {
        if (xQueueReceive(g_urcQ, dummy, 0) == pdTRUE) {
          if (strncmp(dummy, "+CEREG", 6) != 0) {
            // 踢掉这条, 立刻塞新行
            xQueueSend(g_urcQ, line, 0);
            break;
          } else {
            // 是 CEREG 还回去 (队列内部无序, 不能保证位置, 但至少不让状态态丢失)
            xQueueSend(g_urcQ, dummy, 0);
          }
        }
      }
    }
    return;
  }
  // 普通 AT 回复
  if (!g_currentCmd) {
    ESP_LOGW(TAG, "Orphan line: %s", line);
    return;
  }
  at_cmd_ctx_t* ctx = (at_cmd_ctx_t*)g_currentCmd;
  if (ctx->replyLen + strlen(line) + 2 < ctx->replyBufLen) {
    ctx->replyLen += snprintf(ctx->replyBuf + ctx->replyLen,
                              ctx->replyBufLen - ctx->replyLen,
                              "%s\n", line);
  }
  if (!strcmp(line, "OK")) { ctx->result = 0; xSemaphoreGive(ctx->done); }
  else if (!strcmp(line, "ERROR")) { ctx->result = -1; xSemaphoreGive(ctx->done); }
}

static void usb_rx_task(void* param) {
  usbh_cdc_handle_t handle = (usbh_cdc_handle_t)param;
  uint8_t buf[128];
  // v3.6.4 P0 修复: 256 -> 512, 防 3GPP UDH 长头 (UDHL 最大 140 字节 = 280 hex 字符) 截断
  static char line[512];
  static size_t lineLen = 0;
  while (1) {
    size_t dataLen = 0;
    usbh_cdc_get_rx_buffer_size(handle, &dataLen);
    if (dataLen == 0) {
      // 没数据时让步 CPU (防 100% 占用)
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    usbh_cdc_read_bytes(handle, buf, &dataLen, pdMS_TO_TICKS(100));
    // v3.6.2: 用 memchr 找 \n, 一次定位整行, 避免单字节逐个判断
    size_t pos = 0;
    while (pos < dataLen) {
      void* nl = memchr(buf + pos, '\n', dataLen - pos);
      size_t lineEnd = nl ? (size_t)((char*)nl - (char*)(buf + pos)) : (dataLen - pos);
      // 把这一段塞进 line (跳过 \r, 防止 153-154 字节单包丢字)
      for (size_t j = 0; j < lineEnd; j++) {
        char c = buf[pos + j];
        if (c == '\r') continue;
        if (lineLen < sizeof(line)-1) line[lineLen++] = c;
      }
      if (nl) {
        // v3.6.4 P0 修复: 截断保护 — 行超出 buffer 视为烂数据, 丢整行防错配 CMT body
        if (lineLen >= sizeof(line)-1) {
          ESP_LOGE(TAG, "AT line too long (>=%u), drop. first=%c", sizeof(line)-1, line[0]);
          lineLen = 0;
          pos += lineEnd + 1;
          continue;
        }
        line[lineLen] = 0;
        handle_at_line(line);
        lineLen = 0;
        pos += lineEnd + 1;  // 跳过 \n
      } else {
        pos = dataLen;  // 没收尾, 等下次
      }
    }
  }
}

// 4G 模组健康监控 (send_atcmd 末尾调用)
// v3.6.2: 只标记状态, 不递归 send_atcmd, 避免死锁
//   needSoftReinit -> AtTask 主循环检测到后执行软 reinit
//   needHardReset  -> AtTask 主循环检测到后执行硬复位 PWRKEY
#define ML307_L1_FAILS  3
#define ML307_MAX_RESETS 3
static void monitor4GHealth() {
  if (g_ml.failRun < ML307_L1_FAILS) return;
  if (millis() - g_ml.lastFailMs < 5000) return;  // 节流 5s
  g_ml.lastFailMs = millis();
  // 标记: 由 AtTask 在自己的循环里执行
  if (!g_ml.needSoftReinit && !g_ml.needHardReset) {
    g_ml.needSoftReinit = true;
    ESP_LOGW(TAG, "4G module suspected hung (fails=%u), mark needSoftReinit", g_ml.failRun);
  }
}

int send_atcmd(usbh_cdc_handle_t handle, const char* cmd,
               char* reply, size_t replyLen, TickType_t timeout) {
  TickType_t start = xTaskGetTickCount();
  if (xSemaphoreTake(g_atMutex, timeout) != pdTRUE) {
    g_ml.failRun++; g_ml.lastFailMs = millis();
    monitor4GHealth();
    return -1;
  }
  // v3.6.3 P1 修复: 算已花时间, ctx.done take 不能再用同一个 timeout
  TickType_t elapsed = xTaskGetTickCount() - start;
  TickType_t remain = (elapsed < timeout) ? (timeout - elapsed) : 0;
  // v3.6.3 并发优化: 信号量提到全局静态, 一次创建永生, 免堆碎片
  static SemaphoreHandle_t s_atDone = NULL;
  if (s_atDone == NULL) s_atDone = xSemaphoreCreateBinary();
  // v3.6.4 P0 修复: 关键顺序 — 先复位 s_atDone (吃掉上一次残留 give),
  // 再挂 g_currentCmd, 最后才发命令. 防止异步 OK 在挂上下文前已到导致假阳性.
  xSemaphoreTake(s_atDone, 0);
  // v3.6.4 P0 修复: ctx 用静态双缓冲, 避免 &ctx 栈变量外泄给异步上下文
  uint8_t myIdx = s_cmdCtxIdx ^ 1;          // 抢占对侧槽位
  at_cmd_ctx_t* ctx = &s_cmdCtx[myIdx];
  ctx->replyBuf = reply;
  ctx->replyBufLen = replyLen;
  ctx->replyLen = 0;
  ctx->done = s_atDone;
  ctx->result = -1;
  g_currentCmd = ctx;
  // 清空 CDC RX buffer 防残留
  size_t dummy; uint8_t tmp[64];
  while (1) { usbh_cdc_get_rx_buffer_size(handle, &dummy); if (!dummy) break; usbh_cdc_read_bytes(handle, tmp, &dummy, 0); }
  ESP_LOGI(TAG, "AT TX: %s", cmd);
  usbh_cdc_write_bytes(handle, (uint8_t*)cmd, strlen(cmd), pdMS_TO_TICKS(100));
  if (xSemaphoreTake(ctx->done, remain) != pdTRUE) ctx->result = -2;
  g_currentCmd = NULL;
  s_cmdCtxIdx = myIdx;                       // 本次槽位回收, 下次用对侧
  xSemaphoreGive(g_atMutex);
  // 失败计数 + 健康监控
  if (ctx->result != 0) {
    g_ml.failRun++; g_ml.lastFailMs = millis();
    monitor4GHealth();
  } else if (g_ml.failRun > 0) {
    g_ml.failRun = 0;  // 成功清零
  }
  return ctx->result;
}

// =================== AtTask: 4G 模组初始化 + 短信接收 ===================
static int check_signal() {
  int signal = 0;
  if (send_atcmd(cdc_at_handle, "AT+CSQ\r\n", cdc_at_reply, sizeof(cdc_at_reply), pdMS_TO_TICKS(2000)) == 0) {
    int ber = 0;
    char* p = strstr(cdc_at_reply, "+CSQ:");
    if (p) sscanf(p, "+CSQ: %d,%d", &signal, &ber);
  }
  return signal;
}

static void AtTask(void* param) {
  // v3.6.3: USB CDC 初始化已移至 app_main, 避免 cdc_at_handle=0 时 send_atcmd crash
  // v3.6.3: g_atMutex 在 app_main 已创建

  // 等待 AT 探活 (最多 3 次成功)
  int atOk = 0;
  while (atOk < 3) {
    if (send_atcmd(cdc_at_handle, "AT\r\n", cdc_at_reply, sizeof(cdc_at_reply), pdMS_TO_TICKS(2000)) == 0) {
      atOk++;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  g_ml.alive = true;
  ESP_LOGI(TAG, "4G module AT up");

  // SIM 卡就绪 (v3.6.2: 加 60s 超时, 没 SIM 也别卡死)
  uint32_t simWaitStart = millis();
  while (millis() - simWaitStart < 60000) {
    if (send_atcmd(cdc_at_handle, "AT+CPIN?\r\n", cdc_at_reply, sizeof(cdc_at_reply), pdMS_TO_TICKS(3000)) == 0) {
      if (strstr(cdc_at_reply, "READY")) { g_ml.simReady = true; break; }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  if (!g_ml.simReady) {
    ESP_LOGE(TAG, "SIM card not ready in 60s, will mark needHardReset in main loop");
  } else {
    ESP_LOGI(TAG, "SIM ready");
  }

  // 短信配置
  send_atcmd(cdc_at_handle, "AT+CMGF=1\r\n", cdc_at_reply, sizeof(cdc_at_reply), pdMS_TO_TICKS(2000));
  send_atcmd(cdc_at_handle, "AT+CSCS=\"UCS2\"\r\n", cdc_at_reply, sizeof(cdc_at_reply), pdMS_TO_TICKS(2000));
  send_atcmd(cdc_at_handle, "AT+CNMI=2,2,0,0,0\r\n", cdc_at_reply, sizeof(cdc_at_reply), pdMS_TO_TICKS(2000));
  ESP_LOGI(TAG, "SMS engine configured");

  // 主循环: 检查信号 + 处理 URC (短信) + 长短信超时清理 + 4G 健康恢复
  while (1) {
    g_ml.csq = check_signal();

    // v3.6.2: 处理 monitor4GHealth 标记的恢复操作
    if (g_ml.needHardReset) {
      g_ml.needHardReset = false;
      ESP_LOGE(TAG, "Executing HARD RESET (PWRKEY) ...");
      gpio_set_level(PIN_4G_PWRKEY, HIGH); vTaskDelay(pdMS_TO_TICKS(50));
      gpio_set_level(PIN_4G_PWRKEY, LOW);  vTaskDelay(pdMS_TO_TICKS(1500));
      gpio_set_level(PIN_4G_PWRKEY, HIGH); vTaskDelay(pdMS_TO_TICKS(3000));
      g_ml.resetCount++;
      g_ml.lastResetMs = millis();
      ESP_LOGI(TAG, "4G hardware reset #%u done", g_ml.resetCount);
      if (g_ml.resetCount > ML307_MAX_RESETS) {
        ESP_LOGE(TAG, "4G reset %u > %u, ESP.restart()", g_ml.resetCount, ML307_MAX_RESETS);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP.restart();
      }
      g_ml.alive = false; g_ml.simReady = false;
      g_ml.failRun = 0;
      // v3.6.3 P1 修复: 硬复位后不立刻 soft reinit, 等 5 秒让模组启动
      // 之前: 立刻软 reinit -> 失败 -> 升级到硬复位 -> 循环 (3 次 ESP.restart)
      // 现在: 等 5 秒, 软 reinit, 失败再升级
      vTaskDelay(pdMS_TO_TICKS(5000));
      g_ml.needSoftReinit = true;
    }
    if (g_ml.needSoftReinit) {
      g_ml.needSoftReinit = false;
      ESP_LOGW(TAG, "Executing SOFT REINIT ...");
      send_atcmd(cdc_at_handle, "AT+CMGF=1\r\n", cdc_at_reply, sizeof(cdc_at_reply), pdMS_TO_TICKS(2000));
      send_atcmd(cdc_at_handle, "AT+CSCS=\"UCS2\"\r\n", cdc_at_reply, sizeof(cdc_at_reply), pdMS_TO_TICKS(2000));
      send_atcmd(cdc_at_handle, "AT+CNMI=2,2,0,0,0\r\n", cdc_at_reply, sizeof(cdc_at_reply), pdMS_TO_TICKS(2000));
      // 探一下, 失败再升级到硬复位
      if (send_atcmd(cdc_at_handle, "AT+CSQ\r\n", cdc_at_reply, sizeof(cdc_at_reply), pdMS_TO_TICKS(2000)) != 0) {
        ESP_LOGE(TAG, "Soft reinit still failing, escalate to hard reset");
        g_ml.needHardReset = true;
      } else {
        ESP_LOGI(TAG, "4G recovered via soft reinit");
        g_ml.alive = true; g_ml.simReady = true;
        g_ml.failRun = 0;
      }
    }

    char urcLine[257] = {0};
    if (xQueueReceive(g_urcQ, urcLine, pdMS_TO_TICKS(2000)) == pdTRUE) {
      urcLine[256] = 0;  // v3.6.3 P1: 强制封底 (队列元素 256 字节, 防止末尾无 \0)
      ESP_LOGI(TAG, "URC: %s", urcLine);
      if (strstr(urcLine, "+CEREG: 1") || strstr(urcLine, "+CEREG: 5")) {
        ESP_LOGI(TAG, "Network registered (CEREG)");
        g_led4g.flashTrig = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// =================== SmsTask: UCS2 解码 + pushplus payload ===================
static String ucs2ToUtf8(const String& hex) {
  String out; out.reserve(hex.length()/2);
  auto nib = [](char c)->uint8_t {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='A'&&c<='F') return c-'A'+10;
    if (c>='a'&&c<='f') return c-'a'+10;
    return 0;
  };
  for (size_t i=0; i+3<hex.length(); i+=4) {
    uint16_t u = (nib(hex[i])<<12)|(nib(hex[i+1])<<8)|(nib(hex[i+2])<<4)|nib(hex[i+3]);
    // v3.6.4 P0 修复: 拒绝非法码点 (UCS2 hex 损坏或编码异常可能产生 >0x10FFFF)
    // 3 字节 UTF-8 上限 0xFFFF 已覆盖 BMP, 但 >0x10FFFF 序列非法 JSON 序列化器会 panic
    if (!u || u > 0x10FFFF) continue;
    if (u<0x80) out+=(char)u;
    else if (u<0x800) { out+=(char)(0xC0|(u>>6)); out+=(char)(0x80|(u&0x3F)); }
    else if (u < 0xD800 || u > 0xDFFF) {  // 跳过 surrogate pair
      out+=(char)(0xE0|(u>>12)); out+=(char)(0x80|((u>>6)&0x3F)); out+=(char)(0x80|(u&0x3F));
    }
  }
  return out;
}

// UDH 长短信解析 (3GPP 23.040)
// 文本模式下, 长短信 body 开头是 hex 编码的 UDH:
//   <UDHL><IEI><IEDL><IED>...<UD>
// 常见 IEI: 0x00 (concat 8bit) -> 050003<RR><TP><PI>
// 我们的策略: 扫 UDH, 如果发现 IEI=0x00 (concat 8bit), 缓存 (RR, PI) -> TP 这片
//            收齐 (PI+1 == TP) 才合成 dispatch
typedef struct {
  uint8_t  refId;
  uint8_t  totalParts;
  uint8_t  partIndex;
  char     phone[24];   // 发件号码 (UCS2 hex), 防止 refId 碰撞
  char     body[1024];  // v3.6.3 P1: 加大到 1024, 装长短信 (之前 384 会截断)
  bool     received;
} UdhPart;
#define MAX_UDH_REFS 4
#define MAX_UDH_PARTS 8
static UdhPart g_udhTable[MAX_UDH_REFS][MAX_UDH_PARTS];
static uint8_t g_udhReceived[MAX_UDH_REFS];
static uint8_t g_udhTotal[MAX_UDH_REFS];
static unsigned long g_udhFirstMs[MAX_UDH_REFS];  // 首次缓存时间, 用于超时清理
#define UDH_TIMEOUT_MS  60000UL   // 60s 没收齐视为丢片

// 返回值: -1 = 不是长短信 (普通单条)
//         >=0 = UDH refId, 告知这是某长短信的第几片
// 输出参数: partIndex, totalParts, bodyWithoutUdh
static int parseUdh(const String& ucs2Body, uint8_t& partIndex, uint8_t& totalParts, String& bodyOut) {
  // UDH 头是 1 字节 UDHL (hex 2 个字符) + IEI + IEDL + IED 数据
  // 简化: 找 "000300" 模式 (IEI=00 concat 8bit, IEDL=03, 然后 RR TP PI)
  if (ucs2Body.length() < 14) return -1;  // 至少 7 字节 hex = 14 字符
  // UDHL = first byte (hex 2 chars)
  // IEI  = next byte = "00"
  // IEDL = next byte = "03" (concat 8bit 必有 3 字节 IED)
  String iei = ucs2Body.substring(2, 4);  // UDHL 之后 1 字节 IEI
  String iedl = ucs2Body.substring(4, 6);
  if (iei != "00" || iedl != "03") return -1;
  String rr = ucs2Body.substring(6, 8);
  String tp = ucs2Body.substring(8, 10);
  String pi = ucs2Body.substring(10, 12);
  uint8_t refId, total, idx;
  auto nib2 = [](const String& s, int off) -> uint8_t {
    auto h2n = [](char c) -> uint8_t {
      if (c>='0'&&c<='9') return c-'0';
      if (c>='A'&&c<='F') return c-'A'+10;
      if (c>='a'&&c<='f') return c-'a'+10;
      return 0;
    };
    return (h2n(s[off])<<4) | h2n(s[off+1]);
  };
  refId = nib2(rr, 0);
  total = nib2(tp, 0);
  idx   = nib2(pi, 0);
  if (total < 2 || total > MAX_UDH_PARTS || idx >= total) return -1;
  // body 是 UDH 之后的内容: 偏移 12 字符 (6 字节 UDH)
  bodyOut = ucs2Body.substring(12);
  partIndex = idx;
  totalParts = total;
  return refId;
}

// 长短信合并: 把一片塞入对应 refId/idx 的槽
// 返回 true = 该 refId 收齐了所有片
static bool stashUdhPart(int refId, uint8_t partIndex, uint8_t totalParts, const String& phone, const String& body) {
  if (refId < 0 || refId >= MAX_UDH_REFS) return false;
  if (partIndex >= MAX_UDH_PARTS) return false;
  UdhPart* p = &g_udhTable[refId][partIndex];
  // 校验发件人: refId 碰撞时防止错乱
  if (g_udhTotal[refId] > 0) {
    for (int i = 0; i < MAX_UDH_PARTS; i++) {
      if (g_udhTable[refId][i].received &&
          strcmp(g_udhTable[refId][i].phone, phone.c_str()) != 0) {
        ESP_LOGW(TAG, "UDH refId=%d phone mismatch! reset slot", refId);
        clearUdhRef(refId);
        break;
      }
    }
  }
  p->refId = refId;
  p->totalParts = totalParts;
  p->partIndex = partIndex;
  strncpy(p->phone, phone.c_str(), sizeof(p->phone)-1); p->phone[sizeof(p->phone)-1] = 0;
  strncpy(p->body, body.c_str(), sizeof(p->body)-1); p->body[sizeof(p->body)-1] = 0;
  p->received = true;
  g_udhReceived[refId]++;
  g_udhTotal[refId] = totalParts;
  if (g_udhFirstMs[refId] == 0) g_udhFirstMs[refId] = millis();  // 记首次
  return (g_udhReceived[refId] >= g_udhTotal[refId]);
}

// 收齐后 / 超时后清理: 重置 refId 槽位
static void clearUdhRef(int refId) {
  uint8_t total = g_udhTotal[refId];
  for (uint8_t i = 0; i < total; i++) g_udhTable[refId][i].received = false;
  for (uint8_t i = 0; i < total; i++) g_udhTable[refId][i].body[0] = 0;
  g_udhReceived[refId] = 0;
  g_udhTotal[refId] = 0;
  g_udhFirstMs[refId] = 0;
}

// 只拼接已收到的部分 (按 partIndex 顺序, 缺片处用空格占位)
static String concatUdhPartial(int refId) {
  String out;
  uint8_t total = g_udhTotal[refId];
  for (uint8_t i = 0; i < total; i++) {
    if (g_udhTable[refId][i].received) {
      out += String(g_udhTable[refId][i].body);
    } else {
      out += String("[第") + String((unsigned)(i+1)) + "片丢失]";
    }
  }
  clearUdhRef(refId);
  return out;
}

// 超时检查: AtTask 主循环调用, 每 10s 一次
static void checkUdhTimeouts() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_UDH_REFS; i++) {
    if (g_udhReceived[i] > 0 && g_udhReceived[i] < g_udhTotal[i]) {
      // 收齐了一部分, 但还没齐
      if (g_udhFirstMs[i] > 0 && (now - g_udhFirstMs[i]) > UDH_TIMEOUT_MS) {
        uint8_t got = g_udhReceived[i];
        uint8_t total = g_udhTotal[i];
        ESP_LOGW(TAG, "UDH refId=%d timeout (got %u/%u, dispatch partial)",
                 i, got, total);
        // v3.6.3 P1: partialHex 加 PARTIAL: 前缀, SmsTask 看到加提示文案
        // (避免 ucs2ToUtf8 翻译"第片丢失"中文为乱码)
        String partialHex = String("PARTIAL:") + concatUdhPartial(i);
        // 这里不能直接 push, 因为是 SmsTask 干的事, 所以入 SmsQueue 走正常路径
        SmsMsg msg = {0};
        strncpy(msg.phone, "(UDH timeout)", sizeof(msg.phone)-1); msg.phone[sizeof(msg.phone)-1] = 0;
        strncpy(msg.content, partialHex.c_str(), sizeof(msg.content)-1); msg.content[sizeof(msg.content)-1] = 0;
        msg.timestamp = time(nullptr);
        xQueueSend(g_smsQ, &msg, 0);
      }
    }
  }
}

// 收齐后拼出完整 UCS2 hex body
// v3.6.2 优化: 预估总长, 一次 reserve 避免多次 realloc
static String concatUdhParts(int refId) {
  uint8_t total = g_udhTotal[refId];
  size_t totalLen = 0;
  for (uint8_t i = 0; i < total; i++) totalLen += strlen(g_udhTable[refId][i].body);
  String out;
  out.reserve(totalLen);  // 一次到位
  for (uint8_t i = 0; i < total; i++) out += g_udhTable[refId][i].body;
  clearUdhRef(refId);
  return out;
}

// v3.6.2 优化: 改用 stack 缓冲 + snprintf, 避免 4 次 String 拼接
static String buildPushPayload(const String& phone, const String& content, uint32_t ts) {
  StaticJsonDocument<1536> doc;
  doc["token"]    = g_cfg.token;
  doc["template"] = g_cfg.tpl;

  // title: stack 拼接
  char title[80];
  snprintf(title, sizeof(title), "\xF0\x9F\x93\xA9 %s", phone.c_str());
  doc["title"] = title;

  // content: HTML 转义, 一次 snprintf
  // v3.6.3 P1: 700 -> 1024 装长短信 (UDH 8 片 = 536 字符 hex, 转 UTF-8 后 1072 字节 worst case)
  char escaped[1024];
  size_t i = 0, j = 0;
  while (content[i] && j < sizeof(escaped) - 7) {
    char c = content[i];
    if (c == '&')       { memcpy(escaped+j, "&amp;",  5); j += 5; }
    else if (c == '<')  { memcpy(escaped+j, "&lt;",   4); j += 4; }
    else if (c == '>')  { memcpy(escaped+j, "&gt;",   4); j += 4; }
    else if (c == '"') { memcpy(escaped+j, "&quot;", 6); j += 6; }
    else                { escaped[j++] = c; }
    i++;
  }
  escaped[j] = 0;
  char contentBuf[1100];
  snprintf(contentBuf, sizeof(contentBuf),
           "<p>%s</p><hr><small>ts: %lu</small>",
           escaped, (unsigned long)ts);
  doc["content"] = contentBuf;

  if (strlen(g_cfg.topic) > 0) doc["topic"] = g_cfg.topic;

  String out; serializeJson(doc, out);
  return out;
}

static void SmsTask(void* param) {
  // v3.6.2: 用 vTaskDelayUntil 替代 millis() 差值, 免疫 50 天回绕
  TickType_t xLastWakeTime = xTaskGetTickCount();
  while (1) {
    // 精准每 1000ms 唤醒, 扫描 UDH 超时
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
    checkUdhTimeouts();

    SmsMsg msg;
    // 队列非阻塞读: 有就处理, 没有就继续等下一秒
    if (xQueueReceive(g_smsQ, &msg, 0) == pdTRUE) {
      String phoneUtf8 = ucs2ToUtf8(String(msg.phone));  // 号码 UCS2 -> UTF-8
      String bodyHex = String(msg.content);             // body 是 UCS2 hex (可能含 UDH)
      // v3.6: 尝试 UDH 解析 (长短信)
      uint8_t partIdx = 0, totalParts = 0;
      String bodyWithoutUdh;
      int refId = parseUdh(bodyHex, partIdx, totalParts, bodyWithoutUdh);
      if (refId >= 0) {
        // 长短信: 缓存这一片
        ESP_LOGI(TAG, "Long SMS part %u/%u from %s, refId=%d",
                 partIdx+1, totalParts, phoneUtf8.c_str(), refId);
        bool done = stashUdhPart(refId, partIdx, totalParts, phoneUtf8, bodyWithoutUdh);
        if (done) {
          String fullBodyHex = concatUdhParts(refId);
          String content = ucs2ToUtf8(fullBodyHex);
          ESP_LOGI(TAG, "Long SMS assembled: %u parts, %u chars", totalParts, (unsigned)content.length());
          // 注: 这是收齐的长短信, 不带 PARTIAL: 前缀, 走普通 ucs2ToUtf8
          String p = buildPushPayload(phoneUtf8, content, msg.timestamp);
          PushItem item = {};
          strncpy(item.data, p.c_str(), sizeof(item.data)-1); item.data[sizeof(item.data)-1] = 0;
          item.len = strlen(item.data);
          xQueueSend(g_pushQ, &item, 0);
          g_led4g.flashTrig = true;
        }
        // 没收齐: 不 dispatch, 等下一片
      } else {
        String content;
        // v3.6.3 P1: UDH 部分体 (PARTIAL: 前缀) 跳过 UCS2 解码, 加提示
        if (bodyHex.startsWith("PARTIAL:")) {
          content = bodyHex.substring(8) + String(" [部分内容已丢失]");
        } else {
          content = ucs2ToUtf8(bodyHex);
        }
        ESP_LOGI(TAG, "SMS from %s: %s", phoneUtf8.c_str(), content.c_str());
        String p = buildPushPayload(phoneUtf8, content, msg.timestamp);
        PushItem item = {};
        strncpy(item.data, p.c_str(), sizeof(item.data)-1); item.data[sizeof(item.data)-1] = 0;
        item.len = strlen(item.data);
        xQueueSend(g_pushQ, &item, 0);
        g_led4g.flashTrig = true;
      }
    }
  }
}

// 4G RNDIS 推 pushplus (走 esp_http_client + eth_netif)
static bool postPushplusViaRndis(const String& payload) {
  if (!eth_netif) {
    ESP_LOGW(TAG, "4G RNDIS: eth_netif not ready");
    return false;
  }
  esp_http_client_config_t cfg = {
    .url = "https://www.pushplus.plus/send",
    .method = HTTP_METHOD_POST,
    .cert_pem = NULL,        // 公网 CA 走系统信任
    .skip_cert_common_name_check = false,
    .timeout_ms = 10000,
    .use_global_ca_store = true,
    .if_name = esp_netif_get_ifkey(eth_netif),  // ★ 关键: 绑 4G 网口
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return false;
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, payload.c_str(), payload.length());
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "4G RNDIS push err=%d", err);
    return false;
  }
  ESP_LOGI(TAG, "4G RNDIS push OK (HTTP %d)", status);
  return (status >= 200 && status < 300);
}

// =================== NVS 离线队列 (推送失败时暂存, 后续补发) ===================
#define QUEUE_RETRY_MS 30000

// v3.6.3 P0 修复: Preferences 不支持嵌套 begin, 用单次 begin 多操作
// 原来 nvsQDepth/Enqueue/Drain 各自 begin/end, 嵌套 / 二次 begin 会失败
// 现在合并到 single helper, 一次 begin 多操作
#define NVS_Q_NS      "q"
#define NVS_Q_KEY_HD  "head"   // 消费点
#define NVS_Q_KEY_TL  "tail"   // 入队点
#define NVS_Q_PREFIX  "k"
#define NVS_Q_MAX     32

static uint32_t nvsQReadHeadTail(uint32_t* head, uint32_t* tail) {
  Preferences p; p.begin(NVS_Q_NS, true);
  *head = p.getUInt(NVS_Q_KEY_HD, 0);
  *tail = p.getUInt(NVS_Q_KEY_TL, 0);
  p.end();
  return *tail - *head;  // depth
}

// v3.7: 4G 流量管理
#define NVS_DATA_NS     "data"
#define NVS_DATA_TODAY  "td_bytes"
#define NVS_DATA_MONTH  "mo_bytes"
#define NVS_DATA_TDATE  "td_date"   // YYYYMMDD
#define NVS_DATA_MTAG   "mo_tag"    // YYYYMM
#define DAILY_LIMIT_BYTES_DEFAULT  (100UL * 1024UL * 1024UL)  // 100MB/天
#define MONTHLY_LIMIT_BYTES_DEFAULT (3UL * 1024UL * 1024UL * 1024UL)  // 3GB/月

// 取今天日期 YYYYMMDD
static uint16_t getTodayDate() {
  time_t t = time(nullptr);
  if (t < 100000) return 0;  // NTP 未同步
  struct tm* tm = localtime(&t);
  return (tm->tm_year + 1900) * 10000 + (tm->tm_mon + 1) * 100 + tm->tm_mday;
}

// 取本月标签 YYYYMM
static uint16_t getMonthTag() {
  time_t t = time(nullptr);
  if (t < 100000) return 0;
  struct tm* tm = localtime(&t);
  return (tm->tm_year + 1900) * 100 + (tm->tm_mon + 1);
}

// 检查并跨日/跨月清零
static void checkAndResetDataBucket() {
  uint16_t today = getTodayDate();
  uint16_t month = getMonthTag();
  if (today == 0) return;  // NTP 未同步
  Preferences p; p.begin(NVS_DATA_NS, false);
  uint16_t savedDate = p.getUInt(NVS_DATA_TDATE, 0);
  uint16_t savedMonth = p.getUInt(NVS_DATA_MTAG, 0);
  if (savedDate != today) {
    p.putUInt(NVS_DATA_TDATE, today);
    p.putUInt(NVS_DATA_TODAY, 0);
    if (today != 0) ESP_LOGI(TAG, "Data bucket: new day %u, reset today bytes", today);
  }
  if (savedMonth != month && month != 0) {
    p.putUInt(NVS_DATA_MTAG, month);
    p.putUInt(NVS_DATA_MONTH, 0);
    ESP_LOGI(TAG, "Data bucket: new month %u, reset month bytes", month);
  }
  // 同步到 g_net
  g_net.today4gBytes = p.getUInt(NVS_DATA_TODAY, 0);
  g_net.month4gBytes = p.getUInt(NVS_DATA_MONTH, 0);
  g_net.todayDate = savedDate;
  g_net.monthTag = savedMonth;
  p.end();
}

// 累加 4G 流量 (推送成功后调)
static void add4gDataUsage(uint32_t bytes) {
  g_net.today4gBytes += bytes;
  g_net.month4gBytes += bytes;
  Preferences p; p.begin(NVS_DATA_NS, false);
  p.putUInt(NVS_DATA_TODAY, g_net.today4gBytes);
  p.putUInt(NVS_DATA_MONTH, g_net.month4gBytes);
  p.end();
}

// 判定是否超今日 limit
static bool isDailyDataOverLimit() {
  return g_net.today4gBytes >= DAILY_LIMIT_BYTES_DEFAULT;
}
// 判定是否超本月 limit
static bool isMonthlyDataOverLimit() {
  return g_net.month4gBytes >= MONTHLY_LIMIT_BYTES_DEFAULT;
}

static void nvsQEnqueue(const String& payload) {
  // v3.6.3 P0: 单次 begin, 读 head/tail, 写, 不嵌套
  Preferences p;
  // v3.6.4 P1 修复 (E2): begin 失败时打日志返回, 避免后续 put* 静默失败
  if (!p.begin(NVS_Q_NS, false)) {
    ESP_LOGE(TAG, "NVS begin failed, drop enqueue to avoid dup");
    return;
  }
  uint32_t head = p.getUInt(NVS_Q_KEY_HD, 0);
  uint32_t tail = p.getUInt(NVS_Q_KEY_TL, 0);
  // v3.6.3 P1 修复: 满了不再 32 次循环 remove, 直接 p.clear() 整个 namespace
  // (32 次 NVS 写约 160ms, 会阻塞 NetTask 5+ 秒)
  if (tail - head >= NVS_Q_MAX) {
    // v3.6.3 P1 修复: 只淘汰最老 1 个, 保留其余 31 个 (不丢全部)
    // (之前 p.clear() 会丢全部 32 条, 太激进)
    char oldK[16]; snprintf(oldK, sizeof(oldK), NVS_Q_PREFIX "%lu", (unsigned long)head);
    p.remove(oldK);
    head++;
    ESP_LOGW(TAG, "NVS queue full, dropped oldest");
  }
  char key[16]; snprintf(key, sizeof(key), NVS_Q_PREFIX "%lu", (unsigned long)tail);
  size_t put = p.putString(key, payload.c_str());
  // v3.6.4 P1 修复: putString 失败返回 0, 此时不更新 tail, 避免空洞 + dup
  if (put == 0) {
    ESP_LOGE(TAG, "NVS putString failed, tail not advanced");
    p.end();
    return;
  }
  p.putUInt(NVS_Q_KEY_TL, tail + 1);
  p.putUInt(NVS_Q_KEY_HD, head);  // head 也写回去 (淘汰了的话)
  p.end();
  ESP_LOGD(TAG, "NVS queue enqueue, depth=%lu", (unsigned long)(tail - head + 1));
}

static size_t nvsQDepth(void) {
  uint32_t h, t;
  return nvsQReadHeadTail(&h, &t);
}

static void nvsQDrain(void) {
  // v3.6.3 P0: 单次 begin, 读 peek, 试发, 成功就 ack (head++), 不嵌套
  uint32_t head, tail;
  if (nvsQReadHeadTail(&head, &tail) == 0) return;
  // 一次性 read+write 都走同一 begin
  Preferences p; p.begin(NVS_Q_NS, false);
  char key[16]; snprintf(key, sizeof(key), NVS_Q_PREFIX "%lu", (unsigned long)head);
  String payload = p.getString(key, "");
  if (payload.length() == 0) {
    // 空洞 (老 key 之前淘汰了没删干净) - 直接跳
    p.remove(key);
    p.putUInt(NVS_Q_KEY_HD, head + 1);
    p.end();
    return;
  }
  // 尝试 WiFi 发送
  bool sent = false;
  if (g_net.wifiUp) {
    HTTPClient http;
    http.begin("https://www.pushplus.plus/send");
    http.addHeader("Content-Type", "application/json");
    sent = (http.POST(payload) == 200);
    http.end();
  }
  if (sent) {
    p.remove(key);  // v3.6.3 P0: 真删尾, 不只是前移
    p.putUInt(NVS_Q_KEY_HD, head + 1);
    ESP_LOGI(TAG, "NVS queue drained: %lu -> %lu", (unsigned long)(tail - head), (unsigned long)(tail - head - 1));
  }
  // 失败: 保留 (下次再发)
  p.end();
}

// =================== NetTask: WiFi + RNDIS 拨号 + pushplus 推 ===================
static esp_netif_t* eth_netif = NULL;
static iot_eth_handle_t eth_handle = NULL;
static iot_eth_driver_t* rndis_handle = NULL;

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (id == WIFI_EVENT_STA_DISCONNECTED) g_net.wifiUp = false;
  else if (id == IP_EVENT_STA_GOT_IP) g_net.wifiUp = true;
}

static void NetTask(void* param) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

  // v3.6.3 P1 修复: WiFi.begin 异步, status() 立即查返 false (走 4G 备份)
  // wifi_event_handler 异步置 g_net.wifiUp, 但前 1-2 秒是 false
  // 等 3 秒让 WiFi 连上, 然后同步一次状态
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_cfg.ssid, g_cfg.pass);
  vTaskDelay(pdMS_TO_TICKS(3000));
  g_net.wifiUp = (WiFi.status() == WL_CONNECTED);
  ESP_LOGI(TAG, "WiFi STA up: ssid=%s connected=%d", g_cfg.ssid, g_net.wifiUp);
  // v3.7: 启动时同步一次 4G 流量桶 (跨日/跨月清零)
  checkAndResetDataBucket();

  // RNDIS 4G 拨号
  g_net.rndisUp = false;  // v3.6.2: 默认 false, 真拨通才置 true
  // v3.6.4 P1 修复 (E3): 用 goto out 统一释放, 避免部分失败导致 netif/eth/glue 泄漏
  iot_usbh_rndis_config_t rndis_cfg = { .auto_detect = true, .auto_detect_timeout = pdMS_TO_TICKS(1000) };
  esp_err_t rndis_err = iot_eth_new_usb_rndis(&rndis_cfg, &rndis_handle);
  if (rndis_err != ESP_OK) {
    ESP_LOGW(TAG, "iot_eth_new_usb_rndis err=%d", rndis_err);
    goto rndis_out;
  }
  iot_eth_config_t eth_cfg = { .driver = rndis_handle, .stack_input = NULL, .user_data = NULL };
  if (iot_eth_install(&eth_cfg, &eth_handle) != ESP_OK) {
    ESP_LOGE(TAG, "iot_eth_install FAILED");
    iot_eth_del_usb_rndis(rndis_handle); rndis_handle = NULL;
    goto rndis_out;
  }
  {
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&netif_cfg);
    if (!eth_netif) {
      ESP_LOGE(TAG, "esp_netif_new FAILED");
      iot_eth_uninstall(eth_handle); eth_handle = NULL;
      iot_eth_del_usb_rndis(rndis_handle); rndis_handle = NULL;
      goto rndis_out;
    }
    iot_eth_netif_glue_handle_t glue = iot_eth_new_netif_glue(eth_handle);
    if (!glue) {
      ESP_LOGE(TAG, "iot_eth_new_netif_glue FAILED");
      esp_netif_destroy(eth_netif); eth_netif = NULL;
      iot_eth_uninstall(eth_handle); eth_handle = NULL;
      iot_eth_del_usb_rndis(rndis_handle); rndis_handle = NULL;
      goto rndis_out;
    }
    esp_netif_attach(eth_netif, glue);
    if (iot_eth_start(eth_handle) == ESP_OK) {
      g_net.rndisUp = true;
      ESP_LOGI(TAG, "RNDIS 4G netif up");
    } else {
      ESP_LOGE(TAG, "RNDIS 4G netif up FAILED, 4G 路由不可用");
      // 部分清理
      esp_netif_destroy(eth_netif); eth_netif = NULL;
      iot_eth_uninstall(eth_handle); eth_handle = NULL;
      iot_eth_del_usb_rndis(rndis_handle); rndis_handle = NULL;
    }
  }
rndis_out: ;

  // 主循环: 推 pushplus + 检查网络
  while (1) {
    PushItem item;
    if (xQueueReceive(g_pushQ, &item, pdMS_TO_TICKS(1000)) == pdTRUE) {
      // v3.6.3 P1 修复: AP 模式不入 NVS 不发 (避免配网期间疯狂写 flash)
      // v3.6.4 P0: 改用 atomic getter
      if (ap_mode_get()) {
        ESP_LOGW(TAG, "AP mode active, drop push (no NVS write)");
        continue;
      }
      const String& payload = String(item.data, item.len);
      bool sent = false;
      // v3.7: 先检流量限, 超今日/本月直接走 NVS 兜底
      checkAndResetDataBucket();
      if (isDailyDataOverLimit() || isMonthlyDataOverLimit()) {
        ESP_LOGE(TAG, "Data over limit: today=%lu, month=%lu, NVS only",
                 (unsigned long)g_net.today4gBytes, (unsigned long)g_net.month4gBytes);
        nvsQEnqueue(payload);
        continue;
      }
      if (g_net.wifiUp) {
        HTTPClient http;
        http.begin("https://www.pushplus.plus/send");
        http.addHeader("Content-Type", "application/json");
        int code = http.POST(payload);
        http.end();
        if (code == 200) {
          sent = true;
          g_ledNet.flashTrig = true;
          g_net.lastPingOkMs = millis();
          g_net.lastPushVia4G = false;
          g_net.lastPushMs = millis();
          g_net.totalPushOk++;
          uint8_t hr = (millis() / 3600000) % 24;
          g_net.pushHistory[hr]++;
        }
      }
      if (!sent && g_net.rndisUp && !g_net.rndisDown) {
        // 走 4G RNDIS 出公网 (熔断状态跳过)
        sent = postPushplusViaRndis(payload);
        if (sent) {
          g_ledNet.flashTrig = true;
          g_net.rndisFailRun = 0;
          g_net.lastPushVia4G = true;
          g_net.lastPushMs = millis();
          g_net.totalPushOk++;
          g_net.total4gPushOk++;
          uint8_t hr = (millis() / 3600000) % 24;
          g_net.pushHistory[hr]++;
          // v3.7: 4G 推送成功后累加流量 (上行 payload 字节数)
          add4gDataUsage(item.len + 256);
        } else {
          g_net.rndisFailRun++;
          if (g_net.rndisFailRun >= 3) {
            ESP_LOGE(TAG, "4G RNDIS熔断 (连续%u次失败), 入 DOWN 状态", g_net.rndisFailRun);
            g_net.rndisDown = true;
          }
        }
      }
      // ping 通时恢复
      if (g_net.rndisDown && g_net.lastPingOkMs > 0 && millis() - g_net.lastPingOkMs < 30000) {
        g_net.rndisDown = false;
        g_net.rndisFailRun = 0;
        ESP_LOGI(TAG, "4G RNDIS 熔断恢复 (ping OK)");
      }
      if (!sent) {
        // 都不通 (或 4G 熔断) -> 存 NVS
        g_net.totalPushFail++;
        if (!g_net.lastPushVia4G) g_net.total4gPushFail++;  // 区分 4G 失败计数
        ESP_LOGW(TAG, "Save to NVS (depth=%lu)", (unsigned long)(nvsQDepth()+1));
        nvsQEnqueue(payload);
      }
    }
    // 定期 ping 测网络连通 — 智能路由省流量
    // 原则: WiFi 通 -> WiFi 几乎免费, 5s 一次; WiFi 断 -> 走 4G RNDIS, 30s 一次 (省流量)
    // v3.6 开关: 配网页可关掉, 关掉后 NET LED 仅靠 pushplus 推送成功驱动
    if (!g_cfg.pingEnabled) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    // 每 30s 尝试补发 NVS 队列
    static uint32_t lastDrain = 0;
    if (millis() - lastDrain > QUEUE_RETRY_MS) {
      lastDrain = millis();
      nvsQDrain();
    }
    static uint32_t lastPing = 0;
    uint32_t pingInterval = g_net.wifiUp ? 5000 : 30000;  // WiFi 通 = 5s, 4G = 30s
    if (millis() - lastPing > pingInterval) {
      lastPing = millis();
      ip_addr_t target;
      ipaddr_aton("223.5.5.5", &target);
      esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
      cfg.target_addr = target;
      cfg.timeout_ms = g_net.wifiUp ? 2000 : 5000;  // 4G 信号差 -> 给更长超时
      cfg.count = 1;
      // 选网口: WiFi 通 -> 默认 STA 网口; WiFi 断 -> 4G RNDIS 网口
      if (!g_net.wifiUp && eth_netif) {
        cfg.interface = esp_netif_get_netif_impl_index(eth_netif);
        ESP_LOGI(TAG, "ping via 4G RNDIS (WiFi down, save 4G data)");
      } else {
        ESP_LOGD(TAG, "ping via WiFi (free)");
      }
      // v3.6.3 并发修复: 用信号量等 on_ping_end 再 delete, 避免 use-after-free
      static SemaphoreHandle_t s_pingDone = NULL;
      if (s_pingDone == NULL) s_pingDone = xSemaphoreCreateBinary();
      xSemaphoreTake(s_pingDone, 0);
      esp_ping_callbacks_t cbs = {
        .on_ping_success = [](void*, void*){ g_net.lastPingOkMs = millis(); g_net.pingInFlight = false; g_ledNet.flashTrig = true; },
        .on_ping_timeout = [](void*, void*){ g_net.pingInFlight = false; },
        .on_ping_end = [](void*, void*){ xSemaphoreGive(s_pingDone); },
        .cb_args = NULL,
      };
      esp_ping_handle_t ping;
      if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) {
        g_net.pingInFlight = true;
        esp_ping_start(ping);
        // v3.6.3 P0 修复: 阻塞等 on_ping_end 完成, 再 delete
        // 0 超时会 use-after-free (回调可能正在访问 session)
        xSemaphoreTake(s_pingDone, pdMS_TO_TICKS(cfg.timeout_ms + 500));
        esp_ping_delete_session(ping);
        g_net.pingInFlight = false;
      }
    }
  }
}

// =================== OtaTask: 配网页 + OTA (简化, 复用 v3.4.2/3 思路) ===================
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_ota_ops.h>
static AsyncWebServer* g_apServer = nullptr;
static DNSServer*      g_dns = nullptr;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>SMS Fwd v3.6</title>
<style>
body{font-family:system-ui;margin:0;padding:16px;background:#f7f7f7;color:#222}
h1{font-size:18px;margin:0 0 12px}
.card{background:#fff;border-radius:8px;padding:14px;margin-bottom:10px;box-shadow:0 1px 3px rgba(0,0,0,.08)}
label{display:block;margin:6px 0 3px;font-size:12px;color:#666}
input,select{width:100%;padding:7px;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;font-size:13px}
button{padding:8px 14px;border:0;border-radius:4px;background:#0a84ff;color:#fff;font-size:13px;margin:4px 4px 4px 0;cursor:pointer}
.cb-row{display:flex;align-items:center;gap:8px;margin:8px 0}
.cb-row input{width:auto;margin:0}
.tip{font-size:12px;color:#666;margin:4px 0}
</style></head><body>
<h1>📡 SMS Forwarder v3.6 配置</h1>
<div class=card>
  <label>WiFi SSID</label><input id=ssid name=ssid>
  <label>WiFi 密码</label><input id=pass name=pass type=password>
</div>
<div class=card>
  <label>pushplus Token</label><input id=token name=token>
  <label>pushplus Topic (可选)</label><input id=topic name=topic>
  <label>Template</label>
  <select id=tpl name=tpl><option value=html>html</option><option value=markdown>markdown</option><option value=txt>txt</option></select>
</div>
<div class=card>
  <h3 style=margin:0 0 8px>📡 NET 灯控制</h3>
  <div class=cb-row>
    <input type=checkbox id=ping name=ping value=1 checked>
    <label for=ping style=margin:0>启用 ping 检测 (NET 灯亮) — 关闭后省 4G 流量</label>
  </div>
  <p class=tip><b>开</b>: WiFi 通 5s ping 一次 / WiFi 断 30s ping 一次 (走 4G)<br><b>关</b>: NET 灯只靠 pushplus 推送成功驱动, 不耗流量</p>
</div>
<div class=card>
  <button onclick=saveCfg()>💾 保存并重启</button>
  <div id=status style=font-size:12px;color:#888;margin-top:6px>就绪</div>
</div>
<div class=card>
  <h3 style=margin:0 0 8px>📤 固件升级 (Web OTA)</h3>
  <input type=file id=fw accept=".bin" style="margin-bottom:8px">
  <button onclick=uploadFw() style="background:#ff9500">🚀 上传升级</button>
  <progress id=prog value=0 max=100 style="width:100%;height:20px"></progress>
  <div class=tip>从 Arduino IDE 导出编译的 .bin 文件上传. 升级期间设备会重启.</div>
  <div id=otaStatus style=font-size:12px;color:#888;margin-top:6px>就绪</div>
</div>
<script>
function $(id){return document.getElementById(id)}
async function saveCfg(){
  var body='ssid='+encodeURIComponent($('ssid').value)+'&pass='+encodeURIComponent($('pass').value)+'&token='+encodeURIComponent($('token').value)+'&topic='+encodeURIComponent($('topic').value)+'&tpl='+encodeURIComponent($('tpl').value);
  if($('ping').checked) body+='&ping=1';
  try{
    var r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    var j=await r.json();
    if(j.ok){$('status').textContent='已保存, 3秒后重启...';setTimeout(()=>{location.reload()},2000)}
    else $('status').textContent='失败: '+j.err;
  }catch(e){$('status').textContent='错误: '+e}
}
// v3.6.3 P1 修复: 补全 uploadFw JS (之前 HTML 有按钮没函数, 点击没反应)
async function uploadFw(){
  var f=$('fw').files[0];
  if(!f){$('otaStatus').textContent='请先选 .bin 文件';return}
  var fd=new FormData();fd.append('firmware',f);
  var prog=$('prog');prog.value=0;
  $('otaStatus').textContent='上传中...';
  try{
    var r=new XMLHttpRequest();
    r.open('POST','/update',true);
    r.upload.onprogress=function(e){if(e.lengthComputable)prog.value=(e.loaded/e.total)*100};
    r.onload=function(){
      prog.value=100;
      if(r.status==200){$('otaStatus').textContent='升级成功, 设备重启中...';setTimeout(()=>{location.reload()},5000)}
      else{$('otaStatus').textContent='失败: '+r.responseText}
    };
    r.onerror=function(){$('otaStatus').textContent='网络错误'};
    r.send(fd);
  }catch(e){$('otaStatus').textContent='错误: '+e}
}
</script>
</body></html>
)HTML";
static void handleIndex(AsyncWebServerRequest* req) {
  req->send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}
// v3.7 仪表盘 HTML (30s 自动刷新, Chart.js)
static const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>SMS Fwd Dashboard v3.7</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.5.0/dist/chart.umd.js" crossorigin="anonymous"></script>
<style>
body{font-family:system-ui;margin:0;padding:16px;background:#f7f7f7;color:#222}
h1{font-size:18px;margin:0 0 12px}
.card{background:#fff;border-radius:8px;padding:14px;margin-bottom:10px;box-shadow:0 1px 3px rgba(0,0,0,.08)}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}
.stat{font-size:13px;color:#666}
.val{font-size:20px;font-weight:bold}
canvas{max-height:200px}
.row{display:flex;gap:8px}
.col{flex:1}
</style></head><body>
<h1>📊 SMS Forwarder v3.7</h1>
<div class=card>
  <div class=grid>
    <div><div class=stat>IP</div><div class=val id=ip>--</div></div>
    <div><div class=stat>4G CSQ</div><div class=val id=csq>--</div></div>
    <div><div class=stat>可用堆</div><div class=val id=heap>--</div></div>
    <div><div class=stat>队列深度</div><div class=val id=queue>--</div></div>
    <div><div class=stat>推送成功</div><div class=val id=pushOk>--</div></div>
    <div><div class=stat>推送失败</div><div class=val id=pushFail>--</div></div>
    <div><div class=stat>4G 推送成功</div><div class=val id=4gOk>--</div></div>
    <div><div class=stat>4G 推送失败</div><div class=val id=4gFail>--</div></div>
    <div><div class=stat>收 SMS</div><div class=val id=smsRecv>--</div></div>
    <div><div class=stat>启动次数</div><div class=val id=boot>--</div></div>
    <div><div class=stat>今日 4G 流量</div><div class=val id=4gToday>--</div></div>
    <div><div class=stat>本月 4G 流量</div><div class=val id=4gMonth>--</div></div>
  </div>
</div>
<div class=card>
  <canvas id=pushChart></canvas>
</div>
<div class=card>
  <canvas id=smsChart></canvas>
</div>
<div class=card style=font-size:12px;color:#888>
  auto-refresh 30s | v3.7
</div>
<script>
let pushChart, smsChart;
function initCharts(){
  pushChart=new Chart(document.getElementById("pushChart"),{type:"bar",data:{labels:Array.from({length:24},(_,i)=>i+":00"),datasets:[{label:"Push",data:[],backgroundColor:"#0a84ff"}]},options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true}}}});
  smsChart=new Chart(document.getElementById("smsChart"),{type:"bar",data:{labels:Array.from({length:24},(_,i)=>i+":00"),datasets:[{label:"SMS",data:[],backgroundColor:"#34c759"}]},options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true}}}});
}
async function refresh(){
  try{
    let r=await fetch("/api/status");let j=await r.json();
    document.getElementById("ip").textContent=j.ip;document.getElementById("csq").textContent=j.csq+"/31";document.getElementById("heap").textContent=Math.round(j.heap/1024)+" KB";document.getElementById("queue").textContent=j.queue;document.getElementById("pushOk").textContent=j.pushOk;document.getElementById("pushFail").textContent=j.pushFail;document.getElementById("4gOk").textContent=j["4gOk"];document.getElementById("4gFail").textContent=j["4gFail"];document.getElementById("smsRecv").textContent=j.smsRecv;document.getElementById("boot").textContent=j.boot;document.getElementById("4gToday").textContent=Math.round(j["4gToday"]/1024)+" KB";document.getElementById("4gMonth").textContent=Math.round(j["4gMonth"]/1024)+" KB";
    if(pushChart) pushChart.data.datasets[0].data=j.pushHistory;
    if(smsChart) smsChart.data.datasets[0].data=j.smsHistory;
    if(pushChart) pushChart.update();if(smsChart) smsChart.update();
  }catch(e){}
}
initCharts();refresh();setInterval(refresh,30000);
</script>
</body></html>
)HTML";
static void handleDashboard(AsyncWebServerRequest* req) {
  req->send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
}
static void handleIndex(AsyncWebServerRequest* req) {
  req->send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}
// v3.6.3 P1 修复: 逐个 getParam 判空, 缺参返 400
// =================== v3.7 仪表盘: /api/status JSON 端点 ===================
// 返回: ip/csq/heap/queue/pushOk/pushFail/4gOk/4gFail/smsRecv/bootCount/4g今日/本月/24h 推送/SMS
// 频率: 配网页 /dashboard 每 30s 自动拉
static void handleApiStatus(AsyncWebServerRequest* req) {
  StaticJsonDocument<512> doc;
  doc["ip"]      = WiFi.localIP().toString();
  doc["csq"]     = g_ml.csq;
  doc["heap"]    = ESP.getFreeHeap();
  doc["queue"]   = nvsQDepth();
  doc["pushOk"]  = g_net.totalPushOk;
  doc["pushFail"] = g_net.totalPushFail;
  doc["4gOk"]    = g_net.total4gPushOk;
  doc["4gFail"]  = g_net.total4gPushFail;
  doc["smsRecv"] = g_net.totalSmsRecv;
  doc["boot"]    = g_net.bootCount;
  doc["4gToday"] = g_net.today4gBytes;
  doc["4gMonth"] = g_net.month4gBytes;
  // 24h 推送/SMS 历史
  JsonArray pushHistory = doc.createNestedArray("pushHistory");
  for (int i = 0; i < 24; i++) pushHistory.add(g_net.pushHistory[i]);
  JsonArray smsHistory = doc.createNestedArray("smsHistory");
  for (int i = 0; i < 24; i++) smsHistory.add(g_net.smsHistory[i]);
  // 额外: 当前状态
  doc["wifiUp"] = g_net.wifiUp;
  doc["rndisUp"] = g_net.rndisUp;
  doc["rndisDown"] = g_net.rndisDown;
  doc["pingEnabled"] = g_cfg.pingEnabled;
  doc["version"] = FW_VERSION;
  String json; serializeJson(doc, json);
  req->send(200, "application/json", json);
}

static void handleSave(AsyncWebServerRequest* req) {
  Config c;
  memset(&c, 0, sizeof(c));  // 默认值
  c.pingEnabled = true;       // 默认开
  // 必填: ssid, token
  AsyncWebParameter* p;
  if ((p = req->getParam("ssid", true)) == nullptr) { req->send(400, "application/json", "{\"ok\":false,\"err\":\"missing ssid\"}"); return; }
  strncpy(c.ssid, p->value().c_str(), sizeof(c.ssid)-1); c.ssid[sizeof(c.ssid)-1] = 0;
  if ((p = req->getParam("pass", true)) == nullptr) { req->send(400, "application/json", "{\"ok\":false,\"err\":\"missing pass\"}"); return; }
  strncpy(c.pass, p->value().c_str(), sizeof(c.pass)-1); c.pass[sizeof(c.pass)-1] = 0;
  if ((p = req->getParam("token", true)) == nullptr) { req->send(400, "application/json", "{\"ok\":false,\"err\":\"missing token\"}"); return; }
  strncpy(c.token, p->value().c_str(), sizeof(c.token)-1); c.token[sizeof(c.token)-1] = 0;
  // 选填: topic, tpl, ping
  if ((p = req->getParam("topic", true)) != nullptr) {
    strncpy(c.topic, p->value().c_str(), sizeof(c.topic)-1); c.topic[sizeof(c.topic)-1] = 0;
  }
  if ((p = req->getParam("tpl", true)) != nullptr) {
    strncpy(c.tpl, p->value().c_str(), sizeof(c.tpl)-1); c.tpl[sizeof(c.tpl)-1] = 0;
  } else {
    strncpy(c.tpl, "html", sizeof(c.tpl)-1);
  }
  c.pingEnabled = req->hasParam("ping", true) && req->getParam("ping", true)->value() == "1";
  // 校验 token 长度
  if (strlen(c.token) < 16) {
    req->send(400, "application/json", "{\"ok\":false,\"err\":\"token too short (min 16)\"}");
    return;
  }
  saveConfig(c);
  ESP_LOGI(TAG, "Config saved: pingEnabled=%d", c.pingEnabled);
  req->send(200, "application/json", "{\"ok\":true}");
  delay(500); ESP.restart();
}

// Web OTA: /update 端点
// 1) handleUpdateChunk: 每片写入, 第一片 begin, 最后片 end
// 2) handleUpdateDone: 全部完成, 重启
static bool g_otaInProgress = false;
static void handleUpdateDone(AsyncWebServerRequest* req) {
  if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
    ESP_LOGE(TAG, "OTA mark valid failed");
    Update.abort();
    req->send(500, "text/plain", "Mark valid failed");
  } else if (Update.hasError()) {
    ESP_LOGE(TAG, "OTA end hasError: %s", Update.errorString().c_str());
    Update.abort();
    req->send(500, "text/plain", "Update error: " + Update.errorString());
  } else {
    req->send(200, "text/plain", "OK, rebooting ...");
    delay(300);
    ESP.restart();
  }
  g_otaInProgress = false;
}
static void handleUpdateChunk(AsyncWebServerRequest* req, String filename,
                              size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    ESP_LOGW(TAG, "OTA start: %s, size=%u", filename.c_str(), req->contentLength());
    g_otaInProgress = true;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      ESP_LOGE(TAG, "Update.begin failed: %s", Update.errorString());
      // v3.6.3 P1: begin 失败也复位 in_progress, 否则下次 OTA 卡死
      g_otaInProgress = false;
      return;
    }
  }
  if (len) {
    if (Update.write(data, len) != len) {
      ESP_LOGE(TAG, "Update.write failed: %s", Update.errorString());
      // v3.6.3 P1 修复: write 失败立即 abort, 不让错误数据写进 flash
      Update.abort();
      g_otaInProgress = false;
      return;
    }
  }
  if (final) {
    if (Update.end(true)) {
      ESP_LOGI(TAG, "OTA success, size=%u", index + len);
    } else {
      ESP_LOGE(TAG, "Update.end failed: %s", Update.errorString());
    }
  }
}

// v3.6.3 P1: AP 模式标记, NetTask 看这个跳过 push 几分钟
// v3.6.4 P0 修复: ESP32 双核 (S3 是 LX7+Xtensa) volatile bool 不保证跨核可见性,
// 用 portMUX 临界区保护读写; 同时加 setter 函数封装避免散落
static portMUX_TYPE s_apMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool g_apModeActive = false;
static void ap_mode_set(bool on) {
  taskENTER_CRITICAL(&s_apMux);
  g_apModeActive = on;
  taskEXIT_CRITICAL(&s_apMux);
}
static bool ap_mode_get(void) {
  bool v;
  taskENTER_CRITICAL(&s_apMux);
  v = g_apModeActive;
  taskEXIT_CRITICAL(&s_apMux);
  return v;
}

static void OtaTask(void* param) {
  // v3.6.3 P1: BOOT 按钮启用内部上拉, 防止浮空误触发
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  // 等配置有效 (最长 30s) 或 BOOT 按下 -> AP 模式
  bool enterAp = false;
  for (int i = 0; i < 30; i++) {
    if (!isConfigValid()) { enterAp = true; break; }
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) { enterAp = true; break; }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  if (enterAp) {
    // 起 AP + DNS + WebServer
    WiFi.mode(WIFI_AP_STA);
    String mac = WiFi.macAddress(); mac.replace(":", "");
    String ssid = String(AP_SSID_PREFIX) + mac.substring(6);
    WiFi.softAP(ssid.c_str(), "12345678");  // v3.6.3 P1: 默认密码防未授权访问
    g_dns = new DNSServer();
    g_dns->start(53, "*", WiFi.softAPIP());
    g_apServer = new AsyncWebServer(80);
    g_apServer->on("/", HTTP_GET, handleIndex);
    g_apServer->on("/update", HTTP_POST, handleUpdateDone, handleUpdateChunk);
    g_apServer->on("/save", HTTP_POST, handleSave);
    g_apServer->on("/api/status", HTTP_GET, handleApiStatus);
    g_apServer->on("/dashboard", HTTP_GET, handleDashboard);
    g_apServer->begin();
    ESP_LOGW(TAG, "AP mode: %s", ssid.c_str());
    g_ledWifi.flashTrig = true;
    ap_mode_set(true);  // v3.6.4 P0: 跨核可见性保护
    while (1) { if (g_dns) g_dns->processNextRequest(); vTaskDelay(pdMS_TO_TICKS(10)); }
  } else {
    // STA 模式: ArduinoOTA
    ArduinoOTA.setHostname("sms-forwarder");
    ArduinoOTA.begin();
    ESP_LOGI(TAG, "ArduinoOTA ready");
    g_ledWifi.flashTrig = true;
    while (1) {
      ArduinoOTA.handle();
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

// =================== LedTask (DEMO 风格) ===================
static void led_set_level(gpio_num_t gpio, int level) {
  gpio_set_level(gpio, (level == (LED_ACTIVE_LEVEL == HIGH ? 1 : 0)) ? 1 : 0);
}

static void led_update(gpio_num_t gpio, led_state_t state, bool* trigFlag,
                       uint32_t* trigCounter, uint32_t* mainCounter) {
  (*mainCounter)++;
  // 触发闪烁 (10 次刷新 -> 1 次完整闪烁: 亮 5 次灭 5 次)
  if (trigFlag && *trigFlag) {
    if (*trigCounter < 10) {
      led_set_level(gpio, (*trigCounter % 2) ? 1 : 0);
      (*trigCounter)++;
      return;
    } else { *trigFlag = false; *trigCounter = 0; }
  }
  // 正常状态
  switch (state) {
    case LED_OFF: led_set_level(gpio, 0); break;
    case LED_ON: led_set_level(gpio, 1); break;
    case LED_BLINK_SLOW: led_set_level(gpio, (*mainCounter % 100 < 50) ? 1 : 0); break;
    case LED_BLINK_FAST: led_set_level(gpio, (*mainCounter % 10 < 5) ? 1 : 0); break;
  }
}

static void LedTask(void* param) {
  gpio_config_t io = { .pin_bit_mask = (1ULL<<LED_4G)|(1ULL<<LED_WIFI)|(1ULL<<LED_NET),
                       .mode = GPIO_MODE_OUTPUT, .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE };
  gpio_config(&io);
  gpio_set_level(LED_4G, 0); gpio_set_level(LED_WIFI, 0); gpio_set_level(LED_NET, 0);

  uint32_t counter = 0, tc4g = 0, tcWifi = 0, tcNet = 0;
  while (1) {
    // 4G 灯: alive + sim + csq 决定
    if (!g_ml.alive) g_led4g.status = LED_OFF;
    else if (!g_ml.simReady || g_ml.csq < LED_4G_GOOD_CSQ) g_led4g.status = LED_ON;
    else g_led4g.status = LED_BLINK_SLOW;
    // WIFI 灯
    if (!g_net.wifiUp && !WiFi.softAPgetStationNum()) g_ledWifi.status = LED_OFF;
    else if (g_ml.lastResetMs && millis()-g_ml.lastResetMs < 5000) g_ledWifi.status = LED_BLINK_FAST;
    else g_ledWifi.status = LED_ON;
    // NET 灯: 区分 WiFi/4G
    //   60s 内有推送: 快闪=4G 传输, 慢闪=WiFi 传输
    //   60s 内没推送但 ping 通了: 常亮 (代表网络在, 但没活动)
    //   都没: 灭
    bool pushRecent = (g_net.lastPushMs && millis() - g_net.lastPushMs < 60000);
    bool pingRecent = (g_net.lastPingOkMs && millis() - g_net.lastPingOkMs < 60000);
    if (pushRecent) {
      g_ledNet.status = g_net.lastPushVia4G ? LED_BLINK_FAST : LED_BLINK_SLOW;
    } else if (pingRecent) {
      g_ledNet.status = LED_ON;
    } else {
      g_ledNet.status = LED_OFF;
    }

    led_update((gpio_num_t)LED_4G,   g_led4g.status,   (bool*)&g_led4g.flashTrig,   &tc4g,   &counter);
    led_update((gpio_num_t)LED_WIFI, g_ledWifi.status, (bool*)&g_ledWifi.flashTrig, &tcWifi, &counter);
    led_update((gpio_num_t)LED_NET,  g_ledNet.status,  (bool*)&g_ledNet.flashTrig,  &tcNet,  &counter);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// =================== app_main ===================
extern "C" void app_main(void) {
  Serial.begin(115200);
  delay(500);
  ESP_LOGI(TAG, "\n=== SMS Forwarder v3.6 (USB + RNDIS) ===");

  // v3.6.3 P0 修复: g_atMutex 必须在最早建, 任何 send_atcmd 调用之前
  g_atMutex = xSemaphoreCreateMutex();

  // 4G PWR 控制
  gpio_config_t pwr_io = { .pin_bit_mask = (1ULL<<G4_PWR_GPIO),
                           .mode = GPIO_MODE_OUTPUT, .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE };
  gpio_config(&pwr_io);
  gpio_set_level(G4_PWR_GPIO, 1);  // 上电
  vTaskDelay(pdMS_TO_TICKS(3000));

  // 加载配置 (BEFORE 任何 send_atcmd)
  loadConfig();

  // v3.6.3: 队列在 send_atcmd 之前建, 元素类型改固定结构 (不用 String 引用)
  g_smsQ = xQueueCreate(SMS_QUEUE_LEN, sizeof(SmsMsg));
  g_pushQ = xQueueCreate(PUSH_QUEUE_LEN, sizeof(PushItem));
  // v3.6.4 P0 修复: URC 队列 8 -> URC_QUEUE_LEN(32), SIM 注册期间可能 burst
  g_urcQ  = xQueueCreate(URC_QUEUE_LEN, 256);

  // 4G 模组拨号 之前 必须 先有 USB CDC handle 和 cdc_at_reply
  // v3.6.3: 把 USB CDC 初始化从 AtTask 挪到 app_main, 避免 app_main 调 send_atcmd 时 cdc_at_handle=0
  usbh_cdc_driver_config_t cdc_cfg = {
    .task_stack_size = 4096, .task_priority = 5, .task_coreid = 0,
    .skip_init_usb_host_driver = false,
  };
  ESP_ERROR_CHECK(usbh_cdc_driver_install(&cdc_cfg));
  usbh_cdc_device_config_t dev_config = {
    .vid = 0, .pid = 0, .itf_num = 2,
    .rx_buffer_size = 1024, .tx_buffer_size = 1024,
    .cbs = { .connect = NULL, .disconnect = NULL, .user_data = NULL },
  };
  usbh_cdc_create(&dev_config, &cdc_at_handle);
  // v3.6.4 P0 修复: 创建失败检测 — heap 不足时返回 pdFAIL, 不检查则 SMS 全丢
  BaseType_t rxo = xTaskCreate(usb_rx_task, "cdc_rx", 4096, (void*)cdc_at_handle, 2, NULL);
  if (rxo != pdPASS) {
    ESP_LOGE(TAG, "FATAL: usb_rx_task create failed (heap=%u), rebooting", ESP.getFreeHeap());
    delay(100); ESP.restart();
  }

  // v3.6.3 注释: usbh_cdc_create 同步返, cdc_at_handle 立即非 0
  // 之前等循环是死代码 (cdc_at_handle 创建后不会为 0)
  // USB 设备插没插 看不出, AtTask 失败后 monitor4GHealth 兜底
  if (cdc_at_handle == 0) {
    ESP_LOGE(TAG, "4G USB CDC init failed (cdc_at_handle==0)");
  } else {
    ESP_LOGI(TAG, "4G USB CDC init OK, handle=%p", (void*)cdc_at_handle);
  }
  // v3.6.3 P0 修复: 4G 模组上电自动拨号, ESP32 这边不该发 AT+MDIALUP
  g_ml.alive = false;
  vTaskDelay(pdMS_TO_TICKS(2000));  // 等 ML307 内部拨号稳定

  g_net.bootCount++;  // v3.7: 启动次数
  ESP_LOGI(TAG, "Boot #%u", g_net.bootCount);
  // 5 任务
  xTaskCreate(AtTask, "AtTask", 8192, NULL, 5, NULL);
  xTaskCreate(SmsTask, "SmsTask", 4096, NULL, 4, NULL);
  xTaskCreate(NetTask, "NetTask", 8192, NULL, 5, NULL);
  xTaskCreate(OtaTask, "OtaTask", 4096, NULL, 3, NULL);
  xTaskCreate(LedTask, "LedTask", 2048, NULL, 5, NULL);
}
