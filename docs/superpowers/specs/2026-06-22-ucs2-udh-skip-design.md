# UCS-2 / 8-bit decode 路径加 UDH skip — 真机复现 dtacIR 9 段 concat 全乱码

## Context

2026-06-22 翔哥 +66813079348 真机复现 dtacIR 推送乱码 (不是 [[dtac-gateway-anomaly]] 6/20 锁死的 gateway 推错编码 case):

设备收到的 dtac 推广短信 (`Welcome to China, enjoy worry free roaming experience abroad, subscribe to data roaming package GO Travel package...`) 是 **9 段 concat SMS** (refId=0xBB, total=9, seq=1..5+),device 把每段当 **UCS-2 BE** decode,但 **没 skip UDH 头 6 bytes**,被当 codepoint 输出成 `Ԁλँ/ं/ः/ऄ/अ` 等乱码字符。后续 ASCII 凑巧在 UCS-2 BE 解读下还是 ASCII (`XX 00` → U+00XX),所以看着像"拼接完整"。

**根因**: `src/main.cpp:1974` UCS-2 decode path:
```cpp
bodyN = pdu::decode_body_field(udHex, udFullHexLen, bodyBuf, sizeof(bodyBuf));
```
`udHex` 没 skip UDH concat IE 头。同理 `src/main.cpp:1962` 8-bit raw path。7-bit path (line 1941-1943) `numChars -= 7` 只对 packed 7-bit 有效,UCS-2/8-bit 没通用 skip。

**对比 [[dtac-gateway-anomaly]] 6/20**:
- gateway anomaly: dtac gateway 推**真异常 bytes** (0x10/0xFE 高 septet),device 7-bit decode 正确,按 working-code-no-touch **不修 sniff**
- 本 case: concat IE 头 6 bytes 是**正常**标准 UDH,device UCS-2 decode path **不 skip UDH 是真 bug**,要修

**对比 v4.0.23 strict-DCS 撤回 (`ed6c21e`)**:
- strict-DCS: 删 sniff + 严格信 DCS + 错位/reserved 丢 body → 行为破坏 (PASS 变 FAIL),翔哥 2026-06-22 10:55 拍板撤回
- 本 spec: 不改 sniff,不改 DCS 信道,**只在 UCS-2/8-bit decode 路径加 UDH skip** → 行为修复 (concat SMS 干净输出),无 regression 风险

## v4.0.23 当前状态

- 实物 v4.0.23 boot #135 (内存优化版 `b4027ce`+`889f3e0`+`441d343`+`35ae69c`) 已烧
- spec withdraw `ed6c21e` 留历史 reference
- 本 spec 修的是**完全不同 bug**(UCS-2 UDH skip vs strict-DCS drop body),跟 v4.0.23 strict-DCS 撤回不冲突

## 翔哥推送样本 (2026-06-22 真机复现)

```
ԀλँWelcome to China, enjoy worry free roaming experience abroad, subsc
Ԁλंribe to data roaming package GO Travel package without any network
Ԁλःselection: GO Travel (ASIA-AUS) 299 baht/4 GB/3 days - dial *118
Ԁλऄ*0299#  GO Travel (ASIA-AUS) 449 baht/8 GB/10 days - dial '*118*044
Ԁλअ9#  GO Travel (ASIA-AUS) 649 baht/16 GB/10 days - dial *118*0649# GO
@D£(ù@ò@t@<¥@è¡9@¿£(ù¿Σ@i@è¥¡ @Δ@l£xù Ω@c@Λ¥@è@9@Ü£òù Ξ@i@8¥pø@Δ@¿£òù¿Σ@k@è¥pø¡2@¡@8ì Ω@t@ ¥p ¡:@h£@$ Σ@n@d¥@è@7@J£ ì¿Æ@
```

前 5 条 `Ԁλँ/ं/ः/ऄ/अ` 都是 UCS-2 BE 解读 concat IE 头 `05 00 03 BB 09 NN` (UDHL=5, 8-bit concat IE: IEI=00 IEDL=03 refId=BB total=09 seq=NN) 输出的乱码字符。第 6 条是 dtac gateway 真异常 bytes case (跟 [[dtac-gateway-anomaly]] 同源,不修)。

完整拼接:
```
Welcome to China, enjoy worry free roaming experience abroad,
subscribe to data roaming package GO Travel package without any
network selection:
GO Travel (ASIA-AUS) 299 baht/4 GB/3 days - dial *118*0299#
GO Travel (ASIA-AUS) 449 baht/8 GB/10 days - dial '*118*0449#
GO Travel (ASIA-AUS) 649 baht/16 GB/10 days - dial *118*0649# GO
...
```
(共 9 段,翔哥拿到 5 段 + 1 段 gateway anomaly 异常)

## Root cause

`src/main.cpp:1794-1985` `sms_task` decode 路径:

```cpp
// line 1863-1893: 算 udHexOff
if (udHexOff > 0) {
  size_t availBytes = (bodyHexLen - udHexOff) / 2;
  if (udBytes > availBytes) udBytes = availBytes;
  udHexLen = udBytes * 2;
  udFullHexLen = bodyHexLen - udHexOff;  // ← sniff 仍看 full UD (剩余 PDU hex)
} else {
  udHexOff = 0; udBytes = bodyBytes; ...
}
const char* udHex = msg.body_hex + udHexOff;  // ← udHex 是 UD 起点 (含 UDH 头!)
```

`udHex` 跳过的是 **PDU header** (SCA+FO+OA+PID+DCS+SCTS+UDL),**没跳过 UD 内部的 UDH 头** (concat IE 在 UD 开头)。

```cpp
// line 1973-1975: UCS-2 decode
} else {  // isUcs2
  bodyN = pdu::decode_body_field(udHex, udFullHexLen, bodyBuf, sizeof(bodyBuf));
  //          ↑ udHex 含 UDH 头 → UDH 字节被当 UCS-2 codepoint
}
```

同理 8-bit raw path (line 1962) `for (size_t i = 0; i + 1 < udHexLen; ...)` 也是从 `udHex` 开始读,含 UDH 头。

7-bit path (line 1941-1943) `numChars -= 7` 仅对 packed 7-bit 有效 (UDH 占 packed 7 septets),UCS-2/8-bit 不通用。

## 决策 (按 spec brainstorming)

| 决策 | 选项 | 选择 | 理由 |
|---|---|---|---|
| 修法 | 1 加 UDH skip helper / 2 caller 自算 / 3 改 `decode_body_field` / 8-bit raw / `decode_7bit_packed` 签名 | **1 加 UDH skip helper** | caller 统一调 `pdu::pdu_udh_offset_ex` 算 udhSkip,所有 decode path 用 skip 后的 udHex。改动集中,好测好审 |
| helper 位置 | 1 加 pdu_codec.cpp / 2 main.cpp 静态函数 / 3 reuse 现有 `pdu_udh_offset` | **1 加 pdu_codec.cpp `pdu_udh_offset_ex`** | 现有 `pdu_udh_offset` 只对 concat IE 有效 (反查 "0804" / "0003"), 加 `pdu_udh_offset_ex` 支持全 UDHL 扫描 + 单 IE fallback,处理 16-bit concat (`08 04`) + 8-bit concat (`00 03`) + 任何 IEI pattern |
| skip 范围 | 1 仅 UCS-2 path / 2 UCS-2 + 8-bit / 3 全路径 | **2 UCS-2 + 8-bit** | 7-bit path `numChars -= 7` 已正确,不动。UCS-2 + 8-bit raw 都从 udHex 读 UD bytes,都要 skip UDH |
| scope | 1 仅 main.cpp decode / 2 + rx_log 推送路径 / 3 + fixture 期望调整 | **1 仅 main.cpp decode 路径** | 推送走 `SmsMsg.body_hex` 不经 `rx_log_write` 截断,推送内容跟 decode 路径共享。最小改动,scope 1 不动推送流程 |
| bump FW_VERSION | 1 bump v4.0.23 → v4.0.24 / 2 不 bump 留 v4.0.24+ | **2 不 bump** | 跟 v4.0.23 strict-DCS 撤回保持一致 (修完等拍板再 bump)。bump FW_VERSION 跟烧板决策绑,spec 阶段不 bump |
| 烧 v4.0.24 | 1 烧 / 2 等审完 | **2 等审完烧** | 按 [[feedback_code_review_before_flash]],翔哥审完 spec + TDD red + C2 diff 才烧 |

## Scope (spec 1 + 1 文件)

| # | 类别 | 文件 | 内容 |
|---|---|---|---|
| 1 | spec | `docs/superpowers/specs/2026-06-22-ucs2-udh-skip-design.md` | 本 spec |
| 2 | code | `src/pdu_codec.h` + `src/pdu_codec.cpp` | 加 `pdu_udh_offset_ex(hex, hexLen, *outUdhByteLen)` — 跟 `pdu_ud_offset_ex` 平行 API,返回 UDHL 起点 hex 偏移 + UDH 段总字节数 |
| 3 | code | `src/main.cpp:1794-1985` sms_task | 在 `udHex` 计算后,加 `udhSkip = pdu::pdu_udh_offset_ex(...)`, UCS-2 path (`main.cpp:1973-1975`) 跟 8-bit raw path (`main.cpp:1958-1972`) 用 `udHex + udhSkip` 替代 `udHex` |
| 4 | test | `tests/host/test_pdu_codec.cpp` | TDD red fixture G: concat 8-bit IE UCS-2 PDU → body 干净 (无 UDH prefix)。fixture H: concat 16-bit IE UCS-2 → 同。fixture I: concat 8-bit IE 7-bit packed → 仍干净 (7-bit path 已对,防 regression) |
| 5 | housekeeping | 无 bump | FW_VERSION 仍 `"v4.0.23"`, app.h fw-tag 仍 `"FW v4.0.23"` |

### 不实现 (本 spec out of scope)

- **不动 v4.0.23 strict-DCS 撤回决策** (`ed6c21e`) — 留历史 reference
- **不改 sniff helper** (`looks_like_ucs2_be` / `is_strict_utf8` / `pdu_ud_offset_ex`) — 跟 v4.0.23 决策一致,留供未来重启用
- **不改 `pdu_udh_offset`** — 已被 `stash_udh_part` (main.cpp:505-607) 用,不动
- **不动推送路径** (`g_pushQ` → pushplus) — 推送内容跟 decode path 共享,自动修
- **不动 7-bit decode path** (`main.cpp:1941-1943`) — `numChars -= 7` 已对
- **不动 dtac-gateway-anomaly case** (第 6 条完全乱码) — gateway 推错编码,device decode 正确,按 working-code-no-touch 不修
- **不动 v4.0.11.21 review 7 finding** — 按 working-code-no-touch 暂不修
- **不 bump FW_VERSION** — 留 v4.0.24+ 等审完烧板决策

## API 设计

### `pdu_udh_offset_ex` (新加 pdu_codec)

```cpp
// 从 UD 起点 (已 skip SCA+FO+OA+PID+DCS+SCTS+UDL) 反查 UDH 头长度
// 输入: udHex = UD 起点 hex 字符串, udHexLen = UD hex 长度
// 输出: *outUdhByteLen = UDH 段总字节数 (UDHL + IEs, 即 UDHL + 1 + IE bytes)
// 返回: UD 起点后 skip UDH 的 hex 偏移 (UD 真正数据起点)
//       0 = 没找到 UDH (单条 SMS 没 concat IE 或 PDU 格式错, caller 走 fallback)
//
// 逻辑:
//   1. 读 UDHL byte (udHex[0..2] hex → byte)
//   2. 检查 UDHL <= udHexLen/2 - 1 (防 UDH 比 UD 还长)
//   3. *outUdhByteLen = UDHL + 1
//   4. 返回 (*outUdhByteLen) * 2  (UD 真正起点 hex 偏移)
//   0 失败 (UDHL byte > 0xFF 或 UDH 比 UD 长)
size_t pdu_udh_offset_ex(const char* udHex, size_t udHexLen, size_t* outUdhByteLen);
```

**复用现有 `pdu_udh_offset`** (concat IE 反查 "0804"/"0003" pattern) 是可选优化,1st pass 简化版可只读 UDHL byte,**但 caller 仍负责 sniff UDH 是否真存在**(UDHL byte = 0 = 无 UDH, 直接 skip 0 字节)。

### main.cpp caller 改动

```cpp
// 旧 (line 1894):
const char* udHex = msg.body_hex + udHexOff;
size_t udHexLen = udBytes * 2;
size_t udFullHexLen = bodyHexLen - udHexOff;

// 新 (在 udHex 计算后):
const char* udHex = msg.body_hex + udHexOff;
size_t udHexLen = udBytes * 2;
size_t udFullHexLen = bodyHexLen - udHexOff;
// v4.0.24 fix: UDH skip for UCS-2 / 8-bit raw path
//   7-bit path numChars-=7 已对 (UDH 占 packed 7 septets), UCS-2/8-bit 没通用 skip
//   不动 sniff (looks_like_ucs2_be / is_strict_utf8 / pdu_ud_offset_ex) — 跟 ed6c21e 决策一致
//   不动 stash_udh_part (concat 拼接已对) — pdu_udh_offset 跟 pdu_udh_offset_ex 平行,共用
size_t udhByteLen = 0;
size_t udhSkipHex = pdu::pdu_udh_offset_ex(udHex, udFullHexLen, &udhByteLen);
const char* dataHex = udHex + udhSkipHex;
size_t dataHexLen = udFullHexLen - udhSkipHex;
size_t dataByteLen = udhByteLen > 0 ? (udBytes - udhByteLen) : udBytes;
// sniff 看 full UD (跟 v4.0.20.2 fix 一致, 不动)
```

然后 line 1958-1975 改 `dataHex` / `dataHexLen` / `dataByteLen` 替代 `udHex` / `udHexLen` / `udBytes`:

```cpp
if (is7bit) {
  // 7-bit path: numChars 仍从 cmt_length 算, UDH skip 体现在 numChars-=7 (已对)
  //              udHex 给 decode_7bit_packed 内部会处理
  bodyN = pdu::decode_7bit_packed(udHex, udFullHexLen, numChars, ...);
} else if (is8bitData) {
  // 8-bit raw: 改用 dataHex + dataByteLen
  for (size_t i = 0; i + 1 < dataHexLen && bodyN < sizeof(bodyBuf) - 1; i += 2) {
    ...
  }
} else {
  // UCS-2: 改用 dataHex + dataHexLen
  bodyN = pdu::decode_body_field(dataHex, dataHexLen, bodyBuf, sizeof(bodyBuf));
}
```

## Test plan

### TDD red (新加 fixture)

| Fixture | 输入 | 期望 (v4.0.24 fix) |
|---|---|---|
| G: concat 8-bit IE + UCS-2 | PDU 含 `05 00 03 BB 09 01` (UDHL=5, 8-bit concat IE),UD body 是 UCS-2 BE 编码英文 | body = "Welcome..." (干净无 `Ԁλँ` prefix) |
| H: concat 16-bit IE + UCS-2 | PDU 含 `06 08 04 BB 09 01` (UDHL=6, 16-bit concat IE),UD body 是 UCS-2 BE | body = "Welcome..." (干净无乱码 prefix) |
| I: concat 8-bit IE + 7-bit | PDU 含 `05 00 03 BB 09 01`,UD body 是 7-bit packed 英文 | body = "Welcome..." (干净,7-bit path 仍对) |
| J: 单条 SMS (无 UDH) + UCS-2 | UDHL byte = 0x00 (单条无 UDH),UD body UCS-2 | body = "Welcome..." (udhSkip=0, 跟 v4.0.23 行为一致) |

### 历史 regression (跟 v4.0.22 baseline 290/0 比)

| Fixture | v4.0.23 期望 | v4.0.24 期望 |
|---|---|---|
| A: SCA 部分修 | SCA leak check + n>30 PASS | **不变** (fixture A 是 UDH 字节污染 + 字节对齐偏移, v4.0.24 加 UDH skip 后, fixture A 期望可能变, 待审) |
| B: 单段全绿 | udOff 信任 + 无 SCA leak | **不变** |
| C: dtac/TRUE 2 段 | udOff+udBytes*2==bodyLen 不触发 fallback | **不变** |
| D/E/F: TDD red strict-DCS | body="" PASS (不实施 strict-DCS, 跑 v4.0.23 fn 仍 bodyN 解析, fixture D/E/F 是 v4.0.23 strict-DCS spec 的 TDD red, 不适用本 spec) | 不跑 (留历史 reference) |
| G/H/I/J: 新加 TDD red | (本 spec 加) | body 干净 PASS |

**风险**: Fixture A 可能从 PASS 变 FAIL (因为 v4.0.24 加 UDH skip 后, UDH 字节不再被当 SCA leak check 的"污染源", 期望可能调整)。需 TDD red 跑后审 fixture A 期望是否要 amend。

### 烧板验证 (按 [[feedback_code_review_before_flash]], 仅当翔哥拍板烧时执行)

1. `pio run -t upload` SUCCESS
2. `/api/status` → `"fw":"v4.0.23"` ✓ (不 bump)
3. **真机复现** (按 [[feedback_working_code_no_touch]]): 翔哥 +66813079348 触发 dtacIR 推一条真 concat SMS → `/api/recent` 应见 body 干净无 `Ԁλँ` prefix, 推送也干净
4. /api/recent 单条 SMS (无 UDH) 不变, 7-bit decode 不变

## 风险

- **Fixture A regression**: v4.0.24 加 UDH skip 后, fixture A (UDH 字节污染 + 字节对齐偏移) 期望可能需 amend。审 fixture A 是否从 PASS 变 FAIL, 翔哥拍板 amend 或接受。
- **UDHL byte 0x00 误判**: 单条 SMS 没 UDH 时 UDHL=0, `pdu_udh_offset_ex` 返回 0 (skip 0 字节), 行为跟 v4.0.23 一致, 0 risk。
- **多 IE UDH (UDHL > 5)**: UDHL 任意长, helper 都正确 skip。0 risk。
- **DTAC gateway anomaly case (第 6 条)**: 完全乱码, gateway 推错编码, device decode 正确, **不修**, 跟 working-code-no-touch + dtac-gateway-anomaly 一致。
- **行为变更**: v4.0.23 → v4.0.24, UCS-2/8-bit concat SMS 干净输出。0 副作用 (单条 SMS / 7-bit / 已 stash 拼接都不动)。

## 相关 memory / 引用

- [[dtac-gateway-anomaly]] — 2026-06-20 DTAC gateway 推错编码 case, **不要改 sniff**
- [[feedback-working-code-no-touch]] — 功能稳不动, 真机复现要修 (本次是翔哥真机复现 → 修)
- [[feedback_code_review_before_flash]] — 审完代码批量改再烧
- [[feedback-bump-fw-version]] — 改代码 bump 2 处, 修完不烧不 bump
- [[feedback_test_on_production_code]] — host test 优先, 不写 Python 模拟
- [[v4.0.23-strict-DCS-withdraw (`ed6c21e`)]] — strict-DCS 撤回决策, 跟本 spec 方向不同 (本 spec 修 UCS-2 UDH skip, 不改 sniff)
- `docs/superpowers/specs/2026-06-22-v4-0-23-strict-dcs-design.md` — 历史 reference
- `docs/superpowers/specs/2026-06-22-v4-0-23-withdraw.md` — 历史 reference

## Outcome (2026-06-23 TDD red → C2 → green)

| 阶段 | host test | 备注 |
|---|---|---|
| v4.0.23 baseline | 290 PASS / 0 FAIL | 现状 (实物 v4.0.23 内存优化版 Boot #135 已烧) |
| TDD red (加 fixture G/H/I) | 295 PASS / 7 FAIL | G/H FAIL = concat IE 没 skip UDH 的 bug 触发, I PASS = regression baseline |
| C2 改 (pdu_udh_offset_ex + caller skip) | **299 PASS / 3 FAIL** | G/H/I 全 PASS, Fixture A 无 regression (4 PASS 不变), D/E/F 仍 FAIL (v4.0.23 strict-DCS 撤回决策, 历史 reference 正确状态) |
| ESP32 build | SUCCESS 22.61s | RAM 21.1% / Flash 36.8% (无变化, 改动 < 100 行) |

**改动文件**:
- `src/pdu_codec.h` 加 `pdu_udh_offset_ex` 声明 (~20 行注释 + 1 行声明)
- `src/pdu_codec.cpp` 加 `pdu_udh_offset_ex` 实现 (~40 行)
- `src/main.cpp:1894` 加 caller UDH skip (~15 行) + 改 UCS-2/8-bit raw path 用 `dataHex`/`dataHexLen` (~10 行)
- `tests/host/test_pdu_codec.cpp` 加 fixture G/H/I (~95 行) + helper `fixture_sniffer_decode` 加 skip (~10 行) + main() 注册 3 行

**Fixture A 风险验证**: v4.0.24 加 skip UDH 7 bytes (concat IE 头) 后, fixture A (dtac 3 段 concat OTP=5481, refId=0xbcb4) SCA leak check + n>30 PASS 不变 (SCA leak 在更早 udHexOff skip, n>30 因 UD 仍 62 bytes UCS-2 = 31 chars > 30)。无 regression。

**烧板决策**: 待翔哥 2026-06-23 拍板 (按 [[feedback_code_review_before_flash]] 审 diff → 烧 → 真机复现)。bump FW_VERSION 决策同步拍板 (v4.0.23 不 bump vs v4.0.24 bump)。