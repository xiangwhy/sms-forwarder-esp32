# STK 响应路径解禁 (v4.0.15)

## Context

`v4.0.12 v2` (commit 3150936) 解禁了 STK **读** 路径 (只 `AT+STKPCMD=1` + 消费 `+STKPRO` URC + 解析 SETUP_MENU 0x25 → `g_stkMenu[]`), `/api/stk/menu` + `/stk` 控制台页启用。`v4.0.13` (4ff837c) 加 SIM 信息卡。

翔哥 2026-06-20 brainstorm 拍板 **STK 深化 — 解禁响应路径 (MVP: 仅 AT+STKR)**:
让用户在 `/stk` 控制台页能点菜单项 → 设备发 `AT+STKR=<id>` → SIM 推新 `+STKPRO` (新菜单 / DISPLAY_TEXT 等) → 现有读路径接住 → 页面更新。

**关键约束 (stk-paused 规则收紧部分, 见 [[stk-paused]])**:
- 只解禁 `AT+STKR` (菜单 item 选择)。`AT+STKTR` / `AT+STKENV` / `/api/stk/cmd` 仍停
- 不解禁 `/api/stk` (info 日志) / `/api/stk/refresh` (v4.0.12 v2 注释保留)
- 主页不放菜单卡 (v4.0.12 v2 翔哥拍板保留)
- 不解析 DISPLAY_TEXT (0x21) / GET_INPUT (0x27) 等其他 Proactive Command

**MVP 范围决定** (按 brainstorming 4 段确认):
- 范围 = 仅 AT+STKR (拒绝 AT+STKTR / STKENV, MVP 最小集)
- 位置 = `/stk` 控制台页 (主页不动, 沿用 v4.0.12 v2)
- UX 反馈 = POST 立即返 ok (不 wait SIM 响应, 依赖现有 10s /api/stk/menu 轮询)

## Scope

### 启用 (5 code 改动 + 2 housekeeping, 3 file)

| # | 类别 | 文件:行 | 内容 | 备注 |
|---|---|---|---|---|
| 1 | code | `src/main.cpp:~1062 前` | 新内联函数 `validate_stk_select(itemId, cmd, count) → {ok, err, code}` | host test 直接调, 返回结构跟 handler JSON 一致 |
| 2 | code | `src/main.cpp:1200 后` | 新函数 `handleApiStkSelect(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total)` | 紧挨 handleApiStkMenu, ~25 行, 调 `validate_stk_select` + `send_atcmd("AT+STKR=<id>\r\n", 2000)` + `stk_log_write` |
| 3 | code | `src/main.cpp:~2596 后` | 新路由 `srv->on("/api/stk/select", HTTP_POST, [](r){}, nullptr, handleApiStkSelect);` | 跟 /api/stk/menu + /api/stk/siminfo 同段紧贴, body handler 模式 (复用 handleApiBootPush 模板) |
| 4 | code | `src/web/stk.h:~70 li.map` + 新 CSS + 新 JS | `.btn-select` CSS (跟 `.btn` 同款 accent 配色); 每个 menu item `li` 内 `<button class="btn-select" data-id="${it.id}">选</button>` (改 li.map 1 行); 新 JS `selectItem(btn)` + 事件委托到 `#menu_list` ul + status-line 反馈 + 1.5s 防连点 | `loadMenu()` 已有的 map callback 改 1 行; ul.addEventListener 1 次, 重建 li 不丢 |
| 5 | code | `tests/host/test_pdu_codec.cpp` 或新 `test_stk_select.cpp` | +6 host test case (validate_stk_select 6 输入组合) | 210 + 6 = 216 PASS |
| 6 | housekeeping | `src/main.cpp:76` + `src/web/app.h:42` | bump `v4.0.14` → `v4.0.15` (按 [[feedback-bump-fw-version]] 2 处规则: FW_VERSION 宏 + 主页 fw-tag) | commit message 含 bump |
| 7 | housekeeping | `[[stk-paused]]` + `[[project-state]]` | memory 反映新基线 | 跟代码同 batch 写 |

### 不实现 (本 spec out of scope, 仍按 stk-paused)

- `AT+STKTR` / `AT+STKENV` 发送 (Terminal Response, Envelope)
- `DISPLAY_TEXT (0x21)` / `GET_INPUT (0x27)` 解析显示 + 响应 UI
- `+STKEND` URC 处理 (SIM 推送结束标志)
- `/api/stk/cmd` 解禁 (handleApiStkCmd 仍注释, 现有白名单 AT+STK*)
- 主页重新加菜单卡 (v4.0.12 v2 决策保留)
- `0x21` / `0x23` / `0x24` 多 Proactive Command 解析

## Architecture

```
┌────────────────────────┐
│ /stk 页 (浏览器)       │  (现有 v4.0.12 v2 + v4.0.13)
│ - menu_list li 渲染    │
│   (现有 map 改 1 行:   │
│    加 <button>选</button>) │
│ - selectItem(id) 新 JS │  ─── fetch POST ───┐
└────────────────────────┘                    │
                                              ▼
┌─────────────────────────────────────────────────────────────┐
│ AsyncWebServer                                              │
│ /api/stk/select HTTP_POST → handleApiStkSelect              │
│   ① 解析 JSON body {itemId}                                │
│   ② portENTER_CRITICAL(&g_stkMenuMux)                      │
│      读 g_stkMenuCmd + g_stkMenuCount                       │
│   ③ validate_stk_select(itemId, cmd, count) → {ok,err,code}  │
│   ④ portEXIT_CRITICAL                                       │
│   ⑤ if !ok → 返 {ok:false, err, code} (400)                │
│   ⑥ send_atcmd("AT+STKR=<id>\r\n", 2000)                   │
│      - 阻塞最多 2s, 等 OK/ERROR                             │
│      - g_atMutex 内部管理 (跟 cmgs 互斥)                    │
│   ⑦ stk_log_write("[TX] AT+STKR=N")                        │
│   ⑧ 返 {ok:true, itemId} 或 {ok:false, err:"AT fail",code:3}│
└─────────────────────┬───────────────────────────────────────┘
                      │ AT+STKR=N
                      ▼
┌────────────────────────────┐
│ SIM (Quectel ML307)         │
│ - 收 AT+STKR=N             │
│ - 推 +STKPRO: <hex>        │  ── new SETUP_MENU / DISPLAY_TEXT / ...
│   (或 +STKEND 罕)          │
└─────────────────────┬──────┘
                      │ +STKPRO
                      ▼
┌────────────────────────────┐
│ stk_event_task (现有 v4.0.12)│
│ - parse_stkpro_setup_menu  │  ── 写 g_stkMenuCount / g_stkMenuItems
│ - 0 改                      │
└─────────────────────┬──────┘
                      │ (10s 后)
                      ▼
┌────────────────────────────┐
│ /stk 页 setInterval(loadMenu, 10000) (现有) │
│ - 渲染新菜单项             │
│ - 新菜单项也有 "选" 按钮  │
└────────────────────────────┘
```

## Components

### 1. validate_stk_select (新内联函数, host test 直调)

```cpp
// v4.0.15: 校验 /api/stk/select 请求, host test 直调 (不需 mock send_atcmd)
struct StkSelectResult { bool ok; const char* err; int code; };
static inline StkSelectResult validate_stk_select(int itemId, uint8_t cmd, uint8_t count) {
  if (count == 0 || itemId < 1 || itemId > count) {
    return {false, count == 0 ? "no menu" : "out of range", 1};
  }
  if (cmd != 0x25) {  // SETUP_MENU only; 拒绝 0x21 DISPLAY_TEXT 等
    return {false, "not SETUP_MENU", 2};
  }
  return {true, nullptr, 0};
}
```

**关键决策**:
- 返回 `struct` 不是 `bool` — 同时带 err string + code,handler wrap JSON 直接用
- 内联函数 (header 段或 main.cpp 顶部),host test 编译进 .cpp 同 TU
- `code` 复用 handler JSON `code` 字段,前端可编程分支 (1=itemId 问题 / 2=cmd 类型 / 3=AT 失败)

### 2. handleApiStkSelect (新 handler, ~25 行)

```cpp
// v4.0.15: /api/stk/select — POST {itemId:N}, 发 AT+STKR=N (v4.0.15 解禁响应路径 MVP)
//   stk-paused 守: 仅 AT+STKR; 不解禁 AT+STKTR/STKENV /api/stk/cmd
static void handleApiStkSelect(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t /*idx*/, size_t /*total*/) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) { r->send(400, "application/json", "{\"ok\":false,\"err\":\"bad json\"}"); return; }
  int itemId = doc["itemId"] | 0;
  uint8_t cmd, count;
  portENTER_CRITICAL(&g_stkMenuMux);
  cmd = g_stkMenuCmd;
  count = g_stkMenuCount;
  portEXIT_CRITICAL(&g_stkMenuMux);
  StkSelectResult v = validate_stk_select(itemId, cmd, count);
  if (!v.ok) {
    char buf[96]; snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"%s\",\"code\":%d}", v.err, v.code);
    r->send(400, "application/json", buf);
    return;
  }
  char atcmd[24]; snprintf(atcmd, sizeof(atcmd), "AT+STKR=%d\r\n", itemId);
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

**关键决策**:
- 用 POST body handler (`r, data, len, idx, total` 4 参) 不是 GET handler — 因为要 body
- 临界区只包 `g_stkMenuCmd` + `g_stkMenuCount` 2 字节读, < 1ms 无长阻塞
- `send_atcmd` 复用现有 (2000ms timeout, 内部 g_atMutex 管理)
- `stk_log_write` 复用现有 ring buffer (不破 stk-paused — log 仅内部可见,不暴露 `/api/stk`)
- 返 `{"ok":true}` 简版 (itemId 不返, 前端知道自己发的啥)

### 3. /api/stk/select 路由 (新增)

```cpp
// v4.0.15: 解禁响应路径 MVP — 仅 AT+STKR (选菜单 item)
//   AT+STKTR / AT+STKENV / /api/stk/cmd 仍禁
srv->on("/api/stk/select", HTTP_POST, [](AsyncWebServerRequest* r){}, NULL, handleApiStkSelect);
```

**关键**: POST 需要 body handler, ESPAsyncWebServer 模式是 `srv->on(path, method, normalHandler, uploadHandler, bodyHandler)`, 第二个参数 `NULL` 表示无 upload, 第三个是 body handler。

放在 `/api/stk/menu` + `/api/stk/siminfo` (main.cpp:~2596) **同段紧贴后面**, 走 STK 读路径 + 响应路径 分组。

### 4. /stk 页 UI 改动 (新 CSS + 改 li.map + 新 JS)

```css
/* v4.0.15: 菜单项 "选" 按钮 */
.btn-select{display:inline-block;padding:4px 10px;border:1px solid var(--border);border-radius:6px;background:var(--card2);color:var(--text);font-size:12px;cursor:pointer;font-family:inherit;margin-left:8px}
.btn-select:hover{background:var(--accent);color:#0a1014;border-color:transparent}
.btn-select:disabled{opacity:.5;cursor:not-allowed}
```

```html
<!-- menu_list ul.map 改 1 行 -->
<ul id="menu_list" ...>
  ${j.items.map(it =>
    `<li style="...existing...">
       <span style="flex:1"><b style="color:var(--info)">${it.id}.</b> ${escapeHtml(it.text)}</span>
       <span style="color:var(--muted);font-size:12px">id=${it.id}</span>
       <button class="btn-select" data-id="${it.id}">选</button>  <!-- v4.0.15 新加 -->
     </li>`
  ).join('')}
</ul>
```

```javascript
// v4.0.15: 选菜单 item → POST /api/stk/select
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
// menu_list ul 用事件委托 (li 重建后按钮失效)
document.getElementById('menu_list').addEventListener('click', function(e){
  if (e.target.classList.contains('btn-select')) selectItem(e.target);
});
```

**关键决策**:
- 事件**委托**到 `#menu_list` ul — 现有 `loadMenu()` 每 10s 重建 li, 直接 onClick 会丢
- `data-id` 属性存 item id,避免闭包变量捕获老菜单
- `setTimeout 1500ms` 后 enable 按钮 — 防用户连点 (SIM 可能还没推新菜单)
- 成功用 `status-line ok` (绿色), 失败用 `status-line err` (红色), 跟现有 /stk 页 `loadMenu` status 提示一致

### 5. host test (新 6 case)

```cpp
// tests/host/test_stk_select.cpp (新文件, 或合并 test_pdu_codec.cpp)
// 抽 validate_stk_select 出来 host test 直调, 不需要 mock send_atcmd
TEST_CASE("validate_stk_select") {
  SUBCASE("itemId=1 cmd=0x25 cnt=3 → ok")         { auto v = validate_stk_select(1, 0x25, 3); CHECK(v.ok); CHECK(v.code == 0); }
  SUBCASE("itemId=3 cmd=0x25 cnt=3 → ok (边界)")  { auto v = validate_stk_select(3, 0x25, 3); CHECK(v.ok); }
  SUBCASE("itemId=0 cmd=0x25 cnt=3 → err code:1") { auto v = validate_stk_select(0, 0x25, 3); CHECK(!v.ok); CHECK(v.code == 1); }
  SUBCASE("itemId=4 cmd=0x25 cnt=3 → err code:1") { auto v = validate_stk_select(4, 0x25, 3); CHECK(!v.ok); CHECK(v.code == 1); }
  SUBCASE("itemId=1 cmd=0x21 cnt=3 → err code:2") { auto v = validate_stk_select(1, 0x21, 3); CHECK(!v.ok); CHECK(v.code == 2); }
  SUBCASE("itemId=1 cmd=0 cnt=0 → err code:1")    { auto v = validate_stk_select(1, 0,    0); CHECK(!v.ok); CHECK(v.code == 1); }
}
```

**关键决策**:
- `validate_stk_select` 抽 header inline 函数, host test 编译进 .cpp 同 TU → 0 mock
- host test 数: 210 → 216 PASS (本批 +6 case)
- 字段 `count == 0` 优先报 "no menu" 而不是 "out of range" (用户友好)

## Data flow

1. **用户** 在 `/stk` 控制台页看 SIM 卡主动菜单卡 (现有 `loadMenu()` 10s 轮)
2. **用户** 点某菜单项右边 "选" 按钮 → 触发 `selectItem(btn)`
3. **JS** 调 `fetch('/api/stk/select', POST {itemId:N})` → button disable 1.5s
4. **handler** `handleApiStkSelect`:
   - 解析 JSON body `{itemId: N}`
   - 临界区读 `g_stkMenuCmd` (0x25) + `g_stkMenuCount` (≥1)
   - `validate_stk_select` 校验
   - 若 !ok → 返 400 + `{ok:false, err, code}` → JS toast 显示
   - 若 ok → `send_atcmd("AT+STKR=<id>\r\n", 2000)` (mutex 内部管理)
   - `stk_log_write("[TX] AT+STKR=N → rc=…")` (内部 ring buffer, 不破 stk-paused)
   - 返 200 + `{ok:true}` → JS status-line ok 显示 "已选 #N, 等待 SIM 响应…"
5. **SIM** 收 AT+STKR=N → 推新 `+STKPRO: <hex>` (新 SETUP_MENU / DISPLAY_TEXT 等)
6. **stk_event_task** (现有) 消费 `+STKPRO` → `parse_stkpro_setup_menu` 更新 `g_stkMenu*`
7. **/stk 页** `setInterval(loadMenu, 10000)` 拿到新菜单 → 重新渲染 li → 新菜单项也有 "选" 按钮 (委托还在)

## Error handling

| 场景 | 行为 | 用户看到 |
|---|---|---|
| itemId 越界 (itemId < 1 或 > count) | 400 + `{ok:false,err:"out of range",code:1}` | status-line 红色 "失败 #N: out of range (code=1)" |
| count == 0 (SIM 未推菜单) | 400 + `{ok:false,err:"no menu",code:1}` | status-line 红色 "失败 #N: no menu" (按钮本就没显示 — 防御) |
| cmd ≠ 0x25 (DISPLAY_TEXT 0x21 等) | 400 + `{ok:false,err:"not SETUP_MENU",code:2}` | status-line 红色 "失败 #N: not SETUP_MENU" (实测不会发生 — 当前菜单始终 0x25) |
| `send_atcmd` 返非 0 (mutex timeout / SIM 失联 / 4G 模组异常) | 400 + `{ok:false,err:"AT fail",code:3}` | status-line 红色 "失败 #N: AT fail (code=3)" |
| fetch 网络错误 (前端) | catch e | status-line 红色 "网络错误: …" |
| 用户连点 选 按钮 | 前端 `btn.disabled = true` 1.5s | 按钮灰, 不响应 |
| SIM 推 +STKEND (无后续,罕) | **不处理** (out of scope) | UI 保留旧菜单 (用户点其他项不响应, 但不破) |
| SIM 推 DISPLAY_TEXT (0x21) 替代 SETUP_MENU | `parse_stkpro_setup_menu` 返 0 → `g_stkMenuCount` 不更新 → 10s 后 /api/stk/menu 显示 `cmd:0x21, count:0` | "等待 SIM 推送…" 卡 + 按钮仍显示旧菜单 (用户可点, 但 validate code:2 拒绝, 红色提示) |
| SIM 卡拔 / 4G 模组断电 | 后续 +STKPRO 永不推, `send_atcmd` timeout | 用户看到 status-line "AT fail (code=3)" 红色 |

**stk-paused 守住**:
- handler 内 0 `AT+STKTR` / `AT+STKENV` 发送 (仅 `AT+STKR=N`)
- `/api/stk/cmd` 仍注释 (main.cpp:~2583 不动)
- `/api/stk` / `/api/stk/refresh` 仍注释
- `stk_log_write` 复用现有 ring buffer (不暴露 `/api/stk`, 仅 ESP_LOG 跟 `g_stkLog` 内部可见)

## Testing

### 烧板前 (按 [[feedback_code_review_before_flash]])

1. **静态审 diff**:
   - `git diff main...HEAD` = 3 file (src/main.cpp, src/web/stk.h, tests/host/test_pdu_codec.cpp 或新 test_stk_select.cpp)
   - 改动 1-3 是新增 (handler + 路由 + validate), 改动 4 是 /stk 页 1 行 map callback 改 + 新 CSS/JS, 改动 5 是 +6 host test case, 改动 6-7 是 bump + memory
   - 验证新路由位置在 `/api/stk/menu` + `/api/stk/siminfo` 路由同段 (不破坏现有路由顺序)
   - 验证 `handleApiStkSelect` 复用 `g_stkMenuMux` (跟 handleApiStkMenu 一致)
   - 验证 `validate_stk_select` 内联函数可被 host test 编译进 .cpp 同 TU
2. **host test**: 216 PASS / 0 FAIL (210 不变 +6 新增)
3. **memory 检查**: `g_stkMenuMux` 临界区只包 2 字节读 (cmd + count), < 1ms 无长阻塞, 跟 cmgs 互斥不影响
4. **flash 容量检查**: handler ~25 行 + 路由 1 行 + validate ~10 行 + .btn-select CSS ~5 行 + li 改 1 行 + JS ~25 行 + test +30 行 ≈ 100 行 ≈ 4KB flash, 够 (当前 37% / 3.3MB)
5. **memory 风险**: 0 新 BSS / 0 新全局变量 / 0 新 task

### 烧板后 (闭环验证)

1. **boot log**: 应见 `STK event task started` (v4.0.12 已有), 无新 error
2. **curl 5 项**:
   - `#1 无菜单`: `curl -X POST -d '{"itemId":1}' /api/stk/select` (SIM 未推菜单时) → 400 + `{ok:false,err:"no menu",code:1}`
   - `#2 越界`: SIM 已推菜单 (count=3), `curl -X POST -d '{"itemId":4}' /api/stk/select` → 400 + `{ok:false,err:"out of range",code:1}`
   - `#3 cmd≠0x25`: (构造难, 可跳过 — 实际不会触发)
   - `#4 正常选`: SIM 推菜单后, `curl -X POST -d '{"itemId":1}' /api/stk/select` → 200 + `{ok:true}` + serial log `[TX] AT+STKR=1 → rc=0`
   - `#5 AT 失败`: (拔 SIM 卡难, 可跳过 — 设计层面已覆盖)
3. **浏览器 `/stk` 页**:
   - 菜单项 li 右边有 "选" 按钮 (蓝色, hover 变绿)
   - 点 #1 → 按钮灰 1.5s → status-line 绿 "已选 #1, 等待 SIM 响应 (最多 10s)…"
   - 10s 内新菜单出来 (如果 SIM 推了 SETUP_MENU)
   - 如果 SIM 推 DISPLAY_TEXT, `menu_status` 显 "cmd=0x21 (非 SETUP_MENU)", 按钮仍显示旧 (点了会 code:2 红)
4. **stk-paused 守住**:
   - `grep -E "AT\+STK(TR|ENV)" src/main.cpp` → 0 活跃命中 (注释/文档不计)
   - `grep -E "AT\+STKR" src/main.cpp` → +1 活跃命中 (handleApiStkSelect, 在 send_atcmd 调用)
   - `grep -E "api/stk/cmd" src/main.cpp` → 仍注释
5. **fw bump 验证**: `curl /api/status | jq .fw` → `"v4.0.15"` ✓

## Diff estimation

```
3 files code changed, ~100 insertions(+), ~5 deletions(-)
- src/main.cpp: +1 validate inline (~10 行) + 1 handler (~25 行) + 1 路由 (~3 行) + bump FW_VERSION (1 行) + 注释 (~5 行)
- src/web/stk.h: +CSS (~5 行) + li.map 改 1 行 + 新 JS (~25 行) + 1 ul.addEventListener (~3 行)
- tests/host/test_pdu_codec.cpp 或新 test_stk_select.cpp: +6 case (~30 行)

housekeeping (本 batch):
- src/web/app.h: 主页 fw-tag v4.0.14 → v4.0.15 (1 行)
- docs/superpowers/specs/2026-06-20-stk-resp-path.md: 新 spec (本文件, ~290 行, spec commit 单独)
- memory: [[stk-paused]] + [[project-state]] 更新
```

实际 diff 会在 plan 阶段精确化。

## Risks

| Risk | 概率 | 影响 | 缓解 |
|---|---|---|---|
| SIM 不响应 AT+STKR (锁死) | 低 | 用户点完无反馈 | status-line 红 "AT fail (code=3)", 1.5s 防连点 |
| SIM 推 DISPLAY_TEXT 替代新菜单 | 中 | UI 看起来 "无变化" (实际 SIM 在显示文字, 用户看不到) | status-line 显 "cmd=0x21 (非 SETUP_MENU)", 按 working-code-no-touch 暂不解析 DISPLAY_TEXT |
| SIM 推 +STKEND | 罕 | 旧菜单保留, 用户点其他 项无响应 | 不处理 (out of scope), v4.0.16+ 加 |
| `parse_cnum` 2-quote 漏 (v4.0.11.21 review finding #2) 影响 SIM 信息卡 | 中 | MSISDN 显 "-" | UI 容忍, working-code-no-touch 暂不修, 真机复现再动 |
| 跟 cmgs 抢 g_atMutex 20000ms wait | 极低 | AT+STKR 等 20s | 2000ms timeout, 用户看到 status-line 红 |
| stk-paused 部分解禁被误用为全解禁 | 极低 | 翔哥/未来的我误以为可以加 STKTR | memory `[[stk-paused]]` 明确 "MVP 仅 AT+STKR, STKTR/STKENV/cmd 仍禁", 跟代码同 batch 写 |

## Out of scope (本 spec 不做, 可能下个 spec)

- `AT+STKTR` 终端响应 (DISPLAY_TEXT 用户确认 / GET_INPUT 用户输入)
- `AT+STKENV` 主动命令 (Send SMS / Send DTMF / Send USSD / etc)
- `DISPLAY_TEXT (0x21)` / `GET_INPUT (0x27)` / `SELECT_ITEM (0x24)` / `MORE_TIME (0x23)` 等 Proactive Command 解析显示
- `+STKEND` URC 处理
- `/api/stk/cmd` 解禁
- 主页重新加菜单卡 (v4.0.12 v2 决策保留)
- 多用户 / auth (当前 no auth, 跟 /api/stk/menu 一致)

## How to apply (后续 session)

- 实施本 spec 后, [[stk-paused]] memory 更新: "v4.0.15 部分解禁: 响应路径解禁仅 AT+STKR, /stk 页'选'按钮 → POST /api/stk/select。AT+STKTR/AT+STKENV 仍停 + /api/stk/cmd 仍禁"
- 改 STK_PAGE_HTML 时, 复用 `.card` / `.kv` / `.btn` CSS, `.btn-select` 新加在头部 style 块
- 改 handler 时, 临界区只包 `g_stkMenuMux` 读 `cmd` + `count` 2 字节 (跟现有 handleApiStkMenu 一致)
- 新加 STK 命令 (v4.0.16+ 想解禁 AT+STKTR): 新开 spec, 不在本 spec 范围, 复用 `validate_stk_select` 模式加 `validate_stk_tr`
- 主页想重加菜单卡: 新开 spec, 跟 v4.0.12 v2 决策冲突, 翔哥拍板

## 相关 memory

- [[stk-paused]] — v4.0.12 v2 读路径解禁 / v4.0.13 SIM 信息卡; 本 spec 扩展响应路径 MVP
- [[project-state]] — v4.0.14 housekeeping HEAD (7509748); 本 spec 实施后升 v4.0.15
- [[code-review-v4.0.11.21]] — finding #2 (parse_cnum 2-quote 漏) 影响 /stk SIM 信息卡 MSISDN 显示, 暂不修
- [[feedback_code_review_before_flash]] — 烧前必审, 烧完稳定打 tag
- [[feedback-working-code-no-touch]] — 功能稳不动, SIM 推 DISPLAY_TEXT 不解析, 暂不修
- [[feedback_test_on_production_code]] — host test 沿用, 本 spec +6 case 绑 validate_stk_select 内联函数
- [[feedback-bump-fw-version]] — bump 2 处 (FW_VERSION 宏 + app.h 主页 fw-tag), 子页 fw-tag 是 iframe 死代码
- [[feedback_incremental_build]] — host test 增量编译, 别 rm -rf build
- [[stk-github-research]] — pySim/cat.py 未来扩展 Proactive Command 解析用

## Implementation

实施本 spec → 调 `superpowers:writing-plans` skill 出 plan → 按 plan 改 → 烧板验证 → commit `v4.0.15` (2 commits: spec + code+bump) → update memory `[[stk-paused]]` + `[[project-state]]` → 验证 `fw=v4.0.15` + 5 项 curl + stk-paused grep 守住。