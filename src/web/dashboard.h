// v4.0.14: DASHBOARD_HTML 物理抽 (main.cpp:2066-2476), 0 改内容
static const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--info:#60a5fa;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.container{max-width:880px;margin:0 auto;padding:8px 16px 48px}  /* v4.0.11.21: padding-top 24→8 跟 .app-nav margin-bottom 16→8 配对, 总间距 40→16px (用户: 外层和内层之间的宽度调小一些) */
header{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:24px;gap:16px}
header h1{margin:0;font-size:22px;font-weight:600;letter-spacing:-0.01em}
header h1 .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--muted);margin-right:8px;vertical-align:middle;transition:background .3s}
header h1 .dot.ok{background:var(--accent);box-shadow:0 0 8px rgba(74,222,128,.5)}
header h1 .dot.bad{background:var(--err)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:16px;margin-bottom:24px}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;box-shadow:var(--shadow)}
.card h3{margin:0 0 12px;font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:0.06em;color:var(--muted)}
.kv{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid var(--border);font-size:13px}
.kv:last-child{border-bottom:0;padding-bottom:0}
.kv:first-child{padding-top:0}
.kv .k{color:var(--muted)}
.kv .v{font-family:ui-monospace,SFMono-Regular,"SF Mono",Menlo,monospace;font-size:13px;color:var(--text);display:flex;align-items:center;gap:6px}
.tag{display:inline-flex;align-items:center;gap:4px;padding:2px 8px;border-radius:999px;font-size:11px;font-weight:600;letter-spacing:0.02em}
.tag-ok{background:rgba(74,222,128,.12);color:var(--accent)}
.tag-bad{background:rgba(248,113,113,.12);color:var(--err)}
.tag-warn{background:rgba(251,191,36,.12);color:var(--warn)}
.tag-mute{background:var(--card2);color:var(--muted)}
.actions{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:24px;align-items:center;background:var(--card);border:1px solid var(--border);border-radius:12px;padding:12px 16px}  /* v4.0.11.19: 圆角 + 卡片背景, 跟下面 .card 视觉统一 (用户: "UI 头部按钮下边背景里也和下面一样用圆角") */
.actions .restart{margin-left:auto}
@media(max-width:600px){.actions .restart{margin-left:0;width:100%;order:99}}
.btn{display:inline-flex;align-items:center;gap:6px;padding:9px 14px;border-radius:8px;font-size:13px;font-weight:500;text-decoration:none;color:var(--text);background:var(--card2);border:1px solid var(--border);transition:background .15s,border-color .15s,transform .05s;cursor:pointer;font-family:inherit}
.btn:hover{background:#2c3744;border-color:#384454}
.btn:active{transform:translateY(1px)}
.btn.primary{background:var(--accent);color:#0a1014;border-color:transparent}
.btn.primary:hover{background:#5be692}
.btn.warn{background:rgba(248,113,113,.12);color:var(--err);border-color:rgba(248,113,113,.25)}
.btn.warn:hover{background:rgba(248,113,113,.18)}
.btn.mute{background:var(--card);color:var(--muted)}
.form-card{grid-column:1/-1}
.form-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media (max-width:600px){.form-grid{grid-template-columns:1fr}}
.field{display:flex;flex-direction:column;gap:6px}
.field label{font-size:12px;color:var(--muted);font-weight:500}
.field label small{color:#5e6a7a;font-weight:400}
.field input{background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:9px 11px;color:var(--text);font-size:13px;font-family:inherit;transition:border-color .15s,box-shadow .15s}
.field input:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(74,222,128,.15)}
/* P15c: select 暗色化 (浏览器默认白底灰边丑) */
.field select{
  background:var(--bg);border:1px solid var(--border);border-radius:8px;
  padding:9px 32px 9px 11px;color:var(--text);font-size:13px;font-family:inherit;
  appearance:none;-webkit-appearance:none;-moz-appearance:none;cursor:pointer;
  background-image:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'><path fill='%238b95a5' d='M2 4l4 4 4-4z'/></svg>");
  background-repeat:no-repeat;background-position:right 10px center;
  transition:border-color .15s,box-shadow .15s;
}
.field select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(74,222,128,.15)}
.field select option{background:var(--card);color:var(--text);padding:6px}
.field select:disabled{opacity:.5;cursor:not-allowed}
.toggle-row{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:14px 16px;margin-top:16px;background:var(--card2);border:1px solid var(--border);border-radius:10px}
.toggle-row .label{display:flex;flex-direction:column;gap:2px}
.toggle-row .label .t{font-size:14px;font-weight:500;color:var(--text)}
.toggle-row .label .h{font-size:12px;color:var(--muted);line-height:1.4}
.toggle{position:relative;display:inline-block;width:50px;height:30px;flex-shrink:0}
.toggle input{opacity:0;width:0;height:0;position:absolute}
.toggle .slider{position:absolute;inset:0;background:#3a4654;border-radius:999px;transition:background .2s ease;cursor:pointer}
.toggle .slider::before{content:"";position:absolute;top:2px;left:2px;width:26px;height:26px;background:#f1f5f9;border-radius:50%;box-shadow:0 2px 4px rgba(0,0,0,.3);transition:transform .2s cubic-bezier(.4,0,.2,1)}
.toggle input:checked + .slider{background:var(--accent)}
.toggle input:checked + .slider::before{transform:translateX(20px)}
.toggle input:focus-visible + .slider{box-shadow:0 0 0 3px rgba(74,222,128,.25)}
.status-line{margin-top:10px;font-size:12px;color:var(--muted);min-height:16px}
.status-line.ok{color:var(--accent)}
.status-line.err{color:var(--err)}
.section-title{display:flex;align-items:center;justify-content:space-between;margin:8px 0 12px}
.section-title h2{margin:0;font-size:14px;font-weight:600;color:var(--text)}
.history{list-style:none;margin:0;padding:0;max-height:240px;overflow-y:auto;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px}
.history li{display:grid;grid-template-columns:130px 110px 1fr auto;gap:12px;padding:8px 4px;border-bottom:1px solid var(--border);align-items:center}
.history li:last-child{border-bottom:0}
.history .ts{color:var(--muted)}
.history .ph{color:var(--info)}
.history .bd{color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.history .st.ok{color:var(--accent)}
.history .st.bad{color:var(--err)}
.history .empty{text-align:center;color:var(--muted);padding:24px;grid-column:1/-1}
footer{margin-top:32px;text-align:center;font-size:12px;color:var(--muted)}
footer .refresh-dot{display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--accent);margin-right:6px;animation:pulse 2s ease-in-out infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}
@keyframes flash{0%{background:rgba(74,222,128,.25)}100%{background:transparent}}
.flash{animation:flash 1.5s ease-out 1}
</style></head><body>
<div class="container">
  <header id="page-head">
    <h1><span id="status-dot" class="dot"></span>SMS Forwarder</h1>
  </header>
  <div class="actions" id="page-nav">
    <a class="btn" href="/dashboard">Dashboard</a>
    <a class="btn" href="/send">发送</a>
    <a class="btn" href="/stk">STK 控制台</a>
    <a class="btn" href="/config">配置</a>
    <a class="btn primary" href="/update">OTA</a>
    <a class="btn warn restart" href="javascript:doRestart()">重启</a>
  </div>

  <div class="grid">
    <div class="card"><h3>运行</h3>
      <div class="kv"><span class="k">启动次数</span><span class="v" id="boot">-</span></div>
      <div class="kv"><span class="k">开机时间</span><span class="v" id="uptime">-</span></div>
      <div class="kv"><span class="k">WiFi 信号</span><span class="v"><span id="wifi" class="tag tag-mute">off</span><span id="wifiBars" class="k"></span></span></div>
      <div class="kv"><span class="k">4G 模组</span><span class="v"><span id="ml" class="tag tag-mute">off</span></span></div>
      <div class="kv"><span class="k">4G 信号</span><span class="v" id="csqBars">-</span></div>
      <div class="kv"><span class="k">长短信拼接槽</span><span class="v" id="udh">0/4</span></div>
    </div>

    <div class="card"><h3>推送</h3>
      <div class="kv"><span class="k">成功</span><span class="v" style="color:var(--accent)" id="ok">0</span></div>
      <div class="kv"><span class="k">失败</span><span class="v" style="color:var(--err)" id="fal">0</span></div>
      <div class="kv"><span class="k">待推队列</span><span class="v" id="q">0</span></div>
      <div class="kv"><span class="k">上次短信</span><span class="v" id="ls">-</span></div>
      <div class="kv"><span class="k">上次推送</span><span class="v" id="lp">-</span></div>
    </div>

    <div class="card"><h3>系统</h3>
      <div class="kv"><span class="k">设备时间</span><span class="v" id="devTime">-</span></div>
      <div class="kv"><span class="k">时间同步</span><span class="v" id="ntp">-</span></div>
      <div class="kv"><span class="k">总内存</span><span class="v" id="heapTotal">-</span></div>
      <div class="kv"><span class="k">已使用</span><span class="v" id="heapUsed">-</span></div>
      <div class="kv"><span class="k">当前空闲</span><span class="v" id="heap">0 K</span></div>
      <div class="kv"><span class="k">启动以来最低空闲</span><span class="v" id="heapMin">0 K</span></div>
    </div>
  </div>

  <div class="card">
    <div class="section-title">
      <h2>最近接收 SMS</h2>
      <div style="display:flex;gap:8px">
        <button class="btn mute" onclick="loadRecent()">刷新</button>
        <button class="btn warn" onclick="clearRecent()">清空</button>
      </div>
    </div>
    <ul class="history" id="rx"><li class="empty">暂无</li></ul>
  </div>

  <div class="card">
    <div class="section-title">
      <h2>最近发送 SMS</h2>
      <div style="display:flex;gap:8px">
        <button class="btn mute" onclick="loadHist()">刷新</button>
        <button class="btn warn" onclick="clearHist()">清空</button>
      </div>
    </div>
    <ul class="history" id="hist"><li class="empty">加载中...</li></ul>
  </div>
  </div>

  <footer><span class="refresh-dot"></span><span id="refresh-note">30s 自动刷新</span></footer>
</div>

<script>
// v4.0.7: 信号 5 格 (4G CSQ 0-31, WiFi RSSI dBm) — 通用 ASCII ▂▃▄▅█ + 数字
function csqBars(csq){  // 0=无, 1-7=1格, 8-14=2, 15-20=3, 21-26=4, 27-31=5
  if(csq<=0) return '▁▁▁▁▁';
  const n = csq>=27?5:(csq>=21?4:(csq>=15?3:(csq>=8?2:(csq>=1?1:0))));
  return '▂▃▄▅█'.slice(0,n) + '▁'.repeat(5-n);
}
function wifiBars(rssi){  // dBm 阈值: -50/-60/-70/-80/-90
  if(rssi>=0||rssi<-90) return '▁▁▁▁▁';
  const n = rssi>=-50?5:(rssi>=-60?4:(rssi>=-70?3:(rssi>=-80?2:1)));
  return '▂▃▄▅█'.slice(0,n) + '▁'.repeat(5-n);
}
// v4.0.6 P11b: 读 epoch ms, 显示真实 wall-clock ("2026/06/18 14:32:05")
function fmtUtc(epochMs) {
  if (!epochMs) return '-';
  const d = new Date(epochMs);
  const p = n => String(n).padStart(2,'0');
  return d.getFullYear()+'/'+p(d.getMonth()+1)+'/'+p(d.getDate())
    +' '+p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());
}
function ago(epochMs) {
  if (!epochMs) return '-';
  const s = Math.floor((Date.now() - epochMs) / 1000);
  if (s < 60)    return s + '秒前';
  if (s < 3600)  return Math.floor(s/60) + '分钟前';
  if (s < 86400) return Math.floor(s/3600) + '小时前';
  return Math.floor(s/86400) + '天前';
}
// v4.0.7: 开机时长 (millis → d/h/m/s)
function fmtUptime(ms){
  if(!ms && ms!==0) return '-';
  const s = Math.floor(ms/1000);
  const d = Math.floor(s/86400), h = Math.floor((s%86400)/3600),
        m = Math.floor((s%3600)/60),   sec = s%60;
  return (d>0?d+'天 ':'') + (h<10?'0':'') + h + ':' + (m<10?'0':'') + m + ':' + (sec<10?'0':'') + sec;
}
function setStatus(id, text, cls) {
  const el = document.getElementById(id);
  el.textContent = text;
  el.className = 'tag ' + (cls || 'tag-mute');
}
async function poll() {
  try {
    // v4.0.7 P1: SPA 内优先用父窗口共享的 status (1 次 fetch 服务多 frame)
    let j = (window.parent && window.parent.__spaStatus) || null;
    if (!j) {
      const r = await fetch('/api/status', {cache:'no-store'});
      if (!r.ok) return;
      j = await r.json();
    }

    document.getElementById('boot').textContent  = j.boot;
    document.getElementById('uptime').textContent = fmtUptime(j.uptimeMs);
    document.getElementById('ok').textContent    = j.pushOk;
    document.getElementById('fal').textContent   = j.pushFail;
    document.getElementById('q').textContent     = j.qLen;

    setStatus('wifi', j.wifi ? '已连' : '断开', j.wifi ? 'tag-ok' : 'tag-bad');
    // RSSI 已在 wifiBars 行展示, 删独立 rssi 行 (HTML 无 id="rssi" 会抛 null)

    setStatus('ml', j.mlAlive ? '在线' : '离线', j.mlAlive ? 'tag-ok' : 'tag-bad');

    // v4.0.7: 4G + WiFi 5 格信号 (CSQ 0-31 → dBm, RSSI → bars)
    document.getElementById('csqBars').textContent =
      (j.csq >= 0 && j.csq <= 31) ? csqBars(j.csq) + ' · ' + j.csqDbm + ' dBm' : '无信号';
    document.getElementById('wifiBars').textContent =
      (j.wifi && j.wifiRssi) ? wifiBars(j.wifiRssi) + ' ' + j.wifiRssi + ' dBm' : '-';

    // v4.0.7: SIM 信息已搬去 /stk 页, dashboard 不重复
    document.getElementById('udh').textContent = (j.udhActive||0) + '/4';
    document.getElementById('ntp').textContent = j.timeSynced ? '已同步' : '未同步';
    document.getElementById('ntp').className   = 'tag ' + (j.timeSynced ? 'tag-ok' : 'tag-warn');

    // P11b: 用 epoch ms 显示真实时间, 同步前显示 "-"
    // 修复 1970 bug: 不能 fallback 到 lastSmsMs (millis 自开机, 当 epoch 用 new Date() = 1970+uptime)
    const lp = j.lastPushOkUtc || 0;
    const ls = j.lastSmsUtc    || 0;
    const lpEl = document.getElementById('lp');
    const lsEl = document.getElementById('ls');
    lpEl.textContent = lp ? fmtUtc(lp) : '-';
    lsEl.textContent = ls ? fmtUtc(ls) : '-';

    document.getElementById('heap').textContent    = Math.round(j.freeHeap/1024) + ' K';
    document.getElementById('heapMin').textContent = Math.round(j.minFreeHeap/1024) + ' K';
    // v4.0.6 P18: 设备时间 (NTP 同步后才有意义)
    const dt = document.getElementById('devTime');
    if (j.timeSynced && j.deviceTimeMs) {
      dt.textContent = fmtUtc(j.deviceTimeMs);
      dt.style.color = 'var(--text)';
    } else {
      dt.textContent = '- 未同步';
      dt.style.color = 'var(--warn)';
    }
    if (j.heapTotal != null) {
      const total = j.heapTotal, used = j.heapUsed, free = j.freeHeap;
      const pct = total ? Math.round(used*100/total) : 0;
      document.getElementById('heapTotal').textContent = Math.round(total/1024) + ' K';
      document.getElementById('heapUsed').textContent  = Math.round(used/1024) + ' K (' + pct + '%)';
    }

    const dot = document.getElementById('status-dot');
    dot.className = 'dot ' + (j.wifi ? 'ok' : 'bad');
  } catch (e) { console.warn('poll failed', e); }
}

async function loadHist() {
  const ul = document.getElementById('hist');
  try {
    const r = await fetch('/api/sent', {cache:'no-store'});
    const j = await r.json();
    if (!j.items || !j.items.length) {
      ul.innerHTML = '<li class="empty">暂无发送记录</li>';
      return;
    }
    // v4.0.11.19: items 已是新→旧 (server emit 倒序), 直接 map; 字段 ts→ageMs, ph→phone, bp→body, ok→ok, c→err
    ul.innerHTML = j.items.map(it => {
      const ok = it.ok ? '<span class="st ok">OK</span>' : '<span class="st bad">FAIL</span>';
      const ago = agoShort(it.ageMs);
      return '<li>'
        + '<span class="ts">' + ago + '</span>'
        + '<span class="ph">' + escapeHtml(it.phone||'-') + '</span>'
        + '<span class="bd">' + escapeHtml(it.body||'').slice(0,60) + '</span>'
        + ok + '</li>';
    }).join('');
  } catch (e) {
    ul.innerHTML = '<li class="empty">加载失败</li>';
  }
}

function doRestart() {
  if (!confirm('确认重启设备?')) return;
  fetch('/restart').catch(()=>{});
  alert('重启中… 约 5s 后回来');
}

// v4.0.7: 最近接收 SMS (从 /api/recent 拿, dashboard 主页显示, 5s 轮询)
// v4.0.7 P3: 新增条目闪烁动画 — 比较 hash 检测新增
let _recentHash = '';
async function loadRecent(){
  const ul = document.getElementById('rx');
  try {
    const r = await fetch('/api/recent', {cache:'no-store'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    const j = await r.json();
    if (!j.items || !j.items.length) {
      ul.innerHTML = '<li class="empty">暂无接收记录</li>';
      _recentHash = '';
      return;
    }
    const newHash = j.items.map(it => it.id || (it.tsMs||'')+it.body).join('|');
    const isNew = _recentHash && newHash !== _recentHash;
    ul.innerHTML = j.items.map((it, i) => {
      const ago = agoShort(it.ageMs);
      const preview = it.body.length > 60 ? it.body.slice(0,60)+'…' : it.body;
      const flashCls = (isNew && i === 0) ? ' class="flash"' : '';
      return `<li${flashCls}><span class="ts">${ago}</span><span class="ph">${escapeHtml(it.phone)}</span><span class="bd" title="${escapeHtml(it.body)}">${escapeHtml(preview)}</span></li>`;
    }).join('');
    _recentHash = newHash;
  } catch (e) {
    ul.innerHTML = '<li class="empty">加载失败</li>';
  }
}
function agoShort(ms){
  if(ms < 0) return '-';
  const s = Math.floor(ms/1000);
  if(s < 60)    return s+'秒前';
  if(s < 3600)  return Math.floor(s/60)+'分前';
  if(s < 86400) return Math.floor(s/3600)+'时前';
  return Math.floor(s/86400)+'天前';
}
function escapeHtml(s){ return String(s||'').replace(/[&<>"']/g, c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }

async function clearHist() {
  if (!confirm('确认清空所有最近发送记录? (重启就丢, 不会恢复)')) return;
  try {
    const r = await fetch('/api/sent/clear', {method:'POST'});
    if (!r.ok) throw new Error('HTTP '+r.status);
    loadHist();
  } catch (e) {
    alert('清空失败: ' + e.message);
  }
}

// v4.0.11.19: 对称 sent, 清空最近接收 SMS (RAM, 重启本来就丢)
async function clearRecent() {
  if (!confirm('确认清空最近接收 SMS? (重启就丢, 不会恢复)')) return;
  try {
    const r = await fetch('/api/recent/clear', {method:'POST'});
    if (!r.ok) throw new Error('HTTP '+r.status);
    loadRecent();
  } catch (e) {
    alert('清空失败: ' + e.message);
  }
}

// v4.0.6 P24: 配网表单搬去 /config 页 (dashboard 只看状态), 这里不再加载
poll(); loadHist(); loadRecent();
setInterval(poll, 30000);
setInterval(loadHist, 60000);
setInterval(loadRecent, 10000);   // v4.0.7: 最近 SMS 10s 刷一次 (快一点,翔哥看新短信)
</script><script>if(self!==top){['page-head','page-nav'].forEach(function(id){var h=document.getElementById(id); if(h)h.style.display='none';});}</script></body></html>
)HTML";
