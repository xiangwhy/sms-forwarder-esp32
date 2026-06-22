// v4.0.14: APP_PAGE_HTML 物理抽 (main.cpp:2593-2771), 0 改内容
static const char APP_PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--info:#60a5fa;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.app-shell{display:flex;flex-direction:column;height:100vh}
.app-header{display:flex;align-items:center;justify-content:space-between;padding:14px 16px;background:transparent;gap:16px;flex-shrink:0;max-width:880px;width:100%;margin:0 auto;box-sizing:border-box}  /* v4.0.11.21: padding 24→16, 跟 iframe .container/.card 左右 padding 一致 (用户: 外层和内层宽度统一) */
.app-header h1{margin:0;font-size:18px;font-weight:600;letter-spacing:-0.01em;display:flex;align-items:center;gap:10px}
.app-header h1 .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--muted);transition:background .3s}
.app-header h1 .dot.ok{background:var(--accent);box-shadow:0 0 8px rgba(74,222,128,.5)}
.app-header h1 .dot.ml{background:var(--info);box-shadow:0 0 8px rgba(96,165,250,.5)}
.app-header h1 .dot.warn{background:var(--warn);box-shadow:0 0 8px rgba(251,191,36,.5)}
.app-header h1 .dot.bad{background:var(--err)}
.app-header .fw-tag{font-size:11px;color:var(--muted);border:1px solid var(--border);padding:3px 8px;border-radius:4px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
.app-header .btn-restart{font-size:12px;color:var(--muted);text-decoration:none;padding:4px 10px;border:1px solid var(--border);border-radius:6px;transition:all .15s;cursor:pointer;font-family:inherit;background:var(--card2)}
.app-header .btn-restart:hover{color:var(--err);border-color:rgba(248,113,113,.4);background:rgba(248,113,113,.1)}
.app-nav{display:flex;gap:8px;flex-wrap:wrap;padding:12px 16px;background:var(--card);border:1px solid var(--border);border-radius:12px;flex-shrink:0;max-width:880px;width:100%;margin:0 auto 8px;box-sizing:border-box;position:relative}  /* v4.0.11.21: margin-bottom 16→8 + padding 24→16, 跟 iframe 内 .container/.card 视觉对齐, 外层到内层间距 40→16px */
.app-nav .burger{display:none;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:8px;padding:9px 12px;font-size:18px;cursor:pointer;font-family:inherit;line-height:1}
.app-nav .btn{display:inline-flex;align-items:center;gap:6px;padding:9px 14px;border-radius:8px;font-size:13px;font-weight:500;text-decoration:none;color:var(--text);background:var(--card2);border:1px solid var(--border);transition:background .15s;cursor:pointer;font-family:inherit}
.app-nav .btn:hover{background:#2c3744;border-color:#384454}
.app-nav .btn.primary{background:var(--accent);color:#0a1014;border-color:transparent}
.app-nav .btn.primary:hover{background:#5be692}
.app-nav .btn.warn{background:rgba(248,113,113,.12);color:var(--err);border-color:rgba(248,113,113,.25)}
.app-content{flex:1;overflow:auto;background:var(--bg)}
.app-content iframe{width:100%;height:100%;border:0;display:block;background:var(--bg)}
@media (max-width: 640px){
  .app-nav{padding:8px 16px;margin:0 16px 8px}
  .app-nav .burger{display:inline-flex}
  .app-nav .menu{display:none;flex-direction:column;width:100%;gap:6px;margin-top:8px}
  .app-nav.open .menu{display:flex}
  .app-nav .btn{width:100%;justify-content:flex-start}
}
</style></head><body>
<div class="app-shell">
  <div class="app-header">
    <h1><span id="status-dot" class="dot"></span>SMS Forwarder</h1>
    <div style="display:flex;align-items:center;gap:10px">
      <span class="fw-tag">FW v4.0.23</span>
      <a href="javascript:doRestart()" class="btn-restart" title="重启设备">↻ 重启</a>
    </div>
  </div>
  <nav class="app-nav" id="app-nav">
    <button class="burger" id="navBurger" onclick="document.getElementById('app-nav').classList.toggle('open')">☰</button>
    <div class="menu" id="navMenu">
      <a href="#" class="btn" data-page="dashboard">Dashboard</a>
      <a href="#" class="btn" data-page="send">发送</a>
      <a href="#" class="btn" data-page="stk">STK 控制台</a>
      <a href="#" class="btn" data-page="config">配置</a>
      <a href="#" class="btn" data-page="update">OTA</a>
    </div>
  </nav>
  <div class="app-content">
    <iframe id="page" src=""></iframe>
  </div>
</div>
<script>
const PAGES = {dashboard:'/dashboard', send:'/send', stk:'/stk', config:'/config', update:'/update'};
function setActive(p){
  document.querySelectorAll('#app-nav .btn').forEach(a=>{
    a.classList.toggle('primary', a.dataset.page === p);
  });
  // 选完自动收菜单 (移动端)
  document.getElementById('app-nav').classList.remove('open');
}
function load(p){
  if (!PAGES[p]) return;
  setActive(p);
  document.getElementById('page').src = PAGES[p];
  try { history.replaceState(null,'','/app?p='+p); } catch(e){}
}
document.querySelectorAll('#app-nav .btn').forEach(a=>{
  a.addEventListener('click', e=>{ e.preventDefault(); load(a.dataset.page); });
});
// 启动时: ?p=xxx → load, 默认 dashboard
let init = 'dashboard';
try {
  const qs = new URLSearchParams(location.search);
  if (qs.get('p') && PAGES[qs.get('p')]) init = qs.get('p');
} catch(e){}
load(init);

// 状态点: 拉 /api/status 决定 dot 颜色 (跟 dashboard 同步, 不影响子页自己 polling)
// v4.0.7 P1: SPA 父页 fetch /api/status 一次 → 挂到 window.__spaStatus, iframe 子页共用
async function pingStatus(){
  try{
    const r = await fetch('/api/status',{cache:'no-store'});
    if(!r.ok) return;
    const j = await r.json();
    window.__spaStatus = j;       // 暴露给 iframe 子页读
    window.__spaStatusAt = Date.now();
    const d = document.getElementById('status-dot');
    let cls = 'bad';
    if (j.wifi && j.mlAlive) cls = 'ok';          // 全绿
    else if (j.mlAlive)      cls = 'ml';          // 4G 有但 WiFi 没 → 蓝 (AP 模式)
    else if (j.wifi)         cls = 'warn';        // WiFi 有但 4G 没 → 橙
    d.className = 'dot ' + cls;
    d.title = (j.wifi?'WiFi 已连':(j.mlAlive?'AP 模式 (无 WiFi)':'离线'))
            + (j.mlAlive?' · 4G 在线':' · 4G 离线');
  }catch(e){}
}
pingStatus();
setInterval(pingStatus, 5000);
// 子页 fetch 替换函数: 同源同 frame 优先用 SPA 共享状态 (1 次 fetch 服务 5 个 iframe)
async function spaFetch(path, opts){
  // SPA 父页直接 fetch
  const r = await fetch(path, opts);
  return r;
}
// v4.0.7+: 重启按钮 (header 右上, 不在 nav 里因为是动作不是切换)
function doRestart(){
  if (!confirm('确认重启设备?\n\n设备会断网约 5s')) return;
  fetch('/restart').catch(()=>{});
  document.body.style.opacity='0.4';
  document.body.style.pointerEvents='none';
  setTimeout(()=>{ alert('重启中… 5s 后请刷新 http://172.16.1.18/'); location.reload(); }, 1500);
}
</script>
</body></html>
)HTML";
