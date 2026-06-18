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
//   3. 高字节落在 BMP 高频区比例 >= 80% (严格, 防 7-bit packed 偶发命中)
//      - 0x0E-0x0F: 泰文 (U+0E00-U+0E5F)
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
  size_t hiHits = 0;
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
    bool hiOk = (hi >= 0x0E && hi <= 0x0F)   // 泰文
             || (hi >= 0x4E && hi <= 0x9F)   // CJK
             || (hi >= 0x34 && hi <= 0x4B);  // CJK 扩展 A 等
    if (hiOk) hiHits++;
  }
  // 至少 4 对 (8 个 UCS-2 字符) + 80% 高频区命中
  return pairHits >= 4 && (hiHits * 100 / pairHits) >= 80;
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