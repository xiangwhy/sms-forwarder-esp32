# main.cpp HTML 物理抽离 spec (v4.0.14)

> **For agentic workers:** 后跟 `docs/superpowers/plans/2026-06-20-main-cpp-html-extract.md` (writing-plans skill 出)。

## Goal

v4.0.14 — `src/main.cpp` 把 6 段 `R"HTML(...)HTML"` 物理抽到 `src/web/*.h` (6 file), main.cpp 减 ~1952 行 (48% → ~25%), **0 改逻辑 0 改 build 0 改 test**, 1 commit + tag v4.0.14。

**关键词**: 物理拆, 0 业务改动, 0 改路由, 0 改 handler, 0 改 fw-tag, 0 改 FW_VERSION 宏, 0 改 pdu_codec。

## Background

`src/main.cpp` 现状 (commit 4ff837c HEAD):
- 总 4023 行
- 6 段 `R"HTML` 块占 ~1952 行 (48%): DASHBOARD_HTML / OTA_HTML / APP_PAGE_HTML / STK_PAGE_HTML / SEND_PAGE_HTML / CONFIG_HTML
- 40+ `static` function 占 ~1400 行 (35%): NVS + AT + UCP 拼接 + STK 解析 + 推送 + 日志
- setup/loop/AT 命令 ~670 行 (17%)

最大单段: CONFIG_HTML ~669 行, SEND_PAGE_HTML ~469 行 — **UI 块占大头, 不是逻辑问题**, 是物理结构问题。

翔哥 2026-06-20 brainstorm 选 **A. 最小拆分 (仅 HTML 抽)**, 0 风险, 一次烧。

## Architecture

**方案**: 仅物理抽, **0 改逻辑**。
- 6 段 `R"HTML` 块 1:1 复制到 `src/web/*.h` (6 new file, 0 改 HTML 内容)
- 每个 .h 用 `static const char <NAME>[] PROGMEM = R"HTML(...)HTML";` 跟 main.cpp 原型一致 (static = internal linkage, 多 TU 副本 OK, PROGMEM 不变)
- main.cpp 删 6 段 `R"HTML`, 加 6 行 `#include "web/<name>.h"`
- PlatformIO 默认 `src/` include, **0 改 platformio.ini**
- host test 不变 (跑 pdu_codec, 不碰 HTML / main.cpp HTML 段)

**不实现** (out of scope, 留后续):
- STK 模块抽 (B 方案) — 留 v4.0.15+ 跟 STK 深化 (0x21/0x23/0x24 解析) 一起做
- 推送 / 日志 / NVS 抽 (C 方案) — 大重构, 翔哥 2026-06-19 拍板 working-code-no-touch 暂不动功能
- 改 HTML 内容 / 改 fw-tag / 改其他逻辑 — housekeeping 性质, 0 业务改动

## Tech Stack

- PlatformIO (默认 `src/` include, 0 改 platformio.ini)
- C++17, ESP32-S3, ESPAsyncWebServer, ArduinoJson
- 6 new file (`.h` only, 不需要 `.cpp`)
- 1 file modify (`src/main.cpp`)

## Scope

### 启用 (1 commit, 7 改动)

| # | 文件 | 状态 | 责任 |
|---|---|---|---|
| 1 | `src/web/dashboard.h` | **NEW** | `DASHBOARD_HTML` 抽 (~410 行, 2066-2476) |
| 2 | `src/web/ota.h` | **NEW** | `OTA_HTML` 抽 (~115 行, 2477-2592) |
| 3 | `src/web/app.h` | **NEW** | `APP_PAGE_HTML` 抽 (~178 行, 2593-2771) |
| 4 | `src/web/stk.h` | **NEW** | `STK_PAGE_HTML` 抽 (~111 行, 2772-2883, v4.0.13 含 SIM 信息卡) |
| 5 | `src/web/send.h` | **NEW** | `SEND_PAGE_HTML` 抽 (~469 行, 2884-3353) |
| 6 | `src/web/config.h` | **NEW** | `CONFIG_HTML` 抽 (~669 行, 3354-4023, v4.0.13 footer 删) |
| 7 | `src/main.cpp` | **modify** | 删 6 段 `R"HTML` + 加 6 行 `#include "web/<name>.h"` |

### 命名约定

| 源 main.cpp 名 | 抽到文件 | include 行 |
|---|---|---|
| `DASHBOARD_HTML` | `src/web/dashboard.h` | `#include "web/dashboard.h"` |
| `OTA_HTML` | `src/web/ota.h` | `#include "web/ota.h"` |
| `APP_PAGE_HTML` | `src/web/app.h` | `#include "web/app.h"` |
| `STK_PAGE_HTML` | `src/web/stk.h` | `#include "web/stk.h"` |
| `SEND_PAGE_HTML` | `src/web/send.h` | `#include "web/send.h"` |
| `CONFIG_HTML` | `src/web/config.h` | `#include "web/config.h"` |

`.h` 文件模板 (例 `src/web/dashboard.h`):
```cpp
// v4.0.14: DASHBOARD_HTML 物理抽 (main.cpp:2066-2476), 0 改内容
static const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
... (原 main.cpp 2066-2476 内容, 1:1 复制) ...
</html>
)HTML";
```

### 不启用 (out of scope, 明确)

- `STK_PAGE_HTML` 不解 `#if 0` (v4.0.12 v2 已解开, v4.0.13 加 SIM 信息卡, 不动)
- 任何 `static` function 不抽 (B/C 方案)
- 任何 `R"HTML` 块内 JS / CSS 不重构
- 任何 handler / 路由 / API 不改
- 任何 fw-tag / FW_VERSION 宏 不改
- 任何 pdu_codec 不改

## Risks (已知, 接受)

| Risk | 概率 | 影响 | 缓解 |
|---|---|---|---|
| `static const char X[] PROGMEM` 在 .h 里多 TU 重复定义 | 0 | — | `static` = internal linkage, 每个 TU 自己的副本, PROGMEM 也没问题 (跟 main.cpp 现状一致) |
| 编译时间变慢 (6 new file) | 极低 | ~1-2s | 增量 build, 实际无感 |
| 烧板后页面字节数变 (PROGMEM 排版) | 极低 | int < 100 bytes | 烧后 curl 验证 6 页面字节数 跟 v4.0.13 ±10 字节 |
| OTA / Web auth 路由名错 | 0 | — | 0 改路由, 0 改 handler, 纯搬 HTML |
| 主页 iframe / SPA / sub-page JS 引用 X 错 | 0 | — | 0 改引用, 0 改 JS, 0 改 handler, 纯搬 |

## Verification

### 烧板前 (按 [[feedback_code_review_before_flash]])

1. **静态审**:
   - `git diff main...HEAD` = main.cpp 删 6 段 `R"HTML` + 加 6 行 `#include` + 6 new file
   - 验证 6 .h 文件用 `static const char <NAME>[] PROGMEM = R"HTML(...)HTML";` 跟 main.cpp 原型 1:1 一致
   - 验证 0 改 fw-tag, 0 改 FW_VERSION, 0 改 handler, 0 改路由, 0 改 pdu_codec
2. **host test**: 不变 (`./test_pdu_codec` 仍 210 PASS, 本批没动 pdu_codec)
3. **memory / BSS**: 不变 (HTML 字节数不变, PROGMEM 跟 main.cpp 同位置)

### 烧板后验证 (按 [[feedback_code_review_before_flash]] 不迭代烧)

1. **首次 boot**: 看 log 跟 v4.0.13 完全一致:
   - `modem_init_at: STKPCMD=1 rc=0`
   - `STK: querying SIM info...`
   - `MSISDN=+66813079348`, `Operator=CHINA MOBILE`
2. **6 页面 curl 验证**:
   - `GET /dashboard` → 200 + 字节数 跟 v4.0.13 ±10
   - `GET /app` → 200 + 字节数 跟 v4.0.13 ±10
   - `GET /stk` → 200 + 字节数 跟 v4.0.13 ±10 (含 v4.0.13 SIM 信息卡 5 id)
   - `GET /send` → 200 + 字节数 跟 v4.0.13 ±10
   - `GET /config` → 200 + 字节数 跟 v4.0.13 ±10 (footer 已删)
   - `GET /update` → 200 + 字节数 跟 v4.0.13 ±10 (footer 已删)
3. **API 验证**:
   - `GET /api/stk/menu` → `{cmd:0, count:0}` (跟 v4.0.13 一致)
   - `GET /api/stk/siminfo` → 5 字段 JSON (imsi/iccid 空已知, msisdn+operator+ageMs)
4. **浏览器** (翔哥自验): 主页 + 5 sub-page 视觉跟 v4.0.13 完全一致
5. **stk-paused 守**: `grep AT+STKR|STKTR` 0 活跃, 11 命中全注释

### 失败回滚 (1 commit revert)

```bash
git revert <commit-hash>  # 6 new file + main.cpp revert 一次性
```

## Diff estimation

预估:
- 1 file changed, ~1960 insertions(+), 1960 deletions(-) (净 0, 但行数大)
- 6 new file: ~1952 insertions
- main.cpp: +6 include, -1952 HTML 段

实际会比 v4.0.13 净大 ~10 行 (6 行 include + 6 .h 头部注释)

## Out of scope (后续 plan)

- v4.0.15+: STK 模块抽 (B 方案) — 跟 STK 深化 (0x21/0x23/0x24 解析) 一起做
- v4.0.16+: 推送模块抽 (post_pushplus / build_push_payload / http_post_json / post_pushplus_raw / push_boot_notification)
- v4.0.17+: 日志模块抽 (rx_log / tx_log / stk_log)
- v4.0.18+: NVS 模块抽 (loadConfig / saveConfig / isConfigValid / wipeConfig)
- 修 v4.0.11.21 review 7 finding (P0 真机复现才动, 跟 [[feedback-working-code-no-touch]])
- 新功能 (USSD 余额 / 短信模板 / OTA 加固 / 监控统计)

## 相关 memory

- [[feedback_code_review_before_flash]] — 烧前必审 (v4.0.14 改 7 file 必审, 1 commit 烧)
- [[feedback-working-code-no-touch]] — 0 业务改动, housekeeping 性质, 真机复现才动功能
- [[feedback-bump-fw-version]] — 0 改 fw-tag (5 处仍 v4.0.12, v4.0.14 不 bump)
- [[feedback_incremental_build]] — 增量 build, 别 rm -rf
- [[stk-paused]] — 0 解禁, 跟 v4.0.13 一致
- [[project-state]] — v4.0.13 HEAD (4ff837c), v4.0.14 待出
