/*
 * ============================================================================
 *  SMS Forwarder v4.0.5  (ESP32-S3 + USB 4G Modem + pushplus)
 *  Stack: ESP-IDF 5.5 + Arduino-ESP32 3.x (pioarduino platform)
 *
 *  v3.6.4 (IDF 4.4) 重写, 全面适配 IDF 5.5 API。
 *  v4.0.3 删除 RNDIS 死代码 (SDK 栈不稳, iot_eth 0.1.x stack_input 会 NULL deref)
 *  功能:
 *   - USB CDC 接 ML307 4G 模组 AT 通道 (iot_usbh_cdc 3.x API)
 *   - 收 +CMT 短信 (UCS2 hex) → 推送 pushplus
 *   - UDH 长短信自动拼接
 *   - 推失败落 NVS 队列, 重连重发
 *   - Web Dashboard (Chart.js) + /api/status JSON
 *   - OTA web 烧录 (BasicAuth, 密码存 NVS, 首次配网时填)
 *   - 首次没配置 → AP 模式 192.168.4.1 表单配网
 *   - GPIO0 长按 5s → 清 NVS 重启
 *   - 3 个 LED 状态: GPIO7=4G / GPIO15=WIFI / GPIO6=NET
 *   - esp_ping 健康监控
 *   - 推送失败率 5s 重试
 *
 *  已砍: ArduinoOTA (用 web OTA 替代), 数据用量统计
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include "pdu_codec.h"  // PDU 解码 (抽到独立文件以便 host test)
#include <Update.h>

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <driver/gpio.h>

#include <esp_netif_sntp.h>   // v4.0.6 P11: 时间同步, 让 lastSms/lastPush 显示真实时间
#include <esp_heap_caps.h>    // v4.0.6 P13: heap_caps_get_total_size 拿总内存

#include "iot_usbh_cdc.h"
#include "usbh_helper.h"
#include "ping/ping_sock.h"
#include <lwip/sockets.h>
#include <lwip/netdb.h>

// =================== 常量 ===================
static const char* TAG = "SMSFWD";
static const char* TAG_USB = "USBH";

#define G4_PWR_GPIO        8
#define BOOT_BUTTON_PIN    0
#define BOOT_HOLD_MS       15000   // 长按 15s wipe (v4.0.5 翔哥改回; 注: v4.0.4 实测 GPIO0 浮空会误触, BOOT_DEBOUNCE_N=5 + 30s grace 可缓解)
#define BOOT_GRACE_MS      30000   // 启动后 30s 不响应 BOOT (避开 GPIO0 strapping 抖动)
#define BOOT_DEBOUNCE_N    5       // 连续 5 个 100ms 采样 = 500ms 才算真按下

#define AP_SSID_PREFIX     "SMS-Forwarder"
#define AP_PASSWORD        "12345678"
#define FW_VERSION         "v4.0.8"

#define SMS_QUEUE_LEN      16
#define NVS_QUEUE_LEN      32
#define AT_REPLY_BUF       512
#define AT_LINE_BUF        512
#define CMT_HEADER_BUF     256
#define URC_LINE_BUF       256

#define PUSHPLUS_URL       "https://www.pushplus.plus/send"

// LED 引脚 (v3.6.4 一致)
#define LED_4G_GPIO        7
#define LED_WIFI_GPIO      15
#define LED_NET_GPIO       6
#define LED_ACTIVE_LEVEL    HIGH

// UDH 长短信
#define MAX_UDH_REFS       4
#define MAX_UDH_PARTS      8
#define UDH_TIMEOUT_MS     60000

// =================== 配置 ===================
struct Config {
  char ssid[64];
  char pass[64];
  char token[64];
  char topic[64];
  char otaUser[32];
  char otaPass[32];
  bool bootPush;   // 调试期可关 (v4.0.6+, 翔哥 2026-06-18 要求)
};
// v4.0.7: 凭据硬编默认 (翔哥 2026-06-19 要求: NVS 写入不可靠, 直接编译进固件)
// NVS 优先, NVS 缺失时回落这里 → 设备永远能上电连上 REDACTED_SSID
static Config g_cfg = {
  "REDACTED_SSID",                           // ssid  — 硬编
  "REDACTED_WIFI_PASS",                           // pass  — 硬编
  "",                                   // token — 留空, pushplus 不强求
  "",                                   // topic (空 = 个人推送)
  "admin",                              // otaUser (默认)
  "Admin@123",                          // otaPass (默认)
  true                                  // bootPush — 默认开
};

static void loadConfig() {
  // v4.0.7: 硬编默认已填, NVS 仅作为"运行时改"的覆盖层
  // NVS 读失败 (NOT_FOUND 等) 直接用默认, 不报错
  Preferences p;
  bool nvsOk = p.begin("cfg", true);
  if (nvsOk) {
    // NVS 有值才覆盖默认 (避免空 NVS 覆盖硬编)
    String v;
    v = p.getString("wifi.ssid", ""); if (v.length() > 0) strcpy(g_cfg.ssid, v.c_str());
    v = p.getString("wifi.pass", ""); if (v.length() > 0) strcpy(g_cfg.pass, v.c_str());
    v = p.getString("pp.tok",    ""); if (v.length() > 0) strcpy(g_cfg.token, v.c_str());
    v = p.getString("pp.tpc",    ""); if (v.length() > 0) strcpy(g_cfg.topic, v.c_str());
    v = p.getString("ota.user",  ""); if (v.length() > 0) strcpy(g_cfg.otaUser, v.c_str());
    v = p.getString("ota.pass",  ""); if (v.length() > 0) strcpy(g_cfg.otaPass, v.c_str());
    g_cfg.bootPush = p.getUChar("bootPush", 1) != 0;
    p.end();
  } else {
    ESP_LOGW(TAG, "NVS open failed, using hardcoded defaults");
  }
  ESP_LOGI(TAG, "Config: ssid=%s token=%.8s... ota_user=%s bootPush=%d",
           g_cfg.ssid, g_cfg.token, g_cfg.otaUser, g_cfg.bootPush);
}

static void saveConfig(const Config& c) {
  Preferences p;
  p.begin("cfg", false);
  p.putString("wifi.ssid", c.ssid);
  p.putString("wifi.pass", c.pass);
  p.putString("pp.tok",    c.token);
  p.putString("pp.tpc",    c.topic);
  p.putString("ota.user",  c.otaUser);
  p.putString("ota.pass",  c.otaPass);
  p.putUChar("bootPush",  c.bootPush ? 1 : 0);
  p.end();
}

static bool isConfigValid() {
  // v4.0.7: 硬编默认 ssid/pass, token 留空, 凭 ssid 长度判断即可
  if (strlen(g_cfg.ssid) == 0) return false;
  // 拒占位符字符串
  if (strcmp(g_cfg.ssid, "YOUR_WIFI_SSID") == 0) return false;
  if (strcmp(g_cfg.pass, "YOUR_WIFI_PASS") == 0) return false;
  return true;
}

static void wipeConfig() {
  Preferences p;
  p.begin("cfg", false);
  p.clear();
  p.end();
  // 同步 NVS push 队列
  Preferences q;
  q.begin("pqueue", false);
  q.clear();
  q.end();
}

// =================== PDU 解码 → 见 pdu_codec.{h,cpp} ===================
// 4 个解析函数已抽到 src/pdu_codec.cpp (纯 C++, 无 Arduino/IDF 依赖, host 可跑)
// - pdu::ucs2_hex_to_utf8()
// - pdu::decode_phone_field()
// - pdu::decode_body_field()
// - pdu::parse_udh()

// =================== SmsMsg + 全局 ===================
struct SmsMsg {
  char phone_hex[64];     // UCS2 hex 原文
  char body_hex[AT_LINE_BUF];
  uint16_t cmt_length;    // CMT head length field (septs for GSM 7-bit, codepoints for UCS-2)
  uint16_t dcs;           // 0/4/5/F5=7-bit, 8=UCS-2, 0xFF=unknown
  uint32_t ts;            // millis()
};

struct PushItem {
  char payload[1024];
  size_t len;
};

static usbh_cdc_port_handle_t g_cdc = NULL;
static SemaphoreHandle_t      g_atMutex = NULL;
static SemaphoreHandle_t      g_atDone  = NULL;
static SemaphoreHandle_t      g_atPrompt = NULL;   // v4.0.6: ">" prompt (CMGS step 2)
static QueueHandle_t          g_smsQ    = NULL;
static QueueHandle_t          g_pushQ   = NULL;
static QueueHandle_t          g_urcQ    = NULL;

static char g_atReply[AT_REPLY_BUF];
static volatile size_t g_atReplyLen = 0;
static volatile int    g_atResult   = -2;   // 0=OK, -1=ERROR, -2=timeout/pending

static bool g_waitingCmtBody = false;
static char g_cmtHeader[CMT_HEADER_BUF] = {0};
static volatile bool g_waitingCmgsPrompt = false;   // v4.0.6: 等 ">" 后写 PDU
static volatile bool g_inAtReply = false;           // v4.0.7 P0 fix: send_atcmd 期间=true, + 行双写到 g_atReply (URC 队列也保留)

// v4.0.7: 4G 信号强度 (csq_poll_task 每 5s 更新) + SIM 信息 (stk 主动查询)
static volatile int      g_4g_csq      = -1;     // -1=未知, 0-31 信号, 99=未知
static volatile int      g_4g_dbm      = 0;      // -113 + csq*2, 0/99 时=0
static volatile uint32_t g_4g_csqMs    = 0;      // 上次 CSQ 时间 (millis)
static char              g_sim_imsi[16]   = {0}; // 15 位 + '\0'
static char              g_sim_iccid[21]  = {0}; // 19-20 位 + '\0'
static char              g_sim_msisdn[16] = {0}; // 本机号
static char              g_sim_operator[32] = {0}; // 运营商名
static volatile uint32_t g_sim_queryMs   = 0;    // 上次 SIM 查询时间
static portMUX_TYPE      g_sim_mux       = portMUX_INITIALIZER_UNLOCKED;

// LED
typedef enum { LED_OFF, LED_ON, LED_BLINK_SLOW, LED_BLINK_FAST } led_state_t;
struct LedState { led_state_t s; bool flashTrig; uint32_t trig; uint32_t counter; bool last; };
static LedState g_led4g  = {LED_OFF, false, 0, 0, false};
static LedState g_ledW   = {LED_OFF, false, 0, 0, false};
static LedState g_ledNet = {LED_OFF, false, 0, 0, false};

// 网络/业务状态
static volatile bool g_wifiUp     = false;
static volatile int  g_pushOk     = 0;
static volatile int  g_pushFail   = 0;
static volatile uint32_t g_bootCount = 0;
static volatile uint32_t g_lastSmsMs       = 0;  // 上次收短信时间 (millis, 兼容)
static volatile uint32_t g_lastPushOkMs    = 0;  // 上次推送成功时间 (millis, 兼容)
// v4.0.6 P11b: epoch ms (time(NULL) * 1000ULL)
// P25: 改 uint64_t — uint32_t 在 2026 年会被截断成 ~3.6e9 (≈ 115 天前),
//   前端 new Date() 显示 1970/02/12 假数据。64-bit JS Number 安全 (>2^53 才丢精度)
static volatile uint64_t g_lastSmsUtcMs    = 0;
static volatile uint64_t g_lastPushOkUtcMs = 0;
static volatile bool     g_timeSynced      = false;  // SNTP 同步标志

// 4G 模组健康
typedef struct {
  bool alive;
  int  failRun;
  int  resetCount;
  bool needSoftReinit;
  bool needHardReset;
  uint32_t lastResetMs;
} modem_lifecycle_t;
static modem_lifecycle_t g_ml = {false, 0, 0, false, false, 0};

// AP 模式
static volatile bool g_apModeActive = false;
// v4.0.6 P19: /api/factory 触发, setup() 早期检测, nvs_flash_erase 后重启
// 用 RTC slow memory 存, ESP.restart() 不丢
RTC_DATA_ATTR static volatile bool g_factoryReset = false;
static portMUX_TYPE  s_apMux = portMUX_INITIALIZER_UNLOCKED;
static void ap_mode_set(bool on) { taskENTER_CRITICAL(&s_apMux); g_apModeActive = on; taskEXIT_CRITICAL(&s_apMux); }
static bool ap_mode_get()        { taskENTER_CRITICAL(&s_apMux); bool r = g_apModeActive; taskEXIT_CRITICAL(&s_apMux); return r; }

// UDH 长短信
struct UdhPart { char body[AT_LINE_BUF]; size_t len; uint16_t cmt_length; bool present; };
struct UdhRef {
  bool in_use;
  int refId;
  int total;
  int received;
  uint32_t firstMs;
  char phone[64];
  UdhPart parts[MAX_UDH_PARTS];
};
static UdhRef g_udhTable[MAX_UDH_REFS];

// =================== AT 行解析 ===================
static void handle_at_line(const char* line) {
  // +CMT 头/body 配对
  if (g_waitingCmtBody) {
    if (line[0] == '+') {
      // URC 抢断, 放弃配对, 让下面 +CMT 检测重新拾取
      g_waitingCmtBody = false;
      g_cmtHeader[0] = 0;
      // fall through
    } else {
      // 解 phone (在 g_cmtHeader 里两个引号之间)
      const char* q1 = strchr(g_cmtHeader, '"');
      const char* q2 = q1 ? strchr(q1+1, '"') : NULL;
      char phoneHex[64] = {0};
      if (q1 && q2 && (q2 - q1 - 1) < (int)sizeof(phoneHex)) {
        strncpy(phoneHex, q1+1, q2 - q1 - 1);
      }
      // 解 CMT head: 跳引号段数 ',' 找 dcs (field[6]) + length (field[9])
      // 格式: +CMT: oa,alpha,scts,tooa,fo,pid,dcs,sca,tosca,length
      // 兼容 oa/sca/tosca 不带引号的固件 (按 3GPP 27.005 它们是字符串)
      uint16_t dcs = 0xFF, cmtLen = 0;
      {
        const char* p = g_cmtHeader;
        int fieldIdx = 0;
        const char* field[10] = {0};
        const char* fieldEnd[10] = {0};
        while (*p && fieldIdx < 10) {
          // 跳引号段
          if (*p == '"') {
            field[fieldIdx] = p;
            const char* end = strchr(p + 1, '"');
            if (!end) break;
            fieldEnd[fieldIdx] = end;
            fieldIdx++;
            p = end + 1;
            // 吞一个分隔逗号
            if (*p == ',') p++;
            continue;
          }
          // 非引号 field
          field[fieldIdx] = p;
          const char* comma = strchr(p, ',');
          fieldEnd[fieldIdx] = comma ? comma : p + strlen(p);
          fieldIdx++;
          if (!comma) break;
          p = comma + 1;
        }
        if (fieldIdx >= 7 && field[6]) {
          char tmp[8] = {0};
          size_t n = fieldEnd[6] - field[6];
          if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
          memcpy(tmp, field[6], n);
          dcs = (uint16_t)atoi(tmp);
        }
        if (fieldIdx >= 10 && field[9]) {
          char tmp[8] = {0};
          size_t n = fieldEnd[9] - field[9];
          if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
          memcpy(tmp, field[9], n);
          cmtLen = (uint16_t)atoi(tmp);
        }
      }
      SmsMsg msg = {};
      strncpy(msg.phone_hex, phoneHex, sizeof(msg.phone_hex)-1);
      strncpy(msg.body_hex, line, sizeof(msg.body_hex)-1);
      msg.cmt_length = cmtLen;
      msg.dcs = dcs;
      msg.ts = millis();
      ESP_LOGW(TAG, "RAW +CMT BODY: %s (dcs=%u len=%u)", line, dcs, cmtLen);
      if (xQueueSend(g_smsQ, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SmsQueue full, drop SMS from %s", phoneHex);
      } else {
        ESP_LOGI(TAG, "SMS queued: phoneHex=%s len=%u", phoneHex, (unsigned)strlen(line));
      }
      g_waitingCmtBody = false;
      g_cmtHeader[0] = 0;
      return;
    }
  }

  if (strncmp(line, "+CMT:", 5) == 0) {
    strncpy(g_cmtHeader, line, sizeof(g_cmtHeader)-1);
    g_waitingCmtBody = true;
    ESP_LOGW(TAG, "RAW +CMT HEAD: %s", g_cmtHeader);  // 诊断: 看 +CMT 头完整
    return;
  }

  // 其他 URC (如 +CEREG +CSQ) → g_urcQ
  if (line[0] == '+') {
    // v4.0.7 P0 fix: send_atcmd 期间, + 行双写到 g_atReply (AT 查询 reply 格式就是 +CSQ:/+CIMI:/+CCID:/+CNUM:/+COPS:)
    if (g_inAtReply) {
      size_t _l = strlen(line);
      size_t _old = g_atReplyLen;
      if (_old + _l + 2 < sizeof(g_atReply)) {
        memcpy(g_atReply + _old, line, _l);
        g_atReply[_old + _l] = '\n';
        g_atReplyLen = _old + _l + 1;
        g_atReply[g_atReplyLen] = 0;
      }
    }
    if (xQueueSend(g_urcQ, line, 0) != pdTRUE) {
      // 满, 弹一个最老的非关键 URC (避免 +CEREG 等关键 URC 丢)
      char tmp[URC_LINE_BUF];
      (void)xQueueReceive(g_urcQ, tmp, 0);
      if (xQueueSend(g_urcQ, line, 0) != pdTRUE) {
        ESP_LOGW(TAG, "URC full, drop: %s", line);
      }
    }
    return;
  }

  // v4.0.6: CMGS prompt "> " → 触发 g_atPrompt 让 cmgs_send_pdu 写 PDU
  // ("> " 不以 + 开头, 走这里而不是 URC 分支)
  if (g_waitingCmgsPrompt && line[0] == '>' && (line[1] == ' ' || line[1] == 0)) {
    g_waitingCmgsPrompt = false;
    if (g_atPrompt) xSemaphoreGive(g_atPrompt);
    return;   // "> " 行不入 reply
  }

  // 普通行 → 累加到 reply (行末加 '\n' 方便多行响应排错)
  size_t l = strlen(line);
  size_t oldLen = g_atReplyLen;
  if (oldLen + l + 2 < sizeof(g_atReply)) {
    memcpy(g_atReply + oldLen, line, l);
    g_atReply[oldLen + l] = '\n';
    g_atReplyLen = oldLen + l + 1;
    g_atReply[g_atReplyLen] = 0;
  }
  if (!strcmp(line, "OK"))    { g_atResult = 0;  xSemaphoreGive(g_atDone); }
  else if (!strcmp(line, "ERROR")) { g_atResult = -1; xSemaphoreGive(g_atDone); }
}

// =================== UDH 工具 ===================
// parse_udh 已抽到 pdu::parse_udh (src/pdu_codec.cpp)

static UdhRef* find_udh_slot(int refId) {
  for (int i = 0; i < MAX_UDH_REFS; i++) {
    // in_use 守 refId=0 碰撞: clear_udh_ref 把 refId 置 0 但 in_use 仍 true
    // 直到下次 alloc_udh_slot 重置 — 不守的话 refId=0 长短信会命中已清空槽
    if (g_udhTable[i].in_use && g_udhTable[i].refId == refId) return &g_udhTable[i];
  }
  return NULL;
}

static UdhRef* alloc_udh_slot(int refId, int total, const char* phone) {
  // 先找空槽
  for (int i = 0; i < MAX_UDH_REFS; i++) {
    if (!g_udhTable[i].in_use) {
      g_udhTable[i].in_use   = true;
      g_udhTable[i].refId    = refId;
      g_udhTable[i].total    = total;
      g_udhTable[i].received = 0;
      g_udhTable[i].firstMs  = millis();
      strncpy(g_udhTable[i].phone, phone, sizeof(g_udhTable[i].phone)-1);
      for (int j = 0; j < MAX_UDH_PARTS; j++) g_udhTable[i].parts[j].present = false;
      return &g_udhTable[i];
    }
  }
  return NULL;
}

static void clear_udh_ref(int refId) {
  for (int i = 0; i < MAX_UDH_REFS; i++) {
    if (g_udhTable[i].refId == refId) {
      g_udhTable[i].in_use = false;
      g_udhTable[i].refId = 0;
      g_udhTable[i].total = 0;
      g_udhTable[i].received = 0;
      g_udhTable[i].firstMs = 0;
      g_udhTable[i].phone[0] = 0;
      for (int j = 0; j < MAX_UDH_PARTS; j++) {
        g_udhTable[i].parts[j].present = false;
        g_udhTable[i].parts[j].body[0] = 0;
        g_udhTable[i].parts[j].len = 0;
      }
    }
  }
}

static void check_udh_timeouts() {
  for (int i = 0; i < MAX_UDH_REFS; i++) {
    if (g_udhTable[i].in_use &&
        millis() - g_udhTable[i].firstMs > UDH_TIMEOUT_MS) {
      // 超时: 把已收部分拼好入队
      ESP_LOGW(TAG, "UDH ref=%d timeout, push partial %d/%d parts",
               g_udhTable[i].refId, g_udhTable[i].received, g_udhTable[i].total);
      SmsMsg m = {};
      strncpy(m.phone_hex, g_udhTable[i].phone, sizeof(m.phone_hex)-1);
      // 用部分拼接, 加 [partial] 前缀方便识别
      m.body_hex[0] = 0;
      for (int j = 0; j < MAX_UDH_PARTS; j++) {
        if (g_udhTable[i].parts[j].present) {
          strncat(m.body_hex, g_udhTable[i].parts[j].body, sizeof(m.body_hex)-strlen(m.body_hex)-1);
        }
      }
      m.ts = millis();
      ESP_LOGW(TAG, "PARTIAL body_hex=%.200s", m.body_hex);
      xQueueSend(g_smsQ, &m, 0);
      clear_udh_ref(g_udhTable[i].refId);
    }
  }
}

static bool stash_udh_part(const SmsMsg* msg) {
  int refId, total, seq;
  if (!pdu::parse_udh(msg->body_hex, refId, total, seq)) return false;
  ESP_LOGW(TAG, "stash_udh: refId=%d total=%d seq=%d (cmt_len=%u dcs=%u)",
           refId, total, seq, msg->cmt_length, msg->dcs);
  UdhRef* r = find_udh_slot(refId);
  if (!r) r = alloc_udh_slot(refId, total, msg->phone_hex);
  if (!r) return false;
  if (strcmp(r->phone, msg->phone_hex) != 0) {
    // phone mismatch, 重新建
    clear_udh_ref(refId);
    r = alloc_udh_slot(refId, total, msg->phone_hex);
    if (!r) return false;
  }
  // UDH 头长度:
  //   8-bit concat:  IEI(1) 03 ref total seq  = 5 字节 = 10 hex
  //   16-bit concat: IEI(1) 04 refH refL total seq = 6 字节 = 12 hex
  // ML307 标准格式: body[0..1] = UDHLEN(1 byte hex), body[2..] = IE
  //   16-bit: "06 08 04 ..." → "0804" 在 body+2 → skip 14
  //   8-bit:  "05 00 03 ..." → "0003" 在 body+2 → skip 12
  // ML307 剥 UDHL (实测踩过): body[0..] 直接 IE
  //   16-bit: "08 04 ..." → "0804" 在 body+0 → skip 12
  //   8-bit:  "00 03 ..." → "0003" 在 body+0 → skip 10
  size_t udhSkip = 0;
  const char* p16 = strstr(msg->body_hex, "0804");
  const char* p8  = strstr(msg->body_hex, "0003");
  if (p16 != NULL && p16 <= msg->body_hex + 2) {
    udhSkip = (p16 == msg->body_hex + 2) ? 14 : 12;
  } else if (p8 != NULL && p8 <= msg->body_hex + 2) {
    udhSkip = (p8 == msg->body_hex + 2) ? 12 : 10;
  }
  const char* partBody = msg->body_hex + udhSkip;
  if (seq < 1 || seq > MAX_UDH_PARTS) return false;
  if (r->parts[seq-1].present) return false;  // dup
  strncpy(r->parts[seq-1].body, partBody, sizeof(r->parts[seq-1].body)-1);
  r->parts[seq-1].len        = strlen(partBody);
  r->parts[seq-1].cmt_length = msg->cmt_length;  // per-part septet count
  r->parts[seq-1].present    = true;
  r->received++;
  if (r->received == r->total) {
    // 齐了, 拼接
    SmsMsg m = {};
    strncpy(m.phone_hex, r->phone, sizeof(m.phone_hex)-1);
    m.body_hex[0] = 0;
    uint32_t totalSeptets = 0;
    for (int j = 0; j < MAX_UDH_PARTS; j++) {
      if (r->parts[j].present) {
        strncat(m.body_hex, r->parts[j].body, sizeof(m.body_hex)-strlen(m.body_hex)-1);
        totalSeptets += r->parts[j].cmt_length;  // sum per-part
      }
    }
    m.ts = millis();
    m.dcs = msg->dcs;
    // 7-bit concat: total septets 包含每个 part 的 UDH overhead (7 septets per part)
    // user_chars = totalSeptets - 7*total
    // 简化: 编码检测 sms_task 里完成, 这里把 raw totalSeptets 传过去
    m.cmt_length = (uint16_t)totalSeptets;
    xQueueSend(g_smsQ, &m, 0);
    clear_udh_ref(refId);
  }
  return true;
}

// =================== USB RX task ===================
static void usb_rx_task(void* /*param*/) {
  uint8_t buf[256];
  static char line[AT_LINE_BUF];
  static size_t lineLen = 0;
  while (1) {
    if (g_cdc == NULL) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    size_t dataLen = 0;
    usbh_cdc_get_rx_buffer_size(g_cdc, &dataLen);
    if (dataLen == 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    if (dataLen > sizeof(buf)) dataLen = sizeof(buf);
    if (usbh_cdc_read_bytes(g_cdc, buf, &dataLen, pdMS_TO_TICKS(100)) != ESP_OK) continue;

    // P6 调试: 临时打印 RX raw, 看 4G 模组到底回了什么 (收到 > 后会触发 g_atPrompt)
    // P6b debug: 临时打开, 看到 RX 后再关
#define DEBUG_CMGS_RX 1
#ifdef DEBUG_CMGS_RX
    if (g_waitingCmgsPrompt) {
      ESP_LOGW(TAG, "RX[%u]: %.*s", (unsigned)dataLen, (int)dataLen, (const char*)buf);
    }
#endif

    size_t pos = 0;
    while (pos < dataLen) {
      void* nl = memchr(buf + pos, '\n', dataLen - pos);
      size_t lineEnd = nl ? (size_t)((char*)nl - (char*)(buf + pos)) : (dataLen - pos);
      for (size_t j = 0; j < lineEnd; j++) {
        char c = buf[pos + j];
        if (c == '\r') continue;
        if (lineLen < sizeof(line)-1) line[lineLen++] = c;
      }
      if (nl) {
        if (lineLen >= sizeof(line)-1) {
          ESP_LOGE(TAG, "AT line too long, drop");
          lineLen = 0;
        } else {
          line[lineLen] = 0;
          if (lineLen > 0) handle_at_line(line);
          lineLen = 0;
        }
        pos += lineEnd + 1;
      } else {
        pos = dataLen;
      }
    }
  }
}

// =================== send_atcmd ===================
// 0=OK, -1=ERROR, -2=timeout
static int send_atcmd(const char* cmd, uint32_t timeout_ms) {
  if (!g_cdc) return -2;
  if (xSemaphoreTake(g_atMutex, pdMS_TO_TICKS(timeout_ms + 500)) != pdTRUE) return -2;

  // drain stale RX
  size_t dummy = 0;
  uint8_t trash[64];
  while (1) {
    usbh_cdc_get_rx_buffer_size(g_cdc, &dummy);
    if (!dummy) break;
    if (dummy > sizeof(trash)) dummy = sizeof(trash);
    usbh_cdc_read_bytes(g_cdc, trash, &dummy, 0);
  }
  // reset state
  g_atReplyLen = 0;
  g_atReply[0] = 0;
  g_atResult   = -2;
  g_inAtReply  = true;   // v4.0.7 P0 fix: 期间 + 行双写到 g_atReply (URC 队列也保留)
  xSemaphoreTake(g_atDone, 0);   // clear stale give

  ESP_LOGI(TAG, "AT TX: %s", cmd);
  usbh_cdc_write_bytes(g_cdc, (uint8_t*)cmd, strlen(cmd), pdMS_TO_TICKS(100));

  int rc = -2;
  if (xSemaphoreTake(g_atDone, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
    rc = g_atResult;
  } else {
    // 超时, dump raw reply 帮翔哥诊断 (MODEM 真实编码是啥)
    ESP_LOGW(TAG, "AT TIMEOUT, raw reply=%.200s", g_atReply);
  }
  g_inAtReply = false;
  xSemaphoreGive(g_atMutex);
  return rc;
}

// v4.0.7: CSQ 轮询 task (后台 5s 发一次 AT+CSQ, 解析 +CSQ: rssi,ber 写全局)
// 走 send_atcmd 自动串行化 (g_atMutex), 不与 cmgs_worker 抢链路
// 解析失败 / 超时不写全局 (前端 csqAgeMs=-1 表示过期)
static int csq_parse_reply(const char* reply) {
  // reply 形如: "\r\n+CSQ: 25,99\r\n\r\nOK\r\n" 或单行 "+CSQ: 18,0"
  const char* p = strstr(reply, "+CSQ:");
  if (!p) return -1;
  p += 5;
  while (*p == ' ') p++;
  if (*p < '0' || *p > '9') return -1;
  int csq = atoi(p);
  if (csq < 0 || csq > 31) return -1;
  if (csq == 99) return -1;          // 99 = 未知
  return csq;
}

static void csq_poll_task(void* /*param*/) {
  ESP_LOGI(TAG, "CSQ poll task started");
  for (;;) {
    // 等 4G 就绪 (boot 时还在枚举)
    if (!g_cdc || !g_ml.alive) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    int rc = send_atcmd("AT+CSQ\r\n", 3000);
    if (rc == 0) {
      int csq = csq_parse_reply(g_atReply);
      if (csq >= 0) {
        g_4g_csq   = csq;
        g_4g_dbm   = -113 + csq * 2;   // 3GPP TS 27.007: 0=-113dBm, 31=-51dBm
        g_4g_csqMs = millis();
      } else {
        ESP_LOGW(TAG, "CSQ parse fail: %.80s", g_atReply);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5000));  // 5s 间隔
  }
}

/* =================== STK (SIM ToolKit) ===================  暂停 (2026-06-19) ===================
 * v4.0.7: SIM 信息查询 (CIMI/CCID/CNUM/COPS) + STK URC 监听 (+STKPRO/+STKENV/+STKCNF) + AT+STKTR 终端响应
 * AT 命令串行化靠 g_atMutex (send_atcmd 自带), URC 靠 g_urcQ (handle_at_line 已写)
 * 日志走 ring buffer (256 行), 前端 2s 轮询拉
 *
 * 2026-06-19 翔哥决定: STK Proactive 暂停
 *   - 整段 [block comment] 注释, 保留 main.cpp 物理位置方便恢复
 *   - 路由 /stk, /api/stk{,/refresh,/cmd,/menu} 在 setup_web_routes 也注释
 *   - AT+STKPCMD=1 (modem_init_at) 也注释, 模组不再推 +STKPRO URC
 *   - xTaskCreate(stk_event_task/stk_query_task) 之前已注释, 不在跑
 *   - g_sim_* 声明 + handleApiStatus 读 SIM 面板 保留 (初始化为空串)
 *   - 恢复: 删本 [block comment] 块 + 3 处路由 + 1 处 AT+STKPCMD=1
 *   - 参考 memory/stk-paused.md
 */
// =================== 最近 SMS ring buffer (v4.0.7, dashboard 显示用, 与 STK 无关) ===================
// 之前误放进下面 STK #if 0 块导致编译失败, 挪出来
#define RX_LOG_CAP 32
typedef struct {
  uint32_t ms;     // boot millis (与 uptime 对齐)
  char     phone[24];
  char     body[160];
} RxLogEntry;
static RxLogEntry g_rxLog[RX_LOG_CAP] = {{0}};
static volatile uint16_t g_rxLogHead  = 0;
static volatile uint16_t g_rxLogCount = 0;
static portMUX_TYPE     g_rxLogMux    = portMUX_INITIALIZER_UNLOCKED;
static uint32_t         g_bootMs      = 0;   // boot 时的 millis (用作 uptime 基线, = 0)

static void rx_log_write(const char* phone, const char* body) {
  portENTER_CRITICAL(&g_rxLogMux);
  uint16_t idx = g_rxLogHead;
  g_rxLog[idx].ms = millis();
  strncpy(g_rxLog[idx].phone, phone, sizeof(g_rxLog[idx].phone) - 1);
  g_rxLog[idx].phone[sizeof(g_rxLog[idx].phone) - 1] = 0;
  strncpy(g_rxLog[idx].body, body, sizeof(g_rxLog[idx].body) - 1);
  g_rxLog[idx].body[sizeof(g_rxLog[idx].body) - 1] = 0;
  g_rxLogHead = (idx + 1) % RX_LOG_CAP;
  uint16_t cnt = g_rxLogCount;
  if (cnt < RX_LOG_CAP) g_rxLogCount = cnt + 1;
  portEXIT_CRITICAL(&g_rxLogMux);
}

// =================== STK (SIM ToolKit) ===================
// v4.0.7: SIM 信息查询 (CIMI/CCID/CNUM/COPS) + STK URC 监听 (+STKPRO/+STKENV/+STKCNF) + AT+STKTR 终端响应
// AT 命令串行化靠 g_atMutex (send_atcmd 自带), URC 靠 g_urcQ (handle_at_line 已写)
// 日志走 ring buffer (256 行), 前端 2s 轮询拉
#if 0  // STK Proactive 暂停 (2026-06-19)

#define STK_LOG_CAP 256
#define STK_LOG_LEN 96
typedef struct {
  uint32_t ms;
  char     line[STK_LOG_LEN];
} StkLogEntry;
static StkLogEntry g_stkLog[STK_LOG_CAP] = {{0}};
static volatile uint16_t g_stkLogHead = 0;     // 写入位置
static volatile uint16_t g_stkLogCount = 0;    // 已有条数 (≤ STK_LOG_CAP)
static portMUX_TYPE     g_stkLogMux   = portMUX_INITIALIZER_UNLOCKED;

static volatile bool g_stkRefreshReq = false;  // /api/stk/refresh 置位, stk_query_task 看到就跑一次

// v4.0.7: STK 主动菜单解析 (SETUP_MENU 0x25 的 BER-TLV items, 供前端展示 + 用户点选)
// 全局保留最近一次菜单, 16 个 item 上限 (实卡通常 ≤ 10)
typedef struct {
  uint8_t item_id;       // SIM 内部 item id (AT+STKR= 用这个)
  char    text[40];      // UTF-8 解码后的菜单文本
} StkMenuItem;
#define STK_MENU_MAX 16
static StkMenuItem g_stkMenu[STK_MENU_MAX] = {{0}};
static volatile uint8_t g_stkMenuCount = 0;
static volatile uint8_t g_stkMenuCmd   = 0;     // SETUP_MENU (0x25) / SELECT_ITEM (0x24) / DISPLAY_TEXT (0x21) ...
static char g_stkMenuTitle[40] = {0};           // SETUP_MENU 标题 (85 tag alpha_id)
static portMUX_TYPE g_stkMenuMux = portMUX_INITIALIZER_UNLOCKED;

// BER-TLV: 解一个 TLV 单元, 返回 value 长度, 失败 -1
// pos 指向 tag, 末尾设 *end = value 后的位置
static int bertlv_read(const uint8_t* buf, int len, int pos, int* tag, int* end) {
  if (pos + 1 > len) return -1;
  int t = buf[pos++];
  if (pos + 1 > len) return -1;
  int l = buf[pos++];
  if (l & 0x80) {  // 0x81+2byte 或 0x82+...
    int nb = l & 0x7F;
    if (pos + nb > len) return -1;
    l = 0;
    for (int i = 0; i < nb; i++) l = (l << 8) | buf[pos++];
  }
  if (pos + l > len) return -1;
  if (tag) *tag = t;
  if (end) *end = pos + l;
  return l;  // value 长度
}

// UCS-2 BE → UTF-8 (简化: BMP 范围, 不处理 surrogate pair)
static int ucs2be_to_utf8(const uint8_t* p, int n, char* out, int out_cap) {
  int o = 0;
  for (int i = 0; i + 1 < n && o + 4 < out_cap; i += 2) {
    uint16_t cp = ((uint16_t)p[i] << 8) | p[i+1];
    if (cp < 0x80) {
      out[o++] = (char)cp;
    } else if (cp < 0x800) {
      out[o++] = (char)(0xC0 | (cp >> 6));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    } else {
      out[o++] = (char)(0xE0 | (cp >> 12));
      out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  out[o] = 0;
  return o;
}

// GSM7 默认 alphabet unpacked (1 char = 1 byte, 0x00-0x7F)
static int gsm7_unpacked_to_utf8(const uint8_t* p, int n, char* out, int out_cap) {
  int o = 0;
  for (int i = 0; i < n && o + 2 < out_cap; i++) {
    uint8_t c = p[i];
    if (c == 0x00) c = '@';  // null padding → 常见
    if (c < 0x80) out[o++] = (char)c;
    else out[o++] = '?';
  }
  out[o] = 0;
  return o;
}

// 解 alpha_id (85 tag): 先看是否 UCS-2 (前 1 字节 0x80 = UCS-2 BE 前缀), 否则 GSM7
static int decode_alpha_id(const uint8_t* p, int n, char* out, int out_cap) {
  if (n >= 3 && p[0] == 0x80) {
    return ucs2be_to_utf8(p + 1, n - 1, out, out_cap);  // 0x80 prefix skip
  }
  return gsm7_unpacked_to_utf8(p, n, out, out_cap);
}

// +STKPRO: <hex> — 解析 SETUP_MENU, 写 g_stkMenu[]
// 返回 1 = SETUP_MENU 解析成功, 0 = 不是 SETUP_MENU, -1 = 解析失败
// v4.0.7 fix: 大 buffer (buf/items) 改为静态 (放在 .bss), 避免 stk_event_task 栈溢出
static int parse_stkpro_setup_menu(const char* hex) {
  static uint8_t    s_buf[256];
  static StkMenuItem s_items[STK_MENU_MAX];
  static char       s_titleBuf[40];
  // hex → bytes
  int hlen = strlen(hex);
  int blen = hlen / 2;
  if (blen < 4 || blen > (int)sizeof(s_buf)) return 0;
  auto hv = [](char c)->uint8_t{ return (c<='9')?(c-'0'):((c<='F')?(c-'A'+10):(c-'a'+10)); };
  for (int i = 0; i < blen; i++) {
    s_buf[i] = (hv(hex[i*2]) << 4) | hv(hex[i*2+1]);
  }
  // 外层: D0 tag + length + (内嵌 TLVs)
  if (s_buf[0] != 0xD0) return 0;  // 不是 Proactive Command
  int end;
  int l = bertlv_read(s_buf, blen, 0, nullptr, &end);
  if (l < 0) return -1;
  // 解析内嵌 TLVs, 找 81 (Command Details) 取 cmd 类型
  int cmd = 0;
  int titleLen = 0;
  memset(s_titleBuf, 0, sizeof(s_titleBuf));
  int itemsFound = 0;
  memset(s_items, 0, sizeof(s_items));
  int pos = 2;  // skip D0 + length
  while (pos < end) {
    int tag, childEnd;
    int vlen = bertlv_read(s_buf, blen, pos, &tag, &childEnd);
    if (vlen < 0) break;
    int vpos = childEnd - vlen;
    if (tag == 0x81 && vlen >= 1) {
      cmd = s_buf[vpos];  // SETUP_MENU=0x25, SELECT_ITEM=0x24, DISPLAY_TEXT=0x21
    } else if (tag == 0x85 && titleLen == 0) {
      titleLen = decode_alpha_id(s_buf + vpos, vlen, s_titleBuf, sizeof(s_titleBuf));
    } else if (tag == 0x8F && itemsFound < STK_MENU_MAX) {
      // 0x8F = Item: 第一个字节是 item_id, 余是 alpha_id
      if (vlen >= 2) {
        s_items[itemsFound].item_id = s_buf[vpos];
        decode_alpha_id(s_buf + vpos + 1, vlen - 1,
                        s_items[itemsFound].text, sizeof(s_items[itemsFound].text));
        itemsFound++;
      }
    }
    pos = childEnd;
  }
  if (cmd != 0x25) return 0;  // 不是 SETUP_MENU, 不刷新菜单
  portENTER_CRITICAL(&g_stkMenuMux);
  g_stkMenuCmd = cmd;
  strncpy(g_stkMenuTitle, s_titleBuf, sizeof(g_stkMenuTitle) - 1);
  g_stkMenuTitle[sizeof(g_stkMenuTitle) - 1] = 0;
  memcpy(g_stkMenu, s_items, sizeof(s_items));
  g_stkMenuCount = itemsFound;
  portEXIT_CRITICAL(&g_stkMenuMux);
  ESP_LOGI(TAG, "STK SETUP_MENU: title='%s' items=%d", s_titleBuf, itemsFound);
  return 1;
}

// 解析 AT+CIMI / +CIMI: 460001234567890 → 写 g_sim_imsi
static void parse_cimi_reply(const char* reply) {
  const char* p = strchr(reply, '\n');    // 跳过第一行 "+CIMI:" 或空行
  if (!p) p = reply;
  while (*p && (*p < '0' || *p > '9')) p++;  // 找第一个数字
  int n = 0;
  while (p[n] >= '0' && p[n] <= '9' && n < 15) n++;
  if (n >= 10 && n <= 15) {
    portENTER_CRITICAL(&g_sim_mux);
    memcpy(g_sim_imsi, p, n);
    g_sim_imsi[n] = 0;
    portEXIT_CRITICAL(&g_sim_mux);
    ESP_LOGI(TAG, "IMSI=%.*s", n, p);
  } else {
    ESP_LOGW(TAG, "CIMI parse fail (n=%d): %.60s", n, reply);
  }
}

// 解析 AT+CCID / +CCID: "898601..." → 写 g_sim_iccid
static void parse_ccid_reply(const char* reply) {
  const char* p = strstr(reply, "+CCID:");
  if (!p) p = reply;
  p += 6;
  while (*p == ' ' || *p == '"') p++;
  int n = 0;
  while ((p[n] >= '0' && p[n] <= '9') && n < 20) n++;
  if (n >= 18 && n <= 20) {
    portENTER_CRITICAL(&g_sim_mux);
    memcpy(g_sim_iccid, p, n);
    g_sim_iccid[n] = 0;
    portEXIT_CRITICAL(&g_sim_mux);
    ESP_LOGI(TAG, "ICCID=%.*s", n, p);
  } else {
    ESP_LOGW(TAG, "CCID parse fail (n=%d): %.60s", n, reply);
  }
}

// 解析 AT+CNUM / +CNUM: "号码","145",...  → 写 g_sim_msisdn
static void parse_cnum_reply(const char* reply) {
  const char* p = strstr(reply, "+CNUM:");
  if (!p) return;
  p += 6;
  const char* q = strchr(p, '"');          // 第一个 "
  if (!q) return;
  q++;                                     // 进入号码
  const char* r = strchr(q, '"');           // 结束 "
  if (!r || r <= q) return;
  int n = r - q;
  if (n > 0 && n < 16) {
    portENTER_CRITICAL(&g_sim_mux);
    memcpy(g_sim_msisdn, q, n);
    g_sim_msisdn[n] = 0;
    portEXIT_CRITICAL(&g_sim_mux);
    ESP_LOGI(TAG, "MSISDN=%.*s", n, q);
  }
}

// 解析 AT+COPS / +COPS: 0,0,"中国移动",7  → 写 g_sim_operator
static void parse_cops_reply(const char* reply) {
  const char* p = strstr(reply, "+COPS:");
  if (!p) return;
  p += 6;
  const char* q = strchr(p, '"');
  if (!q) return;
  q++;
  const char* r = strchr(q, '"');
  if (!r || r <= q) return;
  int n = r - q;
  if (n > 0 && n < (int)sizeof(g_sim_operator)) {
    portENTER_CRITICAL(&g_sim_mux);
    memcpy(g_sim_operator, q, n);
    g_sim_operator[n] = 0;
    portEXIT_CRITICAL(&g_sim_mux);
    ESP_LOGI(TAG, "Operator=%.*s", n, q);
  }
}

// 写一条 STK 日志 (stk_query_task 和 stk_event_task 都调用)
static void stk_log_write(const char* line) {
  portENTER_CRITICAL(&g_stkLogMux);
  uint16_t idx = g_stkLogHead;
  g_stkLog[idx].ms = millis();
  strncpy(g_stkLog[idx].line, line, STK_LOG_LEN - 1);
  g_stkLog[idx].line[STK_LOG_LEN - 1] = 0;
  g_stkLogHead = (idx + 1) % STK_LOG_CAP;
  uint16_t cnt = g_stkLogCount;
  if (cnt < STK_LOG_CAP) g_stkLogCount = cnt + 1;
  portEXIT_CRITICAL(&g_stkLogMux);
}

// STK URC 监听 task: 从 g_urcQ 消费 STK 相关行, 写 ring buffer
// handle_at_line 已把 + 开头行都塞 g_urcQ, 所以 +STKPRO/+STKENV/+STKCNF 都会进来
static void stk_event_task(void* /*param*/) {
  ESP_LOGI(TAG, "STK event task started");
  char line[URC_LINE_BUF];
  for (;;) {
    if (xQueueReceive(g_urcQ, line, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
    if (strncmp(line, "+STKPRO:", 8) == 0) {
      ESP_LOGI(TAG, "STK URC: %s", line);
      stk_log_write(line);
      // v4.0.7: 解析 SETUP_MENU (0x25) 的 BER-TLV, 提取 items 写 g_stkMenu
      const char* p = line + 8;
      while (*p == ' ') p++;
      int rc = parse_stkpro_setup_menu(p);
      if (rc < 0) ESP_LOGW(TAG, "STKPRO parse fail");
      // rc == 0: 不是 SETUP_MENU (e.g. DISPLAY_TEXT/GET_INPUT), 不刷新菜单
    } else if (strncmp(line, "+STKENV:", 8) == 0 ||
               strncmp(line, "+STKCNF:", 8) == 0) {
      ESP_LOGI(TAG, "STK URC: %s", line);
      stk_log_write(line);
    }
    // 其他 URC (CEREG/CSQN/...) 不管, 由现有逻辑处理
  }
}

// STK SIM 查询 task: boot 5s 后首次, 之后 30min 刷新; /api/stk/refresh 可强制
// AT+CIMI → AT+CCID → AT+CNUM → AT+COPS, 串行靠 send_atcmd 的 g_atMutex
static void stk_query_task(void* /*param*/) {
  ESP_LOGI(TAG, "STK query task started");
  // 等模组就绪
  for (int i = 0; i < 30 && (!g_cdc || !g_ml.alive); i++) vTaskDelay(pdMS_TO_TICKS(1000));
  for (;;) {
    if (!g_cdc || !g_ml.alive) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    ESP_LOGI(TAG, "STK: querying SIM info...");
    if (send_atcmd("AT+CIMI\r\n", 3000) == 0) parse_cimi_reply(g_atReply);
    if (send_atcmd("AT+CCID\r\n", 3000) == 0) parse_ccid_reply(g_atReply);
    if (send_atcmd("AT+CNUM\r\n", 3000) == 0) parse_cnum_reply(g_atReply);
    if (send_atcmd("AT+COPS?\r\n", 5000) == 0) parse_cops_reply(g_atReply);
    g_sim_queryMs = millis();
    stk_log_write("[INFO] SIM info refreshed");

    // 30min 间隔 (期间若 g_stkRefreshReq 置位则立即重跑)
    for (uint32_t i = 0; i < 1800 && !g_stkRefreshReq; i++) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
    g_stkRefreshReq = false;
  }
}

// /api/stk — 返回 SIM 信息 + 最近 N 条 STK 日志
static void handleApiStkInfo(AsyncWebServerRequest* r) {
  JsonDocument doc;
  portENTER_CRITICAL(&g_sim_mux);
  doc["imsi"]     = g_sim_imsi;
  doc["iccid"]    = g_sim_iccid;
  doc["msisdn"]   = g_sim_msisdn;
  doc["operator"] = g_sim_operator;
  portEXIT_CRITICAL(&g_sim_mux);
  doc["ageMs"]    = (int32_t)((g_sim_queryMs && millis() >= g_sim_queryMs) ? (millis() - g_sim_queryMs) : -1);

  // 日志 (从最旧到最新, 最多 50 条)
  JsonArray logs = doc["logs"].to<JsonArray>();
  portENTER_CRITICAL(&g_stkLogMux);
  uint16_t count = g_stkLogCount;
  uint16_t head  = g_stkLogHead;
  portEXIT_CRITICAL(&g_stkLogMux);
  // 起点 (最旧的索引)
  uint16_t start = (count < STK_LOG_CAP) ? 0 : head;
  uint16_t emit  = (count < 50) ? count : 50;
  // 输出最近 emit 条
  for (uint16_t i = 0; i < emit; i++) {
    uint16_t idx = (start + count - emit + i) % STK_LOG_CAP;
    JsonObject o = logs.add<JsonObject>();
    o["t"]  = (int32_t)(g_stkLog[idx].ms / 1000);   // 秒, 浏览器再 *1000
    o["l"]  = g_stkLog[idx].line;
  }
  String out; serializeJson(doc, out);
  r->send(200, "application/json", out);
}

// /api/stk/refresh — POST 触发立即重查
static void handleApiStkRefresh(AsyncWebServerRequest* r) {
  g_stkRefreshReq = true;
  r->send(200, "application/json", "{\"ok\":true}");
}

// v4.0.7: 返回当前 STK 菜单 (解析自最近一次 +STKPRO: SETUP_MENU URC)
static void handleApiStkMenu(AsyncWebServerRequest* r) {
  JsonDocument doc;
  portENTER_CRITICAL(&g_stkMenuMux);
  doc["cmd"]   = g_stkMenuCmd;
  doc["title"] = g_stkMenuTitle;
  doc["count"] = g_stkMenuCount;
  JsonArray items = doc["items"].to<JsonArray>();
  for (int i = 0; i < g_stkMenuCount; i++) {
    JsonObject o = items.add<JsonObject>();
    o["id"]   = g_stkMenu[i].item_id;
    o["text"] = g_stkMenu[i].text;
  }
  portEXIT_CRITICAL(&g_stkMenuMux);
  String out; serializeJson(doc, out);
  r->send(200, "application/json", out);
}

// /api/stk/cmd — POST {"cmd":"..."} 透传 AT (限 STK* 命令, 安全)
// 例: AT+STKTR="..."  AT+STKENV?  AT+STKMENU?
static void handleApiStkCmd(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
  // body 累积
  static char body[512];
  static size_t bodyLen = 0;
  if (index == 0) bodyLen = 0;
  if (bodyLen + len < sizeof(body)) {
    memcpy(body + bodyLen, data, len);
    bodyLen += len;
  }
  if (index + len < total) return;  // 还有 chunk, 等

  body[bodyLen] = 0;
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    r->send(400, "application/json", "{\"ok\":false,\"err\":\"bad_json\"}");
    return;
  }
  const char* cmd = doc["cmd"] | "";
  // 安全: 只允许 STK + SIM 查询命令 (按 waybyte/logicromsdk 实际命名: STKR/STKENV/STKMENU/STKTR)
  // STKR = select menu item, STKENV = envelope (terminate/open menu), STKTR = terminal response
  if (strncmp(cmd, "AT+STK", 6) != 0 && strncmp(cmd, "AT+CIMI", 7) != 0 &&
      strncmp(cmd, "AT+CCID", 7) != 0 && strncmp(cmd, "AT+CNUM", 7) != 0 &&
      strncmp(cmd, "AT+COPS", 7) != 0) {
    r->send(403, "application/json", "{\"ok\":false,\"err\":\"cmd_not_allowed\"}");
    return;
  }
  char fullcmd[128];
  snprintf(fullcmd, sizeof(fullcmd), "%s\r\n", cmd);
  int rc = send_atcmd(fullcmd, 5000);
  // 记日志
  char logline[STK_LOG_LEN];
  snprintf(logline, sizeof(logline), "[CMD] %s -> rc=%d", cmd, rc);
  stk_log_write(logline);
  // 回包
  JsonDocument resp;
  resp["ok"]    = (rc == 0);
  resp["rc"]    = rc;
  resp["reply"] = g_atReply;
  String out; serializeJson(resp, out);
  r->send(rc == 0 ? 200 : 502, "application/json", out);
}
#endif  // STK Proactive 暂停结束 (2026-06-19)

// =================== 最近收到 SMS API (v4.0.7, dashboard 显示用) ===================
// 之前误放进上面 STK #if 0 块导致编译失败, 挪出来
static void handleApiRecent(AsyncWebServerRequest* r) {
  JsonDocument doc;
  // uptime 基线 (boot 时 millis(), 与 g_rxLog[].ms 单位一致)
  uint32_t now = millis();
  doc["uptimeMs"] = (uint32_t)(now - g_bootMs);
  JsonArray arr = doc["items"].to<JsonArray>();
  portENTER_CRITICAL(&g_rxLogMux);
  uint16_t cnt = g_rxLogCount;
  uint16_t head = g_rxLogHead;
  portEXIT_CRITICAL(&g_rxLogMux);
  uint16_t start = (cnt < RX_LOG_CAP) ? 0 : head;
  // 从最新到最旧 (倒序), 最多 20 条
  uint16_t emit = (cnt < 20) ? cnt : 20;
  for (uint16_t i = 0; i < emit; i++) {
    uint16_t idx = (start + cnt - 1 - i + RX_LOG_CAP) % RX_LOG_CAP;
    JsonObject o = arr.add<JsonObject>();
    o["ageMs"] = (uint32_t)(now - g_rxLog[idx].ms);
    o["phone"] = g_rxLog[idx].phone;
    o["body"]  = g_rxLog[idx].body;
  }
  String out; serializeJson(doc, out);
  r->send(200, "application/json", out);
}

// =================== pushplus 推送 (用 esp_http_client, 不走 Arduino HTTPClient) ===================
static bool post_pushplus(const String& phoneUtf8, const String& bodyUtf8, uint32_t ts) {
  if (WiFi.status() != WL_CONNECTED) {
    ESP_LOGW(TAG, "WiFi not connected, skip push");
    return false;
  }

  JsonDocument doc;
  doc["token"]    = g_cfg.token;
  doc["title"]    = String("\xF0\x9F\x92\xAC ") + phoneUtf8;   // 💬
  doc["template"] = "html";
  String esc; esc.reserve(bodyUtf8.length() + 16);
  for (size_t i = 0; i < bodyUtf8.length(); i++) {
    char c = bodyUtf8[i];
    if      (c == '&') esc += "&amp;";
    else if (c == '<') esc += "&lt;";
    else if (c == '>') esc += "&gt;";
    else if (c == '"') esc += "&quot;";
    else esc += c;
  }
  doc["content"]  = String("<p>") + esc + "</p>";
  if (strlen(g_cfg.topic) > 0) doc["topic"] = g_cfg.topic;

  String payload;
  serializeJson(doc, payload);

  esp_http_client_config_t cfg = {};
  cfg.url               = PUSHPLUS_URL;
  cfg.method            = HTTP_METHOD_POST;
  cfg.timeout_ms        = 5000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;   // IDF 自带 CA bundle, 走 HTTPS 不用自己塞证书

  esp_http_client_handle_t cli = esp_http_client_init(&cfg);
  if (!cli) { ESP_LOGE(TAG, "esp_http_client_init failed"); return false; }

  esp_http_client_set_header(cli, "Content-Type", "application/json");
  esp_http_client_set_post_field(cli, payload.c_str(), payload.length());

  esp_err_t err = esp_http_client_perform(cli);
  int code = (err == ESP_OK) ? esp_http_client_get_status_code(cli) : -1;

  // 读 response body 看 pushplus 业务码 (≤256 字节)
  char resp[256] = {0};
  if (err == ESP_OK) {
    int r = esp_http_client_read_response(cli, resp, sizeof(resp) - 1);
    if (r > 0) resp[r] = 0;
  }
  esp_http_client_cleanup(cli);

  ESP_LOGI(TAG, "Pushplus POST code=%d resp=%.120s", code, resp);
  return code == 200;
}

// =================== build_push_payload (给 NVS 队列用) ===================
static size_t build_push_payload(const String& phoneUtf8, const String& bodyUtf8, uint32_t ts,
                                 char* out, size_t outLen) {
  JsonDocument doc;
  doc["token"]    = g_cfg.token;
  doc["title"]    = String("\xF0\x9F\x92\xAC ") + phoneUtf8;
  doc["template"] = "html";
  String esc; esc.reserve(bodyUtf8.length() + 16);
  for (size_t i = 0; i < bodyUtf8.length(); i++) {
    char c = bodyUtf8[i];
    if      (c == '&') esc += "&amp;";
    else if (c == '<') esc += "&lt;";
    else if (c == '>') esc += "&gt;";
    else if (c == '"') esc += "&quot;";
    else esc += c;
  }
  doc["content"]  = String("<p>") + esc + "</p>";
  if (strlen(g_cfg.topic) > 0) doc["topic"] = g_cfg.topic;
  String s; serializeJson(doc, s);
  if (s.length() >= outLen) {
    // 不截断 — 截断会破坏 JSON, pushplus 返 4xx → 坏 payload 进 NVS 队列死循环
    ESP_LOGW(TAG, "Push payload %u >= %u, drop", (unsigned)s.length(), (unsigned)outLen);
    return 0;
  }
  strncpy(out, s.c_str(), outLen - 1);
  out[outLen - 1] = 0;
  return s.length();
}

// 推送成功计数 + 时间戳统一更新(boot 通知 / net_task 共用)
// v4.0.6 P11b: 同时记 epoch ms (g_timeSynced 后才有效; 同步前 epoch<1e9 也无所谓)
// v4.0.6 P11c: 只在 SNTP 同步后写 epoch, 否则保持 0 (前端显 "-")
//   客户反馈 "看到 1970/01/01 08:00:27" — 同步前 time(NULL)=0 写出小值显示 1970
static inline void push_success_inc() {
  __atomic_add_fetch(&g_pushOk, 1, __ATOMIC_RELAXED);
  g_lastPushOkMs = millis();
  if (g_timeSynced) g_lastPushOkUtcMs = (uint64_t)time(NULL) * 1000ULL;  // P25: uint64 epoch ms
}

// 通用 HTTPS POST JSON, 返回 HTTP code (-1 = 网络错 / init 失败)
// 调用方负责 guard 前置条件(WiFi up 等)
static int http_post_json(const char* url, const char* payload, size_t len, uint32_t timeout_ms) {
  esp_http_client_config_t cfg = {};
  cfg.url               = url;
  cfg.method            = HTTP_METHOD_POST;
  cfg.timeout_ms        = timeout_ms;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t cli = esp_http_client_init(&cfg);
  if (!cli) return -1;
  esp_http_client_set_header(cli, "Content-Type", "application/json");
  esp_http_client_set_post_field(cli, payload, len);
  esp_err_t err = esp_http_client_perform(cli);
  int code = (err == ESP_OK) ? esp_http_client_get_status_code(cli) : -1;
  esp_http_client_cleanup(cli);
  return code;
}

// =================== post_pushplus_raw (从 PushItem payload 推) ===================
// v4.0.7 改回 v4.0.6 行为: 只看 HTTP 200. 翔哥证实 v4.0.6 token 填对时推送真成功,
// 之前我怀疑的 "业务码 903 静默失败" 假说站不住 — 实际是 NVS 被擦后 token 丢了.
static bool post_pushplus_raw(const char* payload, size_t len) {
  if (WiFi.status() != WL_CONNECTED) return false;
  int code = http_post_json(PUSHPLUS_URL, payload, len, 5000);
  ESP_LOGI(TAG, "Push(WiFi) code=%d", code);
  return (code == 200);
}

// =================== 开机上线通知 ===================
static void push_boot_notification() {
  if (!g_cfg.bootPush) {
    ESP_LOGI(TAG, "Boot notification skipped (cfg.bootPush=false)");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    ESP_LOGW(TAG, "WiFi not up, skip boot notification");
    return;
  }
  JsonDocument doc;
  doc["token"]    = g_cfg.token;
  doc["title"]    = String("\xE2\x9C\x85 SMS Forwarder 上线 ");   // ✅
  doc["template"] = "html";
  String body = "<p>设备已上线</p><ul>"
                "<li>FW: " FW_VERSION "</li>"
                "<li>Boot #";
  body += g_bootCount;
  body += "</li>";
  if (g_ml.alive) {
    body += "<li>4G 模组: OK</li>";
  } else {
    body += "<li>4G 模组: 未连接</li>";
  }
  body += "<li>IP: ";
  body += WiFi.localIP().toString();
  body += "</li></ul>";
  doc["content"]  = body;
  if (strlen(g_cfg.topic) > 0) doc["topic"] = g_cfg.topic;
  String payload; serializeJson(doc, payload);

  int code = http_post_json(PUSHPLUS_URL, payload.c_str(), payload.length(), 5000);
  ESP_LOGI(TAG, "Boot notification code=%d", code);
  if (code == 200) push_success_inc();
}

// =================== NVS push 队列 ===================
// v4.0.7 P0-fix: 工厂清 NVS 后 namespace "pqueue" 不存在, Preferences 反复 begin 会刷屏
//   (Arduino 库内部 log_e 不可拦), 加 throttle 降到 "5s 一次 summary"
static uint32_t nvsQErrLast = 0;
static uint32_t nvsQErrCount = 0;
static void nvsQNoteErr(const char* op) {
  nvsQErrCount++;
  uint32_t now = millis();
  if (now - nvsQErrLast > 5000) {
    ESP_LOGW(TAG, "PushQueue NVS %s 失败 %u 次 (5s 内, 刷屏已节流)", op, (unsigned)nvsQErrCount);
    nvsQErrLast = now;
    nvsQErrCount = 0;
  }
}

static void nvsQEnqueue(const char* payload, size_t len) {
  Preferences p;
  if (!p.begin("pqueue", false)) { nvsQNoteErr("begin"); return; }
  // 用 push 编号 (NVS key: p0, p1, ...) 滚动写
  uint8_t head = p.getUChar("head", 0);
  uint8_t count = p.getUChar("count", 0);
  char key[8]; snprintf(key, sizeof(key), "p%u", head);
  p.putString(key, payload);
  p.putUChar("head", (head + 1) % NVS_QUEUE_LEN);
  if (count < NVS_QUEUE_LEN) p.putUChar("count", count + 1);
  p.end();
  ESP_LOGW(TAG, "PushQueue enqueue (count=%u) — 待重发", count + 1);
}

static void nvsQEnqueuePhone(const String& phone, const String& body, uint32_t ts) {
  PushItem item = {};
  item.len = build_push_payload(phone, body, ts, item.payload, sizeof(item.payload));
  if (item.len > 0) nvsQEnqueue(item.payload, item.len);
}

static int nvsQDrain() {
  Preferences p;
  if (!p.begin("pqueue", true)) { nvsQNoteErr("drain"); return 0; }
  uint8_t head = p.getUChar("head", 0);
  uint8_t count = p.getUChar("count", 0);
  p.end();
  if (count == 0) return 0;
  // 简化: 从 tail= (head - count + N) mod N 开始, 逐条 pop 并推
  int drained = 0;
  for (int i = 0; i < count; i++) {
    int idx = (head - count + i + NVS_QUEUE_LEN) % NVS_QUEUE_LEN;
    char key[8]; snprintf(key, sizeof(key), "p%u", idx);
    if (!p.begin("pqueue", true)) { nvsQNoteErr("drain-key"); return drained; }
    String s = p.getString(key, "");
    p.end();
    if (s.length() == 0) continue;
    if (post_pushplus_raw(s.c_str(), s.length())) {
      // 成功, 删这条
      p.begin("pqueue", false);
      p.remove(key);
      uint8_t newCount = (count - i - 1);
      p.putUChar("count", newCount);
      p.end();
      drained++;
    } else {
      break;  // 失败停, 留给下次
    }
  }
  if (drained > 0) ESP_LOGI(TAG, "PushQueue drained %d items", drained);
  return drained;
}

// 启动自检: head/count/key 三者不一致会腐烂 (nvsQEnqueue 写到 key 后没 ++ count 时掉电)
// 扫描 p0..p(N-1) 实际非空数, 与 count 对比, 不一致就修
static void nvsQSanityCheck() {
  Preferences p;
  if (!p.begin("pqueue", true)) { nvsQNoteErr("sanity"); return; }
  uint8_t head = p.getUChar("head", 0);
  uint8_t count = p.getUChar("count", 0);
  p.end();

  if (head >= NVS_QUEUE_LEN) {
    ESP_LOGW(TAG, "NVS queue head=%u OOR, reset", head);
    if (!p.begin("pqueue", false)) { nvsQNoteErr("sanity-rst"); return; }
    p.putUChar("head", 0);
    p.putUChar("count", 0);
    p.end();
    return;
  }

  uint8_t actual = 0;
  for (int i = 0; i < NVS_QUEUE_LEN; i++) {
    char key[8]; snprintf(key, sizeof(key), "p%d", i);
    if (!p.begin("pqueue", true)) { nvsQNoteErr("sanity-scan"); return; }
    String s = p.getString(key, "");
    p.end();
    if (s.length() > 0) actual++;
  }

  if (actual != count || count > NVS_QUEUE_LEN) {
    ESP_LOGW(TAG, "NVS queue count=%u actual=%u, fixing", count, actual);
    if (!p.begin("pqueue", false)) { nvsQNoteErr("sanity-fix"); return; }
    p.putUChar("count", actual);
    p.end();
  }
}

// =================== SMS 发送 (v4.0.6+, 双向) ===================
// 限频: 1 分钟 N 条 (滑窗, RAM token bucket) — 调试期放大到 20/min
static constexpr int SEND_RL_CAP = 20;
static uint32_t s_sendTs[SEND_RL_CAP] = {0};
static int      s_sendIdx = 0;
static portMUX_TYPE s_sendMux = portMUX_INITIALIZER_UNLOCKED;
static bool smsRateLimitOk() {
  // TODO: 修完 nvsSentList 噪声 + log 级别后, 恢复限频 (翔哥 2026-06-18 拍板先关闭)
  return true;
}

// NVS "sent" 命名空间, 32 条滚动 (仿 pqueue 模式)
namespace { constexpr const char* NVS_NS_SENT = "sent"; constexpr int SENT_CAP = 32; }

static void nvsSentEnqueue(const char* phone, const char* body_preview,
                           uint8_t ref, bool ok, int err_code) {
  Preferences p;
  p.begin(NVS_NS_SENT, false);
  uint8_t head = p.getUChar("head", 0);
  char key[8]; snprintf(key, sizeof(key), "s%u", head);
  char val[256];
  // body_preview 已 truncate ≤ 40 字符, phone ≤ 16
  snprintf(val, sizeof(val),
    "{\"ts\":%lu,\"ph\":\"%.16s\",\"bp\":\"%.40s\",\"ref\":%u,\"ok\":%d,\"c\":%d}",
    (unsigned long)millis(), phone, body_preview, ref, ok ? 1 : 0, err_code);
  p.putString(key, val);
  p.putUChar("head", (head + 1) % SENT_CAP);
  p.end();
}
// v4.0.6 P14: 清空 sent 历史 (翔哥 2026-06-18 加, dashboard 列表清掉)
static void nvsSentClear() {
  Preferences p;
  p.begin(NVS_NS_SENT, false);
  p.clear();    // 整 namespace 擦 (32 keys + head)
  p.end();
}

// 把 NVS "sent" 32 条返成 JSON 数组 (新 → 旧)
static void nvsSentList(String& out) {
  Preferences p;
  p.begin(NVS_NS_SENT, true);
  uint8_t head = p.getUChar("head", 0);
  p.end();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  // 从 head-1 倒着读 32 条 (新 → 旧)
  for (int i = 0; i < SENT_CAP; i++) {
    int idx = ((int)head - 1 - i + SENT_CAP) % SENT_CAP;
    char key[8]; snprintf(key, sizeof(key), "s%d", idx);
    p.begin(NVS_NS_SENT, true);
    String s = p.getString(key, "");
    p.end();
    if (s.length() == 0) continue;
    JsonDocument item;
    if (deserializeJson(item, s) == DeserializationError::Ok) {
      arr.add(item);
    }
  }
  serializeJson(arr, out);
}

// CMGS 走完整流程: 写 AT+CMGS=<n> → 等 "> " → 写 PDU+Ctrl-Z → 等 OK/ERROR
// 返: 0 = OK (outRef 已写), -1 = ERROR (outErr 已写), -2 = timeout
// 注意: 自己拿/放 g_atMutex
static int cmgs_send_pdu(const char* pdu_hex, uint8_t& outRef, int& outErr, uint32_t timeout_ms) {
  outRef = 0; outErr = 0;
  if (!g_cdc) return -2;
  if (xSemaphoreTake(g_atMutex, pdMS_TO_TICKS(timeout_ms + 500)) != pdTRUE) return -2;

  // drain
  size_t dummy = 0;
  uint8_t trash[64];
  while (1) {
    usbh_cdc_get_rx_buffer_size(g_cdc, &dummy);
    if (!dummy) break;
    if (dummy > sizeof(trash)) dummy = sizeof(trash);
    usbh_cdc_read_bytes(g_cdc, trash, &dummy, 0);
  }
  // reset
  g_atReplyLen = 0;
  g_atReply[0] = 0;
  g_atResult   = -2;
  xSemaphoreTake(g_atDone, 0);
  xSemaphoreTake(g_atPrompt, 0);
  g_waitingCmgsPrompt = true;

  // AT+CMGS=<pdu_byte_len>\r
  int pdu_byte_len = (int)strlen(pdu_hex) / 2;
  char cmd[32];
  snprintf(cmd, sizeof(cmd), "AT+CMGS=%d\r", pdu_byte_len);
  ESP_LOGI(TAG, "AT TX: %s", cmd);
  usbh_cdc_write_bytes(g_cdc, (uint8_t*)cmd, strlen(cmd), pdMS_TO_TICKS(100));

  int rc = -2;
  // v4.0.6 P6b: ML307 PDU 模式可能不回 ">" 提示 (Quectel 风格: AT+CMGS=n 后直接进入 PDU 接收)
  // 先等 200ms, 拿到 > 就走老路; 拿不到就直接发 PDU+CtrlZ
  bool gotPrompt = (xSemaphoreTake(g_atPrompt, pdMS_TO_TICKS(200)) == pdTRUE);
  if (!gotPrompt) ESP_LOGW(TAG, "CMGS no '>' prompt, send PDU directly (ML307 quirk?)");
  // 写 PDU + Ctrl-Z
  size_t n = strlen(pdu_hex);
  char buf[600];
  if (n + 1 >= sizeof(buf)) { g_waitingCmgsPrompt = false; xSemaphoreGive(g_atMutex); return -2; }
  memcpy(buf, pdu_hex, n);
  buf[n] = 0x1A;     // Ctrl-Z
  usbh_cdc_write_bytes(g_cdc, (uint8_t*)buf, n + 1, pdMS_TO_TICKS(200));
  // 等 OK/ERROR
  if (xSemaphoreTake(g_atDone, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
    rc = g_atResult;
    if (rc == 0) {
      const char* p = strstr(g_atReply, "+CMGS:");
      if (p) outRef = (uint8_t)atoi(p + 6);
    } else {
      const char* p = strstr(g_atReply, "+CMS ERROR:");
      if (p) outErr = atoi(p + 11);
    }
  } else {
    ESP_LOGW(TAG, "CMGS no OK/ERROR after %ums", timeout_ms);
  }
  g_waitingCmgsPrompt = false;
  ESP_LOGI(TAG, "CMGS rc=%d ref=%u err=%d reply=%.200s", rc, outRef, outErr, g_atReply);
  xSemaphoreGive(g_atMutex);
  return rc;
}

// =================== CMGS 异步封装 (P6 修) ===================
// v4.0.6 P6 fix: cmgs_send_pdu 同步跑要 10–20s, 直接从 async_tcp 调会饿死 usb_rx_task (TWDT 卡 5s),
// ">" prompt 收不到 → rc=-2。改成派到独立 worker task 持 mutex, handler 端只等 semaphore,
// usb_rx_task 不被占用 → 能正常收 ">" / OK / +CMS ERROR。
//
// 一次只能跑一个 CMGS (g_atMutex 串行化), handler 排队等 worker 通知, 不抢 USB。

typedef struct {
  const char* pdu_hex;
  uint8_t*    outRef;
  int*        outErr;
  int*        outRc;
  uint32_t    timeout_ms;
  SemaphoreHandle_t done;       // binary, 1 = 通知
} CmgsJob;

// 单 worker task, 死循环接 job
static void cmgs_worker_task(void* /*param*/) {
  for (;;) {
    CmgsJob* job = NULL;
    // 等 job (来自 web handler 的 xTaskCreate 投递用通知的方式不行, 这里用计数信号量当 mailbox)
    // 简化: 用 ulTaskNotifyTake 当 mailbox (值 = job ptr)
    uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    job = (CmgsJob*)n;
    if (!job) continue;
    *job->outRc = cmgs_send_pdu(job->pdu_hex, *job->outRef, *job->outErr, job->timeout_ms);
    xSemaphoreGive(job->done);
  }
}

static TaskHandle_t    g_cmgsWorker = NULL;
static SemaphoreHandle_t g_cmgsJobSem = NULL;  // 通知 worker 有 job

// 异步版本: 派 worker, 等 done 信号量 (worker 自己跑 cmgs_send_pdu, 自己拿/放 mutex)
static int cmgs_send_pdu_async(const char* pdu_hex, uint8_t& outRef, int& outErr, uint32_t timeout_ms) {
  outRef = 0; outErr = 0;
  if (!g_cmgsWorker) return -2;  // 还没起来

  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) return -2;

  CmgsJob job;
  job.pdu_hex   = pdu_hex;
  job.outRef    = &outRef;
  job.outErr    = &outErr;
  job.outRc     = (int*)pvPortMalloc(sizeof(int));   // worker 写完, handler 读; 用堆避免 stack 失效
  job.timeout_ms = timeout_ms;
  job.done      = done;
  if (!job.outRc) { vSemaphoreDelete(done); return -2; }

  // 把 job ptr 给 worker (task notification 当 mailbox)
  xTaskNotify(g_cmgsWorker, (uint32_t)&job, eSetValueWithOverwrite);
  // 等 worker 完成 (给足时间 + 缓冲)
  uint32_t total_wait = timeout_ms * 3 + 2000;  // 3x 单段超时就够, 加 2s buffer
  BaseType_t got = xSemaphoreTake(done, pdMS_TO_TICKS(total_wait));
  vSemaphoreDelete(done);

  int rc = -2;
  if (got == pdTRUE) {
    rc = *job.outRc;
  } else {
    ESP_LOGE(TAG, "CMGS worker 超时未返回 (handler 端等了 %ums)", total_wait);
  }
  vPortFree(job.outRc);
  return rc;
}

// =================== SmsTask ===================
static void sms_task(void* /*param*/) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    SmsMsg msg;
    if (xQueueReceive(g_smsQ, &msg, pdMS_TO_TICKS(500)) == pdTRUE) {
      // 先试 UDH 拼接 (如果 body 含 0003...)
      if (stash_udh_part(&msg)) {
        ESP_LOGI(TAG, "SMS UDH part stashed");
        continue;
      }
      char phoneBuf[64], bodyBuf[AT_LINE_BUF];
      size_t phoneN = pdu::decode_phone_field(msg.phone_hex, strnlen(msg.phone_hex, sizeof(msg.phone_hex)),
                                              phoneBuf, sizeof(phoneBuf));
      // v4.0.7.1: alphanumeric sender fallback (DTAC/AIS/Verify 等)
      // ML307 在 alphanumeric OA 时 +CMT 头 oa 字段是空, 真正 sender 在 PDU body 的 OA 段
      // ToA=0xD0 (alphanumeric) → GSM7 packed; ToA=0x81/91 等 → numeric BCD swap
      // 详见 pdu_codec.{h,cpp}::pdu_oa_offset + decode_gsm7_alpha_oa
      if (phoneN == 0 && strnlen(msg.body_hex, sizeof(msg.body_hex)) >= 4) {
        size_t bodyLen = strnlen(msg.body_hex, sizeof(msg.body_hex));
        bool isAlpha = false;
        size_t oaValueOctets = 0;
        size_t oaOff = pdu::pdu_oa_offset(msg.body_hex, bodyLen, &isAlpha, &oaValueOctets);
        if (oaOff > 0) {
          if (isAlpha && oaValueOctets > 0) {
            // GSM7 packed: hex string → raw bytes → unpack → UTF-8
            // oaOff 是 hex 偏移, oaValueOctets*2 是 hex 字符数
            uint8_t raw[12] = {0};
            size_t maxOctets = oaValueOctets < sizeof(raw) ? oaValueOctets : sizeof(raw);
            for (size_t i = 0; i < maxOctets; i++) {
              char h0 = msg.body_hex[oaOff + i*2], h1 = msg.body_hex[oaOff + i*2 + 1];
              auto v = [](char c)->uint8_t{
                return (c<='9')?(c-'0'):((c<='F')?(c-'A'+10):(c-'a'+10));
              };
              raw[i] = (uint8_t)((v(h0) << 4) | v(h1));
            }
            size_t nchars = (oaValueOctets * 8) / 7;  // oaLen 字段 ML307 写错, 反算
            phoneN = pdu::decode_gsm7_alpha_oa((const char*)raw, maxOctets, nchars,
                                               phoneBuf, sizeof(phoneBuf));
            ESP_LOGI(TAG, "SMS: OA alphanumeric sender (octets=%u nchars=%u) → phone='%.*s'",
                     (unsigned)oaValueOctets, (unsigned)nchars, (int)phoneN, phoneBuf);
          }
          // numeric fallback: v4.0.3+ 假定 +CMT 头 oa 字段有内容, 实际我们已经在用
          // 但若未来 ML307 升级填了 OA 数字字段 + 头 oa 字段都没, 这里要加 BCD swap
        }
      }
      // 编码自动检测: DCS 优先 (3GPP TS 23.038 §4 + Wikipedia Data Coding Scheme)
      // 7-bit (含 flash/ME/SIM/TE class): 0x00-0x03, 0x10-0x13, 0xC0/0xD0 (MWI), 0xF0-0xF3
      // 8-bit raw:                          0x04-0x07, 0x14-0x17, 0xF4-0xFB, 0xFC-0xFF
      // UCS-2:                              0x08-0x0B, 0x18-0x1B, 0xE0
      // reserved (启发):                    0x0C-0x0F, 0x1C-0x1F, 0xFF/未知
      // 启发原则 (混合, 参 smspdudecoder + gammu GSM_GetMessageCoding):
      //   1. 严格 DCS 命中 → 信 DCS
      //   2. reserved/未知 → 先 sniff raw bytes (looks_like_ucs2_be), 是 UCS-2 BE → 走 UCS-2
      //   3. 不像 UCS-2 + cmt_length 可信 → 长度匹配试 7-bit
      //   4. 默认 UCS-2 (更安全, 防 DCS=0xFF/未知 + 中英混排短信被误判 7-bit → 乱码)
      size_t bodyHexLen = strnlen(msg.body_hex, sizeof(msg.body_hex));
      size_t bodyBytes  = bodyHexLen / 2;
      auto dcs7 = [](uint16_t d){
        return (d<=0x03) || (d>=0x10&&d<=0x13)
            || (d==0xC0) || (d==0xD0)                              // MWI (3GPP TS 23.040 §9.2.3.10)
            || (d>=0xF0&&d<=0xF3);
      };
      // 3GPP TS 23.038 §4: 0xF8-0xFB = UCS-2 (no class), 0xFC-0xFF = reserved
      // 之前 0xF8-0xFF 全归 8-bit raw, 触发 "UCS-2 当 raw 字节写" 乱码
      // 现 8-bit 只 0x04-0x07 / 0x14-0x17 / 0xF4-0xF7; 0xF8-0xFF 留给 unknown 路径
      auto dcs8 = [](uint16_t d){
        return (d>=0x04&&d<=0x07) || (d>=0x14&&d<=0x17)
            || (d>=0xF4&&d<=0xF7);
      };
      // 1) 跳 PDU 头 (SCA+FO+OA+PID+DCS+SCTS+UDL), 切出 UD 段 — decode 函数只该吃 UD
      //    之前把整条 PDU (含 SCA 等) 喂进 decode, SCA bytes 被当 UCS-2 codepoint 输出
      //    dcs7 只能从 msg.dcs 粗判 (用于 UDL 单位), sniff 后可能再覆盖
      bool dcsSays7bit = dcs7(msg.dcs);
      size_t udHexOff = 0, udBytes = 0, udHexLen = 0;
      if (bodyHexLen >= 4) {
        udHexOff = pdu::pdu_ud_offset(msg.body_hex, bodyHexLen, dcsSays7bit, &udBytes);
      }
      if (udHexOff > 0 && udHexOff + udBytes * 2 <= bodyHexLen) {
        udHexLen = udBytes * 2;
      } else {
        // PDU 格式错 / 太短 → 兜底用全 body 当 UD (跟之前行为对齐, 不退化)
        udHexOff = 0;
        udBytes = bodyBytes;
        udHexLen = bodyHexLen;
      }
      const char* udHex = msg.body_hex + udHexOff;
      bool is7bit = dcs7(msg.dcs);
      bool is8bitData = !is7bit && dcs8(msg.dcs);
      if (!is7bit && !is8bitData) {
        // UCS-2 / reserved / 未知 → 启发, 默认 UCS-2 (更安全)
        if (pdu::looks_like_ucs2_be(udHex, udHexLen)) {
          // DCS=0xFF (或 reserved) 但实际 UCS-2 (gateway 标错, 泰文 0E23...) → 落 UCS-2 路径
          // is7bit = false → 落到下面 else 分支调 decode_body_field
          is7bit = false;
        } else if (msg.cmt_length > 0) {
          // 7-bit: udBytes ≈ cmt_len*7/8 ±2
          size_t expect = (msg.cmt_length * 7 + 7) / 8;
          is7bit = (udBytes >= expect - 2 && udBytes <= expect + 2);
        }
        // else: cmt_length 不可信, 默认 UCS-2
      }
      // 兜底 2: 即使 DCS 标 7-bit (含 DCS=0), raw body 若呈 UCS-2 BE 模式 → 强制走 UCS-2
      //   is_strict_utf8 兜底不靠谱 (GSM7 扩展字符 è/ø/Å/ò 都是合法 2-byte UTF-8)
      //   提前 sniff 避免跑 7-bit decode 浪费 + 防乱码推送
      if (is7bit && pdu::looks_like_ucs2_be(udHex, udHexLen)) {
        ESP_LOGW(TAG, "SMS: raw body UCS-2 BE pattern, bypass 7-bit decode (dcs=%u cmt_len=%u udBytes=%u)",
                 msg.dcs, msg.cmt_length, (unsigned)udBytes);
        is7bit = false;
        is8bitData = false;  // 强制走 else 分支 (decode_body_field = UCS-2)
      }
      size_t bodyN = 0;
      if (is7bit) {
        // 7-bit: numChars (user septets) 推导
        //   单 part (msg 直接从 queue 来的, body 含 UDH 头): user = cmt_length - 7
        //   stash 出来的 concat msg: cmt_length = sum(per_part), 已是 user septets, 直接用
        //   cmt_length=0 (不可信): 用 udBytes 倒推
        size_t numChars = 0;
        if (msg.cmt_length > 0) {
          numChars = msg.cmt_length;
          // body 头部还含 UDH 标志 (单 part 没被 stash 吃) → 减 7 overhead
          if (strstr(udHex, "0804") || strstr(udHex, "0003")) {
            if (numChars > 7) numChars -= 7;
          }
        }
        if (numChars == 0) numChars = (udBytes * 8) / 7;
        bodyN = pdu::decode_7bit_packed(udHex, udHexLen, numChars,
                                        bodyBuf, sizeof(bodyBuf));
        ESP_LOGI(TAG, "SMS: 7-bit decode (dcs=%u cmt_len=%u udBytes=%u → numChars=%u bodyN=%u)",
                 msg.dcs, msg.cmt_length, (unsigned)udBytes, (unsigned)numChars, (unsigned)bodyN);
        // 兜底: GSM7 decode 输出必须是合法 UTF-8
        // DCS=0 但实际 UCS-2 (gateway 标错, 泰文 0E23...) 当 7-bit 解会出无效 UTF-8
        // (含 lone continuation / 4+ byte sequence) → fallback UCS-2
        if (bodyN > 0 && !pdu::is_strict_utf8(bodyBuf, bodyN)) {
          ESP_LOGW(TAG, "SMS: 7-bit decode not strict UTF-8, fallback UCS-2");
          bodyN = pdu::decode_body_field(udHex, udHexLen, bodyBuf, sizeof(bodyBuf));
          is7bit = false;
        }
      } else if (is8bitData) {
        // 8-bit raw: hex → raw bytes, 跳过非 hex 字符
        ESP_LOGI(TAG, "SMS: 8-bit data (dcs=%u cmt_len=%u udBytes=%u)", msg.dcs, msg.cmt_length, (unsigned)udBytes);
        bodyN = 0;
        for (size_t i = 0; i + 1 < udHexLen && bodyN < sizeof(bodyBuf) - 1; i += 2) {
          char h0 = udHex[i], h1 = udHex[i+1];
          auto isH = [](char c){
            return (c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f');
          };
          if (!isH(h0) || !isH(h1)) continue;  // 非 hex 跳过 (防 CR/LF/空段污染)
          auto v = [](char c)->uint8_t{
            return (c<='9')?(c-'0'):((c<='F')?(c-'A'+10):(c-'a'+10));
          };
          bodyBuf[bodyN++] = (char)((v(h0) << 4) | v(h1));
        }
      } else {
        bodyN = pdu::decode_body_field(udHex, udHexLen, bodyBuf, sizeof(bodyBuf));
      }
      String phoneUtf8(phoneBuf, phoneN);
      String bodyUtf8(bodyBuf, bodyN);
      g_lastSmsMs = millis();
      // v4.0.6 P11c: 只在 SNTP 同步后写 epoch, 否则保持 0 (前端显示 "-")
      // 客户反馈 "1970/01/01 08:00:27" — 同步前 time(NULL)≈0, 写出小值
      if (g_timeSynced) g_lastSmsUtcMs = (uint64_t)time(NULL) * 1000ULL;  // P25: uint64 epoch ms
      ESP_LOGI(TAG, "SMS: phone=%s body=%.120s", phoneUtf8.c_str(), bodyUtf8.c_str());
      ESP_LOGI(TAG, "SMS: body_raw=%.200s body_len=%u", msg.body_hex, (unsigned)strlen(msg.body_hex));
      // v4.0.7: 写最近 SMS ring buffer (dashboard 显示用, portMUX 保护)
      rx_log_write(phoneUtf8.c_str(), bodyUtf8.c_str());
      // 诊断: 把解码出的每个字符的 codepoint 列出来
      String codepoints;
      for (size_t i = 0; i < bodyUtf8.length() && codepoints.length() < 200; i++) {
        // UTF-8 简单 decode 取单字节
        uint8_t c = (uint8_t)bodyUtf8[i];
        char buf[8];
        snprintf(buf, sizeof(buf), "U+%02X ", c);
        codepoints += buf;
      }
      ESP_LOGI(TAG, "SMS: body_codepoints=%s", codepoints.c_str());

      // 组 payload 入 pushQ
      PushItem item = {};
      item.len = build_push_payload(phoneUtf8, bodyUtf8, msg.ts, item.payload, sizeof(item.payload));
      if (item.len > 0) {
        if (xQueueSend(g_pushQ, &item, 0) != pdTRUE) {
          ESP_LOGW(TAG, "PushQ full, enqueue to NVS");
          nvsQEnqueue(item.payload, item.len);
        } else {
          g_ledNet.flashTrig = true;
        }
      }
    }
    // 每秒 check UDH 超时
    if (xTaskGetTickCount() - last > pdMS_TO_TICKS(1000)) {
      check_udh_timeouts();
      last = xTaskGetTickCount();
    }
  }
}

// =================== LED ===================
static void led_set_level(gpio_num_t g, int level) {
  gpio_set_level(g, level == (LED_ACTIVE_LEVEL == HIGH ? 1 : 0) ? 1 : 0);
}

static void led_update_one(gpio_num_t g, LedState& s) {
  s.counter++;
  bool on = false;
  switch (s.s) {
    case LED_OFF:          on = false; break;
    case LED_ON:           on = true; break;
    case LED_BLINK_SLOW:   on = (s.counter / 5) & 1; break;  // ~100ms 周期
    case LED_BLINK_FAST:   on = (s.counter / 2) & 1; break;
  }
  if (s.flashTrig) { on = true; s.flashTrig = false; s.trig = s.counter; }
  if (s.trig && (s.counter - s.trig) < 5) on = true;
  else s.trig = 0;
  if (on != s.last) {
    led_set_level(g, on ? 1 : 0);
    s.last = on;
  }
}

static void led_task(void* /*param*/) {
  for (gpio_num_t g : { (gpio_num_t)LED_4G_GPIO, (gpio_num_t)LED_WIFI_GPIO, (gpio_num_t)LED_NET_GPIO }) {
    gpio_config_t io = { .pin_bit_mask = (1ULL << g),
                         .mode = GPIO_MODE_OUTPUT,
                         .pull_up_en = GPIO_PULLUP_DISABLE,
                         .pull_down_en = GPIO_PULLDOWN_DISABLE,
                         .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&io);
    led_set_level(g, 0);
  }
  for (;;) {
    // 4G LED: 模组 alive = 常亮, 等待中 = 慢闪
    if (g_ml.alive) g_led4g.s = LED_ON;
    else            g_led4g.s = LED_BLINK_SLOW;
    // WIFI LED: 连上 = 常亮, 等待中 = 快闪
    if (g_wifiUp) g_ledW.s = LED_ON;
    else          g_ledW.s = LED_BLINK_FAST;
    // NET LED: 推送时 flash, 否则 OFF
    // flashTrig 由 sms_task 触发
    led_update_one((gpio_num_t)LED_4G_GPIO,   g_led4g);
    led_update_one((gpio_num_t)LED_WIFI_GPIO, g_ledW);
    led_update_one((gpio_num_t)LED_NET_GPIO,  g_ledNet);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// =================== SNTP 时间同步 (v4.0.6 P11) ===================
// 客户报 "上次推 / 上次信 没法读" — millis() 上电清零, 前端只能显示 "上电后 3842 秒"
// 接 SNTP 后, g_lastSmsUtcMs / g_lastPushOkUtcMs 改记 epoch ms, 前端 toLocaleString("zh-CN")
// 显示 "2026/06/18 14:32:05"。ESP32-S3 没 RTC 电池, 每次开机 GOT_IP 都要重新同步
// 注: g_timeSynced / g_lastSmsUtcMs / g_lastPushOkUtcMs 已在文件头部声明 (排序需要)

static void sntp_sync_cb(struct timeval *tv) {
  g_timeSynced = true;
  ESP_LOGI(TAG, "SNTP synced: epoch=%lld", (long long)tv->tv_sec);
}

static bool g_sntpStarted = false;

static void sntp_start_once(void) {
  if (g_sntpStarted) return;                 // 重连时别重复 init
  setenv("TZ", "CST-8", 1);                  // Asia/Shanghai = UTC+8 (POSIX: 负数=东)
  tzset();
  // ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE 期望 const char*[CONFIG_LWIP_SNTP_MAX_SERVERS],
  // CONFIG_LWIP_SNTP_MAX_SERVERS 默认 1, 所以只放 1 个 server (够用, SNTP 失败再重试)
  esp_sntp_config_t cfg = {
    .smooth_sync = false,
    .server_from_dhcp = false,
    .wait_for_sync = true,
    .start = true,
    .sync_cb = sntp_sync_cb,
    .renew_servers_after_new_IP = false,
    .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
    .index_of_first_server = 0,
    .num_of_servers = 1,
    .servers = { "ntp.aliyun.com" },
  };
  esp_err_t err = esp_netif_sntp_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SNTP init failed: %d", err);
    return;
  }
  g_sntpStarted = true;
  ESP_LOGI(TAG, "SNTP init OK (wait ~1s for first sync)");
}

// =================== WiFi 事件 ===================
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    g_wifiUp = false;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    g_wifiUp = true;
    ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
    ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&e->ip_info.ip));
    // v4.0.6 P11: WiFi up 后立刻启 SNTP (重连时 sntp_start_once 内部短路)
    sntp_start_once();
  }
}

// =================== ping 健康 ===================
struct PingCtx { SemaphoreHandle_t done; bool ok; };
static void ping_on_end(esp_ping_handle_t hdl, void* args) {
  PingCtx* c = (PingCtx*)args;
  if (c && c->done) xSemaphoreGive(c->done);
}
static void ping_on_success(esp_ping_handle_t hdl, void* args) {
  PingCtx* c = (PingCtx*)args;
  if (c) c->ok = true;
}

static bool ping_target(const char* host, uint32_t timeout_ms) {
  ip_addr_t target;
  if (inet_aton(host, &target) == 0) {
    // 用 getaddrinfo 替代 gethostbyname (新 lwip 默认开启)
    struct addrinfo hints = {}, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    memcpy(&target, &((struct sockaddr_in*)res->ai_addr)->sin_addr, sizeof(target));
    freeaddrinfo(res);
  }
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = target;
  cfg.count       = 1;
  cfg.timeout_ms  = timeout_ms;
  cfg.interval_ms = 100;
  PingCtx ctx = { .done = xSemaphoreCreateBinary(), .ok = false };
  esp_ping_callbacks_t cbs = {
    .cb_args = &ctx,
    .on_ping_success = ping_on_success,
    .on_ping_timeout = NULL,
    .on_ping_end     = ping_on_end,
  };
  esp_ping_handle_t hdl;
  if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) {
    vSemaphoreDelete(ctx.done);
    return false;
  }
  esp_ping_start(hdl);
  xSemaphoreTake(ctx.done, pdMS_TO_TICKS(timeout_ms * 3 + 2000));
  esp_ping_delete_session(hdl);
  vSemaphoreDelete(ctx.done);
  return ctx.ok;
}

// =================== Web 服务 (Dashboard + /api/status) ===================
static AsyncWebServer* g_webServer = NULL;

static const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--info:#60a5fa;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.container{max-width:880px;margin:0 auto;padding:24px 16px 48px}
header{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:24px;gap:16px}
header h1{margin:0;font-size:22px;font-weight:600;letter-spacing:-0.01em}
header h1 .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--muted);margin-right:8px;vertical-align:middle;transition:background .3s}
header h1 .dot.ok{background:var(--accent);box-shadow:0 0 8px rgba(74,222,128,.5)}
header h1 .dot.bad{background:var(--err)}
.fw-tag{font-size:12px;padding:4px 10px;border-radius:999px;background:var(--card2);color:var(--muted);border:1px solid var(--border)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:16px;margin-bottom:24px}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;box-shadow:var(--shadow)}
.card h3{margin:0 0 12px;font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:0.06em;color:var(--muted)}
.kv{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid var(--border);font-size:13px}
.kv:last-child{border-bottom:0;padding-bottom:0}
.kv:first-child{padding-top:0}
.kv .k{color:var(--muted)}
.kv .v{font-family:ui-monospace,SFMono-Regular,"SF Mono",Menlo,monospace;font-size:13px;color:var(--text);display:flex;align-items:center;gap:6px}
.tag{display:inline-flex;align-items:center;gap:4px;padding:2px 8px;border-radius:999px;font-size:11px;font-weight:600;letter-spacing:0.02em}
.tag-ok{background:rgba(74,222,128,.12);color:var(--accent)}
.tag-bad{background:rgba(248,113,113,.12);color:var(--err)}
.tag-warn{background:rgba(251,191,36,.12);color:var(--warn)}
.tag-mute{background:var(--card2);color:var(--muted)}
.actions{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:24px;align-items:center}
.actions .restart{margin-left:auto}
@media(max-width:600px){.actions .restart{margin-left:0;width:100%;order:99}}
.btn{display:inline-flex;align-items:center;gap:6px;padding:9px 14px;border-radius:8px;font-size:13px;font-weight:500;text-decoration:none;color:var(--text);background:var(--card2);border:1px solid var(--border);transition:background .15s,border-color .15s,transform .05s;cursor:pointer;font-family:inherit}
.btn:hover{background:#2c3744;border-color:#384454}
.btn:active{transform:translateY(1px)}
.btn.primary{background:var(--accent);color:#0a1014;border-color:transparent}
.btn.primary:hover{background:#5be692}
.btn.warn{background:rgba(248,113,113,.12);color:var(--err);border-color:rgba(248,113,113,.25)}
.btn.warn:hover{background:rgba(248,113,113,.18)}
.btn.mute{background:var(--card);color:var(--muted)}
.form-card{grid-column:1/-1}
.form-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media (max-width:600px){.form-grid{grid-template-columns:1fr}}
.field{display:flex;flex-direction:column;gap:6px}
.field label{font-size:12px;color:var(--muted);font-weight:500}
.field label small{color:#5e6a7a;font-weight:400}
.field input{background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:9px 11px;color:var(--text);font-size:13px;font-family:inherit;transition:border-color .15s,box-shadow .15s}
.field input:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(74,222,128,.15)}
/* P15c: select 暗色化 (浏览器默认白底灰边丑) */
.field select{
  background:var(--bg);border:1px solid var(--border);border-radius:8px;
  padding:9px 32px 9px 11px;color:var(--text);font-size:13px;font-family:inherit;
  appearance:none;-webkit-appearance:none;-moz-appearance:none;cursor:pointer;
  background-image:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'><path fill='%238b95a5' d='M2 4l4 4 4-4z'/></svg>");
  background-repeat:no-repeat;background-position:right 10px center;
  transition:border-color .15s,box-shadow .15s;
}
.field select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(74,222,128,.15)}
.field select option{background:var(--card);color:var(--text);padding:6px}
.field select:disabled{opacity:.5;cursor:not-allowed}
.toggle-row{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:14px 16px;margin-top:16px;background:var(--card2);border:1px solid var(--border);border-radius:10px}
.toggle-row .label{display:flex;flex-direction:column;gap:2px}
.toggle-row .label .t{font-size:14px;font-weight:500;color:var(--text)}
.toggle-row .label .h{font-size:12px;color:var(--muted);line-height:1.4}
.toggle{position:relative;display:inline-block;width:50px;height:30px;flex-shrink:0}
.toggle input{opacity:0;width:0;height:0;position:absolute}
.toggle .slider{position:absolute;inset:0;background:#3a4654;border-radius:999px;transition:background .2s ease;cursor:pointer}
.toggle .slider::before{content:"";position:absolute;top:2px;left:2px;width:26px;height:26px;background:#f1f5f9;border-radius:50%;box-shadow:0 2px 4px rgba(0,0,0,.3);transition:transform .2s cubic-bezier(.4,0,.2,1)}
.toggle input:checked + .slider{background:var(--accent)}
.toggle input:checked + .slider::before{transform:translateX(20px)}
.toggle input:focus-visible + .slider{box-shadow:0 0 0 3px rgba(74,222,128,.25)}
.status-line{margin-top:10px;font-size:12px;color:var(--muted);min-height:16px}
.status-line.ok{color:var(--accent)}
.status-line.err{color:var(--err)}
.section-title{display:flex;align-items:center;justify-content:space-between;margin:8px 0 12px}
.section-title h2{margin:0;font-size:14px;font-weight:600;color:var(--text)}
.history{list-style:none;margin:0;padding:0;max-height:240px;overflow-y:auto;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px}
.history li{display:grid;grid-template-columns:130px 110px 1fr auto;gap:12px;padding:8px 4px;border-bottom:1px solid var(--border);align-items:center}
.history li:last-child{border-bottom:0}
.history .ts{color:var(--muted)}
.history .ph{color:var(--info)}
.history .bd{color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.history .st.ok{color:var(--accent)}
.history .st.bad{color:var(--err)}
.history .empty{text-align:center;color:var(--muted);padding:24px;grid-column:1/-1}
footer{margin-top:32px;text-align:center;font-size:12px;color:var(--muted)}
footer .refresh-dot{display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--accent);margin-right:6px;animation:pulse 2s ease-in-out infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}
@keyframes flash{0%{background:rgba(74,222,128,.25)}100%{background:transparent}}
.flash{animation:flash 1.5s ease-out 1}
</style></head><body>
<div class="container">
  <header id="page-head">
    <h1><span id="status-dot" class="dot"></span>SMS Forwarder</h1>
    <span class="fw-tag">FW v4.0.7</span>
  </header>
  <div class="actions" id="page-nav">
    <a class="btn" href="/dashboard">Dashboard</a>
    <a class="btn" href="/send">发送</a>
    <a class="btn" href="/stk">STK 控制台</a>
    <a class="btn" href="/config">配置</a>
    <a class="btn primary" href="/update">OTA</a>
    <a class="btn warn restart" href="javascript:doRestart()">重启</a>
  </div>

  <div class="grid">
    <div class="card"><h3>运行</h3>
      <div class="kv"><span class="k">启动次数</span><span class="v" id="boot">-</span></div>
      <div class="kv"><span class="k">开机时间</span><span class="v" id="uptime">-</span></div>
      <div class="kv"><span class="k">WiFi 信号</span><span class="v"><span id="wifi" class="tag tag-mute">off</span><span id="wifiBars" class="k"></span></span></div>
      <div class="kv"><span class="k">4G 模组</span><span class="v"><span id="ml" class="tag tag-mute">off</span></span></div>
      <div class="kv"><span class="k">4G 信号</span><span class="v" id="csqBars">-</span></div>
      <div class="kv"><span class="k">长短信拼接槽</span><span class="v" id="udh">0/4</span></div>
    </div>

    <div class="card"><h3>推送</h3>
      <div class="kv"><span class="k">成功</span><span class="v" style="color:var(--accent)" id="ok">0</span></div>
      <div class="kv"><span class="k">失败</span><span class="v" style="color:var(--err)" id="fal">0</span></div>
      <div class="kv"><span class="k">待推队列</span><span class="v" id="q">0</span></div>
      <div class="kv"><span class="k">上次推送</span><span class="v" id="lp">-</span></div>
      <div class="kv"><span class="k">上次短信</span><span class="v" id="ls">-</span></div>
    </div>

    <div class="card"><h3>系统</h3>
      <div class="kv"><span class="k">设备时间</span><span class="v" id="devTime">-</span></div>
      <div class="kv"><span class="k">时间同步</span><span class="v" id="ntp">-</span></div>
      <div class="kv"><span class="k">总内存</span><span class="v" id="heapTotal">-</span></div>
      <div class="kv"><span class="k">已使用</span><span class="v" id="heapUsed">-</span></div>
      <div class="kv"><span class="k">当前空闲</span><span class="v" id="heap">0 K</span></div>
      <div class="kv"><span class="k">启动以来最低空闲</span><span class="v" id="heapMin">0 K</span></div>
    </div>
  </div>

  <div class="card">
    <div class="section-title">
      <h2>最近发送</h2>
      <div style="display:flex;gap:8px">
        <button class="btn mute" onclick="loadHist()">刷新</button>
        <button class="btn warn" onclick="clearHist()">清空</button>
      </div>
    </div>
    <ul class="history" id="hist"><li class="empty">加载中...</li></ul>
  </div>

  <div class="card">
    <div class="section-title">
      <h2>最近接收 SMS</h2>
      <button class="btn mute" onclick="loadRecent()">刷新</button>
    </div>
    <ul class="history" id="rx"><li class="empty">暂无</li></ul>
  </div>
  </div>

  <footer><span class="refresh-dot"></span><span id="refresh-note">30s 自动刷新</span></footer>
</div>

<script>
// v4.0.7: 信号 5 格 (4G CSQ 0-31, WiFi RSSI dBm) — 通用 ASCII ▂▃▄▅█ + 数字
function csqBars(csq){  // 0=无, 1-7=1格, 8-14=2, 15-20=3, 21-26=4, 27-31=5
  if(csq<=0) return '▁▁▁▁▁';
  const n = csq>=27?5:(csq>=21?4:(csq>=15?3:(csq>=8?2:(csq>=1?1:0))));
  return '▂▃▄▅█'.slice(0,n) + '▁'.repeat(5-n);
}
function wifiBars(rssi){  // dBm 阈值: -50/-60/-70/-80/-90
  if(rssi>=0||rssi<-90) return '▁▁▁▁▁';
  const n = rssi>=-50?5:(rssi>=-60?4:(rssi>=-70?3:(rssi>=-80?2:1)));
  return '▂▃▄▅█'.slice(0,n) + '▁'.repeat(5-n);
}
// v4.0.6 P11b: 读 epoch ms, 显示真实 wall-clock ("2026/06/18 14:32:05")
function fmtUtc(epochMs) {
  if (!epochMs) return '-';
  const d = new Date(epochMs);
  const p = n => String(n).padStart(2,'0');
  return d.getFullYear()+'/'+p(d.getMonth()+1)+'/'+p(d.getDate())
    +' '+p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());
}
function ago(epochMs) {
  if (!epochMs) return '-';
  const s = Math.floor((Date.now() - epochMs) / 1000);
  if (s < 60)    return s + '秒前';
  if (s < 3600)  return Math.floor(s/60) + '分钟前';
  if (s < 86400) return Math.floor(s/3600) + '小时前';
  return Math.floor(s/86400) + '天前';
}
// v4.0.7: 开机时长 (millis → d/h/m/s)
function fmtUptime(ms){
  if(!ms && ms!==0) return '-';
  const s = Math.floor(ms/1000);
  const d = Math.floor(s/86400), h = Math.floor((s%86400)/3600),
        m = Math.floor((s%3600)/60),   sec = s%60;
  return (d>0?d+'天 ':'') + (h<10?'0':'') + h + ':' + (m<10?'0':'') + m + ':' + (sec<10?'0':'') + sec;
}
function setStatus(id, text, cls) {
  const el = document.getElementById(id);
  el.textContent = text;
  el.className = 'tag ' + (cls || 'tag-mute');
}
async function poll() {
  try {
    // v4.0.7 P1: SPA 内优先用父窗口共享的 status (1 次 fetch 服务多 frame)
    let j = (window.parent && window.parent.__spaStatus) || null;
    if (!j) {
      const r = await fetch('/api/status', {cache:'no-store'});
      if (!r.ok) return;
      j = await r.json();
    }

    document.getElementById('boot').textContent  = j.boot;
    document.getElementById('uptime').textContent = fmtUptime(j.uptimeMs);
    document.getElementById('ok').textContent    = j.pushOk;
    document.getElementById('fal').textContent   = j.pushFail;
    document.getElementById('q').textContent     = j.qLen;

    setStatus('wifi', j.wifi ? '已连' : '断开', j.wifi ? 'tag-ok' : 'tag-bad');
    // RSSI 已在 wifiBars 行展示, 删独立 rssi 行 (HTML 无 id="rssi" 会抛 null)

    setStatus('ml', j.mlAlive ? '在线' : '离线', j.mlAlive ? 'tag-ok' : 'tag-bad');

    // v4.0.7: 4G + WiFi 5 格信号 (CSQ 0-31 → dBm, RSSI → bars)
    document.getElementById('csqBars').textContent =
      (j.csq >= 0 && j.csq <= 31) ? csqBars(j.csq) + ' · ' + j.csqDbm + ' dBm' : '无信号';
    document.getElementById('wifiBars').textContent =
      (j.wifi && j.wifiRssi) ? wifiBars(j.wifiRssi) + ' ' + j.wifiRssi + ' dBm' : '-';

    // v4.0.7: SIM 信息已搬去 /stk 页, dashboard 不重复
    document.getElementById('udh').textContent = (j.udhActive||0) + '/4';
    document.getElementById('ntp').textContent = j.timeSynced ? '已同步' : '未同步';
    document.getElementById('ntp').className   = 'tag ' + (j.timeSynced ? 'tag-ok' : 'tag-warn');

    // P11b: 用 epoch ms 显示真实时间, 同步前显示 "-"
    // 修复 1970 bug: 不能 fallback 到 lastSmsMs (millis 自开机, 当 epoch 用 new Date() = 1970+uptime)
    const lp = j.lastPushOkUtc || 0;
    const ls = j.lastSmsUtc    || 0;
    const lpEl = document.getElementById('lp');
    const lsEl = document.getElementById('ls');
    lpEl.textContent = lp ? fmtUtc(lp) : '-';
    lsEl.textContent = ls ? fmtUtc(ls) : '-';

    document.getElementById('heap').textContent    = Math.round(j.freeHeap/1024) + ' K';
    document.getElementById('heapMin').textContent = Math.round(j.minFreeHeap/1024) + ' K';
    // v4.0.6 P18: 设备时间 (NTP 同步后才有意义)
    const dt = document.getElementById('devTime');
    if (j.timeSynced && j.deviceTimeMs) {
      dt.textContent = fmtUtc(j.deviceTimeMs);
      dt.style.color = 'var(--text)';
    } else {
      dt.textContent = '- 未同步';
      dt.style.color = 'var(--warn)';
    }
    if (j.heapTotal != null) {
      const total = j.heapTotal, used = j.heapUsed, free = j.freeHeap;
      const pct = total ? Math.round(used*100/total) : 0;
      document.getElementById('heapTotal').textContent = Math.round(total/1024) + ' K';
      document.getElementById('heapUsed').textContent  = Math.round(used/1024) + ' K (' + pct + '%)';
    }

    const dot = document.getElementById('status-dot');
    dot.className = 'dot ' + (j.wifi ? 'ok' : 'bad');
  } catch (e) { console.warn('poll failed', e); }
}

async function loadHist() {
  const ul = document.getElementById('hist');
  try {
    const r = await fetch('/api/sent', {cache:'no-store'});
    const arr = await r.json();
    if (!Array.isArray(arr) || !arr.length) {
      ul.innerHTML = '<li class="empty">暂无发送记录</li>';
      return;
    }
    ul.innerHTML = arr.slice(-32).reverse().map(it => {
      const ok = it.ok ? '<span class="st ok">OK</span>' : '<span class="st bad">FAIL</span>';
      const ts = it.ts ? fmtUtc(it.ts) : '-';
      return '<li>'
        + '<span class="ts">' + ts + '</span>'
        + '<span class="ph">' + (it.phone||'-') + '</span>'
        + '<span class="bd">' + (it.body||'').replace(/[<>]/g,'') + '</span>'
        + ok + '</li>';
    }).join('');
  } catch (e) {
    ul.innerHTML = '<li class="empty">加载失败</li>';
  }
}

function doRestart() {
  if (!confirm('确认重启设备?')) return;
  fetch('/restart').catch(()=>{});
  alert('重启中… 约 5s 后回来');
}

// v4.0.7: 最近接收 SMS (从 /api/recent 拿, dashboard 主页显示, 5s 轮询)
// v4.0.7 P3: 新增条目闪烁动画 — 比较 hash 检测新增
let _recentHash = '';
async function loadRecent(){
  const ul = document.getElementById('rx');
  try {
    const r = await fetch('/api/recent', {cache:'no-store'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    const j = await r.json();
    if (!j.items || !j.items.length) {
      ul.innerHTML = '<li class="empty">暂无接收记录</li>';
      _recentHash = '';
      return;
    }
    const newHash = j.items.map(it => it.id || (it.tsMs||'')+it.body).join('|');
    const isNew = _recentHash && newHash !== _recentHash;
    ul.innerHTML = j.items.map((it, i) => {
      const ago = agoShort(it.ageMs);
      const preview = it.body.length > 60 ? it.body.slice(0,60)+'…' : it.body;
      const flashCls = (isNew && i === 0) ? ' class="flash"' : '';
      return `<li${flashCls}><span class="ts">${ago}</span><span class="ph">${escapeHtml(it.phone)}</span><span class="bd" title="${escapeHtml(it.body)}">${escapeHtml(preview)}</span></li>`;
    }).join('');
    _recentHash = newHash;
  } catch (e) {
    ul.innerHTML = '<li class="empty">加载失败</li>';
  }
}
function agoShort(ms){
  if(ms < 0) return '-';
  const s = Math.floor(ms/1000);
  if(s < 60)    return s+'秒前';
  if(s < 3600)  return Math.floor(s/60)+'分前';
  if(s < 86400) return Math.floor(s/3600)+'时前';
  return Math.floor(s/86400)+'天前';
}
function escapeHtml(s){ return String(s||'').replace(/[&<>"']/g, c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }

async function clearHist() {
  if (!confirm('确认清空所有最近发送记录?')) return;
  try {
    const r = await fetch('/api/sent/clear', {method:'POST'});
    if (!r.ok) throw new Error('HTTP '+r.status);
    loadHist();
  } catch (e) {
    alert('清空失败: ' + e.message);
  }
}

// v4.0.6 P24: 配网表单搬去 /config 页 (dashboard 只看状态), 这里不再加载
poll(); loadHist(); loadRecent();
setInterval(poll, 30000);
setInterval(loadHist, 60000);
setInterval(loadRecent, 10000);   // v4.0.7: 最近 SMS 10s 刷一次 (快一点,翔哥看新短信)
</script><script>if(self!==top){['page-head','page-nav'].forEach(function(id){var h=document.getElementById(id); if(h)h.style.display='none';});}</script></body></html>
)HTML";

static void handleApiStatus(AsyncWebServerRequest* r) {
  Preferences p;
  p.begin("pqueue", true);
  uint8_t count = p.getUChar("count", 0);
  p.end();
  // 活跃 UDH slot 数
  int udhActive = 0;
  for (int i = 0; i < MAX_UDH_REFS; i++) if (g_udhTable[i].in_use) udhActive++;
  JsonDocument doc;
  doc["boot"]        = g_bootCount;
  doc["uptimeMs"]    = (uint32_t)(millis() - g_bootMs);
  doc["pushOk"]      = g_pushOk;
  doc["pushFail"]    = g_pushFail;
  doc["qLen"]        = count;
  doc["wifi"]        = g_wifiUp;
  doc["wifiRssi"]    = g_wifiUp ? WiFi.RSSI() : 0;
  doc["freeHeap"]    = esp_get_free_heap_size();
  doc["minFreeHeap"] = esp_get_minimum_free_heap_size();
  // v4.0.6 P13: 总内存 (internal) + 已使用
  doc["heapTotal"]   = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  doc["heapUsed"]    = (int32_t)doc["heapTotal"] - (int32_t)doc["freeHeap"];
  doc["lastSmsMs"]      = g_lastSmsMs;       // 兼容旧前端 (millis, 重启清零)
  doc["lastPushOkMs"]   = g_lastPushOkMs;
  doc["lastSmsUtc"]     = g_lastSmsUtcMs;    // v4.0.6 P11b: epoch ms uint64, 前端 toLocaleString
  doc["lastPushOkUtc"]  = g_lastPushOkUtcMs;
  doc["timeSynced"]     = g_timeSynced;      // false 时前端显示 "未同步" 提示
  // v4.0.6 P18: 当前设备时间 (SNTP 同步后才有意义)
  // P25: deviceTimeMs 用 uint64 epoch ms, 避免 uint32 截断成 1970 年
  doc["deviceTimeMs"]   = g_timeSynced ? ((uint64_t)time(NULL) * 1000ULL) : 0;
  doc["mlAlive"]     = g_ml.alive;
  doc["udhActive"]   = udhActive;
  doc["fw"]          = FW_VERSION;
  doc["bootPush"]    = g_cfg.bootPush;   // v4.0.6+: 开关
  // v4.0.7: 4G 信号 (csq_poll_task 5s 刷新)
  doc["csq"]         = (int)g_4g_csq;          // -1=未知, 0-31, 99=无效
  doc["csqDbm"]      = (int)g_4g_dbm;          // dBm (0/99 时=0)
  doc["csqAgeMs"]    = (int32_t)((g_4g_csqMs && millis() >= g_4g_csqMs) ? (millis() - g_4g_csqMs) : -1);
  // v4.0.7: SIM 信息 (portMUX 保护, 因为 stk_query 异步写)
  portENTER_CRITICAL(&g_sim_mux);
  doc["simImsi"]     = g_sim_imsi;
  doc["simIccid"]    = g_sim_iccid;
  doc["simMsisdn"]   = g_sim_msisdn;
  doc["simOperator"] = g_sim_operator;
  portEXIT_CRITICAL(&g_sim_mux);
  doc["simAgeMs"]    = (int32_t)((g_sim_queryMs && millis() >= g_sim_queryMs) ? (millis() - g_sim_queryMs) : -1);
  String out; serializeJson(doc, out);
  r->send(200, "application/json", out);
}

// =================== OTA web (BasicAuth) ===================
static const char OTA_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>OTA · SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.container{max-width:880px;margin:0 auto;padding:24px 16px 48px}
header{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:24px;gap:16px}
header h1{margin:0;font-size:22px;font-weight:600;letter-spacing:-0.01em}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px;box-shadow:var(--shadow);margin-bottom:16px}
.card h2{margin:0 0 14px;font-size:14px;font-weight:600;color:var(--text)}
.file-row{display:flex;align-items:center;gap:12px;padding:14px;background:var(--card2);border:1px dashed var(--border);border-radius:10px;margin-bottom:14px}
.file-row input[type=file]{flex:1;color:var(--text);font-size:13px;font-family:inherit}
.file-row input[type=file]::file-selector-button{background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:6px 12px;font-size:12px;cursor:pointer;font-family:inherit;margin-right:10px}
.file-row input[type=file]::file-selector-button:hover{background:#2c3744}
.actions{display:flex;gap:8px;align-items:center}
.btn{display:inline-flex;align-items:center;gap:6px;padding:10px 18px;border-radius:8px;font-size:13px;font-weight:500;text-decoration:none;color:var(--text);background:var(--card2);border:1px solid var(--border);cursor:pointer;font-family:inherit;transition:background .15s}
.btn:hover{background:#2c3744;border-color:#384454}
.btn.primary{background:var(--accent);color:#0a1014;border-color:transparent;font-weight:600}
.btn.primary:hover{background:#5be692}
.btn.primary:disabled{background:var(--muted);cursor:not-allowed}
.warn-text{color:var(--warn);font-size:12px;margin-top:10px}
.progress-wrap{margin-top:14px;background:var(--card2);border:1px solid var(--border);border-radius:8px;overflow:hidden;height:18px;position:relative}
.progress-bar{height:100%;background:var(--accent);width:0%;transition:width .2s}
.progress-text{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;font-size:11px;color:var(--text);font-family:ui-monospace,monospace}
.status-line{margin-top:14px;font-size:12px;color:var(--muted);min-height:16px}
.status-line.ok{color:var(--accent)}
.status-line.err{color:var(--err)}
footer{margin-top:24px;text-align:center;font-size:12px;color:var(--muted)}
</style></head><body>
<div class="container">
  <header id="page-head">
    <h1>OTA 升级</h1>
    <span style="font-size:12px;color:var(--muted)">选择 .bin 文件, &lt; 3MB</span>
  </header>

  <div class="card">
    <h2>上传固件</h2>
    <div class="file-row">
      <input type="file" id="fwFile" accept=".bin" required>
    </div>
    <div class="actions">
      <button class="btn primary" id="upBtn" onclick="upload()">⤴ 上传并烧录</button>
      <span class="status-line" id="upStatus" style="margin-top:0"></span>
    </div>
    <div class="progress-wrap" id="pwrap" style="display:none">
      <div class="progress-bar" id="pbar"></div>
      <div class="progress-text" id="ptext">0%</div>
    </div>
    <p class="warn-text">⚠ 烧录中请勿断电;成功后设备将自动重启,约 5s 后可用</p>
  </div>

  <footer>FW v4.0.7</footer>
</div>
<script>
async function upload() {
  const f = document.getElementById('fwFile').files[0];
  if (!f) { alert('请先选 .bin 文件'); return; }
  if (f.size > 3*1024*1024) { alert('文件太大 (>3MB)'); return; }
  const btn = document.getElementById('upBtn');
  const st  = document.getElementById('upStatus');
  const pw  = document.getElementById('pwrap');
  const pb  = document.getElementById('pbar');
  const pt  = document.getElementById('ptext');
  btn.disabled = true; btn.textContent = '上传中…';
  st.className = 'status-line'; st.textContent = '上传中…';
  pw.style.display = 'block'; pb.style.width = '0%'; pt.textContent = '0%';
  try {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/update', true);
    xhr.upload.onprogress = e => {
      if (e.lengthComputable) {
        const p = Math.round(e.loaded/e.total*100);
        pb.style.width = p + '%'; pt.textContent = p + '%';
      }
    };
    xhr.onload = () => {
      if (xhr.status === 200) {
        pb.style.width = '100%'; pt.textContent = '100%';
        st.className = 'status-line ok';
        st.textContent = '✓ 烧录成功,设备即将重启…';
        btn.textContent = '✓ 完成';
      } else {
        st.className = 'status-line err';
        st.textContent = '烧录失败: HTTP ' + xhr.status;
        btn.disabled = false; btn.textContent = '⤴ 上传并烧录';
      }
    };
    xhr.onerror = () => {
      st.className = 'status-line err';
      st.textContent = '网络错误 — 设备可能已重启,稍候刷新';
      btn.disabled = false; btn.textContent = '⤴ 上传并烧录';
    };
    const fd = new FormData();
    fd.append('firmware', f);
    xhr.send(fd);
  } catch (e) {
    st.className = 'status-line err';
    st.textContent = '失败: ' + e.message;
    btn.disabled = false; btn.textContent = '⤴ 上传并烧录';
  }
}
</script>
<script>if(self!==top){['page-head','page-nav'].forEach(function(id){var h=document.getElementById(id); if(h)h.style.display='none';});}</script>
</body></html>
)HTML";

// v4.0.7+: dashboard 鉴权暂时禁用 (Basic auth 在 SPA iframe 内 fetch 行为不一致, 用户要求先关掉)
static bool check_dashboard_auth(AsyncWebServerRequest* r) {
  return true;
}

// v4.0.7: SPA 父页 (/app) — 公共 header + nav 按钮 + iframe 内容区
// 点 nav 切换只换下方, 头部/按钮/按钮上面所有部分原封不动
// 子页 (/dashboard /send /stk /config /update) 内 nav/header 加 id="page-head",
// 子页加载时检测 self!==top → 隐藏自己的 page-head, 避免重复
static const char APP_PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--info:#60a5fa;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.app-shell{display:flex;flex-direction:column;height:100vh}
.app-header{display:flex;align-items:center;justify-content:space-between;padding:14px 24px;background:var(--card);border-bottom:1px solid var(--border);gap:16px;flex-shrink:0;max-width:880px;width:100%;margin:0 auto;box-sizing:border-box}
.app-header h1{margin:0;font-size:18px;font-weight:600;letter-spacing:-0.01em;display:flex;align-items:center;gap:10px}
.app-header h1 .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--muted);transition:background .3s}
.app-header h1 .dot.ok{background:var(--accent);box-shadow:0 0 8px rgba(74,222,128,.5)}
.app-header h1 .dot.ml{background:var(--info);box-shadow:0 0 8px rgba(96,165,250,.5)}
.app-header h1 .dot.warn{background:var(--warn);box-shadow:0 0 8px rgba(251,191,36,.5)}
.app-header h1 .dot.bad{background:var(--err)}
.app-header .fw-tag{font-size:11px;color:var(--muted);border:1px solid var(--border);padding:3px 8px;border-radius:4px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
.app-header .btn-restart{font-size:12px;color:var(--muted);text-decoration:none;padding:4px 10px;border:1px solid var(--border);border-radius:6px;transition:all .15s;cursor:pointer;font-family:inherit;background:var(--card2)}
.app-header .btn-restart:hover{color:var(--err);border-color:rgba(248,113,113,.4);background:rgba(248,113,113,.1)}
.app-nav{display:flex;gap:8px;flex-wrap:wrap;padding:12px 24px;background:var(--card);border-bottom:1px solid var(--border);flex-shrink:0;max-width:880px;width:100%;margin:0 auto;box-sizing:border-box;position:relative}
.app-nav .burger{display:none;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:8px;padding:9px 12px;font-size:18px;cursor:pointer;font-family:inherit;line-height:1}
.app-nav .btn{display:inline-flex;align-items:center;gap:6px;padding:9px 14px;border-radius:8px;font-size:13px;font-weight:500;text-decoration:none;color:var(--text);background:var(--card2);border:1px solid var(--border);transition:background .15s;cursor:pointer;font-family:inherit}
.app-nav .btn:hover{background:#2c3744;border-color:#384454}
.app-nav .btn.primary{background:var(--accent);color:#0a1014;border-color:transparent}
.app-nav .btn.primary:hover{background:#5be692}
.app-nav .btn.warn{background:rgba(248,113,113,.12);color:var(--err);border-color:rgba(248,113,113,.25)}
.app-content{flex:1;overflow:auto;background:var(--bg)}
.app-content iframe{width:100%;height:100%;border:0;display:block;background:var(--bg)}
@media (max-width: 640px){
  .app-nav{padding:8px 16px}
  .app-nav .burger{display:inline-flex}
  .app-nav .menu{display:none;flex-direction:column;width:100%;gap:6px;margin-top:8px}
  .app-nav.open .menu{display:flex}
  .app-nav .btn{width:100%;justify-content:flex-start}
}
</style></head><body>
<div class="app-shell">
  <div class="app-header">
    <h1><span id="status-dot" class="dot"></span>SMS Forwarder</h1>
    <div style="display:flex;align-items:center;gap:10px">
      <span class="fw-tag">FW v4.0.7</span>
      <a href="javascript:doRestart()" class="btn-restart" title="重启设备">↻ 重启</a>
    </div>
  </div>
  <nav class="app-nav" id="app-nav">
    <button class="burger" id="navBurger" onclick="document.getElementById('app-nav').classList.toggle('open')">☰</button>
    <div class="menu" id="navMenu">
      <a href="#" class="btn" data-page="dashboard">Dashboard</a>
      <a href="#" class="btn" data-page="send">发送</a>
      <a href="#" class="btn" data-page="stk">STK 控制台</a>
      <a href="#" class="btn" data-page="config">配置</a>
      <a href="#" class="btn" data-page="update">OTA</a>
    </div>
  </nav>
  <div class="app-content">
    <iframe id="page" src=""></iframe>
  </div>
</div>
<script>
const PAGES = {dashboard:'/dashboard', send:'/send', stk:'/stk', config:'/config', update:'/update'};
function setActive(p){
  document.querySelectorAll('#app-nav .btn').forEach(a=>{
    a.classList.toggle('primary', a.dataset.page === p);
  });
  // 选完自动收菜单 (移动端)
  document.getElementById('app-nav').classList.remove('open');
}
function load(p){
  if (!PAGES[p]) return;
  setActive(p);
  document.getElementById('page').src = PAGES[p];
  try { history.replaceState(null,'','/app?p='+p); } catch(e){}
}
document.querySelectorAll('#app-nav .btn').forEach(a=>{
  a.addEventListener('click', e=>{ e.preventDefault(); load(a.dataset.page); });
});
// 启动时: ?p=xxx → load, 默认 dashboard
let init = 'dashboard';
try {
  const qs = new URLSearchParams(location.search);
  if (qs.get('p') && PAGES[qs.get('p')]) init = qs.get('p');
} catch(e){}
load(init);

// 状态点: 拉 /api/status 决定 dot 颜色 (跟 dashboard 同步, 不影响子页自己 polling)
// v4.0.7 P1: SPA 父页 fetch /api/status 一次 → 挂到 window.__spaStatus, iframe 子页共用
async function pingStatus(){
  try{
    const r = await fetch('/api/status',{cache:'no-store'});
    if(!r.ok) return;
    const j = await r.json();
    window.__spaStatus = j;       // 暴露给 iframe 子页读
    window.__spaStatusAt = Date.now();
    const d = document.getElementById('status-dot');
    let cls = 'bad';
    if (j.wifi && j.mlAlive) cls = 'ok';          // 全绿
    else if (j.mlAlive)      cls = 'ml';          // 4G 有但 WiFi 没 → 蓝 (AP 模式)
    else if (j.wifi)         cls = 'warn';        // WiFi 有但 4G 没 → 橙
    d.className = 'dot ' + cls;
    d.title = (j.wifi?'WiFi 已连':(j.mlAlive?'AP 模式 (无 WiFi)':'离线'))
            + (j.mlAlive?' · 4G 在线':' · 4G 离线');
  }catch(e){}
}
pingStatus();
setInterval(pingStatus, 5000);
// 子页 fetch 替换函数: 同源同 frame 优先用 SPA 共享状态 (1 次 fetch 服务 5 个 iframe)
async function spaFetch(path, opts){
  // SPA 父页直接 fetch
  const r = await fetch(path, opts);
  return r;
}
// v4.0.7+: 重启按钮 (header 右上, 不在 nav 里因为是动作不是切换)
function doRestart(){
  if (!confirm('确认重启设备?\n\n设备会断网约 5s')) return;
  fetch('/restart').catch(()=>{});
  document.body.style.opacity='0.4';
  document.body.style.pointerEvents='none';
  setTimeout(()=>{ alert('重启中… 5s 后请刷新 http://172.16.1.18/'); location.reload(); }, 1500);
}
</script>
</body></html>
)HTML";

static void handleAppPage(AsyncWebServerRequest* r) {
  r->send_P(200, "text/html; charset=utf-8", APP_PAGE_HTML);
}

// 各子页 SPA 自隐藏脚本 (统一一段, 5 个页面共用)
// 检测 self!==top → 隐藏 id="page-head" (标题头部) + id="page-nav" (导航条)
// 由调用方在子页 body 末尾插入
static const char SPA_HIDE_SCRIPT[] PROGMEM =
  "<script>if(self!==top){['page-head','page-nav'].forEach(function(id){"
  "var h=document.getElementById(id); if(h)h.style.display='none';});}</script>";

static void handle_ota_page(AsyncWebServerRequest* r) {
  // 顶层访问 → redirect SPA; iframe 内 → 返回完整页 (子页 script 隐藏 nav)
  String ref = r->header("Referer");
  if (ref.indexOf("/app") < 0) { r->redirect("/app?p=update"); return; }
  r->send_P(200, "text/html; charset=utf-8", OTA_HTML);
}

static void handle_ota_chunk(AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      r->send(500, "text/plain", "Update.begin failed");
      return;
    }
    ESP_LOGI(TAG, "OTA: started, file=%s", filename.c_str());
  }
  if (len > 0) Update.write(data, len);
  if (final) {
    if (Update.end(true)) {
      ESP_LOGI(TAG, "OTA: success, rebooting ...");
      delay(300);
      ESP.restart();
    } else {
      ESP_LOGE(TAG, "OTA: failed");
    }
  }
}

static void handle_ota_done(AsyncWebServerRequest* r) {
  if (Update.hasError()) r->send(500, "text/plain", "Update error");
  else                   r->send(200, "text/plain", "OK, rebooting ...");
}

static void handle_restart(AsyncWebServerRequest* r) {
  r->send(200, "text/plain", "Rebooting ...");
  delay(200);
  ESP.restart();
}

// =================== STK 控制台页面 (v4.0.7) ===================
// 风格统一 dashboard (暗色 + --bg/--card/--card2/--accent 等 CSS 变量)
// 顶部导航 + SIM 信息卡 + 手动刷新按钮 + AT 命令输入 + URC 日志区
// STK Proactive 暂停 (2026-06-19): STK_PAGE_HTML + handleStkPage 也注释, 防止 -Wunused-function
#if 0
static const char STK_PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>STK · SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--info:#60a5fa;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.container{max-width:880px;margin:0 auto;padding:24px 16px 48px}
header{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:24px;gap:16px}
header h1{margin:0;font-size:22px;font-weight:600;letter-spacing:-0.01em}
header .fw-tag{font-size:11px;color:var(--muted);border:1px solid var(--border);padding:3px 8px;border-radius:4px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
nav{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 16px}
.btn{display:inline-block;padding:6px 12px;border:1px solid var(--border);border-radius:6px;background:var(--card2);color:var(--text);text-decoration:none;font-size:13px;cursor:pointer;font-family:inherit}
.btn:hover{background:var(--border)}
.btn.primary{background:var(--accent);color:#0a1014;border-color:transparent;font-weight:600}
.btn.warn{background:rgba(248,113,113,.12);color:var(--err);border-color:rgba(248,113,113,.25)}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:16px;box-shadow:var(--shadow);margin-bottom:16px}
.card h3{margin:0 0 12px;font-size:14px;font-weight:600;color:var(--text);display:flex;align-items:center;justify-content:space-between}
.kv{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid var(--border);font-size:13px;gap:12px}
.kv:last-child{border-bottom:none}
.k{color:var(--muted);flex-shrink:0}
.v{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;color:var(--text);text-align:right;word-break:break-all}
input[type=text]{width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px 10px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:13px;margin-bottom:8px}
input[type=text]:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(74,222,128,.15)}
.log{background:#0a0e13;border:1px solid var(--border);border-radius:6px;padding:10px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;height:340px;overflow-y:auto;white-space:pre-wrap;color:var(--muted)}
.log .info{color:var(--info)}.log .cmd{color:var(--warn)}.log .urc{color:var(--accent)}
.log::-webkit-scrollbar{width:8px}.log::-webkit-scrollbar-track{background:var(--card)}.log::-webkit-scrollbar-thumb{background:var(--border);border-radius:4px}
.status-line{margin-top:8px;font-size:12px;color:var(--muted)}
.status-line.ok{color:var(--accent)}.status-line.err{color:var(--err)}
.help{font-size:12px;color:var(--muted);margin-bottom:8px}
</style></head>
<body>
<div class="container">
  <header id="page-head">
    <h1>STK 控制台</h1>
    <span class="fw-tag">v4.0.7</span>
  </header>
  <nav id="page-nav">
    <a href="/dashboard" class="btn">Dashboard</a>
    <a href="/send" class="btn">发送</a>
    <a href="/stk" class="btn primary">STK</a>
    <a href="/config" class="btn">配置</a>
    <a href="/update" class="btn">OTA</a>
  </nav>

  <div class="card">
    <h3>SIM 卡信息</h3>
    <div class="kv"><span class="k">运营商</span><span class="v" id="op">-</span></div>
    <div class="kv"><span class="k">IMSI</span><span class="v" id="imsi">-</span></div>
    <div class="kv"><span class="k">ICCID</span><span class="v" id="iccid">-</span></div>
    <div class="kv"><span class="k">本机号 (MSISDN)</span><span class="v" id="msisdn">-</span></div>
    <div class="kv"><span class="k">刷新时间</span><span class="v" id="age">-</span></div>
    <div style="margin-top:10px">
      <button class="btn primary" onclick="refresh()">立即刷新</button>
      <span class="status-line" id="ref_status"></span>
    </div>
  </div>

  <div class="card">
    <h3>手动 AT 命令</h3>
    <div class="help">允许: AT+CIMI / AT+CCID / AT+CNUM / AT+COPS / AT+STK* (STKR/STKENV/STKTR/STKPCMD/STKMENU)</div>
    <input type=text id="cmd" placeholder="AT+CIMI" value="AT+CIMI">
    <button class="btn primary" onclick="sendCmd()">发送</button>
    <span class="status-line" id="cmd_status"></span>
    <div class="log" id="cmd_reply" style="height:120px;margin-top:8px"></div>
  </div>

  <div class="card">
    <h3>SIM 卡主动菜单 (SETUP_MENU 0x25) <span class="status-line" id="menu_status" style="margin:0">等待 SIM 推送…</span></h3>
    <div id="menu_title" style="font-weight:600;margin-bottom:8px;color:var(--info)"></div>
    <ul id="menu_list" style="list-style:none;margin:0;padding:0"></ul>
  </div>

  <div class="card">
    <h3>STK URC 日志 <span class="status-line" id="log_status" style="margin:0">加载中…</span></h3>
    <div class="log" id="log"></div>
  </div>
</div>
<script>
function ageStr(sec){ if(sec<0) return '从未'; if(sec<60) return sec+'s 前'; if(sec<3600) return Math.floor(sec/60)+'m 前'; return Math.floor(sec/3600)+'h 前'; }

async function loadInfo(){
  const r = await fetch('/api/stk');
  if(!r.ok){ document.getElementById('ref_status').textContent='加载失败 HTTP '+r.status; return; }
  const j = await r.json();
  document.getElementById('op').textContent     = j.operator || '(未知)';
  document.getElementById('imsi').textContent   = j.imsi     || '(未知)';
  document.getElementById('iccid').textContent  = j.iccid    || '(未知)';
  document.getElementById('msisdn').textContent = j.msisdn   || '(未读取)';
  document.getElementById('age').textContent    = ageStr(Math.floor(j.ageMs/1000));
}

async function refresh(){
  const s = document.getElementById('ref_status');
  s.className='status-line'; s.textContent='刷新中…';
  try{
    const r = await fetch('/api/stk/refresh',{method:'POST'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    s.className='status-line ok'; s.textContent='已触发, 等 5s';
    setTimeout(loadInfo, 5000);
  }catch(e){
    s.className='status-line err'; s.textContent='失败: '+e.message;
  }
}

async function sendCmd(){
  const cmd = document.getElementById('cmd').value.trim();
  if(!cmd){ alert('命令不能空'); return; }
  const s = document.getElementById('cmd_status');
  const rep = document.getElementById('cmd_reply');
  s.className='status-line'; s.textContent='发送中…';
  try{
    const r = await fetch('/api/stk/cmd',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify({cmd})
    });
    const j = await r.json();
    s.className='status-line ' + (j.ok?'ok':'err');
    s.textContent = j.ok ? 'OK' : 'FAIL rc=' + j.rc;
    rep.textContent = j.reply || '(空)';
  }catch(e){
    s.className='status-line err'; s.textContent='失败: '+e.message;
  }
}

async function loadLog(){
  const r = await fetch('/api/stk');
  if(!r.ok) return;
  const j = await r.json();
  const log = document.getElementById('log');
  if(j.logs && j.logs.length){
    log.innerHTML = j.logs.map(e => {
      const t = new Date(e.t * 1000).toLocaleTimeString();
      const cls = e.l.startsWith('[CMD]') ? 'cmd' : (e.l.startsWith('[INFO]') ? 'info' : 'urc');
      return '<span class="'+cls+'">['+t+'] '+e.l+'</span>';
    }).join('\n');
    log.scrollTop = log.scrollHeight;
  }
  document.getElementById('log_status').textContent = '共 ' + (j.logs?j.logs.length:0) + ' 条 · 2s 自动';
}

loadInfo(); loadLog();
// v4.0.7: SIM 卡主动菜单 (SETUP_MENU 0x25 → 自动列 items, 点选 → 自动发 AT+STKR=id)
async function loadMenu(){
  const ul = document.getElementById('menu_list');
  const title = document.getElementById('menu_title');
  const status = document.getElementById('menu_status');
  try{
    const r = await fetch('/api/stk/menu');
    if(!r.ok) throw new Error('HTTP '+r.status);
    const j = await r.json();
    if(!j.count){
      title.textContent = '';
      ul.innerHTML = '<li style="padding:12px;color:var(--muted);font-size:12px">暂无菜单 (SIM 卡暂未推送 +STKPRO: 0x25)</li>';
      status.textContent = j.cmd ? 'cmd=0x'+j.cmd.toString(16)+' (非 SETUP_MENU)' : '等待 SIM 推送…';
      return;
    }
    title.textContent = j.title || '(无标题)';
    status.textContent = '共 '+j.count+' 项 · 点选自动发 AT+STKR';
    ul.innerHTML = j.items.map(it =>
      `<li style="padding:8px 0;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;gap:12px">
         <span style="flex:1"><b style="color:var(--info)">${it.id}.</b> ${escapeHtml(it.text)}</span>
         <button class="btn primary" onclick="stkSelect(${it.id})">选</button>
       </li>`
    ).join('');
  }catch(e){
    status.textContent='加载失败: '+e.message;
  }
}
async function stkSelect(id){
  // 自动填到 AT 输入框 + 发送 (与手动发 AT+STKR=id 等价, 但走 sendCmd 路径能记录日志)
  document.getElementById('cmd').value = 'AT+STKR=' + id;
  await sendCmd();
  setTimeout(loadMenu, 1500);   // 菜单可能变化, 1.5s 后重新拉
}
function escapeHtml(s){ return String(s||'').replace(/[&<>"']/g, c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }
loadInfo(); loadLog(); loadMenu();
setInterval(()=>{loadInfo();loadLog();loadMenu();}, 2000);
</script>
<script>if(self!==top){['page-head','page-nav'].forEach(function(id){var h=document.getElementById(id); if(h)h.style.display='none';});}</script>
</body></html>
)HTML";

// v4.0.7: STK (SIM ToolKit) 控制台页 — 必须放在 STK_PAGE_HTML 之后
static void handleStkPage(AsyncWebServerRequest* r) {
  // 顶层访问 → redirect SPA; iframe 内 → 返回完整页 (子页 script 隐藏 nav)
  String ref = r->header("Referer");
  if (ref.indexOf("/app") < 0) { r->redirect("/app?p=stk"); return; }
  r->send_P(200, "text/html; charset=utf-8", STK_PAGE_HTML);
}
#endif  // STK_PAGE_HTML + handleStkPage 暂停结束 (2026-06-19)

// =================== SMS 发送 Web (v4.0.6+) ===================
// 页面: 表单 + 发送结果 + 最近发送历史
static const char SEND_PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>发送 · SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.container{max-width:880px;margin:0 auto;padding:24px 16px 48px}
header{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:24px;gap:16px}
header h1{margin:0;font-size:22px;font-weight:600;letter-spacing:-0.01em}
header .fw-tag{font-size:11px;color:var(--muted);border:1px solid var(--border);padding:3px 8px;border-radius:4px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
nav{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 16px}
.btn{display:inline-flex;align-items:center;gap:6px;padding:9px 14px;border-radius:8px;font-size:13px;font-weight:500;text-decoration:none;color:var(--text);background:var(--card2);border:1px solid var(--border);transition:background .15s;cursor:pointer;font-family:inherit}
.btn:hover{background:#2c3744;border-color:#384454}
.btn.primary{background:var(--accent);color:#0a1014;border-color:transparent}
.btn.primary:hover{background:#5be692}
.btn.warn{background:rgba(248,113,113,.12);color:var(--err);border-color:rgba(248,113,113,.25)}
.btn:disabled{opacity:.5;cursor:not-allowed}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;box-shadow:var(--shadow);margin-bottom:16px}
.card h3{margin:0 0 12px;font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:0.06em;color:var(--muted)}
.field{display:flex;flex-direction:column;gap:6px;margin-bottom:12px}
.field label{font-size:12px;color:var(--muted);font-weight:500}
.field label small{color:#5e6a7a;font-weight:400}
.field input,.field textarea{background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:9px 11px;color:var(--text);font-size:13px;font-family:inherit;transition:border-color .15s,box-shadow .15s}
.field input:focus,.field textarea:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(74,222,128,.15)}
.field textarea{resize:vertical;min-height:90px}
.tag{display:inline-flex;align-items:center;gap:4px;padding:2px 8px;border-radius:999px;font-size:11px;font-weight:600}
.tag-ok{background:rgba(74,222,128,.12);color:var(--accent)}
.tag-bad{background:rgba(248,113,113,.12);color:var(--err)}
.status-line{margin-top:10px;font-size:12px;color:var(--muted);min-height:16px}
.status-line.ok{color:var(--accent)}.status-line.err{color:var(--err)}
.hist-pre{background:#0a0e13;border:1px solid var(--border);border-radius:8px;padding:12px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;max-height:320px;overflow:auto;color:var(--text);white-space:pre-wrap;word-break:break-all;margin:0}
</style></head><body>
<div class="container">
  <header id="page-head">
    <h1>📤 SMS 发送</h1>
    <span class="fw-tag">v4.0.7</span>
  </header>
  <nav id="page-nav">
    <a href="/dashboard" class="btn">Dashboard</a>
    <a href="/send" class="btn primary">发送</a>
    <a href="/stk" class="btn">STK 控制台</a>
    <a href="/config" class="btn">配置</a>
    <a href="/update" class="btn">OTA</a>
  </nav>

  <div class="card">
    <h3>发送短信</h3>
    <div class="field">
      <label>接收方手机号</label>
      <input id="ph" placeholder="13800001234 或 +8613800001234" maxlength="20">
    </div>
    <div class="field">
      <label>短信内容 <small id="cnt">(0 / 70 字符单条上限)</small></label>
      <textarea id="body" maxlength="500" oninput="document.getElementById('cnt').textContent='(' + this.value.length + ' / 70 字符单条上限)'"></textarea>
    </div>
    <button id="go" class="btn primary" onclick="send()">发送</button>
    <div id="out" class="status-line"></div>
  </div>

  <div class="card">
    <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:12px">
      <h3 style="margin:0">最近发送 (最多 32 条)</h3>
      <div style="display:flex;gap:8px">
        <button class="btn" onclick="loadHist()">刷新</button>
        <button class="btn warn" onclick="clearHist()">清空</button>
      </div>
    </div>
    <pre id="hist" class="hist-pre">加载中...</pre>
  </div>
</div>
<script>
async function send() {
  const ph = document.getElementById('ph').value.trim();
  const body = document.getElementById('body').value;
  const out = document.getElementById('out');
  const go = document.getElementById('go');
  if (!ph || !body) { out.className='status-line err'; out.textContent='手机号和内容不能空'; return; }
  go.disabled = true; out.className='status-line'; out.textContent='发送中… (最长 10s)';
  try {
    const r = await fetch('/api/send', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ phone: ph, body: body })
    });
    const j = await r.json();
    out.className = 'status-line ' + (j.ok ? 'ok' : 'err');
    let txt = (j.ok ? '✓ 发送成功' : '✗ 失败') + ' · HTTP ' + r.status;
    if (j.ok) txt += ' · ref=' + j.ref + ' · parts=' + j.parts;
    else      txt += ' · err=' + j.err + (j.code !== undefined ? ' · code=' + j.code : '');
    out.textContent = txt;
    if (j.ok) loadHist();
  } catch (e) {
    out.className='status-line err'; out.textContent='网络错误: ' + e.message;
  } finally {
    go.disabled = false;
  }
}
async function loadHist() {
  try {
    const r = await fetch('/api/sent', {cache:'no-store'});
    const j = await r.json();
    document.getElementById('hist').textContent = JSON.stringify(j, null, 2);
  } catch (e) {
    document.getElementById('hist').textContent = '加载失败: ' + e.message;
  }
}
async function clearHist() {
  if (!confirm('确认清空所有最近发送记录?')) return;
  try {
    const r = await fetch('/api/sent/clear', {method:'POST'});
    if (!r.ok) { alert('清空失败 HTTP '+r.status); return; }
    loadHist();
  } catch (e) { alert('网络错误: '+e.message); }
}
loadHist();
</script>
<script>if(self!==top){['page-head','page-nav'].forEach(function(id){var h=document.getElementById(id); if(h)h.style.display='none';});}</script>
</body></html>
)HTML";

static void handleSendPage(AsyncWebServerRequest* r) {
  // 顶层访问 → redirect SPA; iframe 内 → 返回完整页 (子页 script 隐藏 nav)
  String ref = r->header("Referer");
  if (ref.indexOf("/app") < 0) { r->redirect("/app?p=send"); return; }
  r->send_P(200, "text/html; charset=utf-8", SEND_PAGE_HTML);
}

// =================== SMS 发送 Web (v4.0.6+) ===================
// /api/send 的 body 累积 state (ESP32Async fork 没 r->body, 用 _tempObject 串 body→request handler)
struct SendApiBody {
  String json;        // 累积 body chunks
};
// 释放 _tempObject 的 helper (request handler / onDisconnect 共用)
static void free_send_body(AsyncWebServerRequest* r) {
  if (r->_tempObject) {
    delete static_cast<SendApiBody*>(r->_tempObject);
    r->_tempObject = nullptr;
  }
}
// onRequestBody callback: 在 body chunks 到时累积到 _tempObject
static void handleApiSendBody(AsyncWebServerRequest* r, uint8_t* data, size_t len,
                              size_t index, size_t /*total*/) {
  if (index == 0) {
    r->_tempObject = new SendApiBody();
    // 客户端断连 / parse error 时 request handler 不会被调, onDisconnect 兜底释放
    r->onDisconnect([r]() { free_send_body(r); });
  }
  auto* st = static_cast<SendApiBody*>(r->_tempObject);
  st->json.concat((const char*)data, len);
}

static void handleApiSend(AsyncWebServerRequest* r) {
  // P0 fix #2: 解析 JSON body (前端 fetch + Content-Type: application/json)
  // ESP32Async fork 没 r->body, body 由 handleApiSendBody 累积到 _tempObject
  String json_str;
  if (auto* st = static_cast<SendApiBody*>(r->_tempObject)) {
    json_str = std::move(st->json);
  }
  free_send_body(r);

  JsonDocument doc;
  if (json_str.length() == 0 || deserializeJson(doc, json_str) != DeserializationError::Ok) {
    r->send(400, "application/json", "{\"ok\":false,\"err\":\"invalid_json\"}");
    return;
  }
  String phone = doc["phone"] | "";
  String body  = doc["body"]  | "";
  phone.trim();
  // body 不要 trim — 用户可能想发 leading/trailing 空格

  if (phone.length() == 0) {
    r->send(400, "application/json", "{\"ok\":false,\"err\":\"phone_empty\"}");
    return;
  }
  if (body.length() == 0) {
    r->send(400, "application/json", "{\"ok\":false,\"err\":\"body_empty\"}");
    return;
  }
  // 限频临时关闭 (smsRateLimitOk() 永远 return true)
  if (false && !smsRateLimitOk()) {
    r->send(429, "application/json", "{\"ok\":false,\"err\":\"rate_limit\"}");
    return;
  }

  // UCS2 编码 + UDH 拆段
  char ucs2[1024];
  int ucs2_n = pdu::ucs2_encode(body.c_str(), ucs2, sizeof(ucs2));
  if (ucs2_n < 0) {
    nvsSentEnqueue(phone.c_str(), "[invalid_utf8]", 0, false, -1);
    r->send(400, "application/json", "{\"ok\":false,\"err\":\"invalid_utf8\"}");
    return;
  }
  int total_chars = ucs2_n / 4;
  pdu::SmsPart parts[8];
  // 单条 (≤70 char) 用 max=70, 拼接 (>70) 用 max=67
  int max_per = (total_chars > 70) ? 67 : 70;
  int segs = pdu::sms_split_for_send(total_chars, max_per, parts, 8);
  if (segs < 1) {
    nvsSentEnqueue(phone.c_str(), "[body_too_long]", 0, false, 0);
    r->send(413, "application/json", "{\"ok\":false,\"err\":\"body_too_long\"}");
    return;
  }

  // 选 ref (用 bootCount + millis() 后 8 位, 避免 +CMS ERROR 331 重复)
  uint8_t ref = (uint8_t)((g_bootCount + millis() / 1000) & 0xFF);
  if (ref == 0) ref = 1;

  // 循环发每段
  uint8_t lastRef = 0;
  int lastErr = 0;
  int seg_idx_fail = -1;
  for (int i = 0; i < segs; i++) {
    char pdu[600];
    int pdu_n = pdu::cmgs_build_pdu(phone.c_str(), body.c_str(),
                                    (segs > 1) ? i : -1,
                                    parts, segs, ref,
                                    pdu, sizeof(pdu));
    if (pdu_n < 0) {
      nvsSentEnqueue(phone.c_str(), body.c_str(), 0, false, 0);
      r->send(500, "application/json", "{\"ok\":false,\"err\":\"pdu_build\"}");
      return;
    }
    uint8_t outRef = 0;
    int outErr = 0;
    // v4.0.6 P6: 用 async 版避免饿死 usb_rx_task (同步 10s+ 阻塞 async_tcp)
    int rc = cmgs_send_pdu_async(pdu, outRef, outErr, 10000);
    if (rc != 0) {
      lastErr = outErr;
      seg_idx_fail = i;
      break;
    }
    lastRef = outRef;
  }

  if (seg_idx_fail >= 0) {
    nvsSentEnqueue(phone.c_str(), body.c_str(), 0, false, lastErr);
    char out[128];
    snprintf(out, sizeof(out), "{\"ok\":false,\"err\":\"cmgs_error\",\"code\":%d}", lastErr);
    r->send(502, "application/json", out);
    return;
  }
  // 成功
  nvsSentEnqueue(phone.c_str(), body.c_str(), lastRef, true, 0);
  g_ledNet.flashTrig = true;   // NET LED 闪一下
  char out[128];
  snprintf(out, sizeof(out), "{\"ok\":true,\"ref\":%u,\"parts\":%d}", lastRef, segs);
  r->send(200, "application/json", out);
}

static void handleApiSent(AsyncWebServerRequest* r) {
  String out;
  nvsSentList(out);
  r->send(200, "application/json", out);
}

// v4.0.6 P14: POST /api/sent/clear  清空最近发送 (BasicAuth, 改持久数据)
static void handleApiSentClear(AsyncWebServerRequest* r) {
  if (!check_dashboard_auth(r)) return;
  nvsSentClear();
  r->send(200, "application/json", "{\"ok\":true}");
}

// v4.0.6 P19: POST /api/factory  恢复出厂 (清 NVS 全分区 + 重启进 AP 模式)
// BasicAuth + 二重确认 (前端 confirm), 强操作
// 不能在 async_tcp 任务里直接 ESP.restart() — 关连接时 watchdog 会 panic
// 走一次性 detached task 延后 1.5s 后 ESP.restart
static void factory_restart_task(void* /*arg*/) {
  vTaskDelay(pdMS_TO_TICKS(1500));
  ESP_LOGW(TAG, "FACTORY RESET: ESP.restart()");
  ESP.restart();
  vTaskDelete(nullptr);  // unreachable, 防 lint
}

static void handleApiFactory(AsyncWebServerRequest* r) {
  if (!check_dashboard_auth(r)) return;
  ESP_LOGW(TAG, "FACTORY RESET triggered — will restart in 1.5s");
  r->send(200, "application/json", "{\"ok\":true,\"msg\":\"restarting\"}");
  g_factoryReset = true;
  // detached task 延后重启, 不在 async_tcp 任务里 ESP.restart
  xTaskCreate(factory_restart_task, "factory_rst", 2048, nullptr, 1, nullptr);
}

// v4.0.6 P15: GET /api/scan  扫附近 WiFi 返 [{ssid, rssi, secured}] (BasicAuth, ~3s 阻塞可接受)
static void handleApiScan(AsyncWebServerRequest* r) {
  if (!check_dashboard_auth(r)) return;
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  String cur = WiFi.SSID();
  // 按 RSSI 降序
  int idx[64];
  int cnt = (n > 64) ? 64 : n;
  for (int i = 0; i < cnt; i++) idx[i] = i;
  for (int i = 0; i < cnt; i++)
    for (int j = i + 1; j < cnt; j++)
      if (WiFi.RSSI(idx[j]) > WiFi.RSSI(idx[i])) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < cnt; i++) {
    JsonObject o = arr.add<JsonObject>();
    String ssid = WiFi.SSID(idx[i]);
    o["ssid"]    = ssid;
    o["rssi"]    = WiFi.RSSI(idx[i]);
    o["secured"] = (WiFi.encryptionType(idx[i]) != WIFI_AUTH_OPEN);
    o["current"] = (ssid == cur);
  }
  WiFi.scanDelete();
  String out; serializeJson(doc, out);
  r->send(200, "application/json", out);
}

// v4.0.6+: 开关开机推送 (调试期关掉避免打扰, 翔哥 2026-06-18 加)
// POST /api/bootPush  body: {"on": true|false}  返: {"ok":true,"on":...}
static void handleApiBootPush(AsyncWebServerRequest* r, uint8_t* data, size_t len,
                              size_t index, size_t /*total*/) {
  // 不走 _tempObject (一次性小 JSON, 走 onRequest 也行, 简单起见走 onBody)
  if (index != 0 || len == 0) return;  // 只处理首 chunk
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
    r->send(400, "application/json", "{\"ok\":false,\"err\":\"invalid_json\"}");
    return;
  }
  if (!doc["on"].is<bool>()) {
    r->send(400, "application/json", "{\"ok\":false,\"err\":\"missing_on\"}");
    return;
  }
  bool on = doc["on"].as<bool>();
  g_cfg.bootPush = on;
  saveConfig(g_cfg);
  ESP_LOGI(TAG, "bootPush set to %d", on);
  char out[64];
  snprintf(out, sizeof(out), "{\"ok\":true,\"on\":%s}", on ? "true" : "false");
  r->send(200, "application/json", out);
}

// v4.0.7 调试: 清 otaPass (绕过 auth 锁住; 用户重新在 /config 设密码)
// — 已撤回, 未经用户明确同意不烧此 endpoint —

// v4.0.6+: dashboard 配网编辑 (翔哥 2026-06-18 加, 改完需重启生效)
// GET  /api/cfg -> {ssid, hasPass, token, topic, otaUser, hasOtaPass, bootPush}
// POST /api/cfg body: {ssid?, pass?, token?, topic?, otaUser?, otaPass?, bootPush?}
//   只改提供的字段, 缺省保持现有值. pass/otaPass 留空表示不改
static void handleApiCfgGet(AsyncWebServerRequest* r) {
  JsonDocument doc;
  doc["ssid"]        = g_cfg.ssid;
  doc["hasPass"]     = strlen(g_cfg.pass) > 0;
  doc["token"]       = g_cfg.token;
  doc["topic"]       = g_cfg.topic;
  doc["otaUser"]     = g_cfg.otaUser;
  doc["hasOtaPass"]  = strlen(g_cfg.otaPass) > 0;
  doc["bootPush"]    = g_cfg.bootPush;
  String out; serializeJson(doc, out);
  r->send(200, "application/json", out);
}

static void handleApiCfgPost(AsyncWebServerRequest* r, uint8_t* data, size_t len,
                             size_t index, size_t /*total*/) {
  if (index != 0 || len == 0) return;
  JsonDocument doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
    r->send(400, "application/json", "{\"ok\":false,\"err\":\"invalid_json\"}");
    return;
  }
  bool changed = false;
  if (doc["ssid"].is<const char*>()) {
    strncpy(g_cfg.ssid, doc["ssid"].as<const char*>(), sizeof(g_cfg.ssid) - 1);
    g_cfg.ssid[sizeof(g_cfg.ssid) - 1] = 0;
    changed = true;
  }
  if (doc["pass"].is<const char*>()) {
    const char* p = doc["pass"].as<const char*>();
    if (strlen(p) > 0) {  // 留空不改
      strncpy(g_cfg.pass, p, sizeof(g_cfg.pass) - 1);
      g_cfg.pass[sizeof(g_cfg.pass) - 1] = 0;
      changed = true;
    }
  }
  if (doc["token"].is<const char*>()) {
    strncpy(g_cfg.token, doc["token"].as<const char*>(), sizeof(g_cfg.token) - 1);
    g_cfg.token[sizeof(g_cfg.token) - 1] = 0;
    changed = true;
  }
  if (doc["topic"].is<const char*>()) {
    strncpy(g_cfg.topic, doc["topic"].as<const char*>(), sizeof(g_cfg.topic) - 1);
    g_cfg.topic[sizeof(g_cfg.topic) - 1] = 0;
    changed = true;
  }
  if (doc["otaUser"].is<const char*>()) {
    strncpy(g_cfg.otaUser, doc["otaUser"].as<const char*>(), sizeof(g_cfg.otaUser) - 1);
    g_cfg.otaUser[sizeof(g_cfg.otaUser) - 1] = 0;
    changed = true;
  }
  if (doc["otaPass"].is<const char*>()) {
    const char* p = doc["otaPass"].as<const char*>();
    if (strlen(p) > 0) {
      strncpy(g_cfg.otaPass, p, sizeof(g_cfg.otaPass) - 1);
      g_cfg.otaPass[sizeof(g_cfg.otaPass) - 1] = 0;
      changed = true;
    }
  }
  if (doc["bootPush"].is<bool>()) {
    g_cfg.bootPush = doc["bootPush"].as<bool>();
    changed = true;
  }
  if (changed) {
    saveConfig(g_cfg);
    ESP_LOGI(TAG, "Config updated via API, restart required for wifi/token to take effect");
  }
  r->send(200, "application/json", "{\"ok\":true,\"restart\":true}");
}

// =================== NetTask: 推送 + NVS drain + ping ===================
static void net_task(void* /*param*/) {
  uint32_t lastDrain = 0;
  uint32_t lastPing  = 0;
  for (;;) {
    PushItem item;
    if (xQueueReceive(g_pushQ, &item, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (ap_mode_get()) continue;
      if (post_pushplus_raw(item.payload, item.len)) {
        push_success_inc();
      } else {
        __atomic_add_fetch(&g_pushFail, 1, __ATOMIC_RELAXED);
        nvsQEnqueue(item.payload, item.len);
      }
    }
    if (millis() - lastDrain > 30000) {
      nvsQDrain();
      lastDrain = millis();
    }
    if (millis() - lastPing > (g_wifiUp ? 5000 : 30000)) {
      if (g_wifiUp) {
        ping_target("223.5.5.5", 2000);
      }
      lastPing = millis();
    }
  }
}

// =================== AP 配网 ===================
static AsyncWebServer* g_apServer = NULL;
static DNSServer*      g_apDns    = NULL;

// v4.0.6 P19: 危险操作页 (BasicAuth 保护, 配网表单已并回 dashboard)
// 主要: 恢复出厂清 NVS
static const char CONFIG_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>配置 · SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.container{max-width:880px;margin:0 auto;padding:24px 16px 48px}
header{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:24px;gap:16px}
header h1{margin:0;font-size:22px;font-weight:600;letter-spacing:-0.01em}
header a{color:var(--muted);text-decoration:none;font-size:13px;padding:6px 10px;border-radius:6px;border:1px solid var(--border)}
header a:hover{color:var(--text);background:var(--card2)}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px;box-shadow:var(--shadow);margin-bottom:16px}
.card h2{margin:0 0 14px;font-size:14px;font-weight:600;color:var(--text)}
.card.danger{border-color:rgba(248,113,113,.4)}
.card.danger h2{color:var(--err)}
.danger-row{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:14px;background:var(--card2);border:1px solid rgba(248,113,113,.3);border-radius:10px}
.danger-row .label{display:flex;flex-direction:column;gap:4px}
.danger-row .label .t{font-size:14px;font-weight:500;color:var(--text)}
.danger-row .label .h{font-size:12px;color:var(--muted);line-height:1.4}
.btn{display:inline-flex;align-items:center;gap:6px;padding:10px 18px;border-radius:8px;font-size:13px;font-weight:500;text-decoration:none;color:var(--text);background:var(--card2);border:1px solid var(--border);cursor:pointer;font-family:inherit;transition:background .15s}
.btn:hover{background:#2c3744;border-color:#384454}
.btn.danger{background:rgba(248,113,113,.12);color:var(--err);border-color:rgba(248,113,113,.3)}
.btn.danger:hover{background:rgba(248,113,113,.2);border-color:rgba(248,113,113,.5)}
.status-line{margin-top:14px;font-size:12px;color:var(--muted);min-height:16px}
.status-line.ok{color:var(--accent)}
.status-line.err{color:var(--err)}
footer{margin-top:24px;text-align:center;font-size:12px;color:var(--muted)}
</style></head><body>
<div class="container">
  <header id="page-head">
    <h1>配置</h1>
    <a href="/dashboard">← 返回状态页</a>
  </header>

  <div class="card">
    <h2>配置 (WiFi / 推送 / OTA)</h2>
    <div style="display:flex;gap:8px;margin-bottom:14px;align-items:center">
      <button class="btn mute" onclick="loadCfg()">加载</button>
      <button class="btn primary" onclick="saveCfg()">保存</button>
      <span id="c_dirty" style="font-size:12px;color:var(--warn);display:none">● 未保存</span>
    </div>

    <div style="display:grid;grid-template-columns:1fr 1fr;gap:12px 16px">
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">WiFi SSID <small id="c_curSsid" style="color:var(--accent)"></small></label>
        <div style="display:flex;gap:8px">
          <select id="c_ssidSel" style="flex:1;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px"></select>
          <button class="btn mute" type="button" onclick="scanWifi()" id="c_scanBtn" style="padding:6px 12px;font-size:12px">🔍 扫描</button>
        </div>
        <input id="c_ssid" maxlength="63" style="margin-top:6px;width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box" placeholder="或手动输入 SSID">
        <small id="c_scanHint" style="color:var(--muted);font-size:11px;min-height:14px;display:block;margin-top:2px"></small>
      </div>
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">WiFi 密码 <small id="c_hasPass"></small></label>
        <input id="c_pass" type="password" maxlength="63" style="width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box" placeholder="留空不修改">
      </div>
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">pushplus token</label>
        <input id="c_token" maxlength="63" style="width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box">
      </div>
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">pushplus topic <small style="color:var(--muted)">(空 = 个人推送)</small></label>
        <input id="c_topic" maxlength="63" style="width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box">
      </div>
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">OTA 用户名</label>
        <input id="c_otaUser" maxlength="31" style="width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box">
      </div>
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">OTA 密码 <small id="c_hasOtaPass"></small></label>
        <input id="c_otaPass" type="password" maxlength="31" style="width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box" placeholder="留空不修改">
      </div>
    </div>

    <div style="display:flex;align-items:center;justify-content:space-between;margin-top:14px;padding:12px;background:var(--card2);border:1px solid var(--border);border-radius:8px;gap:12px">
      <div>
        <div style="font-size:13px;font-weight:500">开机推送</div>
        <div style="font-size:11px;color:var(--muted);margin-top:2px">关闭后启动时不再推送通知;短信转发不受影响</div>
      </div>
      <label style="position:relative;display:inline-block;width:46px;height:26px;flex-shrink:0">
        <input type="checkbox" id="c_bp" onchange="toggleBp(this.checked)" style="opacity:0;width:0;height:0;position:absolute">
        <span style="position:absolute;inset:0;background:#3a4654;border-radius:999px;transition:background .2s;cursor:pointer" onclick="document.getElementById('c_bp').click()"></span>
        <span style="position:absolute;top:2px;left:2px;width:22px;height:22px;background:#f1f5f9;border-radius:50%;box-shadow:0 2px 4px rgba(0,0,0,.3);transition:transform .2s" id="c_bpKnob"></span>
      </label>
    </div>

    <div id="c_status" class="status-line"></div>
    <div style="margin-top:8px;font-size:12px;color:var(--muted)">保存后 WiFi / token 改动需重启生效</div>
  </div>

  <div class="card danger">
    <h2>危险操作</h2>
    <div class="danger-row">
      <div class="label">
        <span class="t">恢复出厂设置</span>
        <span class="h">擦除整个 NVS 分区 (WiFi / Token / OTA / 历史), 设备重启进入 AP 配网模式</span>
      </div>
      <button class="btn danger" onclick="doFactory()">⚠ 恢复出厂</button>
    </div>
    <div id="f_status" class="status-line"></div>
  </div>

  <footer>FW v4.0.6</footer>
</div>
<script>
async function doFactory() {
  if (!confirm('⚠ 真的要恢复出厂?\\n\\n将擦除:\\n  · WiFi 凭据\\n  · pushplus token\\n  · OTA 用户密码\\n  · 开机推送设置\\n  · 最近 32 条发送历史\\n\\n设备将重启并进入 AP 配网模式。\\n\\n此操作不可撤销,确认继续?')) return;
  if (!confirm('再次确认: 真的要恢复出厂?')) return;
  const s = document.getElementById('f_status');
  s.className = 'status-line'; s.textContent = '正在擦除 NVS, 设备约 3s 后重启…';
  try {
    const r = await fetch('/api/factory', {method:'POST'});
    if (!r.ok) throw new Error('HTTP '+r.status);
    s.className = 'status-line ok';
    s.textContent = '已发送重启指令, 设备将进入 AP 模式 (热点 SMS-Forwarder-XXXXXX)';
  } catch (e) {
    s.className = 'status-line err';
    s.textContent = '失败: ' + e.message + ' — 设备可能已重启, 刷新试试';
  }
}
// v4.0.6 P24: 配网表单从 dashboard 搬来 (/config 页)
async function loadCfg() {
  const s = document.getElementById('c_status');
  s.className = 'status-line'; s.textContent = '加载中…';
  try {
    const r = await fetch('/api/cfg');
    if (!r.ok) throw new Error('HTTP '+r.status);
    const j = await r.json();
    document.getElementById('c_ssid').value    = j.ssid || '';
    document.getElementById('c_ssidSel').innerHTML = '<option value="">— 选择扫描到的网络 —</option>';
    document.getElementById('c_token').value   = j.token || '';
    document.getElementById('c_topic').value   = j.topic || '';
    document.getElementById('c_otaUser').value = j.otaUser || '';
    document.getElementById('c_hasPass').textContent    = j.hasPass    ? '(已设置)' : '(未设置)';
    document.getElementById('c_hasOtaPass').textContent = j.hasOtaPass ? '(已设置)' : '(未设置)';
    document.getElementById('c_pass').value    = '';
    document.getElementById('c_otaPass').value = '';
    document.getElementById('c_bp').checked    = !!j.bootPush;
    updateBpKnob();
    if (j.ssid) document.getElementById('c_curSsid').textContent = '当前: ' + j.ssid;
    s.className = 'status-line ok'; s.textContent = '已加载';
  } catch (e) {
    s.className = 'status-line err'; s.textContent = '加载失败: ' + e.message;
  }
  markClean();
}
// v4.0.7 P2: dirty 检测 — 表单变化显示"未保存"
function markDirty(){
  const d = document.getElementById('c_dirty');
  if (d) d.style.display = 'inline';
}
function markClean(){
  const d = document.getElementById('c_dirty');
  if (d) d.style.display = 'none';
}
['c_ssid','c_pass','c_token','c_topic','c_otaUser','c_otaPass'].forEach(id=>{
  const el = document.getElementById(id);
  if (el) el.addEventListener('input', markDirty);
});
document.getElementById('c_bp').addEventListener('change', markDirty);
// v4.0.7 P2: status-line 变 err 时自动滚到视口
new MutationObserver(records => {
  records.forEach(r => {
    const el = r.target;
    if (el.classList && el.classList.contains('err')) {
      el.scrollIntoView({behavior:'smooth', block:'center'});
    }
  });
}).observe(document.body, {subtree:true, attributes:true, attributeFilter:['class']});
async function saveCfg() {
  const s = document.getElementById('c_status');
  const body = {
    ssid:    document.getElementById('c_ssid').value.trim(),
    token:   document.getElementById('c_token').value.trim(),
    topic:   document.getElementById('c_topic').value.trim(),
    otaUser: document.getElementById('c_otaUser').value.trim(),
    bootPush:document.getElementById('c_bp').checked,
  };
  const pw  = document.getElementById('c_pass').value;
  const opw = document.getElementById('c_otaPass').value;
  if (pw)  body.pass    = pw;
  if (opw) body.otaPass = opw;
  s.className = 'status-line'; s.textContent = '保存中…';
  try {
    const r = await fetch('/api/cfg', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify(body)
    });
    const j = await r.json();
    if (!r.ok || !j.ok) throw new Error('HTTP '+r.status);
    s.className = 'status-line ok';
    s.textContent = '已保存' + (j.restart ? ' · WiFi / token 改动需重启' : '');
    document.getElementById('c_pass').value    = '';
    document.getElementById('c_otaPass').value = '';
    markClean();
  } catch (e) {
    s.className = 'status-line err'; s.textContent = '保存失败: ' + e.message;
  }
}
async function scanWifi() {
  const btn = document.getElementById('c_scanBtn');
  const hint = document.getElementById('c_scanHint');
  const sel = document.getElementById('c_ssidSel');
  btn.disabled = true; btn.textContent = '…扫描中';
  hint.style.color = 'var(--muted)';
  hint.textContent = '扫描中 (~3s, 阻塞)';
  try {
    const r = await fetch('/api/scan');
    if (!r.ok) throw new Error('HTTP '+r.status);
    const arr = await r.json();
    const cur = document.getElementById('c_ssid').value;
    sel.innerHTML = '<option value="">— 选择扫描到的网络 —</option>'
      + arr.map(n => {
        const tag = n.current ? ' ✓ 已连' : (n.secured ? ' 🔒' : ' 开放');
        return '<option value="' + n.ssid.replace(/"/g,'&quot;') + '"' + (n.ssid===cur?' selected':'') + '>'
          + n.ssid + ' · ' + n.rssi + ' dBm' + tag + '</option>';
      }).join('');
    sel.onchange = () => { if (sel.value) document.getElementById('c_ssid').value = sel.value; };
    hint.style.color = 'var(--accent)';
    hint.textContent = '找到 ' + arr.length + ' 个网络 (按信号强度排序)';
  } catch (e) {
    hint.style.color = 'var(--err)';
    hint.textContent = '扫描失败: ' + e.message;
  } finally {
    btn.disabled = false; btn.textContent = '🔍 扫描';
  }
}
async function toggleBp(on) {
  updateBpKnob();
  try {
    const r = await fetch('/api/bootPush', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({on: on})
    });
    if (!r.ok) { alert('保存失败'); loadCfg(); return; }
  } catch (e) {
    alert('网络错误: ' + e.message); loadCfg();
  }
}
function updateBpKnob() {
  const on = document.getElementById('c_bp').checked;
  const track = document.querySelector('#c_bp').parentElement.children[1];
  const knob  = document.querySelector('#c_bp').parentElement.children[2];
  track.style.background = on ? 'var(--accent)' : '#3a4654';
  knob.style.transform   = on ? 'translateX(20px)' : 'none';
}
loadCfg();
</script><script>if(self!==top){['page-head','page-nav'].forEach(function(id){var h=document.getElementById(id); if(h)h.style.display='none';});}</script></body></html>
)HTML";

static void handleConfigPage(AsyncWebServerRequest* r) {
  // 顶层访问 → redirect SPA
  // iframe 内 → 返回完整页 (写操作 /api/cfg 仍有 auth, GET 无需)
  String ref = r->header("Referer");
  if (ref.indexOf("/app") < 0) { r->redirect("/app?p=config"); return; }
  r->send_P(200, "text/html; charset=utf-8", CONFIG_HTML);
}

// =================== Web 路由 (AP + STA 共用) ===================
// v4.0.6 P-merge: AP 模式也跑 dashboard, 凭这套路由同时挂到 g_webServer 和 g_apServer
// auth 由 check_dashboard_auth 内部根据 g_cfg.otaUser/otaPass 决定 → AP 空 NVS 时是 no-op
// 调用方负责注册 STA 专属的 /api/factory 和 /api/scan (AP 模式没意义)
static void register_web_routes(AsyncWebServer* srv) {
  srv->on("/api/status",       HTTP_GET,  handleApiStatus);
  srv->on("/update",           HTTP_GET,  handle_ota_page);
  srv->on("/update",           HTTP_POST, handle_ota_done,
    [](AsyncWebServerRequest* r, const String& fn, size_t idx, uint8_t* d, size_t l, bool fin) {
      handle_ota_chunk(r, fn, idx, d, l, fin);
    });
  srv->on("/restart",          HTTP_GET,  handle_restart);
  srv->on("/send",             HTTP_GET,  handleSendPage);
  srv->on("/config",           HTTP_GET,  handleConfigPage);
  // srv->on("/stk",              HTTP_GET,  handleStkPage);   // STK Proactive 暂停 (2026-06-19)
  srv->on("/api/send",         HTTP_POST, handleApiSend, nullptr, handleApiSendBody);
  srv->on("/api/sent",         HTTP_GET,  handleApiSent);
  srv->on("/api/sent/clear",   HTTP_POST, handleApiSentClear);
  // v4.0.7: STK (SIM ToolKit) API — 全部暂停 (2026-06-19)
  // srv->on("/api/stk",          HTTP_GET,  handleApiStkInfo);
  // srv->on("/api/stk/refresh",  HTTP_POST,
  //   [](AsyncWebServerRequest* r) { if (!check_dashboard_auth(r)) return; handleApiStkRefresh(r); });
  // srv->on("/api/stk/cmd",      HTTP_POST,
  //   [](AsyncWebServerRequest* r) { if (!check_dashboard_auth(r)) return; },
  //   nullptr,
  //   handleApiStkCmd);
  // v4.0.7: 最近收到 SMS 列表 + uptime (dashboard 显示, 不需 auth — 只读)
  srv->on("/api/recent",       HTTP_GET,  handleApiRecent);
  // v4.0.7: STK 当前菜单 (从最近 +STKPRO SETUP_MENU URC 解析, 只读无 auth) — 暂停 (2026-06-19)
  // srv->on("/api/stk/menu",     HTTP_GET,  handleApiStkMenu);
  srv->on("/api/bootPush",     HTTP_POST,
    [](AsyncWebServerRequest* r) { if (!check_dashboard_auth(r)) return; },
    nullptr,
    handleApiBootPush);
  // GET 只读无 auth (SPA iframe 同源 fetch 不带 Authorization), POST 写保护
  srv->on("/api/cfg",          HTTP_GET,  handleApiCfgGet);
  srv->on("/api/cfg",          HTTP_POST,
    [](AsyncWebServerRequest* r) { if (!check_dashboard_auth(r)) return; },
    nullptr,
    handleApiCfgPost);
}

// =================== AP 配网 ===================
static void start_ap_mode() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "%s", AP_SSID_PREFIX);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, AP_PASSWORD);
  ESP_LOGI(TAG, "AP up: SSID=%s pass=%s ip=%s", ssid, AP_PASSWORD, WiFi.softAPIP().toString().c_str());
  ap_mode_set(true);
  g_ledW.s = LED_BLINK_SLOW;

  g_apDns = new DNSServer();
  g_apDns->start(53, "*", WiFi.softAPIP());

  g_apServer = new AsyncWebServer(80);
  // v4.0.7 P-fix: AP 模式也用 SPA 父页, 让 nav 按钮工作
  // / 和 /app 都返回 APP_PAGE_HTML, 子页路由靠 register_web_routes
  g_apServer->on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send_P(200, "text/html; charset=utf-8", APP_PAGE_HTML);
  });
  g_apServer->on("/app", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send_P(200, "text/html; charset=utf-8", APP_PAGE_HTML);
  });
  // AP 模式 auth 是 no-op (g_cfg.otaUser/otaPass 空) → 共用 helper 注册同一套路由
  register_web_routes(g_apServer);
  g_apServer->onNotFound([](AsyncWebServerRequest* r) {
    r->redirect("/");
  });
  g_apServer->begin();

  // 阻塞在这条 task, 持续跑 DNS + LED update
  while (1) {
    g_apDns->processNextRequest();
    led_update_one((gpio_num_t)LED_4G_GPIO,   g_led4g);
    led_update_one((gpio_num_t)LED_WIFI_GPIO, g_ledW);
    led_update_one((gpio_num_t)LED_NET_GPIO,  g_ledNet);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// =================== BOOT 按钮长按 → 清 NVS ===================
// 防误触策略 (v4.0.4 patch, v4.0.5 调严):
//   1. 启动后 30s grace: 忽略 BOOT (避开 ESP32-S3 GPIO0 strapping 抖动)
//   2. 防抖 BOOT_DEBOUNCE_N=5: 连续 5 个 100ms 采样 = 500ms 才算按下
//      (v4.0.4 实测有 GPIO0 持续低情况 → 15s hold 触发 → 死循环, 60s + 防抖后才稳定)
//   3. 长按 60s: 旧 5s 太容易误触, 旧 15s 仍会被 GPIO0 抖动触发, 60s 给足够余量
//   4. cfg 是 hardcoded 默认值 → 完全 disable BOOT 检测 (避免 GPIO0 卡死循环)
//      用户通过 web 改 cfg → 重启后 BOOT 检测自动恢复
static void boot_button_task(void* /*param*/) {
  gpio_config_t io = { .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
                       .mode = GPIO_MODE_INPUT,
                       .pull_up_en = GPIO_PULLUP_ENABLE,
                       .pull_down_en = GPIO_PULLDOWN_DISABLE,
                       .intr_type = GPIO_INTR_DISABLE };
  gpio_config(&io);
  const uint32_t started = millis();
  uint32_t pressedAt = 0;
  bool warnedAt7s = false;  // LED 闪一下
  int lowCount = 0;          // 防抖计数器
  static bool bootDisabled = false;  // cfg hardcoded 时禁用 BOOT
  for (;;) {
    // 检测 cfg 是否仍是 hardcoded 默认值, 是则禁用 BOOT 防死循环
    bool cfgIsHardcoded = (strcmp(g_cfg.ssid, "REDACTED_SSID") == 0 &&
                           strcmp(g_cfg.otaUser, "admin") == 0);
    if (cfgIsHardcoded) {
      if (!bootDisabled) {
        ESP_LOGW(TAG, "BOOT disabled: cfg matches hardcoded default (avoid GPIO0 卡死 loop)");
        bootDisabled = true;
      }
      pressedAt = 0;
      lowCount = 0;
      warnedAt7s = false;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    bootDisabled = false;  // cfg 改了, 重新启用 BOOT 检测
    bool rawLow = (gpio_get_level((gpio_num_t)BOOT_BUTTON_PIN) == 0);
    if (rawLow) lowCount++;
    else        lowCount = 0;
    bool low = (lowCount >= BOOT_DEBOUNCE_N);  // 真按下: 连续 500ms 低
    if (millis() - started < BOOT_GRACE_MS) {
      pressedAt = 0;  // grace 内不计时
      warnedAt7s = false;
    } else if (low) {
      if (pressedAt == 0) {
        pressedAt = millis();
        warnedAt7s = false;
        ESP_LOGW(TAG, "BOOT pressed (debounced), start counting to %ums", BOOT_HOLD_MS);
      }
      uint32_t held = millis() - pressedAt;
      // 7s 闪一下提示用户 (二次确认点)
      if (!warnedAt7s && held >= 7000) {
        warnedAt7s = true;
        g_ledNet.flashTrig = true;  // 复用 NET LED 闪一下
        ESP_LOGW(TAG, "BOOT held 7s — keep holding to wipe (or release to cancel)");
      }
      if (held >= BOOT_HOLD_MS) {
        if (strlen(g_cfg.ssid) == 0) {
          ESP_LOGW(TAG, "BOOT held %ums but cfg empty — skip wipe, restart only", BOOT_HOLD_MS);
        } else {
          ESP_LOGW(TAG, "BOOT held %ums — wiping NVS & rebooting", BOOT_HOLD_MS);
          wipeConfig();
        }
        delay(200);
        ESP.restart();
      }
    } else {
      pressedAt = 0;
      warnedAt7s = false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// =================== USB CDC bring-up ===================
static void cdc_event_cb(usbh_cdc_device_event_t ev, usbh_cdc_device_event_data_t* data, void* /*user*/) {
  if (ev != CDC_HOST_DEVICE_EVENT_CONNECTED) return;
  if (g_cdc != NULL) return;   // 已开过

  usbh_cdc_port_config_t pc = {
    .dev_addr = data->new_dev.dev_addr,
    .itf_num  = 2,              // ML307 AT 通道 itf, 实测值; 不对就改这里
    .in_ringbuf_size  = 1024,
    .out_ringbuf_size = 1024,
    .in_transfer_buffer_size  = 512,
    .out_transfer_buffer_size = 512,
    .cbs   = {0},
    .flags = USBH_CDC_FLAGS_DISABLE_NOTIFICATION,
  };
  usbh_cdc_port_handle_t h = NULL;
  esp_err_t err = usbh_cdc_port_open(&pc, &h);
  if (err == ESP_OK) {
    g_cdc = h;
    ESP_LOGI(TAG, "USB CDC port opened dev_addr=%u itf=%u handle=%p",
             pc.dev_addr, pc.itf_num, (void*)h);
  } else {
    ESP_LOGE(TAG, "usbh_cdc_port_open failed: %s", esp_err_to_name(err));
  }
}

// =================== 4G 模组 AT 初始化 ===================
static bool modem_init_at() {
  // 等模组就绪
  for (int i = 0; i < 20; i++) {
    if (send_atcmd("AT\r\n", 1000) == 0) {
      ESP_LOGI(TAG, "4G module AT up");
      // 跳 CPIN 检查: 部分 ML307 firmware 只回 OK 不回 +CPIN: 行
      // 直接试 CSQ 看信号, CSQ 返 0,0 也是注册失败
      if (send_atcmd("AT+CSQ\r\n", 2000) == 0) {
        ESP_LOGI(TAG, "CSQ reply: %.80s", g_atReply);
      }
      // 配置 SMS
      send_atcmd("AT+CMGF=0\r\n",         2000);   // PDU 模式 (cmgs_send_pdu 走 PDU 写 hex + Ctrl-Z)
      // v4.0.6 P6b: 删 AT+CSCS="UCS2" — ML307 不支持, 12s 超时导致后续 AT 链路错乱
      // PDU 模式用 hex, 不需要 charset 设置
      send_atcmd("AT+CSDH=1\r\n",         2000);   // 显示 UDH (长短信拼接)
      send_atcmd("AT+CNMI=2,2,0,0,0\r\n", 2000);   // 主动推 +CMT
      // v4.0.7: STK (SIM ToolKit) — 暂停 (2026-06-19 翔哥决定)
      //   启用主动命令 (SIM 卡菜单会推 +STKPRO: URC) — 现在不要
      //   参考 waybyte/logicromsdk/ril_stk.h: AT+STKENV / AT+STKR / AT+STKTR
      //   Quectel 启用方式: AT+STKPCMD=1 (1=启用, 0=禁用)
      // send_atcmd("AT+STKPCMD=1\r\n",     2000);
      ESP_LOGI(TAG, "SMS engine configured");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGE(TAG, "4G module AT no response after 20s");
  return false;
}

// =================== setup / loop ===================
// 注: 这里用 Arduino 的 setup/loop, 不再 extern "C" app_main —— 框架自带 app_main 会调 setup() 然后循环调 loop()
void setup() {
  Serial.begin(115200);
  delay(500);
  g_bootMs = millis();   // v4.0.7: uptime 基线 (dashboard 显示开机时间 + 相对时间)
  // v4.0.6 P3: 默认 log level 是 INFO (sdkconfig 里 ARDUHAL_LOG_DEFAULT_LEVEL=ERROR
  // 会把 W/I 都吞; 但 ESP-IDF 的 ESP_LOGI 默认受 CONFIG_LOG_DEFAULT_LEVEL 控制,
  // 设回 INFO 才能看到我加的 ESP_LOGW/ESP_LOGE)
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("SMSFWD", ESP_LOG_INFO);
  ESP_LOGI(TAG, "\n=== SMS Forwarder %s ===", FW_VERSION);

  // v4.0.6 P19: 检测 /api/factory 标志 — 全擦 NVS 后重启进 AP 模式
  // g_factoryReset 在 RTC RAM (ulp_main 不用, volatile 跨 reset 由 flag 实现, 这里用 NVS 兜底)
  if (g_factoryReset) {
    ESP_LOGE(TAG, "FACTORY RESET — erasing whole NVS partition");
    nvs_flash_erase();   // 全擦 (24KB NVS partition 0x9000)
    nvs_flash_init();
    g_factoryReset = false;
    ESP_LOGW(TAG, "FACTORY RESET done, restarting in 1s");
    delay(1000);
    ESP.restart();
  }

  // 优先把 BOOT 任务起起来 (任何时候长按都能 reset)
  xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 3, NULL);
  // LED 任务 (独立)
  xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);

  loadConfig();
  nvsQSanityCheck();

  // 启动次数 +1 持久化
  {
    Preferences p;
    p.begin("stat", false);
    g_bootCount = p.getUInt("bootCount", 0) + 1;
    p.putUInt("bootCount", g_bootCount);
    p.end();
  }
  ESP_LOGI(TAG, "Boot #%u", g_bootCount);

  // 没合法配置 → AP 模式 永不返回
  if (!isConfigValid()) {
    ESP_LOGW(TAG, "No valid config, entering AP mode");
    start_ap_mode();
    return;
  }

  // 4G 模组上电
  gpio_config_t pwr_io = { .pin_bit_mask = (1ULL << G4_PWR_GPIO),
                           .mode = GPIO_MODE_OUTPUT,
                           .pull_up_en = GPIO_PULLUP_DISABLE,
                           .pull_down_en = GPIO_PULLDOWN_DISABLE,
                           .intr_type = GPIO_INTR_DISABLE };
  gpio_config(&pwr_io);
  gpio_set_level((gpio_num_t)G4_PWR_GPIO, 1);

  // FreeRTOS 同步对象
  g_atMutex = xSemaphoreCreateMutex();
  g_atDone  = xSemaphoreCreateBinary();
  g_atPrompt = xSemaphoreCreateBinary();
  g_smsQ    = xQueueCreate(SMS_QUEUE_LEN, sizeof(SmsMsg));
  g_pushQ   = xQueueCreate(SMS_QUEUE_LEN, sizeof(PushItem));
  g_urcQ    = xQueueCreate(16, URC_LINE_BUF);
  if (!g_atMutex || !g_atDone || !g_atPrompt || !g_smsQ || !g_pushQ || !g_urcQ) {
    ESP_LOGE(TAG, "FATAL: FreeRTOS primitive create failed");
    delay(100); ESP.restart();
  }

  // v4.0.6 P6 fix: cmgs_worker 提前起来, web /api/send handler 派 job 给他
  // 异步版避免 handler 在 async_tcp task 阻塞 10s+ 饿死 usb_rx_task
  xTaskCreate(cmgs_worker_task, "cmgs_worker", 4096, NULL, 4, &g_cmgsWorker);
  ESP_LOGI(TAG, "CMGS worker started");

  // WiFi STA + 事件
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_cfg.ssid, g_cfg.pass);
  ESP_LOGI(TAG, "WiFi connecting to %s ...", g_cfg.ssid);
  for (int i = 0; i < 60; i++) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    g_wifiUp = true;
    ESP_LOGI(TAG, "WiFi connected, IP=%s", WiFi.localIP().toString().c_str());
    // v4.0.6 P11: 首次启动同步等 WiFi 期间没走事件回调, 手动启 SNTP
    // (重连场景 GOT_IP 会再调一次, g_sntpStarted 短路)
    sntp_start_once();
  } else {
    ESP_LOGE(TAG, "WiFi connect timeout — push will keep failing until link up");
  }

  // Web 服务 (Dashboard + API + OTA + Restart)
  g_webServer = new AsyncWebServer(80);
  g_webServer->on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->redirect("/app?p=dashboard");
  });
  // v4.0.7: SPA 父页 — BasicAuth (浏览器输一次后 cookie 记住, iframe 内 fetch 自动带)
  // 这样 /api/cfg POST 等写 API 在 iframe 内也能通过 auth
  g_webServer->on("/app", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!check_dashboard_auth(r)) return;
    r->send_P(200, "text/html; charset=utf-8", APP_PAGE_HTML);
  });
  // v4.0.7+: dashboard 主页免 auth (只看状态), 写操作 (OTA/配置/sent clear) 才要 auth
  // SPA 内 iframe 加载时仍需返回原页面 (子页自己隐藏 nav), 不能 redirect, 否则 iframe 会跳到 /app
  g_webServer->on("/dashboard", HTTP_GET, [](AsyncWebServerRequest* r) {
    // 顶层访问 (无 Referer 或 Referer 非 /app) → redirect 到 SPA
    // iframe 内 (Referer 含 /app) → 返回完整页 (子页 script 自动隐藏 page-head/page-nav)
    String ref = r->header("Referer");
    if (ref.indexOf("/app") < 0) { r->redirect("/app?p=dashboard"); return; }
    r->send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
  });
  // 共用路由 (helper 跳过 /api/factory + /api/scan, AP 模式无意义)
  register_web_routes(g_webServer);
  // P19: 恢复出厂 — STA 专属 (AP 模式已是无凭据状态, 不需要这个)
  g_webServer->on("/api/factory", HTTP_POST, handleApiFactory);
  // P15: WiFi 扫描 — STA 专属 (AP 模式已经在 AP, 扫不到 STA 网络)
  g_webServer->on("/api/scan", HTTP_GET, handleApiScan);
  g_webServer->begin();
  ESP_LOGI(TAG, "Web server up (Dashboard / API / OTA)");

  // USB CDC driver + 等设备到达
  usbh_cdc_driver_config_t drv = { .task_stack_size = 4096,
                                   .task_priority   = 5,
                                   .task_coreid     = 0,
                                   .skip_init_usb_host_driver = false };
  ESP_ERROR_CHECK(usbh_cdc_driver_install(&drv));
  ESP_ERROR_CHECK(usbh_cdc_register_dev_event_cb(ESP_USB_DEVICE_MATCH_ID_ANY, cdc_event_cb, NULL));

  // 启 task
  xTaskCreate(usb_rx_task, "usb_rx", 4096, NULL, 5, NULL);
  xTaskCreate(sms_task,    "sms",    6144, NULL, 4, NULL);
  xTaskCreate(net_task,    "net",    8192, NULL, 4, NULL);
  // v4.0.7: CSQ 后台轮询 (5s 间隔, 写 g_4g_csq/g_4g_dbm 供前端)
  xTaskCreate(csq_poll_task, "csq_poll", 2048, NULL, 2, NULL);
  // v4.0.7 P0-debug: STK task 临时关 (用户担心影响 4G SMS 接收)
  // xTaskCreate(stk_event_task, "stk_event", 4096, NULL, 3, NULL);
  // xTaskCreate(stk_query_task, "stk_query", 3072, NULL, 2, NULL);

  // 等 4G 模组到达 (最多 15s)
  for (int i = 0; i < 150 && g_cdc == NULL; i++) vTaskDelay(pdMS_TO_TICKS(100));
  if (g_cdc == NULL) {
    ESP_LOGE(TAG, "No USB CDC device after 15s — 检查 4G 模组接线 / itf_num");
  } else {
    vTaskDelay(pdMS_TO_TICKS(2000));   // 让 ML307 内部上电稳定
    if (!modem_init_at()) {
      ESP_LOGE(TAG, "Modem AT init failed");
    } else {
      g_ml.alive = true;
      ESP_LOGI(TAG, "Modem ready (4G for SMS only, push over WiFi)");
    }
  }

  // 启动通知: 推一条 "设备已上线" 到 pushplus
  if (g_wifiUp) {
    push_boot_notification();
  } else {
    ESP_LOGW(TAG, "WiFi not up, no boot notification");
  }
}

void loop() {
  // 业务都跑在独立 task, 这里 idle 即可
  delay(1000);
}
