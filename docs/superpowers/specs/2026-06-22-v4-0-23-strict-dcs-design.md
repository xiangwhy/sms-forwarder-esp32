# v4.0.23 — 信 DCS 严格走, 错位/reserved/无效编码 → 丢 body

## Context

翔哥 2026-06-22 真机复现 v4.0.22 乱码: dtac +66813079348 boot #128 收到 1 条 SMS, body 是全 0x00/0xXX 字节 (Å=0xC5, V=0x56, 8=0x38, H=0x48, d=0x64, A=0x41, ø=0xF8, ì=0xEC 等), 没有真 Thai 字符 (没有 0x0E2B 这种 0x0E high byte). 这是 [[dtac-gateway-anomaly]] 6/20 锁死的同源 case: DTAC gateway 偶发 raw UD bytes 异常 (0x10/0xFE 高 septet), device 7-bit 解码**正确**, 但 gateway 推错编码.

v4.0.22 spec (3f76f69) 决策 **sniffer fallback 信任 pdu_ud_offset_ex** 是 SCA 部分修 (不再 fallback udHexOff=0 全 body decode 让 SCA bytes 当 UCS-2 codepoint 输出乱码). 但 v4.0.22 仍保留 2 个 sniff + 1 个 fallback 路径:

1. main.cpp:1874-1896 **兜底 sniff**: DCS 0x80-0xBF reserved / 0xFF 时, 跑 looks_like_ucs2_be / udBytes vs cmt_length 启发式, 默认 7-bit
2. main.cpp:1897-1905 **is7bit + looks_like_ucs2_be bypass**: DCS 标 7-bit 但 raw body 呈 UCS-2 BE 模式 → 强制走 UCS-2
3. main.cpp:1925-1932 **is_strict_utf8 fallback**: 7-bit decode 输出不是合法 UTF-8 → fallback UCS-2 decode

这 3 个 sniff/fallback 跟 [[dtac-gateway-anomaly]] "不要扩 sniff" 警告矛盾, 且真乱码复现证明 sniff 在 DTAC gateway 异常 bytes 场景会**制造**乱码 (而不是消除).

## 业界调研结论 (2026-06-22 AOSP/smstools3 deep dive)

翔哥 拍板 "查 GitHub 项目都怎么处理这个问题". 4 大权威实现行为:

| 实现 | 7-bit 错位 / Reserved DCS 行为 |
|---|---|
| **AOSP SmsMessage.java** (Android, 数十亿设备) | `parseUserData()` switch DCS: bits 3-2 选 7-bit/UCS-2/8-bit, reserved 0x80-0xBF fallthrough ENCODING_UNKNOWN → `mMessageBody = null` (整条丢). 7-bit decode `septetCount*7/8` 整数除法, 末 1-7 bits 丢弃, sub-byte drift, 不 fallback UCS-2. |
| **smstools3** (C, 工业级, 10+ 年生产) | partial decode + warning log, 不做 UCS-2 fallback |
| **python-messaging** (Python, 233★) | 信 DCS 错位, 不 fallback |
| **smspdudecoder** (Python, 84★) | 信 DCS, 错位 IndexError crash |
| **我们 v4.0.22** | sniff + is_strict_utf8 → fallback UCS-2 (乱码但"纯净") |

**业界共识**: 严格信 DCS, 错位/reserved/推错编码 = 运营商/网关 bug, library 不修, **整条丢** (或解码失败).

我们 v4.0.22 实际是"激进派" — 至少给用户看乱码. v4.0.23 改成"严格派", 跟业界一致.

## 决策 (按 brainstorming 2026-06-22)

| 决策 | 选项 | 选择 | 理由 |
|---|---|---|---|
| 路径 | 1 跟业界走 / 2 保持激进 / 3 回滚 v4.0.21.1 / 4 AOSP 丢 body | **1 = 跟业界走 (信 DCS 严格)** | AOSP 行为最权威, 业界 4 大一致, v4.0.22 激进策略 dtac 真机复现失败 |
| 丢 body 显示 | A 占位符 / **B 空字符串 (AOSP)** / C hex dump | **B 空字符串** | AOSP 严格行为, dashboard li 只显 phone + 时间, 用户看不出来 (但 pushplus 推送仍触发, 翔哥 知道有 SMS 来) |
| Scope | 1 仅 main.cpp decode 路径 / 2 + pdu_codec helper / 3 + padding 校验 | **1 仅 main.cpp decode 路径** | 最小改动, sniff helper 留供未来重启用, padding 校验 AOSP 不做 |

## Root cause (具体)

main.cpp:1874-1932 三大问题:

### 问题 1: 兜底 sniff (main.cpp:1874-1896)

```cpp
if (!is7bit && !isUcs2) {
  // 兜底: 既不是 7-bit 也不是 UCS-2 → sniff
  if (pdu::looks_like_ucs2_be(udHex, udFullHexLen)) {  // ← sniff #1
    isUcs2 = true; ...
  } else if (msg.cmt_length > 0 && udBytes > 0) {
    if (udBytes >= expect - 2 && udBytes <= expect + 2) {
      is7bit = true; ...  // ← sniff #2
    }
  } else if (udBytes > 0 && udBytes < 300) {
    is7bit = true; ...  // ← sniff #3 (concat path default 7-bit)
  }
}
```

**问题**: DTAC gateway 推错编码时 (DCS=0 标 7-bit 实为高 septet 异常字节), 这 3 个 sniff 都可能错判 — 比如 raw bytes 像 UCS-2 BE 模式 (连续 0x00/0xXX), sniff #1 强制走 UCS-2 decode, 输出乱码. sniff #2/#3 走 7-bit decode 也错.

### 问题 2: is7bit + looks_like_ucs2_be bypass (main.cpp:1897-1905)

```cpp
if (is7bit && pdu::looks_like_ucs2_be(udHex, udFullHexLen)) {
  // raw body UCS-2 BE pattern, bypass 7-bit decode
  is7bit = false;
  is8bitData = false;  // 强制走 UCS-2
}
```

**问题**: 跟问题 1 sniff #1 同样 — DTAC gateway 异常 bytes 触发, 强制走 UCS-2 decode 错.

### 问题 3: is_strict_utf8 fallback (main.cpp:1925-1932)

```cpp
// 兜底: GSM7 decode 输出必须是合法 UTF-8
if (bodyN > 0 && !pdu::is_strict_utf8(bodyBuf, bodyN)) {
  ESP_LOGW(TAG, "SMS: 7-bit decode not strict UTF-8, fallback UCS-2");
  bodyN = pdu::decode_body_field(udHex, udFullHexLen, bodyBuf, sizeof(bodyBuf));
  is7bit = false;
}
```

**问题**: 跟 [[feedback_working_code_no_touch]] + [[dtac-gateway-anomaly]] "不要扩 sniff" 警告矛盾. 7-bit decode 错位 (sub-byte drift) 时, 输出不是合法 UTF-8, fallback UCS-2 制造乱码.

### 丢 body 触发条件 (新加)

DCS 严格判定后, 以下 4 种情况 → bodyN = 0 (丢 body, 空字符串):

1. **DCS 0x80-0xBF reserved** (3GPP TS 23.038 §4 fallback): AOSP 走 ENCODING_UNKNOWN, 我们 → 丢
2. **DCS 0xFC-0xFF reserved**: 同样 → 丢
3. **7-bit decode 输出不是合法 UTF-8** (sub-byte drift / 错位): 跟 v4.0.22 一样检测, 但不再 fallback UCS-2, 直接丢
4. **UDL < UDH size** (AOSP `bufferLen < 0` silent truncation): 跟 AOSP 一致 → 丢

每种触发 ESP_LOGW 记录: DCS 值 + UDL + udBytes + bodyAvail + 丢的原因分类.

## Scope (按 brainstorming 决策: scope 1 = 仅 main.cpp decode 路径)

### 启用 (3 file 改)

| # | 类别 | 文件:行 | 内容 |
|---|---|---|---|
| 1 | code | `src/main.cpp:1874-1896` | **删**兜底 sniff 3 分支 (looks_like_ucs2_be / cmt_length 启发式 / concat default 7-bit). 改成: `if (!is7bit && !isUcs2)` → **触发丢 body** (DCS 不在 7-bit/UCS-2/8-bit 三类 = reserved / 0xFF, 跟 AOSP `mMessageBody = null` 一致, 不进任何 decode). ESP_LOGW 记录 "DCS reserved/0xFF, drop body". |
| 2 | code | `src/main.cpp:1897-1905` | **删** is7bit + looks_like_ucs2_be bypass. 改成: 7-bit path 一旦 is7bit=true, 不再 sniff bypass |
| 3 | code | `src/main.cpp:1925-1932` | **删** is_strict_utf8 fallback UCS-2. 改成: 7-bit decode 输出不是合法 UTF-8 → 丢 body (bodyN=0, log warn "7-bit decode not strict UTF-8, drop body") |
| 4 | code | `src/main.cpp:~1820-1980` | **加** 4 个丢 body 触发条件 (上面列的) + ESP_LOGW 记录 |
| 5 | test | `tests/host/test_pdu_codec.cpp` (TDD red) | **加** fixture_d: dtac DCS=0xFF + 异常 raw bytes → body="" (main.cpp mirror 函数测). **加** fixture_e: 7-bit decode 不 strict UTF-8 → body="". **加** fixture_f: UDL < UDH size → body="". (host test 仅 main.cpp decode 路径, 不动 pdu_codec fixtures) |
| 6 | housekeeping | `src/main.cpp:77` FW_VERSION 宏 | `"v4.0.22"` → `"v4.0.23"` |
| 7 | housekeeping | `src/web/app.h:42` fw-tag | `FW v4.0.22` → `FW v4.0.23` |

### 不实现 (本 spec out of scope, 留 v4.0.24+)

- **不改 pdu_codec.cpp** (sniff helper looks_like_ucs2_be / is_strict_utf8 / pdu_ud_offset_ex 留, 仅 main.cpp 不调. 供未来"如果业界行为变更"重启用)
- **不加 padding 校验** (AOSP 不做, 业界不算 P0 触发, 加了可能误伤合法 SMS)
- **不删 main.cpp:1847-1868 v4.0.22 sniffer fallback** (trust pdu_ud_offset_ex 是 SCA 部分修, 跟"信 DCS 严格"不矛盾 — 它是 PDU header 解析层, 不是 sniff)
- **不动 STK / send / OTA / dashboard / sanitizeForJson** (无关)
- **不改 v4.0.22 fixtures A/B/C** (它们是 SCA 部分修验证, v4.0.23 仍要它们继续 PASS)
- **不修 v4.0.11.21 review 7 finding** (按 [[feedback_working_code_no_touch]] 翔哥 2026-06-19 拍板暂不修)

## Test plan

### TDD red (写 v4.0.23 fixtures, 跑挂, C2 修)

| Fixture | 输入 | 期望 (v4.0.23 strict-DCS) |
|---|---|---|
| D: DCS=0xFF + 异常 raw bytes | DTAC gateway DCS=0xFF reserved + raw bytes 像 UCS-2 BE (连续 0x00/0xXX) | body="" |
| E: 7-bit decode 不 strict UTF-8 | DCS=0 但 raw 字节高 septet 异常 (sub-byte drift) | body="" |
| F: UDL < UDH size | DCS=0, UDL=5 但 UDH 头 7 字节 (UDHL+UDH 7) → bufferLen<0 | body="" |

### 历史 regression

| Fixture | v4.0.22 期望 | v4.0.23 期望 |
|---|---|---|
| A: SCA 部分修 (UDH 字节污染) | SCA leak check + n>30 PASS | 改成 2 断言: (a) "no SCA leak" (v4.0.22 SCA 部分修), (b) "bodyN=0" (v4.0.23 丢 body 触发) |
| B: 单段全绿 | udOff 信任 + 无 SCA leak | 不变 (7-bit decode 输出合法 UTF-8, body 正常) |
| C: dtac/TRUE 2 段无 regression | udOff+udBytes*2==bodyLen 不触发 fallback | 不变 |
| D/E/F (新) | (TDD red) | body="" PASS |

**Fixture A 期望调整决策**: 当前 v4.0.22 是 "SCA leak + n>30 PASS" (UDH 字节污染 fixture A 头 7 字符, 不可全绿). v4.0.23 删 sniff 后, fixture A 走 7-bit decode → UDH 字节 + 残余 UCS-2 BE 字节 → 不 strict UTF-8 → **丢 body (bodyN=0)**. 这跟 fixture A 原本"确认 SCA 不在 body 里" 的意图部分冲突 (原本验 "SCA leak 不在" + "body 部分可解", 现在验 "丢 body"). **决策**: fixture A 改成 2 个独立断言: (a) "no SCA leak" (跟 v4.0.22 一致, 确认 SCA 解析对), (b) "bodyN=0" (新加, 确认 v4.0.23 丢 body 触发). 这 2 个断言覆盖 v4.0.22 + v4.0.23 双行为.

### 烧板验证

按 [[feedback_code_review_before_flash]]:

1. `pio run -t upload` SUCCESS
2. `/api/status` → `"fw":"v4.0.23"` ✓
3. `/app` curl grep → `FW v4.0.23` ✓
4. **真机复现** (按 [[feedback_working_code_no_touch]]): 翔哥 +66813079348 触发 dtac 推一条真乱码 SMS → `/api/recent` 应见 body="" (空字符串), phone=dtac

## 风险

- **行为变更**: v4.0.22 → v4.0.23, 7-bit decode 不 strict UTF-8 的合法 SMS 也会被丢 body. 影响范围: 翔哥 之前收到能解出来的 7-bit 短信如果偶发 sub-byte drift → 现在变丢 body. 业界一致接受此 trade.
- **推送仍触发**: body="" 但 phone + ts 仍写入 rx_log + pushplus 推送 ([dtac] 无内容). 翔哥 拍板接受 (否则要改推送逻辑, 大重构).
- **Fixture A 期望调整**: 上面 Test plan 提了, 翔哥 拍板.

## 相关 memory / 引用

- [[dtac-gateway-anomaly]] — 2026-06-20 锁死 DTAC gateway 异常 bytes, "不要改 sniff" 警告
- [[feedback_working_code_no_touch]] — 功能稳不动, 但真乱码复现要修
- [[feedback_code_review_before_flash]] — 不再迭代烧, 审完代码批量改再烧
- [[feedback-bump-fw-version]] — 改代码 bump FW_VERSION 2 处 (宏 + app.h fw-tag)
- [[feedback_test_on_production_code]] — 测试绑生产代码 .cpp 跑 (host test), 不写 Python 模拟
- `docs/superpowers/specs/2026-06-22-dtac-dcs255-design.md` — v4.0.22 spec (SCA 部分修, 本 spec 不回滚)
