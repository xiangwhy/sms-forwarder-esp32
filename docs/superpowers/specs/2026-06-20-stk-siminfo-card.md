# /stk 页加 SIM 信息卡 (v4.0.13)

## Context

`v4.0.12 v2` (commit 3150936) 解禁了 STK **读** 路径,`/stk` 控制台页启用并精简为只放 "SIM 卡主动菜单 (SETUP_MENU 0x25)" 一张卡。

翔哥 2026-06-20 拍板 **STK 深化 — 加 SIM 信息卡**: 把 `stk_query_task` 已经在跑的 CIMI/CCID/CNUM/COPS 数据直接展示在 `/stk` 页, 不必查串口日志或 NVS 凭据。

**关键约束 (stk-paused 规则不变, 仍按 [[stk-paused]])**:
- 不发 `AT+STKR` / `AT+STKTR` (响应路径仍暂停)
- 不解禁 `/api/stk` (info 日志) / `/api/stk/refresh` / `/api/stk/cmd` (破 stk-paused)
- 不发任何 STK Proactive Response
- 不动 `stk_event_task` / `parse_stkpro_setup_menu` / `g_stkMenu[]` 数据流
- 不动 DASHBOARD_HTML / 主页 / 其他页 (按 working-code-no-touch, 功能扩展只在新区域)

**已知数据源 (现成, 不需采集)**:
- `g_sim_imsi[16]` (15 位 + '\0', main.cpp:211)
- `g_sim_iccid[21]` (19-20 位 + '\0', main.cpp:212)
- `g_sim_msisdn[16]` (本机号, main.cpp:213)
- `g_sim_operator[32]` (运营商名, main.cpp:214)
- `g_sim_queryMs` (上次 SIM 查询时间, main.cpp:215)
- `g_sim_mux` (critical section, main.cpp:216)

**已知写入路径**:
- `stk_query_task` (main.cpp:~1092) boot 5s 首次 + 30min 周期
- `parse_cimi` (main.cpp:968) 写 g_sim_imsi
- `parse_ccid` (main.cpp:986) 写 g_sim_iccid
- `parse_cnum` (main.cpp:1005) 写 g_sim_msisdn (v4.0.11.21 review finding #2 已知 2-quote 漏, 但 SIM 在线时 CNUM 一般 3-quote, 数据多数时间可用)
- `parse_cops` (main.cpp:1034) 写 g_sim_operator

## Scope

### 启用 (5 改动, 1 file)

| # | 文件:行 | 内容 | 备注 |
|---|---|---|---|
| 1 | `src/main.cpp:1140 后` | 新函数 `handleApiStkSiminfo(AsyncWebServerRequest* r)` | portENTER_CRITICAL 读 g_sim_*, JSON 5 字段, ~25 行 |
| 2 | `src/main.cpp:~3760 后` | 路由解注释 (新增) `srv->on("/api/stk/siminfo", HTTP_GET, handleApiStkSiminfo);` | 跟 /api/stk/menu 同段, 紧贴 |
| 3 | `src/main.cpp:~2796 后` | STK_PAGE_HTML `.container` 内加 `.card "SIM 卡信息"` (4 个 .kv + 1 个刷新时间 .kv) | 复制 DASHBOARD_HTML 2162-2180 SIM 信息 .kv 模式 |
| 4 | `src/main.cpp:~2840 后` | STK_PAGE_HTML JS 加 `async function loadSimInfo()` + `setInterval(loadSimInfo, 5000);` + 首屏调 | 复制 DASHBOARD_HTML loadInfo 模式 (5s 轮, 跟 loadMenu 一致) |
| 5 | 注释更新 | 同步 `stk-paused.md` / `project-state.md` / `stk-paused` memory 反映新基线 | 在 commit 后更新 |

### 不实现 (本 spec out of scope, 仍按 stk-paused)

- `/api/stk` (handleApiStkInfo 含 g_stkLog) — 仍禁
- `/api/stk/refresh` (handleApiStkRefresh 触发 stk_query_task 重查) — 仍禁
- `/api/stk/cmd` (handleApiStkCmd 透传 AT) — 仍禁
- 任何 `AT+STKR` / `AT+STKTR` 发送代码
- 任何 STK Proactive Response
- 主页 / 主页 SPA / Send / Config / Update 页 (按 working-code-no-touch, 不动)

## Architecture

```
┌─────────────────────┐
│ stk_query_task      │  (boot 5s + 30min 周期, 已有)
│ - AT+CIMI           │
│ - AT+CCID           │
│ - AT+CNUM           │
│ - AT+COPS           │
│ - AT+CSCA           │
└──────────┬──────────┘
           │ portENTER_CRITICAL(&g_sim_mux)
           ▼
┌─────────────────────┐
│ g_sim_imsi/iccid/   │  (BSS, main.cpp:211-215, 已有)
│ msisdn/operator     │
│ + g_sim_queryMs     │
└──────────┬──────────┘
           │ portENTER_CRITICAL(&g_sim_mux)
           ▼
┌─────────────────────┐
│ handleApiStkSiminfo │  (新, ~25 行, JSON 输出)
│ HTTP GET            │
│ 200 + application/  │
│ json                │
└──────────┬──────────┘
           │ JSON {imsi, iccid, msisdn, operator, ageMs}
           ▼
┌─────────────────────┐
│ /stk 页             │
│ loadSimInfo()       │  (新 JS, 5s 轮, 跟 loadMenu 一样)
│ ↓                   │
│ 5 个 .kv DOM 更新   │
└─────────────────────┘
```

## Components

### 1. handleApiStkSiminfo (新 handler)

```cpp
// v4.0.13: /api/stk/siminfo — 读 g_sim_* (stk_query_task 已采集), 仅 SIM 信息无 g_stkLog
static void handleApiStkSiminfo(AsyncWebServerRequest* r) {
  JsonDocument doc;
  portENTER_CRITICAL(&g_sim_mux);
  doc["imsi"]     = g_sim_imsi;
  doc["iccid"]    = g_sim_iccid;
  doc["msisdn"]   = g_sim_msisdn;
  doc["operator"] = g_sim_operator;
  portEXIT_CRITICAL(&g_sim_mux);
  doc["ageMs"] = (int32_t)((g_sim_queryMs && millis() >= g_sim_queryMs) ? (millis() - g_sim_queryMs) : -1);
  String out; serializeJson(doc, out);
  r->send(200, "application/json", out);
}
```

**关键决策**:
- 5 字段**不带** `g_stkLog` (避免解禁 handleApiStkInfo 的 logs 数组, 严格遵守 stk-paused)
- 不用 auth (`/api/stk/menu` 也不需, 沿用 stk-paused 注释的"只读无 auth"原则)
- `ageMs = -1` 当 `g_sim_queryMs == 0` (boot 后未查询过) — 跟 handleApiStkInfo (1149) 一致

### 2. /api/stk/siminfo 路由 (新增)

```cpp
// v4.0.13: SIM 信息 (从 stk_query_task 已有 g_sim_* 读, 不含 g_stkLog, 不破 stk-paused)
srv->on("/api/stk/siminfo", HTTP_GET, handleApiStkSiminfo);
```

放在 `/api/stk/menu` 路由 (main.cpp:3752) **同段, 紧贴后面**, 走读路径分组。

### 3. STK_PAGE_HTML "SIM 卡信息" 卡 (新 HTML)

插在 STK_PAGE_HTML 现有 "SIM 卡主动菜单" 卡 (main.cpp:2858-2862) **之前** (stk 信息放上面, 主动菜单放下面 — 翔哥习惯从网络状态看起)。

```html
<div class="card">
  <h3>SIM 卡信息</h3>
  <div class="kv"><span class="k">运营商</span><span class="v" id="simOp">-</span></div>
  <div class="kv"><span class="k">IMSI</span><span class="v" id="simImsi">-</span></div>
  <div class="kv"><span class="k">ICCID</span><span class="v" id="simIccid">-</span></div>
  <div class="kv"><span class="k">本机号 (MSISDN)</span><span class="v" id="simMsisdn">-</span></div>
  <div class="kv"><span class="k">刷新时间</span><span class="v" id="simAge">-</span></div>
</div>
```

**复用**:
- `.card` / `.kv` / `.k` / `.v` CSS 已在 STK_PAGE_HTML 头部 style (v4.0.12 v2 精简版继承自老版)
- 跟 DASHBOARD_HTML:2162-2180 SIM 信息 .kv 模式完全一致 (翔哥看过老版, 复制最稳)

### 4. STK_PAGE_HTML JS loadSimInfo (新 JS)

```javascript
function ageStr(sec){ if(sec<0) return '从未'; if(sec<60) return sec+'s 前'; if(sec<3600) return Math.floor(sec/60)+'m 前'; return Math.floor(sec/3600)+'h 前'; }
async function loadSimInfo(){
  try{
    const r = await fetch('/api/stk/siminfo',{cache:'no-store'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    const j = await r.json();
    document.getElementById('simOp').textContent     = j.operator || '-';
    document.getElementById('simImsi').textContent   = j.imsi     || '-';
    document.getElementById('simIccid').textContent  = j.iccid    || '-';
    document.getElementById('simMsisdn').textContent = j.msisdn   || '-';
    document.getElementById('simAge').textContent    = ageStr(Math.floor((j.ageMs<0?-1:j.ageMs)/1000));
  }catch(e){
    /* fetch 失败, 保留旧值, 不更新 DOM */
  }
}
```

**关键决策**:
- fetch 失败时 **静默保留旧值** (不像 loadMenu 显 "加载失败") — SIM 信息不会突变, 静默更符合监控面板风格
- 5s 轮 (跟 loadMenu 一致, 同步刷新)
- `ageStr` 复用 STK_PAGE_HTML 老版 (v4.0.7 写) — 不重复实现, 直接复制

### 5. 路由 + JS 启用

JS 段尾加首屏调 + 5s 轮:

```javascript
loadMenu();
loadSimInfo();        // v4.0.13: 首屏调
setInterval(loadMenu, 10000);
setInterval(loadSimInfo, 5000);   // v4.0.13: 5s 跟 loadMenu 同步刷
```

## Data flow

1. `stk_query_task` (main.cpp:~1092, **已有**)
   - boot 5s 首次: 依次发 `AT+CIMI` / `AT+CCID` / `AT+CNUM` / `AT+COPS?` / `AT+CSCA?`
   - 30min 周期: 同上
   - 解析 URC 回包 → 调 `parse_cimi` / `parse_ccid` / `parse_cnum` / `parse_cops` → 写 `g_sim_*` + 更新 `g_sim_queryMs`

2. `handleApiStkSiminfo` (HTTP GET, 新):
   - `portENTER_CRITICAL(&g_sim_mux)` 读 4 字符串 + `g_sim_queryMs`
   - 转 `ageMs = millis() - g_sim_queryMs` (或 -1 当未查询)
   - 序列化为 JSON `{imsi, iccid, msisdn, operator, ageMs}` → 200

3. `/stk` 页 JS (新):
   - `loadSimInfo()` 每 5s `fetch('/api/stk/siminfo')`
   - JSON 5 字段独立更新 DOM (`#simOp` / `#simImsi` / `#simIccid` / `#simMsisdn` / `#simAge`)
   - 失败静默

4. 用户 (浏览器) 看 `/stk` 页:
   - 看到 "SIM 卡信息" 卡: 运营商 / IMSI / ICCID / MSISDN / 刷新时间
   - 5s 自动刷新
   - 跟 "SIM 卡主动菜单" 卡 (v4.0.12 v2) 同页

## Error handling

| 场景 | 行为 |
|---|---|
| `g_sim_imsi` 等为空 (boot 5s 内 / SIM 离线) | `doc["imsi"]` 写空串, JS `j.imsi \|\| '-'` 显 "-" |
| `g_sim_queryMs == 0` (boot 后未查) | `ageMs = -1`, JS `ageStr(-1)` 显 "从未" |
| `fetch /api/stk/siminfo` 失败 (网络/路由) | JS 静默, DOM 保留旧值 |
| `stk_query_task` 失败 (modem 失联) | `g_sim_*` 保留上次值, `ageMs` 一直涨, 用户能看到"信号" |
| SIM 卡换 (e.g. 拔插新卡) | `stk_query_task` 30min 周期发现 (boot 5s 后 + 30min) — 用户可等 30min 或重启触发 |
| `parse_cnum` 2-quote 漏 (v4.0.11.21 review finding #2) | `g_sim_msisdn` 留空, UI 显 "-" — 按 working-code-no-touch 暂不修 |

**stk-paused 守住**:
- 不发 `AT+STKR` / `AT+STKTR`
- 不解禁 `/api/stk` / `/api/stk/refresh` / `/api/stk/cmd`
- 0 新 AT 命令发送代码

## Testing

### 烧板前 (按 [[feedback_code_review_before_flash]])

1. **静态审 diff**:
   - `git diff main...HEAD` = 1 file (src/main.cpp), 净 +~50 行
   - 改动 1-2 是新增 (handler + 路由), 改动 3-4 是复制 DASHBOARD_HTML 模板, 改动 5 是 memory
   - 验证新路由位置在 `/api/stk/menu` 路由同段 (不破坏现有路由顺序)
   - 验证 `handleApiStkSiminfo` 复用 `g_sim_mux` (跟其他 4 个 parse 函数同)
   - 验证 STK_PAGE_HTML 新卡用现有 CSS class, 不引新 CSS
2. **host test**: 不变 (210 PASS, 本批没动 pdu_codec, 不动 test_pdu_codec.cpp)
3. **memory 检查**: g_sim_mux 已被 portENTER_CRITICAL 保护, 新 handler 同样保护, 无死锁
4. **flash 容量检查**: STK_PAGE_HTML 净 +~30 行 HTML, ~5 行 CSS, ~15 行 JS = ~50 行 ≈ 2KB flash, 够 (当前 36.6% / 3.3MB, 余 2.1MB)
5. **memory 风险**: 0 新 BSS / 0 新全局变量 (复用 g_sim_*), 0 新 task

### 烧板后 (闭环验证)

1. **boot log**: 应见 `STK: querying SIM info...` (已有), 无新 log
2. **`curl /api/stk/siminfo`** (20s boot 后):
   - HTTP 200
   - JSON 5 字段 (imsi / iccid / msisdn / operator / ageMs)
   - 字段非空 (SIM 在线)
3. **浏览器 `/stk` 页**:
   - "SIM 卡信息" 卡显示 5 字段
   - "刷新时间" 5s 跳一次 (e.g. "5s 前" → "10s 前")
   - 跟 "SIM 卡主动菜单" 卡同页
4. **失败路径验证**:
   - `curl http://device/api/stk/siminfo` 5s 内重复 → JSON 不变 (5s 轮)
   - kill monitor (停 modem 不模拟, 这块不验)
5. **stk-paused 守住**:
   - `grep -rE "AT\\+STKR|AT\\+STKTR" src/main.cpp` → 0 命中 (handler 内 0 处)
   - `grep -E "api/stk/cmd|api/stk/refresh" src/main.cpp` → 仍注释

## Diff estimation

```
1 file changed, ~50 insertions(+), ~5 deletions(-)
- src/main.cpp: 1 handler (~25 行) + 1 路由 (1 行) + 1 HTML 卡片 (~10 行) + 1 JS 函数 (~15 行) + 1 setInterval (1 行) + 1 首屏调 (1 行) + 注释 (~5 行)
```

实际 diff 会在 plan 阶段精确化。

## Risks

| Risk | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 跟 `parse_cnum` 2-quote 漏 (v4.0.11.21 review finding #2) 冲突 | 中 | MSISDN 显 "-" | UI 容忍, working-code-no-touch 暂不修, 真机复现再动 |
| `stk_query_task` 没跑过 (刚 boot 5s 内) | 极低 | 5 字段全空, ageMs=-1 显 "从未" | UI 显 "-", 5s 后刷就有 |
| 5s 轮 + loadMenu 10s 轮不同步 | 极低 | UI 抖动 | fetch 异步, 无 race |
| g_sim_mux 临界区太长 (4 字符串 + uint32 + ageMs 算) | 极低 | 短阻塞 | < 1ms, 无影响 |
| 翔哥要立即看 MSISDN (但 2-quote 漏) | 中 | 看到 "-" 不爽 | 按 working-code-no-touch, 不主动修;若翔哥要,新开 plan |

## Out of scope (本 spec 不做, 可能下个 spec)

- 多 Proactive Command 解析 (0x21 DISPLAY_TEXT / 0x23 GET_INPUT / 0x24 SELECT_ITEM 等) — 协议层, 改 ~200 行
- STK 响应路径解禁 (stkSelect / /api/stk/cmd) — 破 stk-paused, 需翔哥拍板
- `parse_cnum` 2-quote 漏修复 (v4.0.11.21 review finding #2) — 按 working-code-no-touch 暂不修
- SIM 卡换自动重查 (e.g. modem 通知 SIM READY → 立即跑 stk_query_task) — 需要 ML307 URC
- `/api/stk/siminfo` 加 auth (现 no-op, 跟 /api/stk/menu 一样无 auth)

## How to apply (后续 session)

- 实施本 spec 后, [[stk-paused]] memory 更新: "读路径解禁包括 /api/stk/siminfo (SIM 信息), 响应路径仍暂停"
- 改 STK_PAGE_HTML 时, 复用 `.card` / `.kv` / `.k` / `.v` CSS (已存在, 不引新)
- 改 handler 时, **始终** 用 `portENTER_CRITICAL(&g_sim_mux)` 包 4 字符串读 (跟现有 parse 函数一致)
- 如果翔哥要 "改 MSISDN 显示" 修 `parse_cnum` — 新开 spec, 不在本 spec 范围
- 如果翔哥要 "加 CSQ 到 SIM 信息卡" — 新开 spec, 跟 simCsq 全局变量读 (主 main.cpp 已有)

## 相关 memory

- [[stk-paused]] — v4.0.12 v2 读路径解禁; 本 spec 扩展 SIM 信息部分
- [[project-state]] — v4.0.12 v2 HEAD (3150936 + cddd66d); 本 spec 实施后升 v4.0.13
- [[code-review-v4.0.11.21]] — finding #2 (parse_cnum 2-quote 漏) 影响本 spec MSISDN 显示, 暂不修
- [[feedback_code_review_before_flash]] — 烧前必审, 烧完稳定打 tag
- [[feedback-working-code-no-touch]] — 功能稳不动, MSISDN 不修
- [[feedback_test_on_production_code]] — host test 沿用, 本 spec 不动 pdu_codec
- [[stk-github-research]] — pySim/cat.py 未来扩展 Proactive Command 解析用

## Implementation

实施本 spec → 调 `superpowers:writing-plans` skill 出 plan → 按 plan 改 → 烧板验证 → commit `v4.0.13` + tag.
