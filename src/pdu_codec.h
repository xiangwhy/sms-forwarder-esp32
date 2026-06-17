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

}  // namespace pdu