// src/stk_validate.h
// v4.0.15: /api/stk/select 请求校验 (host test 直调, 0 mock)
// 静态内联: 多 TU 副本 OK (函数 6 行, 无 static state, 无副作用)
#pragma once
#include <cstdint>

struct StkSelectResult {
  bool        ok;
  const char* err;
  int         code;
};

static inline StkSelectResult validate_stk_select(int itemId, uint8_t cmd, uint8_t count) {
  if (count == 0)                       return {false, "no menu",       1};
  if (itemId < 1 || itemId > count)     return {false, "out of range",  1};
  if (cmd != 0x25)                      return {false, "not SETUP_MENU", 2};
  return {true, nullptr, 0};
}
