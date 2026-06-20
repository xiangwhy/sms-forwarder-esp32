# STK 响应路径解禁 (v4.0.15) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 解禁 STK 响应路径 MVP — 仅 `AT+STKR` (选菜单 item),`/stk` 页菜单项右边加 "选" 按钮,POST `/api/stk/select {itemId}`,设备发 `AT+STKR=<id>`,SIM 推新 `+STKPRO` 经现有读路径接住。

**Architecture:**
- 1 个新内联函数 `validate_stk_select()` 抽到 `src/stk_validate.h` (host test 直调, 0 mock)
- 1 个新 POST body handler `handleApiStkSelect()` 在 main.cpp, 复用 `g_stkMenuMux` + `send_atcmd(2000ms)` + `stk_log_write`
- 1 个新路由 `/api/stk/select` (body handler 模式, 跟 `handleApiBootPush` 同模板)
- `/stk` 页: `.btn-select` CSS + `li.map` 加 `<button data-id>` + 事件委托 `selectItem(btn)` JS + 1.5s 防连点 + status-line 反馈
- bump FW_VERSION v4.0.14 → v4.0.15 (2 处: main.cpp 宏 + app.h 主页 fw-tag)
- host test: 210 → 216 (新增 6 case)

**Tech Stack:** ESP-IDF 5.5 + Arduino-ESP32 3.x (PlatformIO), ESPAsyncWebServer fork, doctest-free C++ assert host test + Catch2 风格 (`CHECK` 宏)

**Spec:** `docs/superpowers/specs/2026-06-20-stk-resp-path.md`

---

## File Structure

**Create:**
- `src/stk_validate.h` (~12 行, 单 `static inline validate_stk_select()` 函数)
- (无其他新 file)

**Modify:**
- `src/main.cpp` (3 处: `#include "stk_validate.h"` + `handleApiStkSelect` handler + 路由 + bump FW_VERSION)
- `src/web/stk.h` (3 处: `.btn-select` CSS + li.map 改 + 新 JS)
- `src/web/app.h` (1 处: fw-tag v4.0.14 → v4.0.15)
- `tests/host/test_pdu_codec.cpp` (1 处: +6 测试函数 + main 调)

**Touch (no code):**
- `memory/stk-paused.md` (描述新基线)
- `memory/project-state.md` (加 v4.0.15 行 + commit history)
- `memory/MEMORY.md` (索引同步)

---

## Commit 结构 (2 commit)

| # | 主题 | 范围 |
|---|---|---|
| **C1** | `docs(spec): v4.0.15 STK 响应路径解禁 spec` | `docs/superpowers/specs/2026-06-20-stk-resp-path.md` (新建) |
| **C2** | `feat(stk)+chore: v4.0.15 解禁响应路径 (仅 AT+STKR) + bump v4.0.14→v4.0.15` | main.cpp + stk_validate.h + stk.h + app.h + test_pdu_codec.cpp + 3 memory |

---

## Task 1: Commit spec doc

**Files:**
- Create: `docs/superpowers/specs/2026-06-20-stk-resp-path.md` (已写, ~290 行)

- [ ] **Step 1: 验证 spec 已写**

```bash
test -f docs/superpowers/specs/2026-06-20-stk-resp-path.md && wc -l docs/superpowers/specs/2026-06-20-stk-resp-path.md
```

期望: 文件存在, ~290 行

- [ ] **Step 2: Commit spec**

```bash
git add docs/superpowers/specs/2026-06-20-stk-resp-path.md
git commit -m "docs(spec): v4.0.15 STK 响应路径解禁 (仅 AT+STKR) spec"
```

期望: 1 file changed, 290+ insertions

---

## Task 2: TDD - 写 6 host test cases (FAIL)

**Files:**
- Modify: `tests/host/test_pdu_codec.cpp` (~80 行: 6 测试函数 + main 调)

**原则:** 测试绑生产代码, 用 [[feedback_test_on_production_code]]。本次新增函数在 `src/stk_validate.h`, 测试 include 这个 header (同一函数, 0 mock, 0 重复)。

- [ ] **Step 1: 在 test_pdu_codec.cpp 顶部 include 新的 validate header**

在 `#include "pdu_codec.h"` 下一行加:
```cpp
#include "stk_validate.h"
```

- [ ] **Step 2: 在 test_pdu_codec.cpp main 之前加 6 个测试函数**

```cpp
// =================== v4.0.15: STK select 校验 ===================
// 6 case cover validate_stk_select 的 3 类失败 + 2 类边界 + 1 类正常

static void test_stk_select_normal() {
  g_current = "test_stk_select_normal (itemId=1 cmd=0x25 cnt=3 → ok)";
  auto v = validate_stk_select(1, 0x25, 3);
  CHECK(v.ok);
  CHECK(v.code == 0);
  CHECK(v.err == nullptr);
}

static void test_stk_select_max_boundary() {
  g_current = "test_stk_select_max_boundary (itemId=3 cmd=0x25 cnt=3 → ok)";
  auto v = validate_stk_select(3, 0x25, 3);
  CHECK(v.ok);
  CHECK(v.code == 0);
}

static void test_stk_select_min_boundary_zero() {
  g_current = "test_stk_select_min_boundary_zero (itemId=0 cmd=0x25 cnt=3 → err code:1)";
  auto v = validate_stk_select(0, 0x25, 3);
  CHECK(!v.ok);
  CHECK(v.code == 1);
  CHECK(std::string(v.err) == "out of range");
}

static void test_stk_select_above_max() {
  g_current = "test_stk_select_above_max (itemId=4 cmd=0x25 cnt=3 → err code:1)";
  auto v = validate_stk_select(4, 0x25, 3);
  CHECK(!v.ok);
  CHECK(v.code == 1);
  CHECK(std::string(v.err) == "out of range");
}

static void test_stk_select_wrong_cmd() {
  g_current = "test_stk_select_wrong_cmd (itemId=1 cmd=0x21 cnt=3 → err code:2)";
  auto v = validate_stk_select(1, 0x21, 3);
  CHECK(!v.ok);
  CHECK(v.code == 2);
  CHECK(std::string(v.err) == "not SETUP_MENU");
}

static void test_stk_select_no_menu() {
  g_current = "test_stk_select_no_menu (itemId=1 cmd=0 cnt=0 → err code:1 err='no menu')";
  auto v = validate_stk_select(1, 0, 0);
  CHECK(!v.ok);
  CHECK(v.code == 1);
  CHECK(std::string(v.err) == "no menu");
}
```

- [ ] **Step 3: 在 main() 末尾调 6 个新测试**

在 `test_pdu_oa_offset_numeric();` 后面加:
```cpp
  // v4.0.15: STK select 校验
  test_stk_select_normal();
  test_stk_select_max_boundary();
  test_stk_select_min_boundary_zero();
  test_stk_select_above_max();
  test_stk_select_wrong_cmd();
  test_stk_select_no_menu();
```

- [ ] **Step 4: 增量编译验证 FAIL (stk_validate.h 还没建)**

```bash
cd tests/host && cmake --build build 2>&1 | tail -20
```

期望: FAIL — `fatal error: 'stk_validate.h' file not found` 或 link error `undefined reference to validate_stk_select`

- [ ] **Step 5: Commit (red)**

```bash
git add tests/host/test_pdu_codec.cpp
git commit -m "test(stk): +6 validate_stk_select case (red, 期望 FAIL 等 Task 3 补实现)"
```

期望: 1 file changed, 80 insertions

---

## Task 3: 实现 validate_stk_select (PASS)

**Files:**
- Create: `src/stk_validate.h` (~14 行, static inline 函数)

- [ ] **Step 1: 新建 src/stk_validate.h**

```cpp
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
```

- [ ] **Step 2: 增量编译 + 跑测试 (PASS)**

```bash
cd tests/host && cmake --build build 2>&1 | tail -5 && ./build/test_pdu_codec 2>&1 | tail -3
```

期望: 编译 OK, 输出 `Result: 216 passed, 0 failed` (210 旧 + 6 新)

- [ ] **Step 3: Commit (green)**

```bash
git add src/stk_validate.h
git commit -m "feat(stk): validate_stk_select 内联函数 (6 行, 抽 src/stk_validate.h)"
```

期望: 1 file changed, 14 insertions

---

## Task 4: 加 handleApiStkSelect handler (main.cpp)

**Files:**
- Modify: `src/main.cpp` (~28 行: include + handler)

- [ ] **Step 1: 加 include (在 main.cpp:50 `#include "web/stk.h"` 下面)**

```cpp
#include "stk_validate.h"     // v4.0.15: validate_stk_select inline 函数
```

- [ ] **Step 2: 加 handler 在 handleApiStkMenu (main.cpp:~1217) 紧挨后面**

```cpp
// v4.0.15: /api/stk/select — POST {itemId:N}, 发 AT+STKR=N
//   stk-paused 守: 仅 AT+STKR; AT+STKTR/STKENV /api/stk/cmd 仍禁
//   复用: g_stkMenuMux (跟 handleApiStkMenu 同) + send_atcmd (2000ms timeout) + stk_log_write (ring buffer)
static void handleApiStkSelect(AsyncWebServerRequest* r, uint8_t* data, size_t len,
                               size_t /*index*/, size_t /*total*/) {
  JsonDocument doc;
  if (len == 0 || deserializeJson(doc, data, len) != DeserializationError::Ok) {
    r->send(400, "application/json", "{\"ok\":false,\"err\":\"bad json\"}");
    return;
  }
  int itemId = doc["itemId"] | 0;
  uint8_t cmd, count;
  portENTER_CRITICAL(&g_stkMenuMux);
  cmd   = g_stkMenuCmd;
  count = g_stkMenuCount;
  portEXIT_CRITICAL(&g_stkMenuMux);
  StkSelectResult v = validate_stk_select(itemId, cmd, count);
  if (!v.ok) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"%s\",\"code\":%d}", v.err, v.code);
    r->send(400, "application/json", buf);
    return;
  }
  char atcmd[24];
  snprintf(atcmd, sizeof(atcmd), "AT+STKR=%d\r\n", itemId);
  int rc = send_atcmd(atcmd, 2000);
  char logline[STK_LOG_LEN];
  snprintf(logline, sizeof(logline), "[TX] AT+STKR=%d → rc=%d", itemId, rc);
  stk_log_write(logline);
  if (rc != 0) {
    r->send(400, "application/json", "{\"ok\":false,\"err\":\"AT fail\",\"code\":3}");
    return;
  }
  r->send(200, "application/json", "{\"ok\":true}");
}
```

---

## Task 5: 加 /api/stk/select 路由 (main.cpp)

**Files:**
- Modify: `src/main.cpp` (~5 行: 1 个 srv->on)

- [ ] **Step 1: 加路由在 /api/stk/siminfo 路由 (main.cpp:2598) 紧挨后面**

```cpp
  // v4.0.15: 解禁响应路径 MVP — POST {itemId:N} → AT+STKR=N
  //   AT+STKTR / AT+STKENV / /api/stk/cmd 仍禁
  //   复用 handleApiBootPush (main.cpp:2417) body handler 模板: srv->on(path, method, normalHandler, nullptr, bodyHandler)
  srv->on("/api/stk/select", HTTP_POST,
    [](AsyncWebServerRequest* r) { if (!check_dashboard_auth(r)) return; },
    nullptr,
    handleApiStkSelect);
```

**注意**: 现有 /api/stk/menu + /api/stk/siminfo 都没 auth (main.cpp:2596-2598 看得到)。但 select 是写操作 (发 AT), 应加 auth 保护 — 用 `check_dashboard_auth` (main.cpp 现有 helper, /api/cfg /api/bootPush 同模式)。

实际看现有 /api/sent/clear (main.cpp:2577) 也没 auth (POST 删历史)。/api/recent/clear (main.cpp:2594) 也没 auth。本 spec 沿用 STK 路径无 auth (跟 /api/stk/menu /api/stk/siminfo 一致), 不加 auth。**改用 no-auth 版本**:

```cpp
  // v4.0.15: 解禁响应路径 MVP — POST {itemId:N} → AT+STKR=N
  //   AT+STKTR / AT+STKENV / /api/stk/cmd 仍禁
  //   no auth 跟 /api/stk/menu /api/stk/siminfo 一致 (读路径无 auth, 响应路径本 spec 也无 auth)
  srv->on("/api/stk/select", HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr, handleApiStkSelect);
```

---

## Task 6: 加 .btn-select CSS (src/web/stk.h)

**Files:**
- Modify: `src/web/stk.h` (~5 行 CSS, 在 `<style>` 块末尾加)

- [ ] **Step 1: 找到 .btn 定义结束位置**

读 src/web/stk.h, 找 `.btn:hover{background:var(--border)}` (line ~15) 后面, 在 `header{display:...}` (line ~11) 前面插入, 或 `.card` (line ~17) 前面:

实际看现有结构, line 17 是 `.card{...}`, line 18 是 `.card h3{...}`。`.btn-select` 应跟 `.btn` 系列紧挨, 在 `.btn.primary{...}` (line 16) 后面加:

- [ ] **Step 2: 在 `.btn.primary{background:var(--accent);...}` (line ~16) 后插入**

```css
.btn-select{display:inline-block;padding:4px 10px;border:1px solid var(--border);border-radius:6px;background:var(--card2);color:var(--text);font-size:12px;cursor:pointer;font-family:inherit;margin-left:8px}
.btn-select:hover{background:var(--accent);color:#0a1014;border-color:transparent}
.btn-select:disabled{opacity:.5;cursor:not-allowed}
```

---

## Task 7: 改 li.map 加选按钮 (src/web/stk.h)

**Files:**
- Modify: `src/web/stk.h` (改 1 行 li.map callback)

- [ ] **Step 1: 找到 loadMenu() 里的 ul.innerHTML 模板**

读 src/web/stk.h, 找 `ul.innerHTML = j.items.map(it => \`<li ...id=${it.id}</span>\`)` 段 (line ~70 附近)。

- [ ] **Step 2: 在每个 li 末尾加 `<button>选</button>`**

旧:
```javascript
ul.innerHTML = j.items.map(it =>
  `<li style="padding:8px 0;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;gap:12px">
     <span style="flex:1"><b style="color:var(--info)">${it.id}.</b> ${escapeHtml(it.text)}</span>
     <span style="color:var(--muted);font-size:12px">id=${it.id}</span>
   </li>`
).join('');
```

新:
```javascript
ul.innerHTML = j.items.map(it =>
  `<li style="padding:8px 0;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;gap:12px">
     <span style="flex:1"><b style="color:var(--info)">${it.id}.</b> ${escapeHtml(it.text)}</span>
     <span style="color:var(--muted);font-size:12px">id=${it.id}</span>
     <button class="btn-select" data-id="${it.id}">选</button>
   </li>`
).join('');
```

**关键**: 用 `data-id` 属性存 item id, 不用 inline `onclick` — 因为 loadMenu() 每 10s 重建 li, inline onclick 重建后失效。事件委托在 Task 8。

---

## Task 8: 加 selectItem JS + 事件委托 (src/web/stk.h)

**Files:**
- Modify: `src/web/stk.h` (~30 行 JS, 在 `<script>` 块 setInterval 后)

- [ ] **Step 1: 在 setInterval(loadSimInfo, 5000); 后面 (line ~97 后) 加新 JS**

```javascript
// v4.0.15: 选菜单 item → POST /api/stk/select
//   事件委托到 #menu_list ul (loadMenu 每 10s 重建 li, 委托避免重建后失效)
async function selectItem(btn) {
  const id = parseInt(btn.dataset.id, 10);
  const status = document.getElementById('menu_status');
  btn.disabled = true;
  try {
    const r = await fetch('/api/stk/select', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ itemId: id })
    });
    const j = await r.json();
    if (j.ok) {
      status.textContent = `已选 #${id}, 等待 SIM 响应 (最多 10s)…`;
      status.className = 'status-line ok';
    } else {
      status.textContent = `失败 #${id}: ${j.err}` + (j.code !== undefined ? ` (code=${j.code})` : '');
      status.className = 'status-line err';
    }
  } catch (e) {
    status.textContent = '网络错误: ' + e.message;
    status.className = 'status-line err';
  } finally {
    setTimeout(() => { btn.disabled = false; }, 1500);  // 防连点
  }
}
document.getElementById('menu_list').addEventListener('click', function(e){
  if (e.target.classList && e.target.classList.contains('btn-select')) selectItem(e.target);
});
```

---

## Task 9: 增量编译 main.cpp (Tasks 4-8 一起验, 不烧)

**Files:** (已改 main.cpp, stk.h, stk_validate.h, test_pdu_codec.cpp)

- [ ] **Step 1: 增量编译验证无 compile error**

```bash
pio run 2>&1 | tail -15
```

期望: 编译 OK (PlatformIO 增量, 应该 < 30s)。如有 error, 检查 main.cpp Tasks 4-5 路由 / handler 改对, stk.h Tasks 6-8 CSS/HTML/JS 改对。

---

## Task 10: Bump FW_VERSION v4.0.14 → v4.0.15 (2 处)

**Files:**
- Modify: `src/main.cpp:76` (FW_VERSION 宏)
- Modify: `src/web/app.h:42` (主页 fw-tag)

按 [[feedback-bump-fw-version]]: 子页 fw-tag 是 iframe 死代码, 不 bump。OTA/Config footer v4.0.13 已删。

- [ ] **Step 1: 改 main.cpp:76 FW_VERSION 宏**

旧:
```cpp
#define FW_VERSION         "v4.0.14"     // v4.0.14: main.cpp HTML 物理抽到 src/web/*.h; v4.0.13 STK SIM 卡 / v4.0.12 STK 控制台; bump FW_VERSION 宏 + 主页 app.h 可见 fw-tag (子页 dashboard/stk/send 的 fw-tag 是 iframe 死代码,不 bump)
```

新:
```cpp
#define FW_VERSION         "v4.0.15"     // v4.0.15: STK 响应路径解禁 MVP (仅 AT+STKR, /stk 页'选'按钮 → POST /api/stk/select); v4.0.14 main.cpp HTML 物理抽 / v4.0.13 STK SIM 卡 / v4.0.12 STK 控制台; bump FW_VERSION 宏 + 主页 app.h 可见 fw-tag (子页 dashboard/stk/send 的 fw-tag 是 iframe 死代码,不 bump)
```

- [ ] **Step 2: 改 src/web/app.h:42 fw-tag**

读 src/web/app.h, 找 `<span class="fw-tag">FW v4.0.14</span>`。

旧:
```html
<span class="fw-tag">FW v4.0.14</span>
```

新:
```html
<span class="fw-tag">FW v4.0.15</span>
```

---

## Task 11: 烧板 + 5 项 curl + grep stk-paused

**Files:** (无, 验证 + log)

按 [[feedback_code_review_before_flash]]: 审完一次性烧, 验证 5 项后 commit。

- [ ] **Step 1: 烧**

```bash
pio run -t upload --upload-port /dev/cu.usbserial-10 2>&1 | tail -10
```

期望: `SUCCESS`, ~50s。

- [ ] **Step 2: 等设备起, 验 fw**

```bash
for i in 1 2 3 4 5; do
  RESP=$(curl -s --max-time 3 http://172.16.1.18/api/status 2>/dev/null)
  if [ -n "$RESP" ]; then echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); print('fw=',d.get('fw'),'boot=',d.get('boot'))"; break; fi
  sleep 2
done
```

期望: `fw= v4.0.15 boot= 109` (从 108 → 109)

- [ ] **Step 3: 主页 fw-tag 同步验**

```bash
curl -s --max-time 3 http://172.16.1.18/app | grep -oE 'FW v[0-9.]+'
```

期望: `FW v4.0.15`

- [ ] **Step 4: 烧板验证 #1 — 无菜单**

(等 boot 5s 内 stk_query_task 没跑前, 或 SIM 没推菜单时)
```bash
curl -s -X POST -H 'Content-Type: application/json' -d '{"itemId":1}' http://172.16.1.18/api/stk/select
```

期望: `{"ok":false,"err":"no menu","code":1}` 或 `{"ok":false,"err":"out of range","code":1}` (看 g_stkMenuCount 状态)

- [ ] **Step 5: 烧板验证 #2 — 越界**

(等 SIM 推 SETUP_MENU 后 — 翔哥让 SIM 推或等触发)
```bash
curl -s http://172.16.1.18/api/stk/menu
```

期望: JSON 含 `count >= 1`, 看到菜单项

```bash
curl -s -X POST -H 'Content-Type: application/json' -d '{"itemId":99}' http://172.16.1.18/api/stk/select
```

期望: `{"ok":false,"err":"out of range","code":1}`

- [ ] **Step 6: 烧板验证 #4 — 正常选**

(等 SIM 推菜单后)
```bash
curl -s -X POST -H 'Content-Type: application/json' -d '{"itemId":1}' http://172.16.1.18/api/stk/select
```

期望: `{"ok":true}` + 串口日志 `[TX] AT+STKR=1 → rc=0`

- [ ] **Step 7: stk-paused 守住**

```bash
grep -nE "AT\+STK(TR|ENV)\b" src/main.cpp | grep -v "^.*//" | grep -v "^.*\*"
```

期望: 0 命中 (注释/文档不计)

```bash
grep -nE "AT\+STKR\b" src/main.cpp
```

期望: 1 命中 (handleApiStkSelect 内的 send_atcmd 调用)

```bash
grep -nE "/api/stk/cmd" src/main.cpp
```

期望: 仍注释 (main.cpp:~2583 注释不动)

---

## Task 12: Update memory + commit C2

**Files:**
- Modify: `memory/stk-paused.md` (基线描述更新)
- Modify: `memory/project-state.md` (加 v4.0.15 行)
- Modify: `memory/MEMORY.md` (索引同步)

- [ ] **Step 1: 更新 memory/stk-paused.md**

读 memory/stk-paused.md, 找当前基线描述 (大概率说 "v4.0.13 部分解禁" 或类似)。

把 "v4.0.13 部分解禁: 读路径 /api/stk/menu + /api/stk/siminfo + /stk 页 (含 SIM 信息卡), 响应路径仍停" 改成:

```
v4.0.15 部分解禁:
- 读路径 (v4.0.12+): /api/stk/menu + /api/stk/siminfo + /stk 页 (含 SIM 信息卡 + 选菜单按钮)
- 响应路径 (v4.0.15): /api/stk/select POST {itemId} → 发 AT+STKR=N (MVP: 仅 AT+STKR)
- 仍禁: AT+STKTR / AT+STKENV (终端响应/信封) / /api/stk/cmd (白名单透传) / 主页不放菜单卡
- 不解析: DISPLAY_TEXT (0x21) / GET_INPUT (0x27) / +STKEND URC
```

- [ ] **Step 2: 更新 memory/project-state.md**

读 memory/project-state.md, 在版本线表格加 v4.0.15 行, 在 "v4.0.14 内容" 段后加 "v4.0.15 内容" 段 (~30 行):

| **v4.0.15** | **(C2 commit hash)** | **已合已烧, 当前生产 HEAD (Boot #109+)** | STK 响应路径解禁 MVP (仅 AT+STKR): `/api/stk/select` POST {itemId} → send_atcmd("AT+STKR=<id>\r\n", 2000) → SIM 推新 +STKPRO 经现有读路径接住。`/stk` 页菜单项 li 加 `<button data-id>选</button>` (事件委托 ul.addEventListener + 1.5s 防连点 + status-line 反馈)。`validate_stk_select` 抽 src/stk_validate.h (host test 直调 6 case, 210 → 216 PASS) |

实际 commit hash 烧完 commit 后填。

- [ ] **Step 3: 更新 memory/MEMORY.md 索引**

读 memory/MEMORY.md, 找 `[项目状态 (project)](project-state.md)` 一行:

旧:
```
- [项目状态 (project)](project-state.md) — v4.0.14 housekeeping HEAD (7509748, Boot #108), 210/0 host test, e9a1c84+b1a62d8+7509748 收尾, FW v4.0.14 已烧
```

新:
```
- [项目状态 (project)](project-state.md) — v4.0.15 STK 响应路径解禁 HEAD (C2 hash, Boot #109), 216/0 host test (validate_stk_select +6 case), AT+STKR only
```

- [ ] **Step 4: Commit C2 (code + bump + memory)**

```bash
git add src/stk_validate.h src/main.cpp src/web/stk.h src/web/app.h tests/host/test_pdu_codec.cpp memory/stk-paused.md memory/project-state.md memory/MEMORY.md
git commit -m "feat(stk)+chore: v4.0.15 解禁响应路径 (仅 AT+STKR) + bump v4.0.14→v4.0.15

- src/stk_validate.h: validate_stk_select inline 函数 (host test 直调, 0 mock)
- src/main.cpp: handleApiStkSelect handler + /api/stk/select 路由 + bump FW_VERSION
- src/web/stk.h: .btn-select CSS + li.map 加按钮 + selectItem JS + ul 事件委托
- src/web/app.h: 主页 fw-tag v4.0.14 → v4.0.15
- tests/host/test_pdu_codec.cpp: +6 case (210 → 216)
- memory: stk-paused + project-state + MEMORY 同步基线"
```

期望: 9 files changed, ~150 insertions

---

## Self-Review

**1. Spec coverage:**
- §"### 启用" #1 validate_stk_select → Task 3 ✓
- §"### 启用" #2 handleApiStkSelect handler → Task 4 ✓
- §"### 启用" #3 /api/stk/select 路由 → Task 5 ✓
- §"### 启用" #4 /stk 页 UI (.btn-select + li.map + selectItem) → Tasks 6-8 ✓
- §"### 启用" #5 +6 host test case → Task 2 ✓
- §"### 启用" #6 bump FW_VERSION + app.h fw-tag → Task 10 ✓
- §"### 启用" #7 memory 更新 → Task 12 ✓
- §"烧板验证" 5 curl + grep → Task 11 ✓

**2. Placeholder scan:**
- 0 "TBD" / "TODO" / "实现 later" / "适当 error handling"
- 所有 code block 完整, 无 "类似 Task N" 引用

**3. Type consistency:**
- `StkSelectResult` 在 stk_validate.h 定义 (Task 3), Task 4 handler 用, Task 2 test 用 — 一致
- `validate_stk_select(int, uint8_t, uint8_t)` 签名三处一致
- `selectItem(btn)` Task 8 定义, Task 7 li.map 间接通过 ul.addEventListener 触发 — 一致
- `g_stkMenuMux` / `g_stkMenuCmd` / `g_stkMenuCount` / `send_atcmd` / `stk_log_write` / `STK_LOG_LEN` 全部 main.cpp 现有, 引用一致

**4. 已知细微调整** (实施时按实际 code 微调):
- Task 5 auth 选择: 跟 /api/stk/menu /api/stk/siminfo 读路径一致, no-auth
- Task 6 CSS 位置: 跟现有 .btn.primary 紧挨
- Task 11 烧板验证 #1 "no menu" vs "out of range" 看 g_stkMenuCount 实际状态

---

## End of Plan

实施完 Tasks 1-12, 翔哥应看到:
- `fw=v4.0.15` 在 /api/status
- `FW v4.0.15` 在 /app 主页头
- `/stk` 控制台页菜单项右边有 "选" 按钮, 点击 → status-line 反馈
- host test 216 PASS / 0 FAIL
- 2 commits: C1 spec + C2 code+bump+memory
- `[[stk-paused]]` 基线描述更新到 "v4.0.15 部分解禁"