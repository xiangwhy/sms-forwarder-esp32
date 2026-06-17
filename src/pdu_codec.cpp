// PDU 解码实现 — 函数体从 src/main.cpp 原样搬迁, 只把 String 改成 char* 缓冲写
// 原 src/main.cpp 版本是 v4.0.2 实测稳定, 行为对齐避免引入新 bug

#include "pdu_codec.h"

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

}  // namespace pdu