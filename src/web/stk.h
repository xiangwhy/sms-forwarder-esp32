// v4.0.14: STK_PAGE_HTML 物理抽 (main.cpp:2772-2883), 0 改内容
static const char STK_PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>STK · SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--info:#60a5fa;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.container{max-width:880px;margin:0 auto;padding:8px 16px 48px}
header{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:24px;gap:16px}
header h1{margin:0;font-size:22px;font-weight:600;letter-spacing:-0.01em}
header .fw-tag{font-size:11px;color:var(--muted);border:1px solid var(--border);padding:3px 8px;border-radius:4px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
nav{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 16px}
.btn{display:inline-block;padding:6px 12px;border:1px solid var(--border);border-radius:6px;background:var(--card2);color:var(--text);text-decoration:none;font-size:13px;cursor:pointer;font-family:inherit}
.btn:hover{background:var(--border)}
.btn.primary{background:var(--accent);color:#0a1014;border-color:transparent;font-weight:600}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:16px;box-shadow:var(--shadow);margin-bottom:16px}
.card h3{margin:0 0 12px;font-size:14px;font-weight:600;color:var(--text);display:flex;align-items:center;justify-content:space-between}
.status-line{margin-top:8px;font-size:12px;color:var(--muted)}
.status-line.ok{color:var(--accent)}.status-line.err{color:var(--err)}
</style></head>
<body>
<div class="container">
  <header id="page-head">
    <h1>STK 控制台</h1>
    <span class="fw-tag">v4.0.12</span>
  </header>
  <nav id="page-nav">
    <a href="/dashboard" class="btn">Dashboard</a>
    <a href="/send" class="btn">发送</a>
    <a href="/stk" class="btn primary">STK</a>
    <a href="/config" class="btn">配置</a>
    <a href="/update" class="btn">OTA</a>
  </nav>

  <div class="card">
    <h3>SIM 卡信息</h3>
    <div class="kv"><span class="k">运营商</span><span class="v" id="simOp">-</span></div>
    <div class="kv"><span class="k">IMSI</span><span class="v" id="simImsi">-</span></div>
    <div class="kv"><span class="k">ICCID</span><span class="v" id="simIccid">-</span></div>
    <div class="kv"><span class="k">本机号 (MSISDN)</span><span class="v" id="simMsisdn">-</span></div>
    <div class="kv"><span class="k">刷新时间</span><span class="v" id="simAge">-</span></div>
  </div>

  <div class="card">
    <h3>SIM 卡主动菜单 (SETUP_MENU 0x25) <span class="status-line" id="menu_status" style="margin:0">等待 SIM 推送…</span></h3>
    <div id="menu_title" style="font-weight:600;margin-bottom:8px;color:var(--info)"></div>
    <ul id="menu_list" style="list-style:none;margin:0;padding:0"></ul>
    <p style="font-size:12px;color:var(--muted);margin:10px 0 0">只读模式: 显示 SIM 推的 SETUP_MENU, 不发任何 AT+STKR / AT+STKTR 终端响应 (stk-paused 规则)。</p>
  </div>
</div>
<script>
function escapeHtml(s){ return String(s||'').replace(/[&<>"']/g, c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }
async function loadMenu(){
  const ul = document.getElementById('menu_list');
  const title = document.getElementById('menu_title');
  const status = document.getElementById('menu_status');
  try{
    const r = await fetch('/api/stk/menu',{cache:'no-store'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    const j = await r.json();
    if(!j.count){
      title.textContent = '';
      ul.innerHTML = '<li style="padding:12px;color:var(--muted);font-size:12px">暂无菜单 (SIM 卡暂未推送 +STKPRO: 0x25)</li>';
      status.textContent = j.cmd ? 'cmd=0x'+j.cmd.toString(16)+' (非 SETUP_MENU)' : '等待 SIM 推送…';
      return;
    }
    title.textContent = j.title || '(无标题)';
    status.textContent = '共 '+j.count+' 项';
    ul.innerHTML = j.items.map(it =>
      `<li style="padding:8px 0;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;gap:12px">
         <span style="flex:1"><b style="color:var(--info)">${it.id}.</b> ${escapeHtml(it.text)}</span>
         <span style="color:var(--muted);font-size:12px">id=${it.id}</span>
       </li>`
    ).join('');
  }catch(e){
    status.textContent='加载失败: '+e.message;
  }
}
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
    /* fetch 失败, 静默保留旧值, 不显 "加载失败" (SIM 信息不突变) */
  }
}
loadMenu();
loadSimInfo();          // v4.0.13: 首屏拉一次
setInterval(loadMenu, 10000);
setInterval(loadSimInfo, 5000);   // v4.0.13: 5s 跟 loadMenu 异步刷
</script>
<script>if(self!==top){['page-head','page-nav'].forEach(function(id){var h=document.getElementById(id); if(h)h.style.display='none';});}</script>
</body></html>
)HTML";
