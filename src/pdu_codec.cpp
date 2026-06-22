// PDU 解码实现 — 函数体从 src/main.cpp 原样搬迁, 只把 String 改成 char* 缓冲写
// 原 src/main.cpp 版本是 v4.0.2 实测稳定, 行为对齐避免引入新 bug

#include "pdu_codec.h"

#include <cstdio>    // snprintf (cmgs_build_pdu hex 字段格式化)
#include <stdlib.h>  // strtol (host 上 Arduino.h 不存在, 必须显式 include)
#include <string.h>

namespace pdu {

namespace {
  inline uint8_t nib(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='A'&&c<='F') return c-'A'+10;
    if (c>='a'&&c<='f') return c-'a'+10;
    return 0;
  }
  inline bool isHex(char c) {
    return (c>='0'&&c<='9') || (c>='A'&&c<='F') || (c>='a'&&c<='f');
  }
  // 写 1 个 codepoint 到 out[pos..], 返回写入字节数
  // 0 = buffer 满 或跳过 (null/surrogate)
  inline size_t put_codepoint(uint16_t u, char* out, size_t outLen, size_t pos) {
    if (!u) return 0;
    if (u < 0x80) {
      if (pos + 1 > outLen) return 0;
      out[pos] = (char)u;
      return 1;
    }
    if (u < 0x800) {
      if (pos + 2 > outLen) return 0;
      out[pos]   = (char)(0xC0|(u>>6));
      out[pos+1] = (char)(0x80|(u&0x3F));
      return 2;
    }
    if (u < 0xD800 || u > 0xDFFF) {
      if (pos + 3 > outLen) return 0;
      out[pos]   = (char)(0xE0|(u>>12));
      out[pos+1] = (char)(0x80|((u>>6)&0x3F));
      out[pos+2] = (char)(0x80|(u&0x3F));
      return 3;
    }
    return 0;  // surrogate, 跳过
  }
}  // namespace

size_t ucs2_hex_to_utf8(const char* hex, size_t hexLen,
                        char* out, size_t outLen) {
  size_t pos = 0;
  for (size_t i = 0; i + 4 <= hexLen; i += 4) {
    uint16_t u = (nib(hex[i])<<12)|(nib(hex[i+1])<<8)|(nib(hex[i+2])<<4)|nib(hex[i+3]);
    if (u == 0 || (u >= 0xD800 && u <= 0xDFFF)) continue;
    size_t wrote = put_codepoint(u, out, outLen, pos);
    if (wrote == 0) return pos;
    pos += wrote;
  }
  return pos;
}

size_t decode_phone_field(const char* raw, size_t rawLen,
                          char* out, size_t outLen) {
  if (rawLen == 0) return 0;
  // 含 ASCII-safe 字符 → 原样 copy (含短号 "10086910")
  for (size_t i = 0; i < rawLen; i++) {
    char c = raw[i];
    if ((c>='0'&&c<='9') || c=='+' || c=='-' || c==' ' ||
        (c>='A'&&c<='Z') || (c>='a'&&c<='z')) {
      if (rawLen > outLen) return 0;
      memcpy(out, raw, rawLen);
      return rawLen;
    }
  }
  // 全 hex → 当 UCS2 解
  return ucs2_hex_to_utf8(raw, rawLen, out, outLen);
}

size_t decode_body_field(const char* raw, size_t rawLen,
                         char* out, size_t outLen) {
  size_t pos = 0;
  for (size_t i = 0; i < rawLen; ) {
    if (isHex(raw[i]) && i + 3 < rawLen
        && isHex(raw[i+1]) && isHex(raw[i+2]) && isHex(raw[i+3])) {
      uint16_t u = (nib(raw[i])<<12)|(nib(raw[i+1])<<8)|(nib(raw[i+2])<<4)|nib(raw[i+3]);
      i += 4;
      if (u == 0 || (u >= 0xD800 && u <= 0xDFFF)) continue;
      size_t wrote = put_codepoint(u, out, outLen, pos);
      if (wrote == 0) return pos;
      pos += wrote;
    } else {
      if (pos + 1 > outLen) return pos;
      out[pos++] = raw[i++];
    }
  }
  return pos;
}

bool parse_udh(const char* line, int& refId, int& total, int& seq) {
  const char* p = strstr(line, "0804");
  bool is16 = (p != NULL);
  if (!is16) {
    p = strstr(line, "0003");
    if (!p) return false;
  }
  auto get2 = [](const char* q)->int {
    char b[3] = { q[0], q[1], 0 };
    return strtol(b, NULL, 16);
  };
  if (is16) {
    // 16-bit: refH=p[4..5], refL=p[6..7], total=p[8..9], seq=p[10..11]
    int refH = get2(p + 4);
    int refL = get2(p + 6);
    int tot  = get2(p + 8);
    int sq   = get2(p + 10);
    refId = (refH << 8) | refL;
    total = tot;
    seq   = sq;
  } else {
    // 8-bit (per 3GPP TS 23.040 §9.2.3.24, no padding after IEDL):
    int ref = get2(p + 4);
    int tot = get2(p + 6);
    int sq  = get2(p + 8);
    refId = ref;
    total = tot;
    seq   = sq;
  }
  return total >= 2 && total <= 8 && seq >= 1 && seq <= total;
}

// GSM 03.38 §6.2.1 default alphabet → Unicode BMP code point
// 0x00-0x7F 单字节查表; basic = 0x00-0x7F, 用 U+0040 起避免和 ASCII 控制符冲突
// 注: 这个表不包含希腊扩展 (0x10-0x1F 部分是希腊字母), 简化到够用即可
static const uint16_t gsm7_default_to_unicode[128] = {
  0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,  // @£$¥èéùì
  0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,  // òÇLFØøCRÅå
  0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,  // Δ_ΦΓΛΩΠΨ
  0x03A3, 0x0398, 0x03A9, 0x039E, 0x001B, 0x00C6, 0x00E6, 0x00DF,  // ΣΘΩΞESCÆæß
  0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027,  // sp !"#¤%&'
  0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,  // ()*+,-./
  0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,  // 01234567
  0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,  // 89:;<=>?
  0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,  // ¡ABCDEFG
  0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,  // HIJKLMNO
  0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,  // PQRSTUVW
  0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,  // XYZÄÖÑÜ§
  0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,  // ¿abcdefg
  0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,  // hijklmno
  0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,  // pqrstuvw
  0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,  // xyzäöñüà
};

size_t decode_7bit_packed(const char* hex, size_t hexLen, size_t numChars,
                         char* out, size_t outLen) {
  // 1) hex → 原始字节
  size_t byteCount = hexLen / 2;
  if (byteCount == 0 || numChars == 0) return 0;
  uint8_t buf[1024];
  if (byteCount > sizeof(buf)) return 0;
  for (size_t i = 0; i < byteCount; i++) {
    buf[i] = (nib(hex[i*2]) << 4) | nib(hex[i*2 + 1]);
  }
  // 2) 7-bit unpack (LSB first, MSB of each byte 是下一个 char 的高位)
  size_t pos = 0;
  size_t bitOffset = 0;
  for (size_t i = 0; i < numChars; i++) {
    size_t byteIdx = bitOffset / 8;
    if (byteIdx >= byteCount) break;
    int bitShift = bitOffset % 8;
    uint16_t ch = buf[byteIdx] >> bitShift;
    if (bitShift > 1 && byteIdx + 1 < byteCount) {
      ch |= (uint16_t)buf[byteIdx + 1] << (8 - bitShift);
    }
    ch &= 0x7F;
    if (ch < 128) {
      uint16_t u = gsm7_default_to_unicode[ch];
      size_t wrote = put_codepoint(u, out, outLen, pos);
      if (wrote == 0) return pos;
      pos += wrote;
    }
    bitOffset += 7;
  }
  return pos;
}

bool is_strict_utf8(const char* buf, size_t n) {
  size_t i = 0;
  while (i < n) {
    uint8_t c = (uint8_t)buf[i];
    int extra = 0;
    if (c < 0x80)       { extra = 0; }                     // 1-byte ASCII
    else if (c < 0xC0)  { return false; }                 // lone continuation
    else if (c < 0xE0)  { extra = 1; }                    // 2-byte (BMP 0x80-0x7FF)
    else if (c < 0xF0)  { extra = 2; }                    // 3-byte (BMP 0x800-0xFFFF)
    else                { return false; }                 // 4+ byte (UCS-2 不应出现)
    i++;
    while (extra-- > 0) {
      if (i >= n) return false;
      if (((uint8_t)buf[i] & 0xC0) != 0x80) return false;
      i++;
    }
  }
  return true;
}

// Sniff raw body hex 字节是否呈 UCS-2 BE 模式
// 标准: DCS=0 但实际是 UCS-2 (gateway 标错, 如泰文 0E23...) → 应 fallback UCS-2
// 规则:
//   1. hexLen 偶数 (UCS-2 一字 2 字节)
//   2. 至少 4 对完整字符 (>= 8 字节 hex, 即 4 UCS-2 字符) — 避免 1-2 字节巧合
//   3. 高字节落在 BMP 高频区比例 >= 80% (严格, 防 7-bit packed 偶发命中) — 抓 CJK/泰文
//   3b. 或窄区段 (0x09-0x0D/0x10/0x17/0x19 = 印度/缅甸/高棉/Tai Le) 命中 >= 4 对 — 抓 dtac 错标
//      - 0x09-0x0D: 印度系文字 (Devanagari/Bengali/Gurmukhi/Gujarati/Oriya/Tamil/Telugu/Kannada/Malayalam)
//      - 0x0E-0x0F: 泰文 (U+0E00-U+0E5F)
//      - 0x10-0x10: 缅甸文 (U+1000-U+109F) — dtac 错标 DCS=0 的常见 case
//      - 0x17-0x17: 高棉文 (U+1780-U+17FF) — 泰柬边境诈骗短信
//      - 0x19-0x19: Tai Le / Limbu (U+1900-U+197F) — 泰国北部 / 云南
//      - 0x4E-0x9F: CJK 统一表意 (U+4E00-U+9FFF)
//      - 0x34-0x4B: CJK 扩展 A 等 (U+3400-U+4DBF, U+4E00-U+4BFF 边缘)
//   4. 跳过: 高字节 0x00 (ASCII 嵌入) / 高字节 0xD8-0xDF (surrogate)
bool looks_like_ucs2_be(const char* hex, size_t hexLen) {
  if (hexLen == 0 || (hexLen & 1)) return false;
  if (hexLen < 8) return false;  // 至少 2 对 (8 hex = 4 字节 = 2 UCS-2 字符)

  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };

  size_t pairHits = 0;
  size_t hiHits = 0;       // 落在 BMP 宽区 (CJK/泰文/印度/缅甸/...)
  size_t narrowHits = 0;   // 落在窄区 (印度/缅甸/高棉/Tai Le) — dtac 错标 DCS=0 关键信号
  for (size_t i = 0; i + 3 < hexLen; i += 4) {
    int h0 = nib(hex[i]);
    int h1 = nib(hex[i + 1]);
    int h2 = nib(hex[i + 2]);
    int h3 = nib(hex[i + 3]);
    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) continue;
    uint8_t hi = (uint8_t)((h0 << 4) | h1);
    if (hi == 0x00) continue;                // 跳过 ASCII 嵌入对
    if (hi >= 0xD8 && hi <= 0xDF) continue;  // 跳过 surrogate
    pairHits++;
    bool hiOk = (hi >= 0x09 && hi <= 0x0D)   // 印度系文字 (Devanagari/.../Malayalam)
             || (hi >= 0x0E && hi <= 0x0F)   // 泰文
             || (hi == 0x10)                 // 缅甸文 (dtac 错标常见)
             || (hi == 0x17)                 // 高棉文
             || (hi == 0x19)                 // Tai Le / Limbu
             || (hi >= 0x4E && hi <= 0x9F)   // CJK
             || (hi >= 0x34 && hi <= 0x4B);  // CJK 扩展 A 等
    if (hiOk) hiHits++;
    // 窄区只算 0x09-0x0D + 0x10 + 0x17 + 0x19 (不含 0x0E-0x0F 泰文 / 0x34-0x4B CJK 扩展 A)
    // 7-bit packed 随机落在 0x09-0x19 区间概率 = 17/256 ≈ 6.6%, 100 对期望 6.6
    // 设阈值 4 对略高于随机, 拒绝 7-bit 但放过多语种混排 (缅文 + 中文 + 音标拉丁)
    if ((hi >= 0x09 && hi <= 0x0D) || hi == 0x10 || hi == 0x17 || hi == 0x19) {
      narrowHits++;
    }
  }
  // 至少 4 对 (8 个 UCS-2 字符); 命中条件 OR:
  //   A) 80% 落在 BMP 宽区 (CJK/泰文为主) — 严格, 防 7-bit packed 偶发命中
  //   B) 窄区 (印度/缅甸/高棉/Tai Le) 双阈值: 绝对 >= 6 对 且 相对 >= 12%
  //      应对 dtac 错标 DCS=0 的多语种混排 (宽区命中率会掉到 60-70%, 被音标拉丁/数字拉低)
  //      阈值来源: 7-bit packed byte 范围 0-0x7F, 落在 {0x09-0x0D,0x10,0x17,0x19} = 8/128 = 6.25%
  //        Bin(160, 0.0625) 期望 10, P(X>=6) ≈ 93% (泊松 λ=10 近似)
  //        → 单靠 6 绝对阈值放过多, 必须靠 12% 比例防线挡 7-bit
  //        6 绝对阈值是辅助 (排除 pairHits 巨大但比例低的 7-bit 巧合, 例如 200 pair 6 对窄区 = 3% < 12% 挡)
  //        缅甸文典型 (12/71=17%) 仍能命中
  return pairHits >= 4 && (
      (hiHits * 100 / pairHits) >= 80 ||
      (narrowHits >= 6 && (narrowHits * 100 / pairHits) >= 12)
  );
}

// 从完整 PDU hex 跳过头部 (SCA + FO + OA + PID + DCS + SCTS + UDL), 返回 UD 起始 (hex 偏移)
// *outUdByteLen: UD 字节数 (7-bit 按 ceil(UDL*7/8) 算, 8-bit/UCS-2 按 UDL 算)
// 返回 0 = PDU 太短/格式错 (caller 兜底用全 body 当 UD)
// 配套 sms_task 在 CMT 路径: msg.body_hex 是整条 PDU, decode 函数只该吃 UD
size_t pdu_ud_offset(const char* hex, size_t hexLen, bool is7bit, size_t* outUdByteLen) {
  if (outUdByteLen) *outUdByteLen = 0;
  if (!hex || hexLen < 4 || !outUdByteLen) return 0;

  auto isH = [](char c) -> bool {
    return (c>='0'&&c<='9') || (c>='A'&&c<='F') || (c>='a'&&c<='f');
  };
  auto nib = [](char c) -> int {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='A'&&c<='F') return c-'A'+10;
    if (c>='a'&&c<='f') return c-'a'+10;
    return -1;
  };
  // 读 1 byte (2 hex chars), 越界/非法 hex → -1
  auto byte = [&](size_t pos) -> int {
    if (pos + 2 > hexLen) return -1;
    if (!isH(hex[pos]) || !isH(hex[pos+1])) return -1;
    int hi = nib(hex[pos]), lo = nib(hex[pos+1]);
    return (hi<<4) | lo;
  };

  // SCA length (1 byte) + SCA 数据 (scaLen bytes, 0..12)
  int scaLen = byte(0);
  if (scaLen < 0 || scaLen > 12) return 0;
  size_t pos = 2 + (size_t)scaLen * 2;

  // FO (1 byte)
  if (byte(pos) < 0) return 0;
  pos += 2;

  // OA length (1 byte, in digits) + type (1 byte) + BCD (ceil(N/2) bytes)
  int oaLen = byte(pos);
  if (oaLen < 0 || oaLen > 20) return 0;
  pos += 2;                              // length byte
  pos += 2;                              // type byte
  pos += ((oaLen + 1) / 2) * 2;         // BCD bytes, rounded up to hex char

  // PID (1 byte) + DCS (1 byte) + SCTS (7 bytes)
  if (pos + 2 + 2 + 14 > hexLen) return 0;
  pos += 2;  // PID
  pos += 2;  // DCS
  pos += 14; // SCTS

  // UDL (1 byte)
  int udl = byte(pos);
  if (udl < 0 || udl > 255) return 0;
  pos += 2;

  // UD 字节数
  if (is7bit) {
    *outUdByteLen = ((size_t)udl * 7 + 7) / 8;  // ceil(udl*7/8)
  } else {
    *outUdByteLen = (size_t)udl;
  }

  if (pos + *outUdByteLen * 2 > hexLen) return 0;  // UD 越界
  return pos;
}

// v4.0.11: 同 pdu_ud_offset, 但 DCS 自动从 TPDU DCS byte 读 (不信 +CMT 头 dcs 字段)
// ML307 quirk: +CMT 头 dcs=255 / dcs=0 是错的, 真相在 TPDU DCS byte (SCTS 之前 2 hex)
// reserved DCS (0x10-0x1F, 0x20-0xFF 等) 时 sniff UD bytes: 偶数 + 泰/中高字节密集 → 判 UCS-2
// *outIsUcs2: caller 据此选 ucs2_hex_to_utf8 (UCS-2) / decode_7bit_packed (7-bit) / 原样 (8-bit)
// *outIs7bit: DCS=0x00 才是真 7-bit; 其他按 8-bit/UCS-2 算 (octets=UDL)
size_t pdu_ud_offset_ex(const char* hex, size_t hexLen,
                       bool* outIsUcs2, bool* outIs7bit, size_t* outUdByteLen) {
  if (outIsUcs2) *outIsUcs2 = false;
  if (outIs7bit) *outIs7bit = false;
  if (outUdByteLen) *outUdByteLen = 0;
  if (!hex || hexLen < 4) return 0;

  auto nib = [](char c) -> int {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='A'&&c<='F') return c-'A'+10;
    if (c>='a'&&c<='f') return c-'a'+10;
    return -1;
  };
  auto byte = [&](size_t pos) -> int {
    if (pos + 2 > hexLen) return -1;
    int hi = nib(hex[pos]), lo = nib(hex[pos+1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi<<4) | lo;
  };

  // SCA
  int scaLen = byte(0);
  if (scaLen < 0 || scaLen > 12) return 0;
  size_t pos = 2 + (size_t)scaLen * 2;

  // FO
  if (byte(pos) < 0) return 0;
  pos += 2;

  // OA
  int oaLen = byte(pos);
  if (oaLen < 0 || oaLen > 20) return 0;
  pos += 2;                              // OA length byte
  int toa = byte(pos);
  if (toa < 0) return 0;
  pos += 2;                              // OA type byte

  // v4.0.20.1 fix: ML307 alphanumeric sender (ToA=0xD0) oaLen 字段不可信
  // 修前 bug: 沿用 BCD 公式 ((oaLen+1)/2) 算 OA value octets, 不匹配真实 GSM7 packed octets
  //   (翔哥 dtac oaLen=11 实发 9 octets, oaLen=14 实发 7 octets), PID/DCS/SCTS/UDL 全错位
  //   -> 后续 sniff UCS-2 还会 false-positive (泰文密集) -> 解出乱码
  // 修法: ToA=0xD0 时扫 [1, 20] octets 找合法 (PID=0x00, DCS=已知) 组合
  //   DCS 已知集 = 3GPP TS 23.038 §4 (GSM7 0x00-0x03/0x10-0x13/0xC0/0xD0/0xF0-0xF3,
  //   8-bit 0x04-0x07/0x14-0x17/0xF4-0xF7, UCS-2 0x08-0x0B/0x18-0x1B/0xE0/0xF8-0xFB)
  auto dcs_valid = [](int d) {
    if (d < 0) return false;
    if (d <= 0x03) return true;              // GSM7
    if (d >= 0x04 && d <= 0x0B) return true; // 8-bit / UCS-2 (no class)
    if (d >= 0x10 && d <= 0x1B) return true; // class bits
    if (d == 0xC0 || d == 0xC8 || d == 0xD0 || d == 0xD8) return true;  // MWI + UCS-2
    if (d == 0xE0) return true;              // UCS-2 (alt)
    if (d >= 0xF0 && d <= 0xFB) return true; // alt2
    return false;
  };
  bool isAlphaToA = ((toa >> 4) & 0x07) == 0x05;  // 3GPP TS 23.040 §9.1.2.5
  if (isAlphaToA) {
    // oaLen 字段不可信 (ML307 quirk: GSM7 alpha sender 字段是字符数, 不是 octets)
    // 扫 [1, 20] octets 找合法 (PID=0x00, DCS=已知) 组合, 同时校验 SCTS 像 BCD 时间戳
    //   (避免 OA value 中间夹 (0x00, valid_DCS) 误早停)
    // 3GPP TS 23.040 §9.2.3.11: SCTS 7 octets = swapped BCD year/month/day/hour/min/sec + tz
    //   这里只校前 6 octets (year 到 sec), tz 字节可含符号位 (>7 合法)
    size_t posAfterOa = 0;
    bool sctsFound = false;             // v4.0.20.3 fix: 区分 PID/DCS 命中 vs SCTS 命中, fallback 更准
    for (size_t octets = 1; octets <= 20; octets++) {
      size_t trial = pos + octets * 2;
      if (trial + 4 + 14 + 2 > hexLen) break;
      int trialPid = byte(trial);
      int trialDcs = byte(trial + 2);
      if (trialPid != 0x00 || !dcs_valid(trialDcs)) continue;
      // SCTS 二次校验: 前 6 octets 必须 swapped BCD (高低 nibble 都 0-9)
      bool sctsOk = true;
      for (int i = 0; i < 6; i++) {
        int b = byte(trial + 4 + (size_t)i * 2);
        if (b < 0 || ((b >> 4) & 0x0F) > 9 || (b & 0x0F) > 9) { sctsOk = false; break; }
      }
      posAfterOa = trial;
      if (sctsOk) {
        sctsFound = true;
        break;                          // 高置信, 锁住
      }
      // 不 break — 继续往后扫, 找 SCTS 也合法的位置
    }
    if (posAfterOa == 0) {
      // 1-20 octets 全无 (PID=0x00, DCS valid) → hex 太短 / 字段解析错, fallback BCD 公式
      posAfterOa = pos + ((oaLen + 1) / 2) * 2;
    } else if (!sctsFound) {
      // 1-20 octets 全 (PID=0, DCS valid) 但 SCTS 全不 OK → 不信 trial (20 octets 也会错位)
      //   fallback 老 BCD 公式, 错位概率比 20 octets trial 错位小 (老 PDU alpha sender 字段较准)
      posAfterOa = pos + ((oaLen + 1) / 2) * 2;
    }
    pos = posAfterOa;
  } else {
    pos += ((oaLen + 1) / 2) * 2;          // 数字 OA: BCD 公式, 信 oaLen
  }

  // PID (1) + DCS (1) + SCTS (7)
  if (pos + 2 + 2 + 14 > hexLen) return 0;
  pos += 2;  // PID
  int dcs = byte(pos);
  pos += 2;  // DCS
  pos += 14; // SCTS

  // UDL (1 byte)
  int udl = byte(pos);
  if (udl < 0 || udl > 255) return 0;
  pos += 2;

  // DCS 真相 + 算 UD byte 数
  bool is7bit = (dcs == 0x00);
  bool isUcs2 = (dcs == 0x08);
  // 算 UD byte 数 (基于真 DCS)
  size_t udByteLen = is7bit ? ((size_t)udl * 7 + 7) / 8 : (size_t)udl;

  // DCS reserved / 未知 (0x10-0x1F, 0x20-0xFF, 0x0C-0x0F 等) 时 sniff UD
  if (!is7bit && !isUcs2) {
    if (pos + udByteLen * 2 <= hexLen && udByteLen >= 4) {
      // 把 UD 当 UCS-2 BE 试解 + sniff 高字节
      if (looks_like_ucs2_be(hex + pos, udByteLen * 2)) {
        isUcs2 = true;
      }
    }
  }

  if (outIs7bit) *outIs7bit = is7bit;
  if (outIsUcs2) *outIsUcs2 = isUcs2;
  if (outUdByteLen) *outUdByteLen = udByteLen;

  // 注: 不严格检查 UD 越界, 允许 truncated body (caller concat 测试 body 不一定填全 UD)
  // caller 据 outUdByteLen + hexLen 自行 decide UD 是否越界
  return pos;
}

// 从完整 PDU 跳过 SCA+FO+OA+PID+DCS+SCTS+UDL+UDHL+UDH, 返回 UD 起点 (hex 偏移)
// *outUdhByteLen: UDH 段总字节数 (UDHL byte 1 + IEs bytes = UDHL+1), 仅诊断用
// 返回 0 = PDU 太短/格式错 (caller 兜底用全 body 当 UD)
//
// 修前 bug v4.0.9: 旧版用 oaLen 字段算 OA value 长度, 但实测 ML307 alphanumeric sender ToA=0xD0
//   时 oaLen 字段单位 = packed octets × 2 (非标准!), 导致 OA GSM7 跳错, 后续 PID/SCTS/UDL 全错位
// 修前 bug v4.0.9: 旧版返回 UDHL byte 位置, caller 算 udhSkip = udhOff + udhBytes*2 容易算错
// v4.0.10 fix: 改用 UDH IE pattern "0804" (16-bit concat) 或 "0003" (8-bit concat) 反查位置
//   - concat SMS 必有 0804 或 0003 IE, 找它就是 UDH IE 起点 (UDHL byte 之后)
//   - 然后向前反推: UDHL byte = UDH IE 起点 - 2 hex chars (UDHL + IEI byte)
//   - PDU header 总长 = UDHL byte 位置 = UDH IE 起点 - 2
//   - 读 UDHL byte 算 UDH 段总长, UD 起点 = UDHL 位置 + (UDHL+1)*2
//
//   局限: 只对 concat SMS 有效 (单条 SMS 没 UDH IE pattern)。单条走 fallback。
size_t pdu_udh_offset(const char* hex, size_t hexLen, size_t* outUdhByteLen) {
  if (outUdhByteLen) *outUdhByteLen = 0;
  if (!hex || hexLen < 4 || !outUdhByteLen) return 0;

  auto isH = [](char c) -> bool {
    return (c>='0'&&c<='9') || (c>='A'&&c<='F') || (c>='a'&&c<='f');
  };
  auto nib = [](char c) -> int {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='A'&&c<='F') return c-'A'+10;
    if (c>='a'&&c<='f') return c-'a'+10;
    return -1;
  };
  auto byte = [&](size_t pos) -> int {
    if (pos + 2 > hexLen) return -1;
    if (!isH(hex[pos]) || !isH(hex[pos+1])) return -1;
    int hi = nib(hex[pos]), lo = nib(hex[pos+1]);
    return (hi<<4) | lo;
  };

  // 找 concat UDH IE: "0804" (16-bit, IEI=08 + IEDL=04) 或 "0003" (8-bit, IEI=00 + IEDL=03)
  const char* udhIe = NULL;
  size_t udhIeLen = 0;  // IE 段总长 (UDHL+IE) = 2+IEI+IEDL+...
  for (size_t i = 0; i + 4 <= hexLen; i += 2) {
    if (hex[i]=='0' && hex[i+1]=='8' && hex[i+2]=='0' && hex[i+3]=='4') {
      udhIe = hex + i;
      // UDHL byte 在 udhIe 之前 2 hex chars
      // IEI=08 IEDL=04 → IEI+IEDL+refH+refL+total+seq = 6 bytes (含 IEI/IEDL)
      // 整 UDH 段 = UDHL(1) + 6 bytes = 7 bytes = 14 hex
      udhIeLen = 14;
      break;
    }
    if (hex[i]=='0' && hex[i+1]=='0' && hex[i+2]=='0' && hex[i+3]=='3') {
      udhIe = hex + i;
      // IEI=00 IEDL=03 → IEI+IEDL+ref+total+seq = 5 bytes
      // 整 UDH 段 = UDHL(1) + 5 = 6 bytes = 12 hex
      udhIeLen = 12;
      break;
    }
  }
  if (!udhIe) return 0;

  // UDHL byte 位置 = udhIe - 2 hex chars
  size_t udhlPos = (size_t)(udhIe - hex) - 2;
  if (udhlPos + 2 > hexLen) return 0;

  // UDHL byte 值 (UDH 段总长不含 UDHL 自身? 3GPP: UDHL = UDH IE 总字节数,不含 UDHL)
  // UDHL=05 + IE 5 bytes = UDH 段 6 bytes
  int udhl = byte(udhlPos);
  if (udhl < 0 || udhl > 140) return 0;

  *outUdhByteLen = (size_t)(udhl + 1);  // UDH 段总字节数 = UDHL + IEs
  size_t udStart = udhlPos + (size_t)(udhl + 1) * 2;  // UD 起点 = UDHL 后跳 (UDHL+1) bytes
  if (udStart > hexLen) return 0;  // UD 越界 (允许 ==, body 末)
  return udStart;
}

// v4.0.24: 从 UD 起点算 UDH 头长度, 返回 UD 真正数据起点 (hex 偏移)
// 跟 pdu_udh_offset 平行 (后者从完整 PDU 跳 SCA+FO+OA+...+UDL+UDHL+UDH, 给 stash_udh_part 用)
//  本函数从 UD 起点 (跟 pdu_ud_offset_ex 返回的 udHex 一致) 算, 给 sms_task decode path 用
// 逻辑: 读 UDHL byte, 算 UDH 段总长 = UDHL + 1 (UDHL byte + IE bytes)
// 跟 pdu_udh_offset 区别:
//   - pdu_udh_offset 反查 concat IE pattern "0804"/"0003", 局限只对 concat SMS 有效 (单条没 IE pattern fallback)
//   - pdu_udh_offset_ex 直接读 UDHL byte, 任何带 UDHI=1 的 PDU 都对 (concat / 多 IE / 自定义 IE)
//   - 配套 main.cpp UCS-2/8-bit raw decode path caller (前 2 个 path 不收 IE pattern, 第 3 个 7-bit 减 7 septets)
size_t pdu_udh_offset_ex(const char* udHex, size_t udHexLen, size_t* outUdhByteLen) {
  if (outUdhByteLen) *outUdhByteLen = 0;
  if (!udHex || udHexLen < 2 || !outUdhByteLen) return 0;

  auto isH = [](char c) -> bool {
    return (c>='0'&&c<='9') || (c>='A'&&c<='F') || (c>='a'&&c<='f');
  };
  auto nib = [](char c) -> int {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='A'&&c<='F') return c-'A'+10;
    if (c>='a'&&c<='f') return c-'a'+10;
    return -1;
  };

  // 读 UDHL byte (udHex[0..2])
  if (!isH(udHex[0]) || !isH(udHex[1])) return 0;
  int udhl = (nib(udHex[0]) << 4) | nib(udHex[1]);
  if (udhl <= 0) return 0;  // UDHL=0 = 单条 SMS 无 UDH (FO UDHI=0), caller 走无 skip 路径
  if (udhl > 140) return 0;  // 3GPP §9.2.3.24 UDHL 上限 140 bytes

  // UDH 段总字节数 = UDHL byte (1) + IE bytes (udhl)
  size_t udhBytes = (size_t)udhl + 1;
  size_t udhHexLen = udhBytes * 2;
  if (udhHexLen > udHexLen) return 0;  // UDH 段比 UD 还长, PDU 格式错 (跟 AOSP bufferLen<0 一致)

  *outUdhByteLen = udhBytes;
  return udhHexLen;  // UD 真正数据起点 hex 偏移 (UDHL+IE 头之后)
}

// 从完整 PDU hex 跳过 SCA + FO, 返回 OA 起始 (hex 偏移)
// *outIsAlpha: true=TON=alphanumeric (GSM7 packed), false=numeric (BCD nibble swap)
// *outValueOctets: OA value 段 octet 数
// 返回 0 = PDU 太短/格式错
size_t pdu_oa_offset(const char* hex, size_t hexLen, bool* outIsAlpha, size_t* outValueOctets) {
  if (outIsAlpha) *outIsAlpha = false;
  if (outValueOctets) *outValueOctets = 0;
  if (!hex || hexLen < 4 || !outIsAlpha || !outValueOctets) return 0;

  auto isH = [](char c) -> bool {
    return (c>='0'&&c<='9') || (c>='A'&&c<='F') || (c>='a'&&c<='f');
  };
  auto nib = [](char c) -> int {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='A'&&c<='F') return c-'A'+10;
    if (c>='a'&&c<='f') return c-'a'+10;
    return -1;
  };
  auto byte = [&](size_t pos) -> int {
    if (pos + 2 > hexLen) return -1;
    if (!isH(hex[pos]) || !isH(hex[pos+1])) return -1;
    int hi = nib(hex[pos]), lo = nib(hex[pos+1]);
    return (hi<<4) | lo;
  };

  // SCA length (1 byte) + SCA 数据 (scaLen bytes)
  int scaLen = byte(0);
  if (scaLen < 0 || scaLen > 12) return 0;
  size_t pos = 2 + (size_t)scaLen * 2;

  // FO (1 byte)
  if (byte(pos) < 0) return 0;
  pos += 2;

  // OA length (1 byte, in digits/chars) + ToA (1 byte) + value (ceil(N/2) octets)
  int oaLen = byte(pos);
  if (oaLen < 0 || oaLen > 20) return 0;
  pos += 2;  // length byte
  int toa = byte(pos);
  if (toa < 0) return 0;
  pos += 2;  // ToA byte
  size_t valueOctets = ((size_t)oaLen + 1) / 2;
  if (pos + valueOctets * 2 > hexLen) return 0;
  pos += valueOctets * 2;  // value

  // 3GPP TS 23.040 §9.1.2.5 + 23.038 §4: TON bits = (toa >> 4) & 0x07
  // 0b101 = alphanumeric (7-bit packed) — DTAC/AIS/Verify
  // 其它 = numeric BCD nibble swap
  *outIsAlpha = ((toa >> 4) & 0x07) == 0x05;
  *outValueOctets = valueOctets;
  return valueOctets > 0 ? pos - valueOctets * 2 : pos;  // 指向 OA value 起点 (skip length+ToA)
}

// GSM 7-bit packed (LSB-first) → UTF-8, 用于 OA alphanumeric sender
// 不处理 0x1B escape extension (单字符 sender 通常不用, DTAC/AIS/Verify 都是 basic set)
// 解出 nchars 个字符后, 自动 trim 末尾 GSM7 '@' (0x00 septet) — padding bits 必然为 0,
// 真业务名 (DTAC/AIS/TRUE/Verify 等) 从不以 '@' 结尾, trim 安全
size_t decode_gsm7_alpha_oa(const char* octets, size_t octetCount, size_t nchars,
                            char* out, size_t outLen) {
  if (!octets || !out || outLen == 0) return 0;
  size_t pos = 0;
  for (size_t i = 0; i < nchars; i++) {
    // LSB-first 7-bit unpack: char i 占 bit 范围 [i*7, i*7+6]
    uint8_t val = 0;
    for (int b = 0; b < 7; b++) {
      size_t bitPos = i * 7 + b;
      size_t byteIdx = bitPos / 8;
      if (byteIdx >= octetCount) break;  // 越界, 不读 (octets 是 raw bytes, 不能用 0 当哨兵)
      size_t bitInByte = bitPos % 8;
      uint8_t bit = (uint8_t)((octets[byteIdx] >> bitInByte) & 1);
      val |= (uint8_t)(bit << b);
    }
    if (val == 0x1B) continue;  // extension escape — basic-only 简化跳过
    if (val >= 128) continue;  // 越界 septet
    uint16_t u = gsm7_default_to_unicode[val];
    if (u == 0) continue;  // null
    // write UTF-8
    if (u < 0x80) {
      if (pos + 1 > outLen) return pos;
      out[pos++] = (char)u;
    } else if (u < 0x800) {
      if (pos + 2 > outLen) return pos;
      out[pos++] = (char)(0xC0 | (u >> 6));
      out[pos++] = (char)(0x80 | (u & 0x3F));
    } else {
      if (pos + 3 > outLen) return pos;
      out[pos++] = (char)(0xE0 | (u >> 12));
      out[pos++] = (char)(0x80 | ((u >> 6) & 0x3F));
      out[pos++] = (char)(0x80 | (u & 0x3F));
    }
  }
  // Trim 末尾 GSM7 '@' (0x40 = 0x00 septet) — padding bits=0 必然解出 '@', 业务名不会以 '@' 结尾
  // 但保留至少 1 char, 避免 0 解
  while (pos > 1 && pos < outLen && out[pos-1] == '@') pos--;
  return pos;
}

// =================== 发送侧 (v4.0.6+) ===================

int ucs2_encode(const char* utf8, char* ucs2_hex_out, size_t out_cap) {
  if (!utf8 || !ucs2_hex_out || out_cap < 5) return -1;
  static const char H[] = "0123456789ABCDEF";
  size_t pos = 0;        // byte pos in utf8
  size_t out_pos = 0;    // char pos in ucs2_hex_out (每 codepoint 4 hex)

  while (utf8[pos]) {
    uint32_t cp = 0;
    uint8_t b0 = (uint8_t)utf8[pos];
    int extra = 0;
    if      (b0 < 0x80)  { cp = b0;          extra = 0; }
    else if (b0 < 0xC0)  { return -1; }                     // lone continuation
    else if (b0 < 0xE0)  { cp = b0 & 0x1F;   extra = 1; }
    else if (b0 < 0xF0)  { cp = b0 & 0x0F;   extra = 2; }
    else                 { return -1; }                     // 4-byte UCS, 不在 BMP

    pos++;
    for (int i = 0; i < extra; i++) {
      uint8_t bc = (uint8_t)utf8[pos];
      if (bc < 0x80 || bc >= 0xC0) return -1;               // 期望 continuation byte
      cp = (cp << 6) | (bc & 0x3F);
      pos++;
    }
    // 跳过 surrogate (UTF-8 合法输入不应有, 但防御一下)
    if (cp >= 0xD800 && cp <= 0xDFFF) continue;
    if (cp > 0xFFFF) return -1;                             // 非 BMP

    if (out_pos + 4 >= out_cap) return -1;                   // 需要 4 hex + 1 null
    ucs2_hex_out[out_pos++] = H[(cp >> 12) & 0xF];
    ucs2_hex_out[out_pos++] = H[(cp >> 8)  & 0xF];
    ucs2_hex_out[out_pos++] = H[(cp >> 4)  & 0xF];
    ucs2_hex_out[out_pos++] = H[cp & 0xF];
  }

  ucs2_hex_out[out_pos] = 0;
  return (int)out_pos;     // 写入的 hex 字符数 (不含 \0); 1 codepoint = 4 hex
}

int sms_split_for_send(int total_chars, int max_chars_per_seg,
                       SmsPart out_parts[], int max_parts) {
  if (total_chars <= 0 || max_chars_per_seg <= 0 || !out_parts || max_parts <= 0) return 0;

  // ceil(total / max) — GSM UDH ref 8-bit 最大 255 段
  int n = (total_chars + max_chars_per_seg - 1) / max_chars_per_seg;
  if (n > 255 || n > max_parts) return 0;

  int offset = 0;
  int remaining = total_chars;
  for (int i = 0; i < n; i++) {
    int len = (remaining > max_chars_per_seg) ? max_chars_per_seg : remaining;
    out_parts[i].offset_chars = offset;
    out_parts[i].len_chars = len;
    offset += len;
    remaining -= len;
  }
  return n;
}

// BCD 翻转编码 (GSM 04.08 §10.5.4.7) — 输出 hex, 返回 digit count (DA length 用)
int bcd_encode_phone(const char* phone_in, char* out, size_t out_cap) {
  if (!phone_in || !out) return -1;
  char digits[32];
  int n = 0;
  for (const char* p = phone_in; *p && n < (int)sizeof(digits) - 1; p++) {
    if (*p >= '0' && *p <= '9') digits[n++] = *p;
    else if (*p == '+' || *p == '-' || *p == ' ') continue;
    else return -1;  // 非数字字符 → 拒绝
  }
  if (n == 0 || n > 20) return -1;
  size_t pos = 0;
  for (int i = 0; i < n; i += 2) {
    if (pos + 2 >= out_cap) return -1;
    if (i + 1 < n) {
      out[pos++] = digits[i + 1];   // 高 nibble = 原偶数位
      out[pos++] = digits[i];       // 低 nibble = 原奇数位
    } else {
      out[pos++] = 'F';             // 奇数位: 高 nibble = F 填充
      out[pos++] = digits[i];       // 低 nibble = 末位奇数位
    }
  }
  out[pos] = 0;
  return n;  // decimal digit count (DA length 字段用此值, 不是 hex char 数)
}

// 构造 CMGS TPDU hex (不含 SCA)
int cmgs_build_pdu(const char* phone, const char* body,
                   int seg_idx, const SmsPart parts[], int total,
                   uint8_t ref, char* pdu_out, size_t out_cap) {
  if (!phone || !body || !pdu_out) return -1;
  // 1. BCD 编码 phone → DA digits + digit count
  char da_digits[32];
  int da_digits_count = bcd_encode_phone(phone, da_digits, sizeof(da_digits));
  if (da_digits_count < 0) return -1;
  int da_hex_len = (int)strlen(da_digits);  // hex chars 数 (= BCD bytes × 2)
  // 2. UCS2 编码 body
  char ucs2[1024];
  int ucs2_n = ucs2_encode(body, ucs2, sizeof(ucs2));
  if (ucs2_n < 0) return -1;
  int total_chars = ucs2_n / 4;
  // 3. 拼 PDU (hex 大写, 不含 SCA)
  // 单条: PDU-type 0x11 (submit + UDHI=0 + relative VP)
  // 拼接: PDU-type 0x51 (submit + UDHI=1 + relative VP) — UDH = 05 00 03 RR TT SS
  bool concat = (seg_idx >= 0 && parts && total > 1);
  size_t pos = 0;
  // PDU-type
  if (pos + 2 >= out_cap) return -1;
  pdu_out[pos++] = concat ? '5' : '1';
  pdu_out[pos++] = '1';
  // MR
  pdu_out[pos++] = '0';
  pdu_out[pos++] = '0';
  // DA length (decimal digit count, 不是 hex char 数 — GSM 03.40 §9.1.2.5)
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%02X", da_digits_count);
  if (pos + 2 >= out_cap) return -1;
  pdu_out[pos++] = tmp[0];
  pdu_out[pos++] = tmp[1];
  // ToA (固定 0x81 unknown/ISDN; 国际号需 0x91 — F4 留 P1)
  pdu_out[pos++] = '8';
  pdu_out[pos++] = '1';
  // DA digits (hex)
  for (int i = 0; i < da_hex_len; i++) {
    if (pos + 1 >= out_cap) return -1;
    char c = da_digits[i];
    if (c >= 'a' && c <= 'f') c -= 32;
    pdu_out[pos++] = c;
  }
  // PID
  pdu_out[pos++] = '0';
  pdu_out[pos++] = '0';
  // DCS: 0x08 = UCS2
  pdu_out[pos++] = '0';
  pdu_out[pos++] = '8';
  // VP: 0xAA = 4 days (relative)
  pdu_out[pos++] = 'A';
  pdu_out[pos++] = 'A';
  // 4. UDH (拼接时)
  int udh_bytes = 0;
  if (concat) {
    // 8-bit concat: 05 00 03 RR TT SS (5 bytes = 10 hex)
    udh_bytes = 5;
    if (pos + 10 >= out_cap) return -1;
    pdu_out[pos++] = '0'; pdu_out[pos++] = '5';
    pdu_out[pos++] = '0'; pdu_out[pos++] = '0';
    pdu_out[pos++] = '0'; pdu_out[pos++] = '3';
    snprintf(tmp, sizeof(tmp), "%02X", ref);
    pdu_out[pos++] = tmp[0]; pdu_out[pos++] = tmp[1];
    snprintf(tmp, sizeof(tmp), "%02X", total);
    pdu_out[pos++] = tmp[0]; pdu_out[pos++] = tmp[1];
    snprintf(tmp, sizeof(tmp), "%02X", seg_idx + 1);
    pdu_out[pos++] = tmp[0]; pdu_out[pos++] = tmp[1];
  }
  // 5. UDL = UD 字节数 (UDH bytes + UCS2 user bytes = udh_bytes + part_len*2)
  int part_len = (seg_idx >= 0 && parts) ? parts[seg_idx].len_chars : total_chars;
  int ud_bytes = udh_bytes + part_len * 2;
  snprintf(tmp, sizeof(tmp), "%02X", ud_bytes);
  pdu_out[pos++] = tmp[0]; pdu_out[pos++] = tmp[1];
  // 6. UD: UDH hex (拼接时已写) + 本段 UCS2 hex
  const char* ud_start = ucs2 + ((seg_idx >= 0 && parts) ? parts[seg_idx].offset_chars * 4 : 0);
  int ud_hex_len = part_len * 4;
  for (int i = 0; i < ud_hex_len; i++) {
    if (pos + 1 >= out_cap) return -1;
    char c = ud_start[i];
    if (c >= 'a' && c <= 'f') c -= 32;
    pdu_out[pos++] = c;
  }
  pdu_out[pos] = 0;
  return pos;
}

}  // namespace pdu