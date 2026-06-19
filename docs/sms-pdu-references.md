# SMS PDU 发送参考实现 (GitHub)

> 调研日期: 2026-06-19
> 目的: 对照 v4.0.11.17 的 AT+CMGS PDU 实现, 找出与成熟实现的差异, 修复已知 bug
> 调研范围: 4 个 ESP32/Arduino 4G modem SMS 项目

## TL;DR — 与我们实现的差异

| 项目 | 我们 v4.0.11.17 | ESP-SMS-Relay | ha-china/ml307r | TinyGSM |
|------|------------------|---------------|------------------|---------|
| PDU 模式 | `AT+CMGF=0` ✅ | ✅ | ✅ | 文本模式 |
| `AT+CMGS=<length>` | ✅ | ✅ | ✅ | (文本) |
| 等 `>` prompt | 信号量 (8s) | polling (DEFAULT_AT_TIMEOUT_MS) | polling (5s) | (文本) |
| PDU + Ctrl-Z 写入 | USB CDC 异步 | `simSerial.print` + 0x1A | `write_str` + `write_byte(0x1A)` | (文本) |
| 解析 `+CMGS:` | ✅ | ✅ (在 response 找 `+CMGS:` + `OK`) | ✅ (找 `OK`) | n/a |
| 解析 `+CMS ERROR:<code>` | ✅ (v4.0.11.16) | ❌ (只查 `ERROR`) | ❌ (只查 `ERROR`) | n/a |
| 解析 `+CSCA?` | ✅ (v4.0.11.14) | ❌ (参数必填) | ❌ (setSCAnumber() 默认) | n/a |
| 重试 | ❌ | ❌ | ❌ | n/a |
| PDU 长度计算 | `hex_len/2` | `strlen(pdu)/2 - 1` (含 SMSC 减 1) | (用 pdulib encodePDU 返回) | n/a |

---

## 1. lostmaniac/ESP-SMS-Relay (⭐⭐⭐) — **最完整 PDU 实现**

仓库: https://github.com/lostmaniac/ESP-SMS-Relay
文件: `lib/sms_sender/sms_sender.cpp`
库依赖: pdulib (encodePDU / getSMS)
硬件: ESP32 + SIM800/SIM900 GSM 模块

### 1.1 关键 sendPduData 函数 (L371-411)

```cpp
bool SmsSender::sendPduData(const char* pdu_data, int tpdu_length) {
  // 1) 构造 AT+CMGS=tpdu_length (注意: tpdu_length 不含 SMSC)
  String cmgs_command = "AT+CMGS=" + String(tpdu_length);

  // 2) 发送并等 '>' 提示符 (专用 prompt 模式)
  if (!sendAtCommand(cmgs_command, "", DEFAULT_AT_COMMAND_TIMEOUT_MS, /*wait_for_prompt=*/true)) {
    last_error_ = "发送AT+CMGS命令失败";
    return false;
  }

  // 3) PDU 数据 (pdulib getSMS() 已经包含 Ctrl-Z 结束符)
  simSerial.print(pdu_data);

  // 4) 等 +CMGS + OK 或 ERROR
  unsigned long start_time = millis();
  String response = "";

  while (millis() - start_time < DEFAULT_SMS_SEND_TIMEOUT_MS) {
    if (simSerial.available()) {
      char c = simSerial.read();
      response += c;
    }

    // 成功: +CMGS: <mr>  \r\nOK
    if (response.indexOf("+CMGS:") != -1 && response.indexOf("OK") != -1) {
      return true;
    }

    // 失败: ERROR 或 +CMS ERROR: <code>
    if (response.indexOf("ERROR") != -1) {
      last_error_ = "PDU发送失败: " + response;
      return false;
    }

    vTaskDelay(10);
  }

  last_error_ = "PDU发送超时";
  return false;
}
```

### 1.2 sendAtCommand 通用函数 (L318-362)

```cpp
bool SmsSender::sendAtCommand(const String& command, const String& expected_response,
                              unsigned long timeout, bool wait_for_prompt) {
  // 1) 清空串口缓冲 (避免污染响应)
  while (simSerial.available()) {
    simSerial.read();
  }

  // 2) 发送命令
  if (command.length() > 0) {
    simSerial.println(command);
  }

  unsigned long start_time = millis();
  String response = "";

  if (wait_for_prompt) {
    // 3a) 等 '>' 字符 (单字符 polling)
    while (millis() - start_time < timeout) {
      if (simSerial.available()) {
        char c = simSerial.read();
        if (c == '>') {
          return true;  // 不等后续 '\n' / 空格
        }
      }
      vTaskDelay(1);
    }
  } else {
    // 3b) 等期望响应字符串
    while (millis() - start_time < timeout) {
      if (simSerial.available()) {
        char c = simSerial.read();
        response += c;
      }
      if (expected_response.length() == 0 || response.indexOf(expected_response) != -1) {
        return true;
      }
      vTaskDelay(1);
    }
  }

  last_error_ = "AT命令执行失败: " + command;
  return false;
}
```

### 1.3 关键观察

1. **`wait_for_prompt=true` 模式**: 单独用 `wait_for_prompt` 参数切分, 不混 OK 检测和 prompt 检测 (避免状态机混乱)
2. **5 个连续串口 byte** 都可能是 prompt 部分: 我们 v4.0.11.14 已修 (在 CMT body 之前判断)
3. **`response.indexOf("+CMGS:") != -1 && response.indexOf("OK") != -1`**: 严格两个都要有才算成功 (单 `OK` 可能是上一条命令的回复)
4. **没解析 +CMS ERROR 码**: 只查 `"ERROR"` 字符串, 错误码被吞

---

## 2. ha-china/esphome_external_componnets/ml307r (⭐⭐⭐) — **ML307 同款芯片!**

仓库: https://github.com/ha-china/esphome_external_componnets
文件: `components/ml307r/ml307r.cpp`
库依赖: pdulib
硬件: ESP32 + **ML307R** 4G Cat-1 (跟我们同款)

### 2.1 send_sms 完整函数 (L218-286)

```cpp
bool ML307RComponent::send_sms(const std::string &phone_number, const std::string &message) {
  ESP_LOGI(TAG, "send sms to %s", phone_number.c_str());

  // 1) PDU 编码
  this->pdu_.setSCAnumber();  // 用默认短信中心, 不显式 AT+CSCA
  int pduLen = this->pdu_.encodePDU(phone_number.c_str(), message.c_str());
  if (pduLen < 0) {
    ESP_LOGW(TAG, "pdu encode error %d", pduLen);
    return false;
  }

  // 2) 发 AT+CMGS=pduLen (不等 OK, 等 prompt)
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", pduLen);
  this->send_at_command(cmd);

  // 3) polling 等 '>' 字符, 5s 超时
  uint32_t start = millis();
  bool got_prompt = false;
  while (millis() - start < 5000) {
    if (this->available()) {
      char c = this->read();
      if (c == '>') {
        got_prompt = true;
        break;
      }
    }
  }

  if (!got_prompt) {
    ESP_LOGE(TAG, "timeout receive > prompt");
    return false;
  }

  // 4) 写 PDU 字符串 + Ctrl-Z
  this->write_str(this->pdu_.getSMS());
  this->write_byte(0x1A);

  // 5) 等 OK 或 ERROR, 30s
  bool success = false;
  start = millis();
  std::string response;
  while (millis() - start < 30000) {
    while (this->available()) {
      response += (char) this->read();
      if (response.find("OK") != std::string::npos) {
        success = true;
        break;
      }
      if (response.find("ERROR") != std::string::npos) {
        break;
      }
    }
    delay(10);
  }

  return success;
}
```

### 2.2 关键观察 (ML307 4G 模组特殊)

1. **5s prompt 超时就够** (他们 ESPhome 是同步阻塞, 跑在 task 里没问题)
2. **未解析 +CMS ERROR 错误码**: 只查 `ERROR` 字符串
3. **未显式 `AT+CSCA`**: 注释写 "用默认短信中心" — 实际是依赖 PDU 编码器内部默认
4. **没查 `+CMGS:` mr 字段**: 只看 OK
5. **没 `+CSMP` 设置**: FO/VP 硬编码在 PDU 字符串里

---

## 3. TinyGSM (vshymanskyy) (⭐ 2199)

仓库: https://github.com/vshymanskyy/TinyGSM
文件: `src/TinyGsmSMS.tpp`
**注**: TinyGSM 主仓库**只支持文本模式** (`AT+CMGS="+number"`), 不支持 PDU 模式 (`AT+CMGS=<length>`)

```cpp
// TinyGSM 文本模式 (我们用不到)
thisModem().sendAT(GF("+CMGS=\""), number, GF("\""));
if (thisModem().waitResponse(GF(">")) != 1) { return false; }
thisModem().stream.print(text);
thisModem().stream.write(static_cast<char>(0x1A));
thisModem().stream.flush();
return thisModem().waitResponse(60000L) == 1;
```

**为什么 TinyGSM 不做 PDU**: 库追求通用性, 文本模式在 7-bit 字符集场景够用, PDU 编码是上层应用的责任

---

## 4. sonegillis/SMS (Python 参考) — 长度算法

```python
command = bytes("AT+CMGS=" + str(int((len(PDU)/2) - SMSC_length - 1)) + "\r", "ascii")
```

**PDU length 算法**:
- `len(PDU)/2` = PDU 字符串对应字节数
- `- SMSC_length` (字节) = 减去 SMSC 部分
- `- 1` = 减去 FIRST_OCTET 字节
- 结果 = TPDU 部分字节数 (AT+CMGS 的 length 参数)

**对应 3GPP TS 27.005**: `AT+CMGS=<length>` 的 length 字段是 TPDU 字节数, 不含 SCA

---

## 5. 对我们 v4.0.11.17 的具体改进点

### 5.1 ✅ 已修的 (v4.0.11.14-16)
- [x] P0: `>` prompt 检查放到 CMT body 判断之前 (`handle_at_line`)
- [x] P0: `+CMS ERROR` 写入 `g_atReply` (不走 URC)
- [x] P0: 4G init 强制 `AT+CSCA="+8613800010500",145`
- [x] P1: 8s prompt 超时 (覆盖 4G LTE 模组慢响应)

### 5.2 🔧 待修 (v4.0.11.18 计划)
- [ ] **parse_cnum_reply off-by-one**: 复制 ESP-SMS-Relay 用 `strchr` 三次找引号的模式
- [ ] **prompt 处理**: 把 HAL debug 升级为正式 logging (`g_atPrompt` 释放时机)
- [ ] **CSCA 设置错误处理**: 现在 `send_atcmd("AT+CSCA=...", 2000)` 失败时无重试, 4G 漫游场景可能默认 SCA 是 +66

### 5.3 ⚠️ 已知无法代码层修复
- **+CMS ERROR: 500**: SIM 是 +66 泰国号漫游到 China Mobile, 跨网发送可能需 IMS 注册
- 这种情况需用户/运营商侧处理, 代码层无法解决

---

## 6. 来源

- https://github.com/lostmaniac/ESP-SMS-Relay/blob/master/lib/sms_sender/sms_sender.cpp
- https://github.com/ha-china/esphome_external_componnets/blob/main/components/ml307r/ml307r.cpp
- https://github.com/vshymanskyy/TinyGSM/blob/master/src/TinyGsmSMS.tpp
- https://github.com/sonegillis/SMS/blob/master/pysms.py
