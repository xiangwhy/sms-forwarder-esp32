#pragma once
// PDU 解码: 纯 C++, 无 Arduino / ESP-IDF 依赖,可在 PC 上编译跑 host test
// 用于把 src/main.cpp 里的短信解码逻辑抽出, 解决"Python 测试与生产代码不同源"的盲区
//
// 签名约定: 入参 const char* + size_t, 出参写满 caller 提供的 buffer, 返回写入字节数
// buffer 不够时返回已写字节数 (无截断指示符, caller 自己保证 buf 足够大)

#include <stddef.h>
#include <stdint.h>

namespace pdu {

// UCS2 BE hex 串 → UTF-8 字节
// 4 hex 字符 → 1 codepoint, 跳过 null (u==0) 和 surrogate (0xD800-0xDFFF)
// 需要 out 至少 hexLen/2 + 1 字节 (UTF-8 可能比 UCS2 短, 留余)
// 返回写入字节数
size_t ucs2_hex_to_utf8(const char* hex, size_t hexLen,
                        char* out, size_t outLen);

// 解 phone 字段: 含 ASCII-safe 字符 (数字/+/-/空格/字母) → 原样
// 否则当 UCS2 hex 解。修短号 "10086910" 误判为 hex 解出乱码
// 返回写入字节数, 0 = 入参空
size_t decode_phone_field(const char* raw, size_t rawLen,
                          char* out, size_t outLen);

// 解 body 字段: ML307 在 UCS2 模式下 body 是 hex, 但 ASCII 字符仍原样
// 字符级扫描: 4 个连续 hex 字符 → UCS2 → UTF-8, 否则 1 字符原样保留
// 返回写入字节数
size_t decode_body_field(const char* raw, size_t rawLen,
                         char* out, size_t outLen);

// UDH 长短信 ref/total/seq 解析
// 找 "0804" (16-bit concat IEI+IEDL) 或 "0003" (8-bit concat)
// 验证 total ∈ [2,8] 且 seq ∈ [1,total]
// 返回 true = 解析成功
bool parse_udh(const char* line, int& refId, int& total, int& seq);

// GSM 7-bit packed 解码 (3GPP TS 23.038 §6.2.1 default alphabet)
// hex → 7-bit unpack → GSM 03.38 → UTF-8
// numChars: 实际字符数 (CMT head 的 length 字段, 7-bit 模式下是 septets)
// 返回写入字节数
size_t decode_7bit_packed(const char* hex, size_t hexLen, size_t numChars,
                         char* out, size_t outLen);

// 严格 UTF-8 校验 — 合法 GSM7 decode 输出必合法 UTF-8
// DCS=0 但实际是 UCS-2 (gateway 标错, 如泰文 0E23...) 当 7-bit 解会出无效 UTF-8
// 返回 true = buf 是合法 UTF-8, false = 大概率不是合法 7-bit 输出, 应 fallback UCS-2
bool is_strict_utf8(const char* buf, size_t n);

// Sniff raw body hex 是否呈 UCS-2 BE 模式
// 用于: DCS 标 reserved/未知 (0x0C-0x0F, 0x1C-0x1F, 0xFF 等) 时, 决定走 7-bit 还是 UCS-2
// 标准: 偶数字节 + 高字节落在 BMP 高频区 (0x0E-0x0F 泰文 / 0x4E-0x9F CJK / 0x34-0x4B CJK 扩展 A)
//       至少 4 对字符 (8 字节 / 8 UCS-2 字符) + 80% 命中
// 返回 true = 大概率 UCS-2 BE, false = 不是 UCS-2
bool looks_like_ucs2_be(const char* hex, size_t hexLen);

// 从完整 PDU (SCA+FO+OA+PID+DCS+SCTS+UDL+UD) hex 跳过头部, 返回 UD 起始 (hex 偏移)
// *outUdByteLen: UD 字节数 (7-bit 按 ceil(UDL*7/8), 8-bit/UCS-2 按 UDL)
// is7bit: DCS 是否 7-bit, 决定 UDL 单位 (septets vs bytes)
// 返回 0 = PDU 太短/格式错 (caller 兜底用全 body 当 UD)
size_t pdu_ud_offset(const char* hex, size_t hexLen, bool is7bit, size_t* outUdByteLen);

// v4.0.11: 同 pdu_ud_offset, 但 DCS 自动从 TPDU DCS byte 读 (不信 +CMT 头 dcs 字段)
// *outIsUcs2: true=UCS-2 (DCS=0x08 或 reserved+sniff 命中), false=7-bit 或 8-bit
//             caller 据此选 ucs2_hex_to_utf8 / decode_7bit_packed / 原样
// *outIs7bit: DCS=0x00 才是真 7-bit; 其他按 8-bit/UCS-2 算 (octets=UDL)
// *outUdByteLen: UD 字节数 (7-bit 按 ceil(UDL*7/8), 其他按 UDL)
// 返回 0 = PDU 太短/格式错
size_t pdu_ud_offset_ex(const char* hex, size_t hexLen,
                       bool* outIsUcs2, bool* outIs7bit, size_t* outUdByteLen);

// 从完整 PDU 跳过 SCA+FO+OA+PID+DCS+SCTS+UDL+UDHL+UDH, 返回 UD 起点 (hex 偏移)
// *outUdhByteLen: UDH 段总字节数 (UDHL byte 1 + IEs bytes = UDHL+1), 仅诊断用
// 返回 0 = PDU 太短/格式错 (caller 兜底用全 body 当 UD)
// 配套 stash_udh_part: 用于切出 concat SMS part 的纯 UD body (剥 UDH 头)
// 修前 bug: 旧版本返回 UDHL byte 位置 + 让 caller 算 udhSkip = udhOff + udhBytes*2,容易算错 (v4.0.9)
//   现版本直接返回 UD 起点, caller 直接用 udhOff 作为 partBody 偏移
size_t pdu_udh_offset(const char* hex, size_t hexLen, size_t* outUdhByteLen);

// v4.0.24: 从 UD 起点 (已 skip PDU header) 算 UDH 头长度, 返回 UD 真正数据起点 (hex 偏移)
// *outUdhByteLen: UDH 段总字节数 (UDHL byte + IE bytes, 等于 (UDHL+1))
// 输入: udHex = UD 起点 hex (跟 pdu_ud_offset_ex 返回的 udHex 一致), udHexLen = UD 段 hex 长度
// 返回: UD 真正数据起点 hex 偏移 (skip UDH 头长度)
//       0 = UDHL byte = 0 (单条 SMS 无 UDH) 或 PDU 太短 (UDHL byte 后 bytes 不够)
//       caller 直接用 dataHex = udHex + retVal, dataHexLen = udHexLen - retVal
// 配套 main.cpp:1973-1975 UCS-2 decode + main.cpp:1958-1972 8-bit raw path: 这 2 个 path
//   之前从 udHex 直接解, 没 skip UDH concat IE 头 → IE `05 00 03 BB 09 NN` 6 bytes
//   被当 UCS-2 codepoint 输出成 `Ԁλँ` 字符 (翔哥 2026-06-22 真机复现 dtacIR 9 段 concat case)
//   加 UDH skip 后, IE 头不再被当 codepoint, concat SMS 输出干净
// 7-bit path (main.cpp:1941-1943 numChars -= 7) 已对 (UDH 占 packed 7 septets), 不动
// 配套 stash_udh_part (concat 拼接) 用 pdu_udh_offset (跟 pdu_udh_offset_ex 平行, 共存)
size_t pdu_udh_offset_ex(const char* udHex, size_t udHexLen, size_t* outUdhByteLen);

// 从完整 PDU (SCA+FO+OA+PID+...) hex 跳过 SCA+FO, 返回 OA 起始 (hex 偏移)
// *outIsAlpha: true=TON=alphanumeric(GSM7 packed), false=numeric(BCD nibble swap)
// *outValueOctets: OA value 段 octet 数 (= ceil(oaLen/2))
//   alphanumeric 时实际 nchars = floor(octets*8/7) — oaLen 字段 ML307 写错不可信
// 返回 0 = PDU 太短/格式错
size_t pdu_oa_offset(const char* hex, size_t hexLen, bool* outIsAlpha, size_t* outValueOctets);

// GSM 7-bit packed → UTF-8, 用于 OA alphanumeric sender (DTAC/AIS/TRUE/Verify 等)
// LSB-first 7-bit unpack + GSM 03.38 default alphabet (basic char set only, 不含 0x1B escape extension)
// octets: 原始 octets 指针 (caller 保证 ≥ ceil(nchars*7/8) bytes)
// octetCount: octets 缓冲实际可读字节数 (防越界; 来自 pdu_oa_offset 的 outValueOctets)
// nchars: 字符数 (caller 已知, alphanumeric 时由 floor(octets*8/7) 反算得最大值)
// 返回写入字节数 (自动 trim 末尾 GSM7 '@' 字符, 即 padding 0x00 septet — 业务名不会以 '@' 结尾,
//  安全; 真 sender 4 chars 装 6 octets → 解 6 chars → trim "@@" → 4 chars)
size_t decode_gsm7_alpha_oa(const char* octets, size_t octetCount, size_t nchars,
                            char* out, size_t outLen);

// =================== 发送侧 (v4.0.6+) ===================

// UCS2 编码 (发送): UTF-8 → UCS-2 BE hex (1 codepoint = 4 hex 字符)
// 例: "Hi" → "00480069"; "你好" → "4F60597D"; "สวัสดี" → "0E2A0E27..."
// 写入 ucs2_hex_out (null-terminated, 大写 hex), 需要 out_cap >= 4*utf8_chars + 1
// 返回写入的 hex 字符数 (不含 \0); -1 = 输入无效 (不合法 UTF-8 截断 / buffer 不够)
int ucs2_encode(const char* utf8, char* ucs2_hex_out, size_t out_cap);

// 一段长短信在原字符流中的位置 (输出给 sms_split_for_send)
// offset_chars / len_chars 都是以"UCS2 字符"为单位 (1 char = 4 hex)
struct SmsPart {
  int offset_chars;
  int len_chars;
};

// UCS2 字符流拆段 (发送): total_chars 个字符按 max_chars_per_seg 拆
// max_chars_per_seg: 70 单条 / 67 拼接 (UDH 占 6 字符)
// max_parts: 缓冲数组长度 (本项目固定 8, GSM 协议最大 255)
// 返回段数; 0 = 太多段 (> max_parts 或 > 255 GSM 上限) — caller 应 413
int sms_split_for_send(int total_chars, int max_chars_per_seg,
                       SmsPart out_parts[], int max_parts);

// BCD 翻转编码 (GSM 04.08 §10.5.4.7), 写入 TPDU 的 DA 字段
// 输入 phone_in: "13800001234" / "+8613800001234" / "138-0000-1234"
//   '+' '-' ' ' 静默剥除; 其它非数字 → 拒绝 (-1)
// 输出 out: 大写 hex BCD, null-terminated. 例: "13800001234" → "3108000010324F"
//   末位奇数 → 高 nibble = F (填充)
// 返回 decimal digit count (10/11/12...) — DA length 字段按 GSM 03.40 §9.1.2.5 用此值, 不算半字节
// 例: "13800001234" (11 位) → 写入 12 hex char, 返回 11
//     "1380000123"  (10 位) → 写入 10 hex char, 返回 10
// 返回 -1 = phone_in/out 空 / 长度 0 或 > 20 / 含非法字符
int bcd_encode_phone(const char* phone_in, char* out, size_t out_cap);

// 构造 CMGS TPDU hex (不含 SCA, 从 PDU-type 开始, 大写 hex, null-terminated)
// phone: "13800001234" / "+8613800001234"
// body:  UTF-8 短信内容
// seg_idx: < 0 → 单条 (≤70 UCS-2 char, 不带 UDH)
//         >= 0 → 拼接第 seg_idx 段, parts[]/total 由 sms_split_for_send 输出
// ref:    8-bit concat reference (caller 自选, 避免 +CMS ERROR 331 重复)
// pdu_out / out_cap: 输出缓冲 (建议 ≥ 600 字节)
// 返: 写入 hex 字符数 (UDL 那段按 byte 数算); -1 = 失败 (phone/body/pdu_out 空 / 缓冲不够 / UTF-8 非法)
//
// 布局 (hex chars):
//   PDU-type(2) MR(2) DA-len(2) ToA(2) DA-digits(2*da_digits) PID(2) DCS(2) VP(2)
//   [UDH(10) if concat] UDL(2) UD-hex(2*UD_bytes)
//
// ToA 固定 0x81 (unknown, ISDN); 国际号需 0x91 + 剥前缀 — v4.0.6 范围外, 留 P1 (F4)
int cmgs_build_pdu(const char* phone, const char* body,
                   int seg_idx, const SmsPart parts[], int total,
                   uint8_t ref, char* pdu_out, size_t out_cap);

}  // namespace pdu