// v4.0.14: SEND_PAGE_HTML 物理抽 (main.cpp:2884-3353), 0 改内容
static const char SEND_PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>发送 · SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.container{max-width:880px;margin:0 auto;padding:8px 16px 48px}  /* v4.0.11.21: padding-top 24→8 跟 .app-nav margin-bottom 16→8 配对, 总间距 40→16px (用户: 外层和内层之间的宽度调小一些) */
header{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:24px;gap:16px}
header h1{margin:0;font-size:22px;font-weight:600;letter-spacing:-0.01em}
header .fw-tag{font-size:11px;color:var(--muted);border:1px solid var(--border);padding:3px 8px;border-radius:4px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
nav{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 16px}
.btn{display:inline-flex;align-items:center;gap:6px;padding:9px 14px;border-radius:8px;font-size:13px;font-weight:500;text-decoration:none;color:var(--text);background:var(--card2);border:1px solid var(--border);transition:background .15s;cursor:pointer;font-family:inherit}
.btn:hover{background:#2c3744;border-color:#384454}
.btn.primary{background:var(--accent);color:#0a1014;border-color:transparent}
.btn.primary:hover{background:#5be692}
.btn.warn{background:rgba(248,113,113,.12);color:var(--err);border-color:rgba(248,113,113,.25)}
.btn:disabled{opacity:.5;cursor:not-allowed}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;box-shadow:var(--shadow);margin-bottom:16px}
.card h3{margin:0 0 12px;font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:0.06em;color:var(--muted)}
.field{display:flex;flex-direction:column;gap:6px;margin-bottom:12px}
.field label{font-size:12px;color:var(--muted);font-weight:500}
.field label small{color:#5e6a7a;font-weight:400}
.field input,.field textarea{background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:9px 11px;color:var(--text);font-size:13px;font-family:inherit;transition:border-color .15s,box-shadow .15s}
.field input:focus,.field textarea:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(74,222,128,.15)}
.field textarea{resize:vertical;min-height:90px}
.tag{display:inline-flex;align-items:center;gap:4px;padding:2px 8px;border-radius:999px;font-size:11px;font-weight:600}
.tag-ok{background:rgba(74,222,128,.12);color:var(--accent)}
.tag-bad{background:rgba(248,113,113,.12);color:var(--err)}
.status-line{margin-top:10px;font-size:12px;color:var(--muted);min-height:16px}
.status-line.ok{color:var(--accent)}.status-line.err{color:var(--err)}
.hist-pre{background:#0a0e13;border:1px solid var(--border);border-radius:8px;padding:12px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;max-height:320px;overflow:auto;color:var(--text);white-space:pre-wrap;word-break:break-all;margin:0}
</style></head><body>
<div class="container">
  <header id="page-head">
    <h1>📤 SMS 发送</h1>
    <span class="fw-tag">v4.0.12</span>
  </header>
  <nav id="page-nav">
    <a href="/dashboard" class="btn">Dashboard</a>
    <a href="/send" class="btn primary">发送</a>
    <a href="/stk" class="btn">STK 控制台</a>
    <a href="/config" class="btn">配置</a>
    <a href="/update" class="btn">OTA</a>
  </nav>

  <div class="card">
    <h3>发送短信</h3>
    <div class="field">
      <label>接收方手机号</label>
      <input id="ph" placeholder="13800001234 或 +8613800001234" maxlength="20">
    </div>
    <div class="field">
      <label>短信内容 <small id="cnt">(0 / 70 字符单条上限)</small></label>
      <textarea id="body" maxlength="500" oninput="document.getElementById('cnt').textContent='(' + this.value.length + ' / 70 字符单条上限)'"></textarea>
    </div>
    <button id="go" class="btn primary" onclick="send()">发送</button>
    <div id="out" class="status-line"></div>
  </div>

  <div class="card">
    <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:12px">
      <h3 style="margin:0">最近发送 (最多 32 条)</h3>
      <div style="display:flex;gap:8px">
        <button class="btn" onclick="loadHist()">刷新</button>
        <button class="btn warn" onclick="clearHist()">清空</button>
      </div>
    </div>
    <pre id="hist" class="hist-pre">加载中...</pre>
  </div>
</div>
<script>
async function send() {
  const ph = document.getElementById('ph').value.trim();
  const body = document.getElementById('body').value;
  const out = document.getElementById('out');
  const go = document.getElementById('go');
  if (!ph || !body) { out.className='status-line err'; out.textContent='手机号和内容不能空'; return; }
  go.disabled = true; out.className='status-line'; out.textContent='发送中… (最长 10s)';
  try {
    const r = await fetch('/api/send', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ phone: ph, body: body })
    });
    const j = await r.json();
    out.className = 'status-line ' + (j.ok ? 'ok' : 'err');
    let txt = (j.ok ? '✓ 发送成功' : '✗ 失败') + ' · HTTP ' + r.status;
    if (j.ok) txt += ' · ref=' + j.ref + ' · parts=' + j.parts;
    else      txt += ' · err=' + j.err + (j.code !== undefined ? ' · code=' + j.code : '');
    out.textContent = txt;
    if (j.ok) loadHist();
  } catch (e) {
    out.className='status-line err'; out.textContent='网络错误: ' + e.message;
  } finally {
    go.disabled = false;
  }
}
async function loadHist() {
  try {
    const r = await fetch('/api/sent', {cache:'no-store'});
    const j = await r.json();
    document.getElementById('hist').textContent = JSON.stringify(j, null, 2);
  } catch (e) {
    document.getElementById('hist').textContent = '加载失败: ' + e.message;
  }
}
async function clearHist() {
  if (!confirm('确认清空所有最近发送记录?')) return;
  try {
    const r = await fetch('/api/sent/clear', {method:'POST'});
    if (!r.ok) { alert('清空失败 HTTP '+r.status); return; }
    loadHist();
  } catch (e) { alert('网络错误: '+e.message); }
}
loadHist();
</script>
<script>if(self!==top){['page-head','page-nav'].forEach(function(id){var h=document.getElementById(id); if(h)h.style.display='none';});}</script>
</body></html>
)HTML";
