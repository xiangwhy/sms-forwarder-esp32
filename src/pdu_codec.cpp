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

}  // namespace pdu