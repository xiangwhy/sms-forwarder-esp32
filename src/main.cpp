/*
 * ============================================================================
 *  SMS Forwarder v4.0.3  (ESP32-S3 + USB 4G Modem + pushplus)
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
#define BOOT_HOLD_MS       15000   // 长按 15s 才 wipe (避免 5s 误触)
#define BOOT_GRACE_MS      30000   // 启动后 30s 不响应 BOOT (避开 GPIO0 strapping 抖动)

#define AP_SSID_PREFIX     "SMS-Forwarder-"
#define AP_PASSWORD        "12345678"
#define FW_VERSION         "v4.0.3"

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
};
// 硬编码默认配置 (v4.0.4 patch, 烧板/重启后直接可用, 避免反复进 AP 配网)
// NVS 优先, NVS 为空时回落到这里的默认值
static Config g_cfg = {
  "REDACTED_SSID",                          // ssid
  "REDACTED_WIFI_PASS",                          // pass
  "49b6fce94afe49c2b5cf7cb59873800f",  // token
  "",                                  // topic (空 = 个人推送)
  "admin",                             // otaUser
  "Admin@123"                          // otaPass
};

static void loadConfig() {
  Preferences p;
  p.begin("cfg", true);
  p.getString("wifi.ssid", g_cfg.ssid, sizeof(g_cfg.ssid));
  p.getString("wifi.pass", g_cfg.pass, sizeof(g_cfg.pass));
  p.getString("pp.tok",    g_cfg.token, sizeof(g_cfg.token));
  p.getString("pp.tpc",    g_cfg.topic, sizeof(g_cfg.topic));
  p.getString("ota.user",  g_cfg.otaUser, sizeof(g_cfg.otaUser));
  p.getString("ota.pass",  g_cfg.otaPass, sizeof(g_cfg.otaPass));
  p.end();
  ESP_LOGI(TAG, "Config: ssid=%s token=%.8s... ota_user=%s",
           g_cfg.ssid, g_cfg.token, g_cfg.otaUser);
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
  p.end();
}

static bool isConfigValid() {
  return strlen(g_cfg.ssid) > 0 && strlen(g_cfg.token) >= 16;
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
static QueueHandle_t          g_smsQ    = NULL;
static QueueHandle_t          g_pushQ   = NULL;
static QueueHandle_t          g_urcQ    = NULL;

static char g_atReply[AT_REPLY_BUF];
static volatile size_t g_atReplyLen = 0;
static volatile int    g_atResult   = -2;   // 0=OK, -1=ERROR, -2=timeout/pending

static bool g_waitingCmtBody = false;
static char g_cmtHeader[CMT_HEADER_BUF] = {0};

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
static volatile uint32_t g_lastSmsMs    = 0;  // 上次收短信时间 (millis)
static volatile uint32_t g_lastPushOkMs = 0;  // 上次推送成功时间 (millis)

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
  xSemaphoreGive(g_atMutex);
  return rc;
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
static inline void push_success_inc() {
  __atomic_add_fetch(&g_pushOk, 1, __ATOMIC_RELAXED);
  g_lastPushOkMs = millis();
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
static bool post_pushplus_raw(const char* payload, size_t len) {
  if (WiFi.status() != WL_CONNECTED) return false;
  int code = http_post_json(PUSHPLUS_URL, payload, len, 5000);
  ESP_LOGI(TAG, "Push(WiFi) code=%d", code);
  return (code == 200);
}

// =================== 开机上线通知 ===================
static void push_boot_notification() {
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
static void nvsQEnqueue(const char* payload, size_t len) {
  Preferences p;
  p.begin("pqueue", false);
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
  p.begin("pqueue", true);
  uint8_t head = p.getUChar("head", 0);
  uint8_t count = p.getUChar("count", 0);
  p.end();
  if (count == 0) return 0;
  // 简化: 从 tail= (head - count + N) mod N 开始, 逐条 pop 并推
  int drained = 0;
  for (int i = 0; i < count; i++) {
    int idx = (head - count + i + NVS_QUEUE_LEN) % NVS_QUEUE_LEN;
    char key[8]; snprintf(key, sizeof(key), "p%u", idx);
    p.begin("pqueue", true);
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
  p.begin("pqueue", true);
  uint8_t head = p.getUChar("head", 0);
  uint8_t count = p.getUChar("count", 0);
  p.end();

  if (head >= NVS_QUEUE_LEN) {
    ESP_LOGW(TAG, "NVS queue head=%u OOR, reset", head);
    p.begin("pqueue", false);
    p.putUChar("head", 0);
    p.putUChar("count", 0);
    p.end();
    return;
  }

  uint8_t actual = 0;
  for (int i = 0; i < NVS_QUEUE_LEN; i++) {
    char key[8]; snprintf(key, sizeof(key), "p%d", i);
    p.begin("pqueue", true);
    String s = p.getString(key, "");
    p.end();
    if (s.length() > 0) actual++;
  }

  if (actual != count || count > NVS_QUEUE_LEN) {
    ESP_LOGW(TAG, "NVS queue count=%u actual=%u, fixing", count, actual);
    p.begin("pqueue", false);
    p.putUChar("count", actual);
    p.end();
  }
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
      // 编码自动检测: DCS 优先 (3GPP TS 23.038 §4 + Wikipedia Data Coding Scheme)
      // 7-bit (含 flash/ME/SIM/TE class): 0x00-0x03, 0x10-0x13, 0xF0-0xF3, 0xF8-0xFB
      // 8-bit raw:                          0x04-0x07, 0x14-0x17, 0xF4-0xF7, 0xFC-0xFF
      // UCS-2:                              0x08-0x0B, 0x18-0x1B
      // reserved (启发):                    0x0C-0x0F, 0x1C-0x1F
      // 启发原则: 严格匹配 → 7-bit (bodyBytes ≈ cmt_len*7/8 ±2), 其他默认 UCS-2
      //   默认 UCS-2 防止 DCS=0xFF/未知 + 中英混排短信被误判 7-bit → 乱码
      size_t bodyHexLen = strnlen(msg.body_hex, sizeof(msg.body_hex));
      size_t bodyBytes  = bodyHexLen / 2;
      auto dcs7 = [](uint16_t d){
        return (d<=0x03) || (d>=0x10&&d<=0x13) || (d>=0xF0&&d<=0xF3) || (d>=0xF8&&d<=0xFB);
      };
      auto dcs8 = [](uint16_t d){
        return (d>=0x04&&d<=0x07) || (d>=0x14&&d<=0x17) || (d>=0xF4&&d<=0xF7) || (d>=0xFC&&d<=0xFF);
      };
      bool is7bit = dcs7(msg.dcs);
      bool is8bitData = !is7bit && dcs8(msg.dcs);
      if (!is7bit && !is8bitData) {
        // UCS-2 / reserved / 未知 → 启发, 默认 UCS-2 (更安全)
        if (msg.cmt_length > 0) {
          // 7-bit: bodyBytes ≈ cmt_len*7/8 ±2
          size_t expect = (msg.cmt_length * 7 + 7) / 8;
          is7bit = (bodyBytes >= expect - 2 && bodyBytes <= expect + 2);
        }
        // else: cmt_length 不可信, 默认 UCS-2
      }
      size_t bodyN = 0;
      if (is7bit) {
        // 7-bit: numChars (user septets) 推导
        //   单 part (msg 直接从 queue 来的, body 含 UDH 头): user = cmt_length - 7
        //   stash 出来的 concat msg: cmt_length = sum(per_part), 已是 user septets, 直接用
        //   cmt_length=0 (不可信): 用 bodyBytes 倒推
        size_t numChars = 0;
        if (msg.cmt_length > 0) {
          numChars = msg.cmt_length;
          // body 头部还含 UDH 标志 (单 part 没被 stash 吃) → 减 7 overhead
          if (strstr(msg.body_hex, "0804") || strstr(msg.body_hex, "0003")) {
            if (numChars > 7) numChars -= 7;
          }
        }
        if (numChars == 0) numChars = (bodyBytes * 8) / 7;
        bodyN = pdu::decode_7bit_packed(msg.body_hex, bodyHexLen, numChars,
                                        bodyBuf, sizeof(bodyBuf));
        ESP_LOGI(TAG, "SMS: 7-bit decode (dcs=%u cmt_len=%u bodyBytes=%u → numChars=%u bodyN=%u)",
                 msg.dcs, msg.cmt_length, (unsigned)bodyBytes, (unsigned)numChars, (unsigned)bodyN);
      } else if (is8bitData) {
        // 8-bit raw: hex → raw bytes, 跳过非 hex 字符
        ESP_LOGI(TAG, "SMS: 8-bit data (dcs=%u cmt_len=%u bodyBytes=%u)", msg.dcs, msg.cmt_length, (unsigned)bodyBytes);
        bodyN = 0;
        for (size_t i = 0; i + 1 < bodyHexLen && bodyN < sizeof(bodyBuf) - 1; i += 2) {
          char h0 = msg.body_hex[i], h1 = msg.body_hex[i+1];
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
        bodyN = pdu::decode_body_field(msg.body_hex, bodyHexLen, bodyBuf, sizeof(bodyBuf));
      }
      String phoneUtf8(phoneBuf, phoneN);
      String bodyUtf8(bodyBuf, bodyN);
      g_lastSmsMs = millis();
      ESP_LOGI(TAG, "SMS: phone=%s body=%.120s", phoneUtf8.c_str(), bodyUtf8.c_str());
      ESP_LOGI(TAG, "SMS: body_raw=%.200s body_len=%u", msg.body_hex, (unsigned)strlen(msg.body_hex));
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

// =================== WiFi 事件 ===================
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    g_wifiUp = false;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    g_wifiUp = true;
    ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
    ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&e->ip_info.ip));
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
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>SMS Forwarder Dashboard</title>
<style>
body{font-family:-apple-system,sans-serif;max-width:760px;margin:20px auto;padding:0 16px;background:#fafafa}
h1{font-size:20px}.row{display:flex;gap:12px;flex-wrap:wrap;margin:12px 0}
.card{flex:1;min-width:200px;background:#fff;padding:12px;border-radius:8px;box-shadow:0 1px 3px #0001}
.kv{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #f0f0f0}
.kv:last-child{border:0}.kv b{color:#555;font-weight:500}.kv span{font-family:monospace}
.btn{display:inline-block;padding:6px 12px;background:#06f;color:#fff;border-radius:4px;text-decoration:none;margin-top:8px}
.tag{display:inline-block;padding:2px 6px;border-radius:3px;font-size:12px}
.tag-ok{background:#dfd;color:#282}.tag-bad{background:#fdd;color:#822}
</style></head><body>
<h1>SMS Forwarder <span class=tag>FW v4.0.3</span></h1>
<div class=row>
  <div class=card><h3>运行</h3>
    <div class=kv><b>Boot</b><span id=boot>0</span></div>
    <div class=kv><b>WiFi</b><span id=wifi class=tag-bad>off</span><span id=rssi></span></div>
    <div class=kv><b>4G</b><span id=ml class=tag-bad>off</span></div>
    <div class=kv><b>UDH</b><span id=udh>0</span></div>
  </div>
  <div class=card><h3>推送</h3>
    <div class=kv><b>成功</b><span id=ok>0</span></div>
    <div class=kv><b>失败</b><span id=fal>0</span></div>
    <div class=kv><b>队列</b><span id=q>0</span></div>
    <div class=kv><b>上次推</b><span id=lp>-</span></div>
    <div class=kv><b>上次信</b><span id=ls>-</span></div>
  </div>
  <div class=card><h3>系统</h3>
    <div class=kv><b>Heap</b><span id=heap>0</span></div>
    <div class=kv><b>Min</b><span id=heapMin>0</span></div>
  </div>
</div>
<a class=btn href=/update>OTA 升级</a>
<a class=btn href=/restart style=background:#888>重启</a>
<p><small>30s 自动刷新</small></p>
<script>
function ago(ms) {
  if (!ms) return '-';
  let s = Math.floor((Date.now() - ms) / 1000);
  if (s < 60) return s + 's';
  if (s < 3600) return Math.floor(s/60) + 'm';
  return Math.floor(s/3600) + 'h';
}
async function poll() {
  let r = await fetch('/api/status'); if (!r.ok) return;
  let j = await r.json();
  boot.textContent = j.boot;
  ok.textContent = j.pushOk;
  fal.textContent = j.pushFail;
  q.textContent = j.qLen;
  wifi.textContent = j.wifi ? 'on' : 'off';
  wifi.className = 'tag ' + (j.wifi ? 'tag-ok' : 'tag-bad');
  rssi.textContent = j.wifi && j.wifiRssi ? ' (' + j.wifiRssi + 'dBm)' : '';
  ml.textContent = j.mlAlive ? 'on' : 'off';
  ml.className = 'tag ' + (j.mlAlive ? 'tag-ok' : 'tag-bad');
  udh.textContent = j.udhActive + '/4';
  lp.textContent = ago(j.lastPushOkMs) + '前';
  ls.textContent = ago(j.lastSmsMs) + '前';
  heap.textContent = Math.round(j.freeHeap/1024) + 'K';
  heapMin.textContent = Math.round(j.minFreeHeap/1024) + 'K';
}
poll(); setInterval(poll, 30000);
</script></body></html>
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
  doc["pushOk"]      = g_pushOk;
  doc["pushFail"]    = g_pushFail;
  doc["qLen"]        = count;
  doc["wifi"]        = g_wifiUp;
  doc["wifiRssi"]    = g_wifiUp ? WiFi.RSSI() : 0;
  doc["freeHeap"]    = esp_get_free_heap_size();
  doc["minFreeHeap"] = esp_get_minimum_free_heap_size();
  doc["lastSmsMs"]   = g_lastSmsMs;
  doc["lastPushOkMs"]= g_lastPushOkMs;
  doc["mlAlive"]     = g_ml.alive;
  doc["udhActive"]   = udhActive;
  doc["fw"]          = FW_VERSION;
  String out; serializeJson(doc, out);
  r->send(200, "application/json", out);
}

// =================== OTA web (BasicAuth) ===================
static const char OTA_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset=utf-8>
<title>OTA</title>
<style>body{font-family:-apple-system,sans-serif;max-width:480px;margin:30px auto;padding:0 16px}
form{background:#fff;padding:16px;border-radius:8px;box-shadow:0 1px 3px #0001}
input[type=file]{margin:8px 0}
input[type=submit]{background:#06f;color:#fff;border:0;padding:10px 20px;border-radius:6px;cursor:pointer}
a.btn{display:inline-block;padding:8px 16px;background:#888;color:#fff;border-radius:4px;text-decoration:none;margin-left:8px}
</style></head><body>
<h2>OTA 升级</h2>
<form method=POST action="/update" enctype=multipart/form-data>
<input type=file name=firmware required>
<input type=submit value=烧录>
<a class=btn href=/dashboard>返回</a>
</form>
<p><small>选择 .bin 文件, 大小 &lt; 3MB</small></p>
</body></html>
)HTML";

static bool check_ota_auth(AsyncWebServerRequest* r) {
  if (strlen(g_cfg.otaUser) > 0 && strlen(g_cfg.otaPass) > 0) {
    if (!r->authenticate(g_cfg.otaUser, g_cfg.otaPass)) {
      r->requestAuthentication();
      return false;
    }
  }
  return true;
}

static void handle_ota_page(AsyncWebServerRequest* r) {
  if (!check_ota_auth(r)) return;
  r->send_P(200, "text/html; charset=utf-8", OTA_HTML);
}

static void handle_ota_chunk(AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
  if (!check_ota_auth(r)) return;
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
  if (!check_ota_auth(r)) return;
  if (Update.hasError()) r->send(500, "text/plain", "Update error");
  else                   r->send(200, "text/plain", "OK, rebooting ...");
}

static void handle_restart(AsyncWebServerRequest* r) {
  if (!check_ota_auth(r)) return;
  r->send(200, "text/plain", "Rebooting ...");
  delay(200);
  ESP.restart();
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

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SMS Forwarder 配网</title>
<style>
body{font-family:-apple-system,sans-serif;max-width:420px;margin:30px auto;padding:0 16px}
h1{font-size:20px}label{display:block;margin:12px 0 4px;font-size:14px;color:#555}
input{width:100%;box-sizing:border-box;padding:10px;font-size:15px;border:1px solid #ccc;border-radius:6px}
button{margin-top:20px;width:100%;padding:12px;font-size:16px;background:#06f;color:#fff;border:0;border-radius:6px}
small{color:#888}
fieldset{border:1px solid #ddd;border-radius:6px;padding:8px 12px;margin-top:18px}
legend{font-size:13px;color:#666;padding:0 6px}
</style></head><body>
<h1>SMS Forwarder 配网</h1>
<form method="POST" action="/save">
<label>WiFi SSID</label><input name="ssid" required maxlength="63">
<label>WiFi 密码</label><input name="pass" type="password" maxlength="63">
<label>pushplus token <small>(pushplus.plus 个人中心)</small></label>
<input name="token" required minlength="16" maxlength="63">
<label>pushplus topic <small>(可空, 群组推送用)</small></label>
<input name="topic" maxlength="63">
<fieldset>
<legend>Web OTA 烧录 (防别人乱刷)</legend>
<label>OTA 用户名</label><input name="ota_user" value="admin" maxlength="31">
<label>OTA 密码</label><input name="ota_pass" type="password" minlength="4" maxlength="31">
</fieldset>
<button>保存并重启</button>
</form>
<p><small>v4.0.2 / 重启后自动连 WiFi</small></p>
</body></html>
)HTML";

static void handleSave(AsyncWebServerRequest* req) {
  Config c = {};
  if (req->hasParam("ssid", true))     strncpy(c.ssid,    req->getParam("ssid",    true)->value().c_str(), sizeof(c.ssid)-1);
  if (req->hasParam("pass", true))     strncpy(c.pass,    req->getParam("pass",    true)->value().c_str(), sizeof(c.pass)-1);
  if (req->hasParam("token", true))    strncpy(c.token,   req->getParam("token",   true)->value().c_str(), sizeof(c.token)-1);
  if (req->hasParam("topic", true))    strncpy(c.topic,   req->getParam("topic",   true)->value().c_str(), sizeof(c.topic)-1);
  if (req->hasParam("ota_user", true)) strncpy(c.otaUser, req->getParam("ota_user",true)->value().c_str(), sizeof(c.otaUser)-1);
  if (req->hasParam("ota_pass", true)) strncpy(c.otaPass, req->getParam("ota_pass",true)->value().c_str(), sizeof(c.otaPass)-1);
  if (strlen(c.ssid) == 0 || strlen(c.token) < 16) {
    req->send(400, "text/plain; charset=utf-8", "SSID 不能空, token 至少 16 位");
    return;
  }
  if (strlen(c.otaUser) == 0) strncpy(c.otaUser, "admin", sizeof(c.otaUser)-1);
  if (strlen(c.otaPass) < 4) {
    req->send(400, "text/plain; charset=utf-8", "OTA 密码至少 4 位 (防别人刷固件)");
    return;
  }
  saveConfig(c);
  req->send(200, "text/plain; charset=utf-8", "保存成功, 设备 1 秒后重启 ...");
  delay(800);
  ESP.restart();
}

static void start_ap_mode() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "%s%02X%02X%02X", AP_SSID_PREFIX, mac[3], mac[4], mac[5]);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, AP_PASSWORD);
  ESP_LOGI(TAG, "AP up: SSID=%s pass=%s ip=%s", ssid, AP_PASSWORD, WiFi.softAPIP().toString().c_str());
  ap_mode_set(true);
  g_ledW.s = LED_BLINK_SLOW;

  g_apDns = new DNSServer();
  g_apDns->start(53, "*", WiFi.softAPIP());

  g_apServer = new AsyncWebServer(80);
  g_apServer->on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });
  g_apServer->on("/save", HTTP_POST, handleSave);
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
// 防误触策略 (v4.0.4 patch):
//   1. 启动后 30s grace: 忽略 BOOT (避开 ESP32-S3 GPIO0 strapping 抖动)
//   2. 长按 15s: 5s 太容易误触 (USB 插拔 / 静电 / 浮空) 触发 wipe
//   3. 二次确认: 按到 7s 时 LED 快闪 1s, 提示用户再继续按才算 wipe
//   4. cfg 已空: 不重复 wipe (避免无意义 reboot loop)
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
  for (;;) {
    bool low = (gpio_get_level((gpio_num_t)BOOT_BUTTON_PIN) == 0);
    if (millis() - started < BOOT_GRACE_MS) {
      pressedAt = 0;  // grace 内不计时
      warnedAt7s = false;
    } else if (low) {
      if (pressedAt == 0) {
        pressedAt = millis();
        warnedAt7s = false;
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
          ESP_LOGW(TAG, "BOOT held %ums but cfg already empty — skip wipe, restart only", BOOT_HOLD_MS);
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
      send_atcmd("AT+CMGF=1\r\n",         2000);   // 文本模式
      send_atcmd("AT+CSCS=\"UCS2\"\r\n",  2000);   // UCS2 编码
      send_atcmd("AT+CSDH=1\r\n",         2000);   // 显示 UDH (长短信拼接)
      send_atcmd("AT+CNMI=2,2,0,0,0\r\n", 2000);   // 主动推 +CMT
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
  ESP_LOGI(TAG, "\n=== SMS Forwarder %s ===", FW_VERSION);

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
  g_smsQ    = xQueueCreate(SMS_QUEUE_LEN, sizeof(SmsMsg));
  g_pushQ   = xQueueCreate(SMS_QUEUE_LEN, sizeof(PushItem));
  g_urcQ    = xQueueCreate(16, URC_LINE_BUF);
  if (!g_atMutex || !g_atDone || !g_smsQ || !g_pushQ || !g_urcQ) {
    ESP_LOGE(TAG, "FATAL: FreeRTOS primitive create failed");
    delay(100); ESP.restart();
  }

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
  } else {
    ESP_LOGE(TAG, "WiFi connect timeout — push will keep failing until link up");
  }

  // Web 服务 (Dashboard + API + OTA + Restart)
  g_webServer = new AsyncWebServer(80);
  g_webServer->on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->redirect("/dashboard");
  });
  g_webServer->on("/dashboard", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
  });
  g_webServer->on("/api/status", HTTP_GET, handleApiStatus);
  g_webServer->on("/update",     HTTP_GET, handle_ota_page);
  g_webServer->on("/update",     HTTP_POST, handle_ota_done,
    [](AsyncWebServerRequest* r, const String& fn, size_t idx, uint8_t* d, size_t l, bool fin) {
      handle_ota_chunk(r, fn, idx, d, l, fin);
    });
  g_webServer->on("/restart", HTTP_GET, handle_restart);
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
