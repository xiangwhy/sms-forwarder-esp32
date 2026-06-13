#!/usr/bin/env python3
"""
PDU 解析器单元测试 (Python 模拟)

参考 C++ 代码:
  gsm7bit_unpack(const uint8_t* in, int in_bytes, int septet_count, char* out, int out_max)
  ucs2_bytes_to_utf8(const uint8_t* in, int in_bytes, char* out, int out_max)
  parse_pdu_sms_deliver(pdu_hex, SmsMsg* msg)  // v4.1: 直接填 SmsMsg, 字段:
                                              //   phone_utf8, body_utf8,
                                              //   concat_ref, concat_total, concat_seq

翔哥用这个先在本地确认 PDU 解析器逻辑对, 然后再烧 v4.1.1 板子。
"""

import sys

# =================== GSM 03.38 字符表 (与 C++ 表一致) ===================
GSMBASIC = bytearray([
    0x40, 0xA3, 0x24, 0xA5, 0xE8, 0xE9, 0xF9, 0xEC, 0xF2, 0xC7, 0x0A, 0xD8, 0xF8, 0x0D, 0xC5, 0xE5,
    0xCE, 0xC6, 0xE6, 0xDF, 0xC9, 0x20, 0xC4, 0xD6, 0xD1, 0xDC, 0xA7, 0xBF, 0xCB, 0xC0, 0xC2, 0xC1,
    0x21, 0x22, 0x23, 0xA4, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40,
    0xA1, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0xC4, 0xD6, 0xD1, 0xDC, 0xA7,
    0xBF, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0xC4, 0xD6, 0xD1, 0xDC, 0xA7,
])

GSMEXT = bytearray(128)


def gsm7bit_unpack(packed_bytes, in_bytes, septet_count):
    """对照 C++ gsm7bit_unpack (v4.1: 加 in_bytes 参数避免 strlen)"""
    out = []
    bit_off = 0
    in_ext = False
    for i in range(septet_count):
        byte_idx = bit_off // 8
        if byte_idx >= in_bytes + 1:
            break
        if byte_idx < in_bytes:
            b = packed_bytes[byte_idx]
            if bit_off % 8 > 1 and byte_idx + 1 < in_bytes:
                b = (b >> (bit_off % 8)) | (packed_bytes[byte_idx + 1] << (8 - bit_off % 8))
            else:
                b = b >> (bit_off % 8)
            septet = b & 0x7F
        else:
            septet = 0
        bit_off += 7

        if in_ext:
            in_ext = False
            if septet < len(GSMEXT) and GSMEXT[septet]:
                out.append(chr(GSMEXT[septet]))
        elif septet == 0x1B:
            in_ext = True
        else:
            if septet < len(GSMBASIC):
                out.append(chr(GSMBASIC[septet]))
    return ''.join(out)


def ucs2_bytes_to_utf8(ucs2_bytes):
    """对照 C++ ucs2_bytes_to_utf8 (BE 字节序)

    Python 字符串是 unicode, 直接 chr(u), 不要 encode UTF-8 再 latin-1 decode.
    """
    out = []
    for i in range(0, len(ucs2_bytes) - 1, 2):
        u = (ucs2_bytes[i] << 8) | ucs2_bytes[i + 1]
        if u == 0:
            continue
        if u < 0x80:
            out.append(chr(u))
        elif u < 0x800:
            out.append(chr(u))  # Python 直接接受 unicode
        elif u < 0xD800 or u > 0xDFFF:
            out.append(chr(u))
    return ''.join(out)


def parse_pdu_sms_deliver(pdu_hex):
    """对照 C++ parse_pdu_sms_deliver (v4.1: 返回 SmsMsg-style dict)
    3GPP TS 23.040 §9.1.2.5: OA/DA 数字 BCD 是低位 nibble 先 (LO-HI).
    """
    if not pdu_hex or len(pdu_hex) < 2:
        return None
    b = bytes.fromhex(pdu_hex)

    pos = 0
    if pos + 1 > len(b): return None
    sca_len = b[pos]; pos += 1
    if sca_len > 0:
        if pos + sca_len > len(b): return None
        pos += sca_len
    if pos + 1 > len(b): return None
    pdu_type = b[pos]; pos += 1
    udhi = (pdu_type & 0x40) != 0
    if pos + 1 > len(b): return None
    oa_len = b[pos]; pos += 1
    if oa_len == 0 or pos + 2 + (oa_len + 1) // 2 > len(b):
        return None
    oa_type = b[pos]; pos += 1
    oa_bytes = (oa_len + 1) // 2  # 11 数字 → 6 字节 BCD
    phone = ''
    for i in range(oa_bytes):
        if pos + i >= len(b): break
        byte = b[pos + i]
        # 3GPP 顺序: 低 nibble 先 (早位), 高 nibble 后 (晚位)
        lo = byte & 0x0F
        hi = (byte >> 4) & 0x0F
        is_last = (i == oa_bytes - 1)
        # 先输出 lo
        phone += chr(ord('0') + lo)
        # 输出 hi 除非是 F pad
        if not (is_last and hi == 0x0F):
            phone += chr(ord('0') + hi)
    pos += oa_bytes
    if pos + 1 > len(b): return None
    pid = b[pos]; pos += 1
    if pos + 1 > len(b): return None
    dcs = b[pos]; pos += 1
    if pos + 7 > len(b): return None
    pos += 7  # SCTS
    if pos + 1 > len(b): return None
    udl = b[pos]; pos += 1
    if udl == 0:
        return {'phone': phone, 'body': '', 'udh': b'', 'udh_len': 0, 'udhi': udhi,
                'dcs': dcs,
                'concat_ref': 0, 'concat_total': 0, 'concat_seq': 0}
    udh_len = 0
    udh = b''
    if udhi and pos < len(b):
        if pos + 1 > len(b): return None
        udhl = b[pos]; pos += 1
        if pos + udhl > len(b): return None   # 越界保护
        udh_len = min(udhl, 64)
        udh = b[pos:pos+udh_len]
        pos += udhl
    ud_bytes = len(b) - pos
    if ud_bytes < 0: ud_bytes = 0
    if ud_bytes > udl: ud_bytes = udl
    ud = b[pos:pos+ud_bytes]
    if dcs == 8:
        body = ucs2_bytes_to_utf8(ud)
    elif dcs == 0:
        body = gsm7bit_unpack(ud, ud_bytes, udl)
    else:
        # 8-bit data: 透传
        body = ud.decode('latin-1', errors='replace')

    concat_ref = 0
    concat_total = 0
    concat_seq = 0
    if udh_len > 0:
        p = 0
        while p + 1 < udh_len:
            iei = udh[p]
            iedl = udh[p+1]
            if p + 2 + iedl > udh_len: break
            if iei == 0x00 and iedl == 0x03:
                concat_ref   = udh[p+2]
                concat_total = udh[p+3]
                concat_seq   = udh[p+4]
                break
            if iei == 0x08 and iedl == 0x04:
                concat_ref   = (udh[p+2] << 8) | udh[p+3]
                concat_total = udh[p+4]
                concat_seq   = udh[p+5]
                break
            p += 2 + iedl

    return {'phone': phone, 'body': body, 'udh': udh, 'udh_len': udh_len,
            'udhi': udhi, 'dcs': dcs,
            'concat_ref': concat_ref, 'concat_total': concat_total, 'concat_seq': concat_seq}


# =================== 测试用例 ===================

def test_gsm7bit_basic():
    """GSM 7-bit unpack: 简单 'Hello' (5 字符)
    Packed bytes 'Hello' = 0xC8 0x32 0x9B 0xFD 0x06
    """
    bytes_packed = bytes([0xC8, 0x32, 0x9B, 0xFD, 0x06])
    result = gsm7bit_unpack(bytes_packed, len(bytes_packed), 5)
    assert result == "Hello", f"got {result!r}"
    print(f"  GSM 7-bit 'Hello' unpack: '{result}' ✓")


def test_gsm7bit_thai():
    """GSM 7-bit: 'Test' (4 字符)
    Packed bytes = 0xD4 0xF2 0x9C 0x0E
    """
    bytes_packed = bytes([0xD4, 0xF2, 0x9C, 0x0E])
    result = gsm7bit_unpack(bytes_packed, len(bytes_packed), 4)
    assert result == "Test", f"got {result!r}"
    print(f"  GSM 7-bit 'Test' unpack: '{result}' ✓")


def test_ucs2_thai():
    """UCS-2 BE: 'รหัส' (4 泰文字符, U+0E23 U+0E2B U+0E31 U+0E2A)"""
    raw = bytes([0x0E, 0x23, 0x0E, 0x2B, 0x0E, 0x31, 0x0E, 0x2A])
    result = ucs2_bytes_to_utf8(raw)
    assert result == "รหัส", f"got {result!r} (len={len(result)})"
    print(f"  UCS-2 'รหัส' decode: '{result}' (len={len(result)}) ✓")


def test_ucs2_chinese():
    """UCS-2 BE: '你好' (U+4F60 U+597D)"""
    raw = bytes([0x4F, 0x60, 0x59, 0x7D])
    result = ucs2_bytes_to_utf8(raw)
    assert result == "你好", f"got {result!r}"
    print(f"  UCS-2 '你好' decode: '{result}' ✓")


def test_ucs2_mixed():
    """UCS-2 BE: 'A你B' (0x41 0x4F60 0x42)"""
    raw = bytes([0x00, 0x41, 0x4F, 0x60, 0x00, 0x42])
    result = ucs2_bytes_to_utf8(raw)
    assert result == "A你B", f"got {result!r}"
    print(f"  UCS-2 'A你B' decode: '{result}' ✓")


def test_parse_pdu_short_ucs2():
    """TPDU 解析: 短 UCS-2 SMS, sender +86138REDACTED_WIFI_PASS, body 'Test123 上线测试'
    3GPP spec: OA 数字 BCD 是低 nibble 先, 11 数字 + F pad → 6 bytes
      digits: 8 6 1 3 8 0 7 7 1 0 7 4F
      bytes : 0x68 0x31 0x08 0x77 0x10 0x4F
    """
    sca = "00"
    pdu_type = "04"  # SMS-DELIVER no UDHI
    oa_len = "0B"  # 11 digits
    oa_type = "91"  # international
    oa = "6831087701F7"  # spec LO-HI: 11 digits 86138077107 + F pad in HIGH nibble
    pid = "00"
    dcs = "08"  # UCS-2
    scts = "0" * 14  # 7 bytes zero
    body = ("0054" + "0065" + "0073" + "0074" +  # Test
            "0031" + "0032" + "0033" +  # 123
            "0020" +  # space
            "4E0A" + "7EBF" + "6D4B" + "8BD5")  # 上线测试
    udl = format(len(body) // 2, '02X')
    pdu_hex = sca + pdu_type + oa_len + oa_type + oa + pid + dcs + scts + udl + body
    expected_body = "Test123 上线测试"
    expected_phone = "86138077107"  # 11 digits, F pad stripped

    print(f"  PDU hex: {pdu_hex}")
    result = parse_pdu_sms_deliver(pdu_hex)
    print(f"  parsed: {result}")
    assert result is not None
    assert result['phone'] == expected_phone, f"phone got {result['phone']!r}"
    assert result['body'] == expected_body, f"body got {result['body']!r}"
    assert result['concat_total'] == 0
    print(f"  PDU short UCS-2: phone={result['phone']} body={result['body']!r} ✓")


def test_parse_pdu_long_concat_8bit():
    """TPDU 解析: 长 UCS-2 SMS 8-bit concat (IEI=00), 2 段
    Sender +86138REDACTED_WIFI_PASS
    Body 'ABCD' (4 chars 段 1)
    """
    pdu_type = "44"  # SMS-DELIVER UDHI=1
    oa_len = "0B"
    oa_type = "91"
    oa = "6831087701F7"  # spec LO-HI
    pid = "00"
    dcs = "08"
    scts = "0" * 14
    # UDH: 8-bit concat (IEI=00, IEDL=03, ref=42, total=2, seq=1)
    # UDHL = 5, then IEI + IEDL + 3 data = 5 bytes
    udh = "05" + "00" + "03" + "2A0201"
    body_data = "0041004200430044"  # ABCD
    udl = format((len(udh) + len(body_data)) // 2, '02X')
    pdu_hex = "00" + pdu_type + oa_len + oa_type + oa + pid + dcs + scts + udl + udh + body_data
    print(f"  PDU hex: {pdu_hex}")
    result = parse_pdu_sms_deliver(pdu_hex)
    assert result is not None
    assert result['phone'] == "86138077107", f"phone got {result['phone']!r}"
    assert result['body'] == "ABCD", f"body got {result['body']!r}"
    assert result['concat_ref'] == 42, f"concat_ref got {result['concat_ref']}"
    assert result['concat_total'] == 2
    assert result['concat_seq'] == 1
    print(f"  PDU long 8-bit concat: phone={result['phone']} body={result['body']!r} refId=42 t=2 s=1 ✓")


def test_parse_pdu_long_concat_16bit():
    """TPDU 解析: 长 UCS-2 SMS 16-bit concat (IEI=08), 2 段
    Sender +86138REDACTED_WIFI_PASS
    Body 'E' (1 char 段 1)
    """
    pdu_type = "44"
    oa_len = "0B"
    oa_type = "91"
    oa = "6831087701F7"  # spec LO-HI
    pid = "00"
    dcs = "08"
    scts = "0" * 14
    # UDH: 16-bit concat (IEI=08, IEDL=04, refH=7E, refL=FC=0x7EFC=32508, total=2, seq=1)
    udh = "06" + "08" + "04" + "7EFC0201"
    body_data = "0045"
    udl = format((len(udh) // 2) + (len(body_data) // 2), '02X')
    pdu_hex = "00" + pdu_type + oa_len + oa_type + oa + pid + dcs + scts + udl + udh + body_data
    print(f"  PDU hex: {pdu_hex}")
    result = parse_pdu_sms_deliver(pdu_hex)
    print(f"  parse: {result}")
    assert result is not None
    assert result['body'] == "E", f"body got {result['body']!r}"
    assert result['concat_ref'] == 0x7EFC, f"concat_ref got {result['concat_ref']}"
    assert result['concat_total'] == 2
    assert result['concat_seq'] == 1
    print(f"  PDU long 16-bit concat: body={result['body']!r} refId=0x7EFC t=2 s=1 ✓")


def test_parse_pdu_gsm7bit():
    """TPDU 解析: 短 GSM 7-bit SMS, body 'Hello' (5 chars)
    """
    sca = "00"
    pdu_type = "04"
    oa_len = "0B"
    oa_type = "91"
    oa = "6831087701F7"  # spec LO-HI
    pid = "00"
    dcs = "00"  # GSM 7-bit
    scts = "0" * 14
    body_data = "C8329BFD06"  # 'Hello' packed, septet_count=5
    udl = format(5, '02X')
    pdu_hex = sca + pdu_type + oa_len + oa_type + oa + pid + dcs + scts + udl + body_data
    print(f"  PDU hex: {pdu_hex}")
    result = parse_pdu_sms_deliver(pdu_hex)
    print(f"  parse: {result}")
    assert result is not None
    assert result['body'] == "Hello", f"got {result['body']!r}"
    assert result['concat_total'] == 0
    print(f"  PDU GSM 7-bit 'Hello': body={result['body']!r} ✓")


def test_parse_pdu_truncated():
    """P1-7 corner case: 畸形 PDU (SCA 长度被截断, 应返回 None 不崩)"""
    # 头几个字节就被截断
    assert parse_pdu_sms_deliver("") is None
    assert parse_pdu_sms_deliver("0") is None   # 奇数长度, Python 拒 (C++ strlen 也拒)
    assert parse_pdu_sms_deliver("00") is None      # 只有 SCA len, 没 PDU-type
    # SCA 长度超大, 但实际没那么多字节
    assert parse_pdu_sms_deliver("FF") is None
    # 完整头但 OA 字节数不够
    assert parse_pdu_sms_deliver("000491") is None  # OA 0B 但只有 1 字节
    # 看似正常但 UDL 之后没数据
    pdu = ("00" + "04" + "0B" + "91" + "6831087701F7" + "00" + "08" + "0" * 14 + "00")
    result = parse_pdu_sms_deliver(pdu)
    assert result is not None
    assert result['body'] == "", f"empty UDL should give empty body, got {result['body']!r}"
    print(f"  PDU truncated/edge cases: 6 inputs all handled safely ✓")


def test_parse_pdu_udhl_overflow():
    """P1-7 corner case: UDHL 声明超长, 应返回 None (不越界读)"""
    # 正常头 + UDHIE=1 (pdu_type=44) + UDHL=20 (声明 20 字节 UDH) 但实际只有 5 字节
    sca = "00"
    pdu_type = "44"  # UDHI=1
    oa = "6831087701F7"
    udhl = "14"  # 声明 20 字节 UDH
    real_udh = "0003420201"  # 5 字节 (实际)
    body = "0041"  # 2 字节
    udl = format(len(real_udh) // 2 + len(body) // 2, '02X')
    pdu = sca + pdu_type + "0B" + "91" + oa + "00" + "08" + "0" * 14 + udl + udhl + real_udh + body
    # UDHL 声明 20, 但后面只有 5+2=7 字节 → 越界, parser 应返回 None
    result = parse_pdu_sms_deliver(pdu)
    assert result is None, f"UDHL overflow should return None, got {result}"
    print(f"  PDU UDHL overflow: rejected as None ✓")


def test_parse_cmgl_reply():
    """模拟 scan_sms_storage 解析: 3 条 +CMGL 累加的 reply"""
    # 真实 ML307 +CMGL=0 回复格式: +CMGL: idx,stat,,len \n PDU \n +CMGL: ...
    reply = ("+CMGL: 5,0,,14\n"        # idx=5, stat=0 unread, 14 字节 PDU
             "0791163108707147F0040B916831087710F7000800000000000000000AA0AE8329BFD0E\n"  # 14 字节 PDU (凑)
             "+CMGL: 6,0,,14\n"
             "0791163108707147F0040B916831087710F7000800000000000000000AA0AE8329BFD0E\n"
             "+CMGL: 7,0,,14\n"
             "0791163108707147F0040B916831087710F7000800000000000000000AA0AE8329BFD0E\n"
             "OK\n")
    # 模拟 C 解析: 找 "+CMGL:", 跳到 colon+1, sscanf " %d"
    import re
    indices = []
    for m in re.finditer(r'\+CMGL: (\d+)', reply):
        indices.append(int(m.group(1)))
    assert indices == [5, 6, 7], f"got {indices}"
    print(f"  scan_sms_storage simulation: extracted indices {indices} ✓")


def test_parse_cereg_reply():
    """模拟 wait_network_registered 解析"""
    def parse_cereg_stat(reply):
        r = reply.find("+CEREG:")
        if r < 0: return None
        comma = reply.find(',', r)
        if comma < 0: return None
        sp = comma + 1
        while sp < len(reply) and reply[sp] in ' \t':
            sp += 1
        return reply[sp] if sp < len(reply) else None
    # 不同 stat
    assert parse_cereg_stat("+CEREG: 0,1\nOK") == '1'    # home
    assert parse_cereg_stat("+CEREG: 0,5\nOK") == '5'    # roaming
    assert parse_cereg_stat("+CEREG: 0,2\nOK") == '2'    # searching (not registered)
    assert parse_cereg_stat("+CEREG: 0,0\nOK") == '0'    # not registered
    # 扩展格式 (n,stat,tac,ci,AcT)
    assert parse_cereg_stat("+CEREG: 0,1,1234,5678,7\nOK") == '1'
    print(f"  wait_network_registered simulation: 5 cases all matched ✓")


def main():
    print("=" * 60)
    print("PDU 解析器单元测试")
    print("=" * 60)
    print("\n[1] GSM 7-bit unpack")
    try:
        test_gsm7bit_basic()
        test_gsm7bit_thai()
    except AssertionError as e:
        print(f"  ✗ FAIL: {e}")
        return 1

    print("\n[2] UCS-2 BE decode")
    try:
        test_ucs2_thai()
        test_ucs2_chinese()
        test_ucs2_mixed()
    except AssertionError as e:
        print(f"  ✗ FAIL: {e}")
        return 1

    print("\n[3] TPDU parse_pdu_sms_deliver")
    try:
        test_parse_pdu_short_ucs2()
        test_parse_pdu_long_concat_8bit()
        test_parse_pdu_long_concat_16bit()
        test_parse_pdu_gsm7bit()
        test_parse_pdu_truncated()
        test_parse_pdu_udhl_overflow()
    except AssertionError as e:
        print(f"  ✗ FAIL: {e}")
        return 1

    print("\n[4] AT reply parsers (scan / CEREG)")
    try:
        test_parse_cmgl_reply()
        test_parse_cereg_reply()
    except AssertionError as e:
        print(f"  ✗ FAIL: {e}")
        return 1

    print("\n" + "=" * 60)
    print("✓ ALL TESTS PASSED")
    print("=" * 60)
    return 0


if __name__ == '__main__':
    sys.exit(main())
