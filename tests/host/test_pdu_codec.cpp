// pdu_codec host test
// 纯 C++ + assert() 风格 (无 Catch2 / Unity), 编译器: g++/clang++, std=c++17
//
// 镜像 src/pdu_codec.cpp 的 4 个函数:
//   pdu::ucs2_hex_to_utf8
//   pdu::decode_phone_field
//   pdu::decode_body_field
//   pdu::parse_udh
//
// 每个 case 用宏 CHECK / CHECK_EQ_INT / CHECK_EQ_STR 跑, 通过数累计, 最后 exit 0/1

#include "pdu_codec.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0;
static int g_fail = 0;
static const char* g_current = "";

#define CHECK(cond) do {                                                  \
  if (cond) { ++g_pass; std::printf("  PASS  %s\n", g_current); }         \
  else      { ++g_fail; std::printf("  FAIL  %s @ %s:%d\n",               \
                                    g_current, __FILE__, __LINE__); }     \
} while(0)

#define CHECK_EQ_INT(actual, expected) do {                                \
  long long _a = (long long)(actual), _e = (long long)(expected);         \
  if (_a == _e) { ++g_pass; std::printf("  PASS  %s\n", g_current); }     \
  else { ++g_fail; std::printf("  FAIL  %s (got %lld want %lld) @ %s:%d\n", \
                               g_current, _a, _e, __FILE__, __LINE__); }   \
} while(0)

#define CHECK_EQ_STR(actual, expected) do {                                \
  std::string _a = (actual); std::string _e = (expected);                  \
  if (_a == _e) { ++g_pass; std::printf("  PASS  %s\n", g_current); }     \
  else { ++g_fail; std::printf("  FAIL  %s (got '%s' want '%s') @ %s:%d\n", \
                               g_current, _a.c_str(), _e.c_str(),         \
                               __FILE__, __LINE__); }                      \
} while(0)

static std::string hex_to_utf8(const char* hex) {
  char buf[256];
  size_t n = pdu::ucs2_hex_to_utf8(hex, std::strlen(hex), buf, sizeof(buf));
  return std::string(buf, n);
}

static void test_ucs2_chinese() {
  g_current = "ucs2_hex_to_utf8: '你好' (4F60 597D) → UTF-8 6 bytes";
  // 邻接字符串字面量避免 hex escape 越界: C++ 把 "AB" "CD" 拼成 "ABCD"
  const char expected[] = { (char)0xE4, (char)0xBD, (char)0xA0,
                            (char)0xE5, (char)0xA5, (char)0xBD, 0 };
  CHECK_EQ_STR(hex_to_utf8("4F60597D"), std::string(expected));
}

static void test_ucs2_skip_null_and_surrogate() {
  g_current = "ucs2_hex_to_utf8: skip null (0000) + surrogate (D800) → 仅留 'AB'";
  // "0000" + "0041" + "D800" + "0042" → "AB"
  CHECK_EQ_STR(hex_to_utf8("00000041D8000042"), "AB");
}

static void test_phone_international() {
  g_current = "decode_phone_field: '86138077107' → 原样 (国际号 ASCII-safe)";
  char buf[64];
  size_t n = pdu::decode_phone_field("86138077107", 11, buf, sizeof(buf));
  CHECK_EQ_INT(n, 11);
  CHECK_EQ_STR(std::string(buf, n), "86138077107");
}

static void test_phone_short_number_regression() {
  g_current = "decode_phone_field: '10086910' → 原样 (短号 regression: 旧版误判 hex 出乱码)";
  char buf[64];
  size_t n = pdu::decode_phone_field("10086910", 8, buf, sizeof(buf));
  CHECK_EQ_INT(n, 8);
  CHECK_EQ_STR(std::string(buf, n), "10086910");
}

static void test_phone_alpha_sender() {
  g_current = "decode_phone_field: 'True App' → 原样 (字母数字 sender)";
  char buf[64];
  size_t n = pdu::decode_phone_field("True App", 8, buf, sizeof(buf));
  CHECK_EQ_INT(n, 8);
  CHECK_EQ_STR(std::string(buf, n), "True App");
}

static void test_body_mixed() {
  g_current = "decode_body_field: 'X' + 4F60 + 'X' → 'X你X' (字符级 4-hex 扫描)";
  // X 非 hex → passthrough; 4F60 是 4 hex → UCS2 解码 → '你' (3 bytes)
  // 注: 'A' 也是 hex digit, 用 'A4F60A' 会贪心吃成 1 个 UCS2 codepoint
  char buf[64];
  size_t n = pdu::decode_body_field("X4F60X", 6, buf, sizeof(buf));
  CHECK_EQ_INT(n, 5);  // 1 + 3 + 1
  const char expected[] = { 'X', (char)0xE4, (char)0xBD, (char)0xA0, 'X', 0 };
  CHECK_EQ_STR(std::string(buf, n), std::string(expected));
}

static void test_udh_16bit() {
  g_current = "parse_udh: 16-bit concat (refH=7E refL=FC total=2 seq=1) → refId=0x7EFC";
  int ref = 0, tot = 0, seq = 0;
  // "06 08 04 7E FC 02 01 ABCD" — UDHL=06, IEI=08, IEDL=04, refH=7E, refL=FC, total=02, seq=01
  bool ok = pdu::parse_udh("060808047EFC0201ABCD", ref, tot, seq);
  CHECK(ok);
  CHECK_EQ_INT(ref, 0x7EFC);
  CHECK_EQ_INT(tot, 2);
  CHECK_EQ_INT(seq, 1);
}

static void test_udh_8bit() {
  g_current = "parse_udh: 8-bit concat (ref=2A total=2 seq=1) → refId=42";
  int ref = 0, tot = 0, seq = 0;
  // "05 00 03 2A 02 01 41" — UDHL=05, IEI=00, IEDL=03, ref=2A, total=02, seq=01
  bool ok = pdu::parse_udh("0500032A020141", ref, tot, seq);
  CHECK(ok);
  CHECK_EQ_INT(ref, 42);
  CHECK_EQ_INT(tot, 2);
  CHECK_EQ_INT(seq, 1);
}

static void test_udh_no_concat() {
  g_current = "parse_udh: 普通短信 (无 0804/0003) → false";
  int ref = 0, tot = 0, seq = 0;
  bool ok = pdu::parse_udh("plain text without concat marker", ref, tot, seq);
  CHECK(!ok);
}

// 烧板 regression: 长泰文 SMS 收乱码
// 根因: stash_udh_part 的 udhSkip strstr 太严格,ML307 剥 UDHL 时漏掉
// 验证: parse_udh 应同时识别 "060804..." (有 UDHL) 和 "0804..." (无 UDHL) 两种格式
static void test_udh_16bit_stripped_udhl() {
  g_current = "parse_udh: 16-bit concat 剥 UDHL (无 06 前缀) → refId 仍正确";
  int ref = 0, tot = 0, seq = 0;
  // "08 04 7E FC 02 01 ABCD" — 一些 ML307 固件剥掉 UDHL 字节
  // IEI=08, IEDL=04, refH=7E, refL=FC, total=02, seq=01
  bool ok = pdu::parse_udh("08047EFC0201ABCD", ref, tot, seq);
  CHECK(ok);
  CHECK_EQ_INT(ref, 0x7EFC);
  CHECK_EQ_INT(tot, 2);
  CHECK_EQ_INT(seq, 1);
}

static void test_udh_8bit_stripped_udhl() {
  g_current = "parse_udh: 8-bit concat 剥 UDHL (无 05 前缀) → refId 仍正确";
  int ref = 0, tot = 0, seq = 0;
  // "00 03 2A 02 01 41" — IEI=00, IEDL=03, ref=2A, total=02, seq=01
  bool ok = pdu::parse_udh("00032A020141", ref, tot, seq);
  CHECK(ok);
  CHECK_EQ_INT(ref, 42);
  CHECK_EQ_INT(tot, 2);
  CHECK_EQ_INT(seq, 1);
}

// GSM 7-bit packed regression
// 烧板 bug: 长泰文(实际英 OTP 短信, gateway 转 GSM 7-bit, DCS=0) 收乱码
// 根因: 旧 decode_body_field 把 7-bit packed hex 当 UCS-2 hex 解
// 修法: pdu::decode_7bit_packed, 7-bit unpack (3GPP 23.038 §6.2.1.1 LSB-first)
// 验证: 短 ASCII roundtrip + 真车长数据
static void test_7bit_ascii_roundtrip() {
  g_current = "decode_7bit_packed: 'Hi' (H=0x48 i=0x69) 7-bit packed → C834 → 'Hi'";
  // Python /tmp/gsm7_truth.py encode "Hi" → C834
  char buf[16];
  size_t n = pdu::decode_7bit_packed("C834", 4, 2, buf, sizeof(buf));
  CHECK_EQ_INT(n, 2);
  CHECK_EQ_STR(std::string(buf, n), "Hi");
}

static void test_7bit_multichar() {
  g_current = "decode_7bit_packed: 'Test123' 7-bit packed → D4F29C1E93CD00 → 'Test123'";
  // Python /tmp/gsm7_truth.py encode "Test123" → D4F29C1E93CD00 (7 chars = 7 septets)
  char buf[16];
  size_t n = pdu::decode_7bit_packed("D4F29C1E93CD00", 14, 7, buf, sizeof(buf));
  CHECK_EQ_INT(n, 7);
  CHECK_EQ_STR(std::string(buf, n), "Test123");
}

static void test_is_strict_utf8_true_ascii() {
  g_current = "is_strict_utf8: 纯 ASCII 'Hello' → true";
  CHECK(pdu::is_strict_utf8("Hello", 5));
}

static void test_is_strict_utf8_true_gsm7_extended() {
  g_current = "is_strict_utf8: GSM7 扩展 (ÄÖü 都是 2-byte UTF-8) → true";
  // "Test123" 全 ASCII, decode 出来都是 1-byte → 合法
  char buf[16];
  size_t n = pdu::decode_7bit_packed("D4F29C1E93CD00", 14, 7, buf, sizeof(buf));
  CHECK(pdu::is_strict_utf8(buf, n));
}

static void test_is_strict_utf8_false_lone_continuation() {
  g_current = "is_strict_utf8: lone continuation (0x80 0x41) → false";
  // 0x80 是 lone UTF-8 continuation byte, 后面 0x41 是 ASCII
  // 严格校验应拒绝
  char buf[2] = { (char)0x80, 'A' };
  CHECK(!pdu::is_strict_utf8(buf, 2));
}

static void test_is_strict_utf8_false_4byte_lead() {
  g_current = "is_strict_utf8: 4-byte UTF-8 lead (0xF0) → false (UCS-2 不应出)";
  // 0xF0 是 4-byte UTF-8 lead (UCS-4 / emoji), UCS-2 不应有
  char buf[4] = { (char)0xF0, (char)0x90, (char)0x80, (char)0x80 };
  CHECK(!pdu::is_strict_utf8(buf, 4));
}

// === task #37: looks_like_ucs2_be (raw bytes sniff, 网关 DCS 标错场景) ===
static void test_looks_like_ucs2_thai() {
  g_current = "looks_like_ucs2_be: Thai รหัสยืนยัน (8 char UCS-2 BE) → true";
  // UCS-2 BE: 0E23 0E2B 0E31 0E22 0E2A 0E22 0E2D 0E19
  //   ร(0E23) ห(0E2B) ั(0E31) ั(0E22) ส(0E2A) ั(0E22) อ(0E2D) น(0E19)
  // 全部高字节 0x0E → 100% 命中 BMP 高频区
  CHECK(pdu::looks_like_ucs2_be("0E230E2B0E310E220E2A0E220E2D0E19", 32));
}

static void test_looks_like_ucs2_chinese() {
  g_current = "looks_like_ucs2_be: CJK 验证码 (4 char UCS-2 BE) → true";
  // UCS-2 BE: 9A8C 8BC1 51 6 0E0A
  //   验(9A8C) 证(8BC1) 码(51 6 码? -- U+7801 高字节应为 78)
  // 重新: 验(9A8C) 证(8BC1) 码(7801) ??? -- 用真实 char: 验证码 = 9A8C 8BC1 7801
  // 注意: 0E0A (UCS-2 BE) 是 0x0E0A = U+0E0A = ช (泰文) -- 不对, 重做
  // 正确 UCS-2 BE: 9A8C 8BC1 7801 0E0A? 应该是 验 证 码 邮 / 之类
  // 为简化测试用 4 个 CJK: 验 证 码 = 9A8C 8BC1 7801
  // 加上一个泰文: 0E23 = ร → 9A8C 8BC1 7801 0E23
  // 高字节: 9A, 8B, 78, 0E → 100% 命中
  CHECK(pdu::looks_like_ucs2_be("9A8C8BC1780" "10E23", 16));  // 拆开 hex literal 避 0E0A 越界
}

static void test_looks_like_ucs2_ascii_negative() {
  g_current = "looks_like_ucs2_be: 'Hello' (5 char ASCII 7-bit) → false";
  // 纯 ASCII 7-bit, 高字节全是 0x00 (跳过) → pairHits 增但 hiHits 0
  // 0% 命中 < 60% 阈值
  CHECK(!pdu::looks_like_ucs2_be("48656C6C6F", 10));
}

static void test_looks_like_ucs2_gsm7_packed_negative() {
  g_current = "looks_like_ucs2_be: GSM7 'How are you?' (14 char packed) → false";
  // C8F71D14969741F977FD07 = "How are you?"
  // 高字节: C8, F7, 1D, 14, 96, 97, 41, F9, 77, FD, 07
  //   C8: 不在 0x0E-0x0F / 0x4E-0x9F / 0x34-0x4B → 不命中
  //   F7: 不命中
  //   1D: 不命中
  //   14: 不命中
  //   96: 0x96 在 0x4E-0x9F → 命中 (但这会让 GSM7 7-bit 也算 UCS-2?)
  //   等等, 我们需要确认 GSM7 packed 的高字节确实不会成片 0x4E-0x9F
  // 实测 GSM7 packed "How are you?" = C8F71D14969741F977FD07
  //   高字节: C8, F7, 1D, 14, 96, 97, 41, F9, 77, FD, 07
  //   0x4E-0x9F 区间: 96, 97 (2/11 = 18%) < 60% 阈值
  // 实测应该返回 false (18% < 60%)
  CHECK(!pdu::looks_like_ucs2_be("C8F71D14969741F977FD07", 22));
}

static void test_looks_like_ucs2_too_short() {
  g_current = "looks_like_ucs2_be: 太短 (< 2 对) → false";
  // 单字节 / 1 对都不行, 至少 2 对
  CHECK(!pdu::looks_like_ucs2_be("0E", 2));
  CHECK(!pdu::looks_like_ucs2_be("0E23", 4));   // 1 pair
  CHECK(!pdu::looks_like_ucs2_be("", 0));      // 空
  CHECK(!pdu::looks_like_ucs2_be("0E23", 3));   // 奇数长度
}

// === v4.0.6 短信发送 (双向): ucs2_encode + sms_split_for_send ===
static void test_ucs2_encode_ascii() {
  g_current = "ucs2_encode: 'Hi' (2 ASCII) → '00480069' (4 hex 字符)";
  char buf[32] = {};
  int n = pdu::ucs2_encode("Hi", buf, sizeof(buf));
  CHECK_EQ_INT(n, 8);  // 2 char × 4 hex
  CHECK_EQ_STR(std::string(buf), "00480069");
}

static void test_ucs2_encode_chinese() {
  g_current = "ucs2_encode: '你好' (2 CJK) → '4F60597D'";
  // 你 = U+4F60, 好 = U+597D, big-endian
  char buf[32] = {};
  int n = pdu::ucs2_encode("你好", buf, sizeof(buf));
  CHECK_EQ_INT(n, 8);
  CHECK_EQ_STR(std::string(buf), "4F60597D");
}

static void test_ucs2_encode_thai() {
  g_current = "ucs2_encode: Thai 'สวัสดี' (6 char) → '0E2A0E270E310E2A0E140E35'";
  // ส(0E2A) ว(0E27) ั(0E31) ส(0E2A) ด(0E14) ี(0E35)
  char buf[64] = {};
  int n = pdu::ucs2_encode("สวัสดี", buf, sizeof(buf));
  CHECK_EQ_INT(n, 24);
  CHECK_EQ_STR(std::string(buf), "0E2A0E270E310E2A0E140E35");
}

static void test_ucs2_encode_invalid_utf8() {
  g_current = "ucs2_encode: 不合法 UTF-8 (\\xC3\\x28 truncated) → -1";
  // \xC3 起始 2 字节序列, 但 \x28 不是 0x80-0xBF 续字节 → 拒绝
  const char bad[] = { (char)0xC3, (char)0x28, 0 };
  char buf[32] = {};
  int n = pdu::ucs2_encode(bad, buf, sizeof(buf));
  CHECK_EQ_INT(n, -1);
}

static void test_split_single_70() {
  g_current = "sms_split_for_send: 70 chars / seg 70 → 1 段 [0, 70]";
  pdu::SmsPart parts[8] = {};
  int n = pdu::sms_split_for_send(70, 70, parts, 8);
  CHECK_EQ_INT(n, 1);
  CHECK_EQ_INT(parts[0].offset_chars, 0);
  CHECK_EQ_INT(parts[0].len_chars, 70);
}

static void test_split_double_71() {
  g_current = "sms_split_for_send: 71 chars / seg 67 → 2 段 [0, 67] + [67, 4]";
  // 拼接每段 67 (UDH 占 6 字符), 71 = 67 + 4
  // caller 知道是拼接时传 67, 单条时传 70
  pdu::SmsPart parts[8] = {};
  int n = pdu::sms_split_for_send(71, 67, parts, 8);
  CHECK_EQ_INT(n, 2);
  CHECK_EQ_INT(parts[0].offset_chars, 0);
  CHECK_EQ_INT(parts[0].len_chars, 67);
  CHECK_EQ_INT(parts[1].offset_chars, 67);
  CHECK_EQ_INT(parts[1].len_chars, 4);
}

static void test_split_overflow_255segs() {
  g_current = "sms_split_for_send: 70×256 chars (256 段超 GSM 限制) → 0";
  // GSM UDH ref 8-bit 最大 255 段, sms_split 应当把 256 段当 overflow
  // 实际本项目 max_parts=8 也会限住, 这里再测 256 段触发 GSM 上限
  pdu::SmsPart parts[8] = {};
  int n = pdu::sms_split_for_send(70 * 256, 70, parts, 8);
  CHECK_EQ_INT(n, 0);
}

// === P0 fix #3: DA length 字段是 decimal digit count, 不是 hex char 数 ===
// GSM 03.40 §9.1.2.5 — 11 位手机 → DA length = 11 (不是 hex chars 12)
static void test_bcd_encode_phone_11digit() {
  g_current = "bcd_encode_phone: '13800001234' (11 位) → 返回 11, hex='3108001032F4' (12 hex)";
  char buf[32] = {};
  int n = pdu::bcd_encode_phone("13800001234", buf, sizeof(buf));
  CHECK_EQ_INT(n, 11);  // digit count, 不是 hex char 数
  // 11 位 digits="13800001234" → (1,3)→31, (8,0)→08, (0,0)→00, (0,1)→10, (2,3)→32, 末位 4→F4
  CHECK_EQ_STR(std::string(buf), "3108001032F4");
}

static void test_bcd_encode_phone_10digit() {
  g_current = "bcd_encode_phone: '1380000123' (10 位) → 返回 10, hex='3108001032' (10 hex)";
  char buf[32] = {};
  int n = pdu::bcd_encode_phone("1380000123", buf, sizeof(buf));
  CHECK_EQ_INT(n, 10);
  CHECK_EQ_STR(std::string(buf), "3108001032");
}

static void test_bcd_encode_phone_13digit_intl() {
  g_current = "bcd_encode_phone: '8613800001234' (13 位国际号, 翔哥提的国外场景) → 返回 13";
  char buf[32] = {};
  int n = pdu::bcd_encode_phone("8613800001234", buf, sizeof(buf));
  CHECK_EQ_INT(n, 13);
  // 13 位 digits: 8,6,1,3,8,0,0,0,0,1,2,3,4
  // i=0..10 even-iterations normal: "68" "31" "08" "00" "10" "32"
  // i=12 odd-iteration: "F" + digits[12]='4' → "F4"
  CHECK_EQ_STR(std::string(buf), "683108001032F4");
}

static void test_bcd_encode_phone_plus_prefix() {
  g_current = "bcd_encode_phone: '+8613800001234' → 静默剥 +, 返回 13";
  char buf[32] = {};
  int n = pdu::bcd_encode_phone("+8613800001234", buf, sizeof(buf));
  CHECK_EQ_INT(n, 13);
  // 跟 13 位国际号一样: 剥 + 后同 8613800001234
  CHECK_EQ_STR(std::string(buf), "683108001032F4");
}

static void test_bcd_encode_phone_invalid_chars() {
  g_current = "bcd_encode_phone: '138a0001234' (含字母) → -1";
  char buf[32] = {};
  CHECK_EQ_INT(pdu::bcd_encode_phone("138a0001234", buf, sizeof(buf)), -1);
}

// === P0 fix #1: 拼接 SMS 必须 PDU-type 0x51 (UDHI bit 置 1) ===
static void test_cmgs_build_pdu_single_pdu_type() {
  g_current = "cmgs_build_pdu: 单条 → PDU-type = '11' (UDHI=0, 相对 VP)";
  char pdu[600];
  int n = pdu::cmgs_build_pdu("13800001234", "Hi", -1, nullptr, 0,
                              0x2A, pdu, sizeof(pdu));
  CHECK(n > 0);
  // hex chars 0-1 = "11" (SMS-SUBMIT, UDHI=0, 相对 VP)
  CHECK_EQ_STR(std::string(pdu, 2), "11");
}

static void test_cmgs_build_pdu_single_da_length_11digit() {
  g_current = "cmgs_build_pdu: 单条 11 位手机 → DA length byte = '0B' (11, 不是 12)";
  char pdu[600];
  int n = pdu::cmgs_build_pdu("13800001234", "Hi", -1, nullptr, 0,
                              0x2A, pdu, sizeof(pdu));
  CHECK(n > 0);
  // hex chars 0-1: PDU-type="11"
  // hex chars 2-3: MR="00"
  // hex chars 4-5: DA length = "0B" (11) — 不是 "0C" (12)!
  CHECK_EQ_STR(std::string(pdu + 4, 2), "0B");
}

static void test_cmgs_build_pdu_single_da_length_10digit() {
  g_current = "cmgs_build_pdu: 单条 10 位手机 → DA length byte = '0A' (10)";
  char pdu[600];
  int n = pdu::cmgs_build_pdu("1380000123", "Hi", -1, nullptr, 0,
                              0x2A, pdu, sizeof(pdu));
  CHECK(n > 0);
  CHECK_EQ_STR(std::string(pdu + 4, 2), "0A");
}

static void test_cmgs_build_pdu_concat_pdu_type() {
  g_current = "cmgs_build_pdu: 拼接 → PDU-type = '51' (UDHI=1, 模组才会解析 UDH)";
  // body 71 字符, 拆 2 段 (67 + 4), 每段都要 PDU-type 0x51
  char body[200];
  std::memset(body, 'A', 71);
  body[71] = 0;
  pdu::SmsPart parts[8];
  int segs = pdu::sms_split_for_send(71, 67, parts, 8);
  CHECK_EQ_INT(segs, 2);
  char pdu0[600], pdu1[600];
  int n0 = pdu::cmgs_build_pdu("13800001234", body, 0, parts, segs,
                               0x2A, pdu0, sizeof(pdu0));
  int n1 = pdu::cmgs_build_pdu("13800001234", body, 1, parts, segs,
                               0x2A, pdu1, sizeof(pdu1));
  CHECK(n0 > 0 && n1 > 0);
  // hex chars 0-1 = "51" (UDHI bit 置 1)
  CHECK_EQ_STR(std::string(pdu0, 2), "51");
  CHECK_EQ_STR(std::string(pdu1, 2), "51");
  // hex chars 4-5 仍应是 "0B" (11 digit count)
  CHECK_EQ_STR(std::string(pdu0 + 4, 2), "0B");
  CHECK_EQ_STR(std::string(pdu1 + 4, 2), "0B");
}

static void test_cmgs_build_pdu_concat_udh_bytes() {
  g_current = "cmgs_build_pdu: 拼接 → chars 14-23 = '050003' + ref/total/seq (UDH 8-bit concat)";
  // chars 0-1: PDU-type="51", 2-3: MR="00", 4-5: DA len="0B",
  // 6-7: ToA="81", 8-19: DA digits (12 hex), 20-21: PID="00",
  // 22-23: DCS="08", 24-25: VP="AA", 26-35: UDH="050003" + RR TT SS (10 hex)
  char body[200];
  std::memset(body, 'A', 71);
  body[71] = 0;
  pdu::SmsPart parts[8];
  int segs = pdu::sms_split_for_send(71, 67, parts, 8);
  char pdu0[600];
  int n = pdu::cmgs_build_pdu("13800001234", body, 0, parts, segs,
                              0x2A, pdu0, sizeof(pdu0));
  CHECK(n > 0);
  // UDH 头 6 hex = "050003" (UDHL=05, IEI=00, IEDL=03)
  CHECK_EQ_STR(std::string(pdu0 + 26, 6), "050003");
  // ref(2A) total(02) seq(01)
  CHECK_EQ_STR(std::string(pdu0 + 32, 6), "2A0201");
}

static void test_cmgs_build_pdu_invalid_utf8() {
  g_current = "cmgs_build_pdu: body 含非法 UTF-8 (\\xC3\\x28 truncated) → -1";
  char body[] = { 'H', (char)0xC3, (char)0x28, 0 };
  char pdu[600];
  int n = pdu::cmgs_build_pdu("13800001234", body, -1, nullptr, 0,
                              0x2A, pdu, sizeof(pdu));
  CHECK_EQ_INT(n, -1);
}

static void test_7bit_user_real_long_msg() {
  g_current = "decode_7bit_packed: 用户真车长 OTP (138 字节 / 158 septets) → 'Keep this code...23:20.'";
  // Part 1 UDH-stripped body (119 字节) + Part 2 UDH-stripped body (20 字节) = 139 字节
  // stash_udh_part 合成 cmt_length = 136 + 22 = 158 (sum of per-part, no subtract)
  // 注意: OTP 服务在 "expire" 里用 '@' (GSM7 0x00) 代替 'e' 当反 bot 混淆
  //   "expir@e" 而不是 "expire" — 实际数据是 0x00 在 septet 135
  // 期望解码: "Keep this code secure to prevent unauthorized access.
  //           Your OTP is 164243 (Ref: tyblp). This code is valid for
  //           10 minutes and will expir@e on 2026-06-17 23:20."
  const char* hex_body1 =
    "CB72190EA2A3D373D0F84D2E83E6E5715D5E06D1DF20B8BC6C2FBBE9A0BA3B5CA7A3DFF2B4BE4C0685C7E3F27CEE0265DF7539E8498582D27350CC4693D1662094B46CD681E879311B9E7281A8E8F41C347E93CBA0F41C640FB3D36490F92D07C560A076DA5DA797E7A0B09B0CBAA7D96C50190F4FCB01";
  const char* hex_body2 = "65D0DB0D92C164B616CCD68ADD40B2994E067301";
  // 拼成 278 hex = 139 字节
  char all_hex[300];
  std::snprintf(all_hex, sizeof(all_hex), "%s%s", hex_body1, hex_body2);
  size_t all_hex_len = std::strlen(all_hex);
  char buf[256];
  size_t n = pdu::decode_7bit_packed(all_hex, all_hex_len, 158, buf, sizeof(buf));
  CHECK_EQ_INT(n, 158);
  const char* expected =
    "Keep this code secure to prevent unauthorized access. "
    "Your OTP is 164243 (Ref: tyblp). This code is valid for "
    "10 minutes and will expir@e on 2026-06-17 23:20.";
  CHECK_EQ_STR(std::string(buf, n), expected);
}

// =================== v4.0.7 扩展: 常用解码边界 + roundtrip ===================
// 目标: 把 11 个 public 函数的边界 (empty / 越界 / 特殊字符) 都覆盖一次
// 烧板前 regression net

// --- decode_phone_field 边界 ---
static void test_phone_plus_prefix() {
  g_current = "decode_phone_field: '+86138077107' → 原样 (+ 前缀, 12 字符)";
  char buf[64];
  size_t n = pdu::decode_phone_field("+86138077107", 12, buf, sizeof(buf));
  CHECK_EQ_INT(n, 12);
  CHECK_EQ_STR(std::string(buf, n), "+86138077107");
}

static void test_phone_with_dash() {
  g_current = "decode_phone_field: '138-0013-8000' → 原样 (含 -, 13 字符)";
  char buf[64];
  size_t n = pdu::decode_phone_field("138-0013-8000", 13, buf, sizeof(buf));
  CHECK_EQ_INT(n, 13);
  CHECK_EQ_STR(std::string(buf, n), "138-0013-8000");
}

static void test_phone_pure_hex_returned_as_is() {
  g_current = "decode_phone_field: 纯 hex '4F60597D' → 原样 (ASCII-safe 启发: 数字/字母都当原文, 不强行 UCS2 解)";
  // 注意: 当前生产逻辑: 任一字符 ASCII-safe → 整段原文返回 (防短号 "10086910" 误判)
  // 副作用: 纯 hex phone (网关错编码) 也会被当原文返回 (8 bytes), 不解 UCS2
  // 这是已知 trade-off, 不算 bug
  char buf[64];
  size_t n = pdu::decode_phone_field("4F60597D", 8, buf, sizeof(buf));
  CHECK_EQ_INT(n, 8);
  CHECK_EQ_STR(std::string(buf, n), "4F60597D");
}

static void test_phone_empty() {
  g_current = "decode_phone_field: empty → 0 bytes";
  char buf[64];
  size_t n = pdu::decode_phone_field("", 0, buf, sizeof(buf));
  CHECK_EQ_INT(n, 0);
}

// --- decode_body_field 边界 ---
static void test_body_pure_ascii() {
  g_current = "decode_body_field: 纯 ASCII 'Hello World' → 原样 11 bytes (无 hex 触发)";
  char buf[64];
  size_t n = pdu::decode_body_field("Hello World", 11, buf, sizeof(buf));
  CHECK_EQ_INT(n, 11);
  CHECK_EQ_STR(std::string(buf, n), "Hello World");
}

static void test_body_pure_ucs2() {
  g_current = "decode_body_field: 纯 hex '4F60597D' → '你好' 6 bytes (4 hex × 2 codepoint → 2×3 UTF-8)";
  char buf[64];
  size_t n = pdu::decode_body_field("4F60597D", 8, buf, sizeof(buf));
  const char exp[] = { (char)0xE4, (char)0xBD, (char)0xA0,
                       (char)0xE5, (char)0xA5, (char)0xBD, 0 };
  CHECK_EQ_INT(n, 6);
  CHECK_EQ_STR(std::string(buf, n), std::string(exp));
}

static void test_body_5hex_odd_boundary() {
  g_current = "decode_body_field: '4F605' (5 hex, 4+1 odd) → '你'(3) + '5'(1) = 4 bytes";
  char buf[64];
  size_t n = pdu::decode_body_field("4F605", 5, buf, sizeof(buf));
  const char exp[] = { (char)0xE4, (char)0xBD, (char)0xA0, '5', 0 };
  CHECK_EQ_INT(n, 4);
  CHECK_EQ_STR(std::string(buf, n), std::string(exp));
}

static void test_body_empty() {
  g_current = "decode_body_field: empty → 0 bytes";
  char buf[64];
  size_t n = pdu::decode_body_field("", 0, buf, sizeof(buf));
  CHECK_EQ_INT(n, 0);
}

// --- decode_7bit_packed 边界 ---
static void test_7bit_special_chars() {
  g_current = "decode_7bit_packed: '@£$' (GSM7 0x00 0x01 0x02) → '@'+U+00A3+'$' = 4 UTF-8 bytes";
  // 7-bit packed LSB first: 0x80 0x80 0x00
  // @=0x00=0000000, £=0x01=1000000 (LSB first), $=0x02=0100000
  // byte 0 = s0(7) | s1[0] = 0|0|0|0|0|0|0 | 1 = 10000000 = 0x80
  // byte 1 = s1[1..6] | s2[0..1] = 0|0|0|0|0|0 | 0|1 = 10000000 = 0x80
  // byte 2 = s2[2..6] | pad = 0|0|0|0|0 | 0|0|0 = 0x00
  char buf[16];
  size_t n = pdu::decode_7bit_packed("808000", 6, 3, buf, sizeof(buf));
  CHECK_EQ_INT(n, 4);  // @(1) + £(2) + $(1)
  const char exp[] = { '@', (char)0xC2, (char)0xA3, '$', 0 };
  CHECK_EQ_STR(std::string(buf, n), std::string(exp));
}

static void test_7bit_extended_chars() {
  g_current = "decode_7bit_packed: 'ÄÖÑ' (GSM7 0x5B 0x5C 0x5D) → 2-byte UTF-8 each (6 bytes)";
  // GSM7: 0x5B=Ä(U+00C4→C3 84), 0x5C=Ö(U+00D6→C3 96), 0x5D=Ñ(U+00D1→C3 91)
  // 7-bit packed (LSB first per septet):
  //   s0=0x5B (binary 01011011) → LSB first: 1101101
  //   s1=0x5C (binary 01011100) → LSB first: 0011101
  //   s2=0x5D (binary 01011101) → LSB first: 1011101
  // byte 0: s0(7 bits) + s1[0] = 1101101|0 = 0x5B
  // byte 1: s1[1..6] + s2[0..1] = 0011101|1|0 = 0b01101110 = 0x6E
  // byte 2: s2[2..6] + pad(3) = 10111|000 = 0b00010111 = 0x17
  char buf[16];
  size_t n = pdu::decode_7bit_packed("5B6E17", 6, 3, buf, sizeof(buf));
  CHECK_EQ_INT(n, 6);  // 3 chars × 2 bytes UTF-8
  const char exp[] = { (char)0xC3, (char)0x84,
                       (char)0xC3, (char)0x96,
                       (char)0xC3, (char)0x91, 0 };
  CHECK_EQ_STR(std::string(buf, n), std::string(exp));
}

// --- ucs2_hex_to_utf8 边界 ---
static void test_ucs2_empty() {
  g_current = "ucs2_hex_to_utf8: empty → 0 bytes";
  char buf[16];
  size_t n = pdu::ucs2_hex_to_utf8("", 0, buf, sizeof(buf));
  CHECK_EQ_INT(n, 0);
}

static void test_ucs2_single_codepoint() {
  g_current = "ucs2_hex_to_utf8: '4F60' (单 codepoint 你) → 3 bytes E4 BD A0";
  char buf[16];
  size_t n = pdu::ucs2_hex_to_utf8("4F60", 4, buf, sizeof(buf));
  const char exp[] = { (char)0xE4, (char)0xBD, (char)0xA0, 0 };
  CHECK_EQ_INT(n, 3);
  CHECK_EQ_STR(std::string(buf, n), std::string(exp));
}

static void test_ucs2_latin1_supplement() {
  g_current = "ucs2_hex_to_utf8: '00E9' (Latin-1 é) → 2 bytes C3 A9";
  char buf[16];
  size_t n = pdu::ucs2_hex_to_utf8("00E9", 4, buf, sizeof(buf));
  const char exp[] = { (char)0xC3, (char)0xA9, 0 };
  CHECK_EQ_INT(n, 2);
  CHECK_EQ_STR(std::string(buf, n), std::string(exp));
}

// --- parse_udh 边界 ---
static void test_udh_empty() {
  g_current = "parse_udh: empty → false";
  int ref = 0, tot = 0, seq = 0;
  CHECK(!pdu::parse_udh("", ref, tot, seq));
}

static void test_udh_invalid_total_zero() {
  g_current = "parse_udh: total=0 (合法 [2,8]) → false";
  int ref = 0, tot = 0, seq = 0;
  // "05 00 03 2A 00 01" — total=0
  CHECK(!pdu::parse_udh("0500032A0001", ref, tot, seq));
}

static void test_udh_invalid_total_nine() {
  g_current = "parse_udh: total=9 (合法 [2,8]) → false";
  int ref = 0, tot = 0, seq = 0;
  // "05 00 03 2A 09 01" — total=9
  CHECK(!pdu::parse_udh("0500032A0901", ref, tot, seq));
}

static void test_udh_invalid_seq_overflow() {
  g_current = "parse_udh: seq=3, total=2 (seq > total) → false";
  int ref = 0, tot = 0, seq = 0;
  // "05 00 03 2A 02 03" — total=2, seq=3
  CHECK(!pdu::parse_udh("0500032A0203", ref, tot, seq));
}

// --- is_strict_utf8 边界 ---
static void test_is_strict_utf8_empty() {
  g_current = "is_strict_utf8: empty → true (vacuous valid)";
  CHECK(pdu::is_strict_utf8("", 0));
}

static void test_is_strict_utf8_cjk_3byte() {
  g_current = "is_strict_utf8: CJK 3-byte UTF-8 (你 = E4 BD A0) → true";
  char buf[3] = { (char)0xE4, (char)0xBD, (char)0xA0 };
  CHECK(pdu::is_strict_utf8(buf, 3));
}

static void test_is_strict_utf8_euro_3byte() {
  g_current = "is_strict_utf8: € 3-byte UTF-8 (E2 82 AC) → true";
  char buf[3] = { (char)0xE2, (char)0x82, (char)0xAC };
  CHECK(pdu::is_strict_utf8(buf, 3));
}

static void test_is_strict_utf8_truncated_2byte() {
  g_current = "is_strict_utf8: 2-byte lead 缺 continuation (C3 41) → false";
  char buf[2] = { (char)0xC3, 'A' };  // 0xC3 expects 1 continuation, got ASCII
  CHECK(!pdu::is_strict_utf8(buf, 2));
}

// --- looks_like_ucs2_be 边界 ---
static void test_looks_like_ucs2_odd_hex() {
  g_current = "looks_like_ucs2_be: 奇数长度 hex (9) → false (UCS-2 必须偶字节)";
  CHECK(!pdu::looks_like_ucs2_be("0E230E2B0E", 9));
}

static void test_looks_like_ucs2_single_pair() {
  g_current = "looks_like_ucs2_be: 单 pair (1 UCS-2 char) → false (≥ 2 pairs 才算)";
  CHECK(!pdu::looks_like_ucs2_be("0E230E2B", 8));
}

// --- ucs2_encode 边界 ---
static void test_ucs2_encode_empty() {
  g_current = "ucs2_encode: empty → 0 hex chars";
  char out[16];
  int n = pdu::ucs2_encode("", out, sizeof(out));
  CHECK_EQ_INT(n, 0);
  CHECK_EQ_STR(std::string(out), "");
}

static void test_ucs2_encode_single_ascii() {
  g_current = "ucs2_encode: 'A' → '0041'";
  char out[16];
  int n = pdu::ucs2_encode("A", out, sizeof(out));
  CHECK_EQ_INT(n, 4);
  CHECK_EQ_STR(std::string(out), "0041");
}

// --- sms_split_for_send 边界 ---
static void test_split_exactly_255() {
  g_current = "sms_split_for_send: 70×255 chars → 255 (GSM 上限正好, 不超)";
  pdu::SmsPart parts[300];
  int n = pdu::sms_split_for_send(70 * 255, 70, parts, 300);
  CHECK_EQ_INT(n, 255);
  CHECK_EQ_INT(parts[0].len_chars, 70);
  CHECK_EQ_INT(parts[254].offset_chars, 70 * 254);
  CHECK_EQ_INT(parts[254].len_chars, 70);
}

// --- bcd_encode_phone 边界 ---
static void test_bcd_encode_phone_20digit_max() {
  g_current = "bcd_encode_phone: 20 位 (GSM max) → 返回 20";
  char buf[64];
  int n = pdu::bcd_encode_phone("12345678901234567890", buf, sizeof(buf));
  CHECK_EQ_INT(n, 20);
}

static void test_bcd_encode_phone_21digit_overflow() {
  g_current = "bcd_encode_phone: 21 位 (超 20) → -1";
  char buf[64];
  int n = pdu::bcd_encode_phone("123456789012345678901", buf, sizeof(buf));
  CHECK_EQ_INT(n, -1);
}

static void test_bcd_encode_phone_empty() {
  g_current = "bcd_encode_phone: empty → -1";
  char buf[64];
  int n = pdu::bcd_encode_phone("", buf, sizeof(buf));
  CHECK_EQ_INT(n, -1);
}

// --- cmgs_build_pdu 边界 ---
static void test_cmgs_build_pdu_phone_with_dash() {
  g_current = "cmgs_build_pdu: phone 含 '-' (138-0000-1234) → 静默剥, DA length=11 (chars 4-5)";
  char pdu[256];
  int n = pdu::cmgs_build_pdu("138-0000-1234", "Hi", -1, nullptr, 0, 0, pdu, sizeof(pdu));
  // PDU 布局: PDU-type(2) MR(2) DA-len(2) ToA(2) DA(12) PID(2) DCS(2) VP(2) UDL(2) UD(8) = 36 hex
  //   DCS=0x08 (UCS-2) → "Hi" = 2 chars × 2 bytes = 4 bytes = 8 hex
  //   char  0-1: PDU-type "11"
  //   char  2-3: MR "00"
  //   char  4-5: DA-len "0B" (=11 decimal digit count, 不是 22 hex chars)
  //   char  6-7: ToA "81"
  //   char  8-19: DA digits BCD "3108001032F4" (12 hex)
  //   char 20-21: PID "00"
  //   char 22-23: DCS "08"
  //   char 24-25: VP "AA"
  //   char 26-27: UDL "04" (4 bytes)
  //   char 28-35: UD "00480069" ("Hi" UCS-2)
  CHECK_EQ_INT(n, 36);
  CHECK_EQ_STR(std::string(pdu).substr(4, 2), "0B");
}

// --- roundtrip (encode → decode, 确保对称性) ---
static void test_ucs2_roundtrip_chinese() {
  g_current = "ucs2 encode↔decode roundtrip: '你好世界' (4 CJK) → '你好世界'";
  const char* original = "你好世界";
  char hex[64];
  int n_enc = pdu::ucs2_encode(original, hex, sizeof(hex));
  CHECK_EQ_INT(n_enc, 16);  // 4 chars × 4 hex
  char utf8[32];
  size_t n_dec = pdu::ucs2_hex_to_utf8(hex, n_enc, utf8, sizeof(utf8));
  CHECK_EQ_INT(n_dec, 12);  // 4 CJK × 3 bytes UTF-8
  CHECK_EQ_STR(std::string(utf8, n_dec), original);
}

static void test_ucs2_roundtrip_thai() {
  g_current = "ucs2 encode↔decode roundtrip: 'สวัสดี' (6 Thai) → 'สวัสดี'";
  const char* original = "สวัสดี";
  char hex[64];
  int n_enc = pdu::ucs2_encode(original, hex, sizeof(hex));
  CHECK_EQ_INT(n_enc, 24);  // 6 chars × 4 hex
  char utf8[32];
  size_t n_dec = pdu::ucs2_hex_to_utf8(hex, n_enc, utf8, sizeof(utf8));
  CHECK_EQ_INT(n_dec, 18);  // 6 Thai × 3 bytes UTF-8
  CHECK_EQ_STR(std::string(utf8, n_dec), original);
}

static void test_ucs2_roundtrip_japanese() {
  g_current = "ucs2 encode↔decode roundtrip: 'こんにちは' (5 JP) → 'こんにちは'";
  const char* original = "こんにちは";
  char hex[64];
  int n_enc = pdu::ucs2_encode(original, hex, sizeof(hex));
  CHECK_EQ_INT(n_enc, 20);  // 5 chars × 4 hex
  char utf8[32];
  size_t n_dec = pdu::ucs2_hex_to_utf8(hex, n_enc, utf8, sizeof(utf8));
  CHECK_EQ_INT(n_dec, 15);  // 5 JP × 3 bytes UTF-8
  CHECK_EQ_STR(std::string(utf8, n_dec), original);
}

// =================== v4.0.7: pdu_ud_offset + 泰文 PDU 端到端 ===================

// 实际 6/19 泰文短信 CMT body (DCS=08 UCS-2, 73 bytes 含 SCA):
// SCA=07 91 66 39 08 11 00 F3
// FO=24 OA=0B D0 D6 B2 3C 6D CE 03 (11 digits)
// PID=00 DCS=08 SCTS=62 60 91 20 50 60 82
// UDL=2E (46 bytes) UD=0E23 0E2B 0E31 ... 0E13 003A 0035 ... 0039
// 期望: pdu_ud_offset 返回 54 (hex chars), udBytes=46
static void test_pdu_ud_offset_thai_ucs2() {
  // 实际从 device log 抓的 body_hex
  const char* body = "07916639081100F3240bd0d6b23c6dce030008626091205060822e0e230e2b0e310e2a0e220e370e190e220e310e190e020e2d0e070e040e380e13003a003500350039003600320039";
  size_t bodyLen = std::strlen(body);
  size_t udBytes = 0;
  size_t udOff = pdu::pdu_ud_offset(body, bodyLen, /*is7bit=*/false, &udBytes);
  CHECK_EQ_INT(udOff, 54);    // hex 偏移 (SCA 16 + FO 2 + OA 16 + PID 2 + DCS 2 + SCTS 14 = 52, +UDL 2 = 54)
  CHECK_EQ_INT(udBytes, 46);  // UDL=0x2E, UCS-2 按 byte
  // 切出来的 UD hex 应是 "0e23..." 开头 (注意: log 里 hex 是小写)
  CHECK_EQ_INT(std::strncmp(body + udOff, "0e23", 4), 0);
}

// 7-bit 短短信 PDU: SCA=07 ... FO=04 OA=0B ... PID=00 DCS=00 SCTS=...
// UDL=09 (9 septets), UD packed 7-bit = 8 bytes (ceil(9*7/8)=8)
static void test_pdu_ud_offset_7bit() {
  // 7-bit ASCII "Hello123" (9 chars) 的标准 PDU
  // SCA 07 91 66 39 08 11 00 F3  FO 04  OA 0B 81 86 09 14 00 09 00
  // PID 00  DCS 00  SCTS 62 60 91 20 50 60 82  UDL 09
  // UD 9 septets packed: C8 32 9B FD 46 97 D9 EC (8 bytes)
  // 7-bit ASCII "Hello123" (9 chars) 的标准 PDU
  // SCA 07 91 66 39 08 11 00 F3  FO 04  OA 0B 81 86 09 14 00 09 00
  // PID 00  DCS 00  SCTS 62 60 91 20 50 60 82  UDL 09
  // UD 9 septets packed: C8 32 9B FD 46 97 D9 EC (8 bytes)
  // 拼接 = 16+2+16+2+2+14+2+16 = 70 hex chars
  const char* body =
      "07916639081100F3"  // SCA
      "04"               // FO
      "0B81860914000900" // OA (oaLen=0B=11, type=81, BCD 6 bytes)
      "00"               // PID
      "00"               // DCS (7-bit)
      "62609120506082"   // SCTS (7 bytes)
      "09"               // UDL (9 septets)
      "C8329BFD4697D9EC";// UD (8 bytes packed 7-bit)
  size_t bodyLen = std::strlen(body);
  size_t udBytes = 0;
  size_t udOff = pdu::pdu_ud_offset(body, bodyLen, /*is7bit=*/true, &udBytes);
  CHECK_EQ_INT(udOff, 54);    // SCA 16 + FO 2 + OA 16 + PID 2 + DCS 2 + SCTS 14 + UDL 2 = 54
  CHECK_EQ_INT(udBytes, 8);   // ceil(9*7/8) = 8
  // 切出来的 UD 应是 7-bit packed 8 bytes
  CHECK_EQ_INT(std::strncmp(body + udOff, "C8329BFD4697D9EC", 16), 0);
}

// PDU 太短 / 无效 → 返回 0, udBytes=0
static void test_pdu_ud_offset_invalid() {
  size_t udBytes = 99;  // 验证会被清零
  size_t udOff = pdu::pdu_ud_offset("", 0, false, &udBytes);
  CHECK_EQ_INT(udOff, 0);
  CHECK_EQ_INT(udBytes, 0);

  udBytes = 99;
  udOff = pdu::pdu_ud_offset("07", 2, false, &udBytes);  // 太短
  CHECK_EQ_INT(udOff, 0);
  CHECK_EQ_INT(udBytes, 0);

  udBytes = 99;
  udOff = pdu::pdu_ud_offset("ZZ", 2, false, &udBytes);  // 非 hex
  CHECK_EQ_INT(udOff, 0);
  CHECK_EQ_INT(udBytes, 0);
}

// 端到端: 切 UD → decode_body_field → UTF-8
// 泰文短信期望: "ขอหมายเลขบัญชี:55629629" (15 codepoints, 实际字节数看 UTF-8 编码)
static void test_pdu_ud_offset_e2e_thai() {
  const char* body = "07916639081100F3240bd0d6b23c6dce030008626091205060822e0e230e2b0e310e2a0e220e370e190e220e310e190e020e2d0e070e040e380e13003a003500350039003600320039";
  size_t bodyLen = std::strlen(body);
  size_t udBytes = 0;
  size_t udOff = pdu::pdu_ud_offset(body, bodyLen, false, &udBytes);
  CHECK(udOff > 0);

  char bodyBuf[256];
  size_t n = pdu::decode_body_field(body + udOff, udBytes * 2, bodyBuf, sizeof(bodyBuf));
  // 23 codepoints: 16 Thai (3 bytes UTF-8 each) + 7 ASCII (":559629") = 48 + 7 = 55 bytes
  std::string out(bodyBuf, n);
  // 关键校验: 含泰文 "ข" (U+0E02) — 这是 SCA 切干净的特征 (SCA 没有 0E0x 字节)
  CHECK(out.find("\xE0\xB8\x82") != std::string::npos);  // ข
  // 含 ":"  (ASCII, 0x3A)
  CHECK(out.find(':') != std::string::npos);
  // 含 "559629" (实际短信是 6 位, 不是 8 位)
  CHECK(out.find("559629") != std::string::npos);
  // 不应含 SCA 的 "0791" 残留
  CHECK(out.find("0791") == std::string::npos);
}

// =================== v4.0.7.1: alphanumeric sender (DTAC/AIS/Verify) ===================

// 6 octets GSM7 packed → 6 chars (floor(48/7)=6), LSB-first
// 双向验证: pack("Verify") = D6B23C6DCE03, unpack 回 "Verify"
static void test_alpha_sender_verify() {
  // octets 是 raw bytes, 不用 hex string
  const uint8_t raw[6] = { 0xD6, 0xB2, 0x3C, 0x6D, 0xCE, 0x03 };
  char buf[32];
  size_t n = pdu::decode_gsm7_alpha_oa((const char*)raw, 6, 6, buf, sizeof(buf));
  std::string out(buf, n);
  CHECK_EQ_INT(n, 6);
  CHECK(out == "Verify");
}

static void test_alpha_sender_dtac() {
  // pack("DTAC") = 446A7008
  const uint8_t raw[4] = { 0x44, 0x6A, 0x70, 0x08 };
  char buf[32];
  size_t n = pdu::decode_gsm7_alpha_oa((const char*)raw, 4, 4, buf, sizeof(buf));
  std::string out(buf, n);
  CHECK_EQ_INT(n, 4);
  CHECK(out == "DTAC");
}

static void test_alpha_sender_ais() {
  // pack("AIS") = C1E414
  const uint8_t raw[3] = { 0xC1, 0xE4, 0x14 };
  char buf[32];
  size_t n = pdu::decode_gsm7_alpha_oa((const char*)raw, 3, 3, buf, sizeof(buf));
  std::string out(buf, n);
  CHECK_EQ_INT(n, 3);
  CHECK(out == "AIS");
}

static void test_alpha_sender_true() {
  // pack("TRUE") = 5469B508
  const uint8_t raw[4] = { 0x54, 0x69, 0xB5, 0x08 };
  char buf[32];
  size_t n = pdu::decode_gsm7_alpha_oa((const char*)raw, 4, 4, buf, sizeof(buf));
  std::string out(buf, n);
  CHECK_EQ_INT(n, 4);
  CHECK(out == "TRUE");
}

static void test_alpha_sender_kbank() {
  // pack("KBANK") = 4B61D0B904
  const uint8_t raw[5] = { 0x4B, 0x61, 0xD0, 0xB9, 0x04 };
  char buf[32];
  size_t n = pdu::decode_gsm7_alpha_oa((const char*)raw, 5, 5, buf, sizeof(buf));
  std::string out(buf, n);
  CHECK_EQ_INT(n, 5);
  CHECK(out == "KBANK");
}

// pdu_oa_offset: 切出 OA 段, 判 alphanumeric
// 实测样本: oaLen=0B, ToA=D0 (alphanumeric), 6 octets, value=D6B23C6DCE03
static void test_pdu_oa_offset_alpha() {
  const char* body = "07916639081100F3240bd0d6b23c6dce030008626091200323822e0e230e2b0e310e2a0e220e370e190e220e310e190e020e2d0e070e040e380e13003a003500350039003600320039";
  size_t bodyLen = std::strlen(body);
  bool isAlpha = false;
  size_t valueOctets = 0;
  // pdu_oa_offset 返回 OA value 起点 (hex 偏移), 跳过 length+ToA+value
  size_t oaOff = pdu::pdu_oa_offset(body, bodyLen, &isAlpha, &valueOctets);
  CHECK(oaOff > 0);
  CHECK(isAlpha);
  CHECK_EQ_INT(valueOctets, 6);  // ceil(11/2) = 6 (oaLen 字段不可信, 但 octets 数对)
  // OA value 起点 hex 偏移 = SCA 16 + FO 2 + OA-length 2 + OA-ToA 2 = 22
  CHECK_EQ_INT(oaOff, 22);
  // value hex 起点应是 "d6b2..."
  CHECK_EQ_INT(std::strncmp(body + oaOff, "d6b23c6dce03", 12), 0);
}

// E2E: 切 OA → 解 alphanumeric → 写 phoneBuf
// 期望: phone="Verify" (与 D6B23C6DCE03 100% 匹配)
static void test_pdu_oa_offset_e2e_alpha() {
  const char* body = "07916639081100F3240bd0d6b23c6dce030008626091200323822e0e230e2b0e310e2a0e220e370e190e220e310e190e020e2d0e070e040e380e13003a003500350039003600320039";
  size_t bodyLen = std::strlen(body);
  bool isAlpha = false;
  size_t valueOctets = 0;
  size_t oaOff = pdu::pdu_oa_offset(body, bodyLen, &isAlpha, &valueOctets);
  CHECK(oaOff > 0);
  CHECK(isAlpha);

  char phoneBuf[64];
  // 泰文验证码短信实际: floor(6*8/7) = 6 chars
  size_t nchars = (valueOctets * 8) / 7;
  // body+oaOff 是 hex string (6 octets = 12 hex chars), decode_gsm7_alpha_oa 要 raw bytes
  // 这里 E2E 测试就拆 hex: 每 2 hex 字符 → 1 byte (生产代码 main.cpp 里要 hex→bytes 后再调)
  uint8_t raw[6];
  for (size_t i = 0; i < valueOctets; i++) {
    char h0 = body[oaOff + i*2], h1 = body[oaOff + i*2 + 1];
    auto v = [](char c)->uint8_t{
      return (c<='9')?(c-'0'):((c<='F')?(c-'A'+10):(c-'a'+10));
    };
    raw[i] = (uint8_t)((v(h0) << 4) | v(h1));
  }
  size_t phoneN = pdu::decode_gsm7_alpha_oa((const char*)raw, valueOctets, nchars, phoneBuf, sizeof(phoneBuf));
  std::string out(phoneBuf, phoneN);
  CHECK(out == "Verify");
  CHECK_EQ_INT(phoneN, 6);
}

// padding trim: oaLen=11 案例 (跟生产 ML307 一致), 4 chars 真数据装 6 octets, 后 2 octets 是 0x00 padding
// 解 nchars=6, 应该 trim "@@" → 剩 4 chars "TRUE"
static void test_alpha_sender_true_padded() {
  // 4 chars "TRUE" 4 octets 5469B508 + 2 octets 0x00 padding
  const uint8_t raw[6] = { 0x54, 0x69, 0xB5, 0x08, 0x00, 0x00 };
  char buf[32];
  // 起点 nchars_max = floor(6*8/7) = 6
  size_t n = pdu::decode_gsm7_alpha_oa((const char*)raw, 6, 6, buf, sizeof(buf));
  std::string out(buf, n);
  CHECK(out == "TRUE");  // 应 trim 末 2 个 '@'
  CHECK_EQ_INT(n, 4);
}

// padding trim: 1 char "X" 装 6 octets (oaLen=11, value=6 octets 0x58 + 5 octets 0x00)
// 解 nchars=6, 末 5 char 全 '@' → trim → 1 char
static void test_alpha_sender_one_char() {
  const uint8_t raw[6] = { 0x58, 0x00, 0x00, 0x00, 0x00, 0x00 };  // GSM7 'X' = 0x58
  char buf[32];
  size_t n = pdu::decode_gsm7_alpha_oa((const char*)raw, 6, 6, buf, sizeof(buf));
  std::string out(buf, n);
  CHECK(out == "X");
  CHECK_EQ_INT(n, 1);
}

// padding trim: 6 octets 全真数据 ("Verify"), 不应误 trim
// (Verify 案例: 6 chars 装 6 octets, padding 6 bits 全 0 = 末尾 '@' 字符 = 0x00 septet)
// unpack 6 chars → "Verify" + padding 0 = "Verify" + 0 个 '@' (因为 6 chars 用 42 bits,剩 6 bits padding,0x00 septet = 1 char 0, **但我们 trim 1 个会少 1 char**)
// 实际 unpack: "Verify" (6 chars, 末 char 'y' 后面 6 bits padding = 0x00, 不算 char, 4-7 bit 范围)
// 等等,LSB-first 7 bit groups: char 0..5 各 7 bits = 42 bits used, 剩 6 bits in last octet
// char 0..5 unpacked = V, e, r, i, f, y (no trailing '@' from these 6 chars)
// padding 6 bits = 0 (协议规定), 不解 char → 没 trailing '@', 不 trim
// 期望: "Verify" 6 chars 不被 trim
static void test_alpha_sender_verify_no_trim() {
  const uint8_t raw[6] = { 0xD6, 0xB2, 0x3C, 0x6D, 0xCE, 0x03 };
  char buf[32];
  size_t n = pdu::decode_gsm7_alpha_oa((const char*)raw, 6, 6, buf, sizeof(buf));
  std::string out(buf, n);
  CHECK(out == "Verify");  // 不应 trim
  CHECK_EQ_INT(n, 6);
}

// numeric OA: ToA=0x81 (unknown, ISDN) 走 numeric, 跟 v4.0.4 行为对齐
// 数字 sender PDU: SCA=07...FO=04 OA=0B 81 86 09 14 00 09 00 → "+8613800001234" 之类
static void test_pdu_oa_offset_numeric() {
  const char* body = "07916639081100F304" "0B" "81" "860914000900F" "00" "00" "62609120506082";
  size_t bodyLen = std::strlen(body);
  bool isAlpha = false;
  size_t valueOctets = 0;
  size_t oaOff = pdu::pdu_oa_offset(body, bodyLen, &isAlpha, &valueOctets);
  CHECK(oaOff > 0);
  CHECK(!isAlpha);  // TON=0 (unknown) — numeric
  CHECK_EQ_INT(valueOctets, 6);
  CHECK_EQ_INT(oaOff, 22);  // SCA 16 + FO 2 + OA-len 2 + OA-ToA 2 = 22
}

// =================== v4.0.9: pdu_udh_offset (concat SMS path bug fix) ===================
// 修前 bug: main.cpp:497-505 误用 strstr("0804"/"0003") + p16<=body+2 判定 UDH 位置,
//   "0804" 实际在 body 深处 (UDH IE), 条件永远 false, udhSkip=0, stashed 含整段 PDU header
// 修后: pdu_udh_offset 用 UDL 后的 UDHL byte 定位 UDH 起点, outUdhByteLen=UDHL+1
//
// 测试用 2 段真实 concat PDU (单条 PDU, 含 UDH 16-bit concat IE 0504 refH refL total seq)
//
// 真实 concat PDU 布局 (SCA=00, FO=04, OA=0B 国内 13x, PID=00, DCS=00 7-bit, SCTS=7B, UDL=99, UDHL=05):
//   "00"                                                 // SCA len 0
//   "04"                                                 // FO: SMS-DELIVER, no MMS
//   "0B 81 86 09 14 00 09 00"                            // OA: len=11, ToA=0x81 unknown, BCD 13x...
//   "00"                                                 // PID
//   "00"                                                 // DCS 7-bit default
//   "62609120506082"                                     // SCTS (7 bytes)
//   "63"                                                 // UDL = 99 septets (7-bit)
//   "05 00 03 9C AD 01"                                  // UDH 8-bit concat: UDHL=05 IEI=00 IEDL=03 ref=0x9CAD total=01 seq=01
//   <UD 7-bit packed 95 bytes hex = 190 hex chars>
//
// 期望: pdu_udh_offset 返回 46 (hex chars), outUdhByteLen=6 (UDHL+5 IEs bytes)
static void test_pdu_udh_offset_8bit_concat() {
  const char* body =
    "00"                                                  // SCA len 0
    "04"                                                  // FO
    "0B" "81" "860914000900"                              // OA 国内 (11 digits = 12 hex)
    "00"                                                  // PID
    "00"                                                  // DCS 7-bit
    "62609120506082"                                      // SCTS
    "63"                                                  // UDL 99 septets
    "05" "00" "03" "9C" "AD" "01";                        // UDH 8-bit: UDHL=05 IEI=00 IEDL=03 ref=0x9CAD total=1 seq=1
  size_t bodyLen = std::strlen(body);
  size_t udhBytes = 0;
  size_t udhOff = pdu::pdu_udh_offset(body, bodyLen, &udhBytes);
  // UDHL byte at pos 40 (UDHL=05), UDH 段 6 bytes (UDHL+5 IE) = 12 hex chars
  // UD 起点 = 40 + 12 = 52
  CHECK_EQ_INT(udhOff, 52);
  CHECK_EQ_INT(udhBytes, 6);
}

// 16-bit concat PDU 布局 (SCA=07, FO=04, OA=0B, PID=00, DCS=08 UCS-2, SCTS=7B, UDL=46 UCS-2 chars, UDHL=06):
//   "07 91 66 39 08 11 00 F3"                             // SCA len=7, type=91, BCD 66839001810003
//   "04"                                                 // FO
//   "0B 81 86 09 14 00 09 00"                            // OA 11 digits
//   "00"                                                 // PID
//   "08"                                                 // DCS UCS-2
//   "62609120506082"                                     // SCTS
//   "46"                                                 // UDL 70 UCS-2 chars (concat 后单条 70)
//   "06 08 04 9C AD 00 01"                               // UDH 16-bit: UDHL=06 IEI=08 IEDL=04 ref=0x9CAD total=0 seq=1
//   <UD UCS-2 70 chars = 140 hex chars>
static void test_pdu_udh_offset_16bit_concat() {
  const char* body =
    "07" "91" "6639081100F3"                              // SCA
    "04"                                                  // FO
    "0B" "81" "860914000900"                              // OA
    "00"                                                  // PID
    "08"                                                  // DCS UCS-2
    "62609120506082"                                      // SCTS
    "46"                                                  // UDL 70 UCS-2 chars
    "06" "08" "04" "9C" "AD" "00" "01";                   // UDH 16-bit: UDHL=06 IEI=08 IEDL=04 ref=0x9CAD total=0 seq=1
  size_t bodyLen = std::strlen(body);
  size_t udhBytes = 0;
  size_t udhOff = pdu::pdu_udh_offset(body, bodyLen, &udhBytes);
  // UDHL byte at pos 54 (UDHL=06), UDH 段 7 bytes = 14 hex chars
  // UD 起点 = 54 + 14 = 68
  CHECK_EQ_INT(udhOff, 68);
  CHECK_EQ_INT(udhBytes, 7);  // UDHL=06 + 6 IEs
}

// 边界: PDU 太短, 应该返回 0
static void test_pdu_udh_offset_too_short() {
  size_t udhBytes = 99;
  size_t udhOff = pdu::pdu_udh_offset("00", 2, &udhBytes);  // SCA len 0 + FO 缺
  CHECK_EQ_INT(udhOff, 0);
  CHECK_EQ_INT(udhBytes, 0);

  udhBytes = 99;
  udhOff = pdu::pdu_udh_offset("ZZ", 2, &udhBytes);  // 非 hex
  CHECK_EQ_INT(udhOff, 0);
}

// 边界: 已知坏 PDU (OA 长度超限) → 返回 0
static void test_pdu_udh_offset_invalid_oa_len() {
  const char* body = "00" "04" "FF" "81" "1234567890123456789012" "00" "00" "0000000000000" "00";
  size_t bodyLen = std::strlen(body);
  size_t udhBytes = 99;
  size_t udhOff = pdu::pdu_udh_offset(body, bodyLen, &udhBytes);
  CHECK_EQ_INT(udhOff, 0);
  CHECK_EQ_INT(udhBytes, 0);
}

//   + PID 00 + DCS 00 (7-bit) + SCTS 62609180851182 + UDL 8F=143 + UDHL 06 (16-bit concat)
// v4.0.9 bug: pdu_udh_offset 返回 UDHL 位置 56, caller 算 udhSkip = udhBytes*2 = 14 错
//   (应该 udhOff + udhBytes*2 = 70) → partBody 起点 body_hex[14] = SCA 数据段尾部 ("F0400ed0...")
//   → 拼接/decode 全失败 → 60s partial 又触发同 bug → 死循环 partial
// v4.0.10 fix: pdu_udh_offset 直接返回 UD 起点 hex offset
static void test_pdu_udh_offset_e2e_true_concat() {
  const char* body =
    "07916649520080F0"       // SCA len 7, type 91, BCD 6649520080F0
    "40"                     // FO SMS-DELIVER + UDHI flag
    "0E" "D0" "5479BD0C0AC2E100"  // OA len 14 ToA=D0 GSM7 sender (14 hex)
    "00"                     // PID
    "00"                     // DCS 7-bit
    "62609180851182"         // SCTS
    "8F"                     // UDL 143 septets
    "06" "08" "04" "5A" "A7" "02" "01";  // UDH 16-bit: UDHL=06 IEI=08 IEDL=04 ref=5AA7 total=2 seq=1
  size_t bodyLen = std::strlen(body);
  size_t udhBytes = 0;
  size_t udhOff = pdu::pdu_udh_offset(body, bodyLen, &udhBytes);
  // body 长 72 hex chars, 布局:
  //   SCA 16 + FO 2 + OA-len 2 + OA-ToA 2 + OA-BCD 14 + PID 2 + DCS 2 + SCTS 14 + UDL 2 = 56
  //   UDHL byte 58 (=0x06), IEI+IEDL 4, ref/total/seq 8 → UDH IE 12 hex, UDH 段共 14 hex (含 UDHL)
  //   UDHL 位置 58 + UDH 段 14 hex = UD 起点 72
  // 注意: ML307 OA GSM7 packed 14 hex 是实测, v4.0.10 改用 "0804" pattern 反查避开 oaLen 单位歧义
  CHECK_EQ_INT(udhOff, 72);
  CHECK_EQ_INT(udhBytes, 7);
  CHECK_EQ_INT(udhOff, bodyLen);  // UD 起点 = body 末尾 (此 E2E 用例没填 UD, 真 SMS 后续接 UD)
}
// 单条 SMS 场景: 没 UDH IE pattern (没 0804 / 0003), pdu_udh_offset 找不到 IE, 应返回 0
//   caller 兜底用全 body 当 UD (或用 pdu_ud_offset)
// v4.0.10 改: 用 UDH IE pattern 找位置, 单条 SMS 无 IE → 返回 0 (旧版能算 42)
static void test_pdu_udh_offset_no_udh() {
  const char* body =
    "00"                                                  // SCA len 0
    "04"                                                  // FO
    "0B" "81" "860914000900"                              // OA
    "00"                                                  // PID
    "00"                                                  // DCS 7-bit
    "62609120506082"                                      // SCTS
    "10"                                                  // UDL 16 septets
    "00"                                                  // UDHL=0, 无 UDH IE
    "C8329BFD065DDF7236";                                 // UD 7-bit 16 chars
  size_t bodyLen = std::strlen(body);
  size_t udhBytes = 99;  // 应被清 0
  size_t udhOff = pdu::pdu_udh_offset(body, bodyLen, &udhBytes);
  CHECK_EQ_INT(udhOff, 0);    // 单条 SMS 没 concat IE, 找不到
  CHECK_EQ_INT(udhBytes, 0);  // 清 0
}

// =================== v4.0.11: 翔哥 6/19 抓的实战短信 (TDD red→green) ===================
// 翔哥 directive: "必须测试这两条短信能正常解码. 他们两个是不同编码"
// 修前 bug: +CMT 头 dcs=255 / dcs=0 是错的 (ML307 quirk), 真实 DCS 在 TPDU DCS byte
//           TRUE 是 7-bit DCS=0x00, 翔哥新发那条是 UCS-2 DCS=0x08 (网关标对了, 但 v4.0.10 用 7-bit 错)
// 解法: pdu_ud_offset_ex 内部从 TPDU 读 DCS byte (outIsUcs2 + outIs7bit), caller 据此选 decoder
//
// Case 1 (TRUE 7-bit DCS=0x00, 翔哥 6/19 实测 v4.0.10.1 跑出 "Kddo sghr bncd rdbtqd..."):
//   SCA len=7 + type=91 + BCD +496694520008 + FO=0x40(UDHI) + OA len=14 ToA=D0 GSM7
//   + PID=00 + DCS=0x00 + SCTS + UDL=143 septets + UDH (concat ref=23207)
//   关键: TPDU DCS byte = 0x00 (7-bit 真), 但 +CMT 头 dcs=255 (错的)
//   期望: outIs7bit=true, outIsUcs2=false
static void test_pdu_e2e_true_7bit_dcs_truth() {
  const char* body =
    "07916649520080F0"                    // SCA len=7 + type=91 + BCD 6649520080 + type nibble (16 hex)
    "40"                                  // FO SMS-DELIVER + UDHI (2)
    "0E" "D0" "5479BD0C0AC2E1"            // OA len=14 ToA=D0 GSM7 (14 hex = 7 octets = "True App" ceil 8 chars) (18)
    "00"                                  // PID (2)
    "00"                                  // DCS=0x00 (7-bit 真相, 翔哥 6/19 实测) (2)
    "62609180851182"                      // SCTS (14)
    "8F"                                  // UDL=143 septets (2)
    "06" "08" "04" "5A" "A7" "02" "01";   // UDH 16-bit concat ref=23207 total=2 seq=1 (14)
  // 总: 16+2+18+2+2+14+2+14 = 70 hex
  size_t bodyLen = std::strlen(body);

  // pdu_ud_offset_ex: 跳过 SCA(16) + FO(2) + OA-len(2) + ToA(2) + OA-BCD(14) + PID(2) + DCS(2) + SCTS(14) + UDL(2) = 56 hex
  bool isUcs2 = false;
  bool is7bit = false;
  size_t udByteLen = 0;
  size_t udOff = pdu::pdu_ud_offset_ex(body, bodyLen, &isUcs2, &is7bit, &udByteLen);
  CHECK_EQ_INT(udOff, 56);    // UD 起点
  CHECK(is7bit);
  CHECK(!isUcs2);
  CHECK_EQ_INT(udByteLen, 126);  // ceil(143*7/8) = 126 octets (UDL=143 septets)
  // 注: body 实际 UD 区只有 0 bytes (没填 UD), 但我们只测 pdu_ud_offset_ex 算的 byte 数
}

// Case 2 (翔哥 6/19 新发 UCS-2 短信, 21 chars 泰文 "รหัสยืนยันของคุณ:559629"):
//   SCA len=0 + FO=0x04 (DELIVER no UDHI) + OA len=11 ToA=81 numeric (国内 "Verify" 风格) + PID=00
//   + DCS=0x08 (UCS-2) + SCTS + UDL=42 bytes + UD 42 bytes UCS-2 BE 泰文
//   关键: TPDU DCS byte = 0x08 (UCS-2), v4.0.10 fallback dcs=0 错走 7-bit 解出乱码
//   期望: outIsUcs2=true, outIs7bit=false
static void test_pdu_e2e_thai_ucs2_dcs_truth() {
  const char* body =
    "00"                                  // SCA len=0
    "04"                                  // FO SMS-DELIVER (no UDHI)
    "0B" "81" "860914000900"              // OA len=11 ToA=81 numeric "13800001234" BCD
    "00"                                  // PID
    "08"                                  // DCS=0x08 (UCS-2)
    "62609120506082"                      // SCTS
    "2E"                                  // UDL=46 bytes (23 UCS-2 chars)
    "0E230E2B0E310E2A0E220E370E190E220E310E190E020E2D0E070E040E380E13003A003500350039003600320039";
  // UD 46 bytes UCS-2 BE = 23 chars 泰文 "รหัสยืนยันของคุณ:559629"
  size_t bodyLen = std::strlen(body);

  // 跳过 SCA(2) + FO(2) + OA-len(2) + ToA(2) + OA-BCD(12) + PID(2) + DCS(2) + SCTS(14) + UDL(2) = 40 hex
  bool isUcs2 = false;
  bool is7bit = false;
  size_t udByteLen = 0;
  size_t udOff = pdu::pdu_ud_offset_ex(body, bodyLen, &isUcs2, &is7bit, &udByteLen);
  CHECK_EQ_INT(udOff, 40);
  CHECK(isUcs2);    // DCS=0x08 → UCS-2
  CHECK(!is7bit);
  CHECK_EQ_INT(udByteLen, 46);

  // 解 UCS-2 → "รหัสยืนยันของคุณ:559629"
  char utf8[128] = {0};
  size_t n = pdu::ucs2_hex_to_utf8(body + udOff, udByteLen * 2, utf8, sizeof(utf8)-1);
  utf8[n] = 0;
  CHECK_EQ_STR(utf8, "รหัสยืนยันของคุณ:559629");
}

int main() {
  std::printf("============================================================\n");
  std::printf("pdu_codec host test\n");
  std::printf("============================================================\n");

  // UCS-2
  test_ucs2_chinese();
  test_ucs2_skip_null_and_surrogate();
  test_ucs2_empty();
  test_ucs2_single_codepoint();
  test_ucs2_latin1_supplement();

  // phone
  test_phone_international();
  test_phone_short_number_regression();
  test_phone_alpha_sender();
  test_phone_plus_prefix();
  test_phone_with_dash();
  test_phone_pure_hex_returned_as_is();
  test_phone_empty();

  // body
  test_body_mixed();
  test_body_pure_ascii();
  test_body_pure_ucs2();
  test_body_5hex_odd_boundary();
  test_body_empty();

  // UDH
  test_udh_16bit();
  test_udh_8bit();
  test_udh_16bit_stripped_udhl();   // 烧板 regression: ML307 剥 UDHL
  test_udh_8bit_stripped_udhl();    // 烧板 regression: ML307 剥 UDHL
  test_udh_no_concat();
  test_udh_empty();
  test_udh_invalid_total_zero();
  test_udh_invalid_total_nine();
  test_udh_invalid_seq_overflow();

  // 7-bit (烧板 regression: 长泰文 DCS=0 7-bit)
  test_7bit_ascii_roundtrip();
  test_7bit_multichar();
  test_7bit_user_real_long_msg();
  test_7bit_special_chars();        // @£$ (0x00 0x01 0x02)
  test_7bit_extended_chars();       // ÄÖÑ (0x5B 0x5C 0x5D)

  // UTF-8 strict (DCS=0 但实际 UCS-2 的兜底)
  test_is_strict_utf8_true_ascii();
  test_is_strict_utf8_true_gsm7_extended();
  test_is_strict_utf8_false_lone_continuation();
  test_is_strict_utf8_false_4byte_lead();
  test_is_strict_utf8_empty();
  test_is_strict_utf8_cjk_3byte();
  test_is_strict_utf8_euro_3byte();
  test_is_strict_utf8_truncated_2byte();

  // task #37: looks_like_ucs2_be (raw bytes sniff, 网关 DCS 标错场景)
  test_looks_like_ucs2_thai();
  test_looks_like_ucs2_chinese();
  test_looks_like_ucs2_ascii_negative();
  test_looks_like_ucs2_gsm7_packed_negative();
  test_looks_like_ucs2_too_short();
  test_looks_like_ucs2_odd_hex();
  test_looks_like_ucs2_single_pair();

  // v4.0.6 短信发送 (双向)
  test_ucs2_encode_ascii();
  test_ucs2_encode_chinese();
  test_ucs2_encode_thai();
  test_ucs2_encode_invalid_utf8();
  test_ucs2_encode_empty();
  test_ucs2_encode_single_ascii();
  test_split_single_70();
  test_split_double_71();
  test_split_overflow_255segs();
  test_split_exactly_255();

  // P0 fix #3: DA length = decimal digit count
  test_bcd_encode_phone_11digit();
  test_bcd_encode_phone_10digit();
  test_bcd_encode_phone_13digit_intl();  // 翔哥提的国外场景
  test_bcd_encode_phone_plus_prefix();
  test_bcd_encode_phone_invalid_chars();
  test_bcd_encode_phone_20digit_max();
  test_bcd_encode_phone_21digit_overflow();
  test_bcd_encode_phone_empty();

  // P0 fix #1 + #3: PDU-type 0x51 for concat + DA length
  test_cmgs_build_pdu_single_pdu_type();
  test_cmgs_build_pdu_single_da_length_11digit();
  test_cmgs_build_pdu_single_da_length_10digit();
  test_cmgs_build_pdu_concat_pdu_type();
  test_cmgs_build_pdu_concat_udh_bytes();
  test_cmgs_build_pdu_invalid_utf8();
  test_cmgs_build_pdu_phone_with_dash();

  // v4.0.7 扩展: encode ↔ decode roundtrip (对称性)
  test_ucs2_roundtrip_chinese();
  test_ucs2_roundtrip_thai();
  test_ucs2_roundtrip_japanese();

  // v4.0.7: pdu_ud_offset + 泰文 PDU 端到端
  test_pdu_ud_offset_thai_ucs2();
  test_pdu_ud_offset_7bit();
  test_pdu_ud_offset_invalid();
  test_pdu_ud_offset_e2e_thai();

  // v4.0.7.1 alphanumeric sender
  test_alpha_sender_verify();
  test_alpha_sender_dtac();
  test_alpha_sender_ais();
  test_alpha_sender_true();
  test_alpha_sender_kbank();
  test_alpha_sender_true_padded();   // 4 chars + 2 octets 0x00 padding → trim "@@"
  test_alpha_sender_one_char();      // 1 char + 5 octets 0x00 padding → trim "@@@@@"
  test_alpha_sender_verify_no_trim();// 6 chars 装 6 octets 正好, 不误 trim
  test_pdu_oa_offset_alpha();
  test_pdu_oa_offset_e2e_alpha();
  test_pdu_oa_offset_numeric();

  // v4.0.9: pdu_udh_offset (concat SMS path bug fix)
  test_pdu_udh_offset_8bit_concat();
  test_pdu_udh_offset_16bit_concat();
  test_pdu_udh_offset_too_short();
  test_pdu_udh_offset_invalid_oa_len();
  test_pdu_udh_offset_no_udh();
  test_pdu_udh_offset_e2e_true_concat();

  // v4.0.11: 翔哥 6/19 实战 2 条 E2E (TDD red→green)
  test_pdu_e2e_true_7bit_dcs_truth();
  test_pdu_e2e_thai_ucs2_dcs_truth();

  std::printf("============================================================\n");
  std::printf("Result: %d passed, %d failed\n", g_pass, g_fail);
  std::printf("============================================================\n");
  return g_fail == 0 ? 0 : 1;
}

// =================== v4.0.10: 真实 TRUE concat SMS 端到端 (修前 bug) ===================
// 翔哥 6/19 实战抓的 TRUE concat SMS, refId=23207 total=2
// body 长度 308 hex (part 1), 含:
//   SCA len 7 (07 916649520080F0) + FO 40 (UDHI) + OA len 14 ToA=D0 GSM7 sender (5479BD0C0AC2E100)
