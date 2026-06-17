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

static void test_is_valid_7bit_true() {
  g_current = "is_valid_7bit: 真 7-bit 'Test123' → true (sniff 通过)";
  // 复用 test_7bit_multichar 的数据, 期望 sniff 认为合法
  CHECK(pdu::is_valid_7bit("D4F29C1E93CD00", 14, 7));
}

static void test_is_valid_7bit_false_ucs2_as_7bit() {
  g_current = "is_valid_7bit: UCS-2 hex 当 7-bit 解 → false (触发 fallback UCS-2)";
  // 模拟用户新短 SMS (UCS-2 编码 "宇\n..."): hex 含 0x00 0x0A 等控制字符
  //   "5B87 000A" = "宇\n" 真实 UCS-2; 当 7-bit 解 (6 septets) 会出控制字符
  //   期望 sniff fail, 主流程降级走 UCS-2 解码
  CHECK(!pdu::is_valid_7bit("5B87000A0011", 12, 6));
}

static void test_is_valid_7bit_false_dense_controls() {
  g_current = "is_valid_7bit: 全 0x01 hex → false (解出 £ 等控制区字符过多)";
  // 0x01 septet (GSM7 table[1] = £, [2] = $, [0x0A] = LF 等), 4 字节全 0x01 时
  //   bit 7 上溢会产生孤立 0x80-0xBF → sniff 标为可疑
  CHECK(!pdu::is_valid_7bit("01010101", 8, 4));
}

int main() {
  std::printf("============================================================\n");
  std::printf("pdu_codec host test\n");
  std::printf("============================================================\n");

  // UCS-2
  test_ucs2_chinese();
  test_ucs2_skip_null_and_surrogate();

  // phone
  test_phone_international();
  test_phone_short_number_regression();
  test_phone_alpha_sender();

  // body
  test_body_mixed();

  // UDH
  test_udh_16bit();
  test_udh_8bit();
  test_udh_16bit_stripped_udhl();   // 烧板 regression: ML307 剥 UDHL
  test_udh_8bit_stripped_udhl();    // 烧板 regression: ML307 剥 UDHL
  test_udh_no_concat();

  // 7-bit (烧板 regression: 长泰文 DCS=0 7-bit)
  test_7bit_ascii_roundtrip();
  test_7bit_multichar();
  test_7bit_user_real_long_msg();

  // 7-bit sniff (误判 UCS-2 当 7-bit → fallback)
  test_is_valid_7bit_true();
  test_is_valid_7bit_false_ucs2_as_7bit();
  test_is_valid_7bit_false_dense_controls();

  std::printf("============================================================\n");
  std::printf("Result: %d passed, %d failed\n", g_pass, g_fail);
  std::printf("============================================================\n");
  return g_fail == 0 ? 0 : 1;
}
