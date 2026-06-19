// v4.0.14: CONFIG_HTML 物理抽 (main.cpp:3354-4023), 0 改内容
static const char CONFIG_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>配置 · SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.container{max-width:880px;margin:0 auto;padding:8px 16px 48px}  /* v4.0.11.21: padding-top 24→8 跟 .app-nav margin-bottom 16→8 配对, 总间距 40→16px (用户: 外层和内层之间的宽度调小一些) */
header{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:24px;gap:16px}
header h1{margin:0;font-size:22px;font-weight:600;letter-spacing:-0.01em}
header a{color:var(--muted);text-decoration:none;font-size:13px;padding:6px 10px;border-radius:6px;border:1px solid var(--border)}
header a:hover{color:var(--text);background:var(--card2)}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px;box-shadow:var(--shadow);margin-bottom:16px}
.card h2{margin:0 0 14px;font-size:14px;font-weight:600;color:var(--text)}
.card.danger{border-color:rgba(248,113,113,.4)}
.card.danger h2{color:var(--err)}
.danger-row{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:14px;background:var(--card2);border:1px solid rgba(248,113,113,.3);border-radius:10px}
.danger-row .label{display:flex;flex-direction:column;gap:4px}
.danger-row .label .t{font-size:14px;font-weight:500;color:var(--text)}
.danger-row .label .h{font-size:12px;color:var(--muted);line-height:1.4}
.btn{display:inline-flex;align-items:center;gap:6px;padding:10px 18px;border-radius:8px;font-size:13px;font-weight:500;text-decoration:none;color:var(--text);background:var(--card2);border:1px solid var(--border);cursor:pointer;font-family:inherit;transition:background .15s}
.btn:hover{background:#2c3744;border-color:#384454}
.btn.danger{background:rgba(248,113,113,.12);color:var(--err);border-color:rgba(248,113,113,.3)}
.btn.danger:hover{background:rgba(248,113,113,.2);border-color:rgba(248,113,113,.5)}
.status-line{margin-top:14px;font-size:12px;color:var(--muted);min-height:16px}
.status-line.ok{color:var(--accent)}
.status-line.err{color:var(--err)}
footer{margin-top:24px;text-align:center;font-size:12px;color:var(--muted)}
</style></head><body>
<div class="container">
  <header id="page-head">
    <h1>配置</h1>
    <a href="/dashboard">← 返回状态页</a>
  </header>

  <div class="card">
    <h2>配置 (WiFi / 推送 / OTA)</h2>
    <div style="display:flex;gap:8px;margin-bottom:14px;align-items:center">
      <button class="btn mute" onclick="loadCfg()">加载</button>
      <button class="btn primary" onclick="saveCfg()">保存</button>
      <span id="c_dirty" style="font-size:12px;color:var(--warn);display:none">● 未保存</span>
    </div>

    <div style="display:grid;grid-template-columns:1fr 1fr;gap:12px 16px">
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">WiFi SSID <small id="c_curSsid" style="color:var(--accent)"></small></label>
        <div style="display:flex;gap:8px">
          <select id="c_ssidSel" style="flex:1;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px"></select>
          <button class="btn mute" type="button" onclick="scanWifi()" id="c_scanBtn" style="padding:6px 12px;font-size:12px">🔍 扫描</button>
        </div>
        <input id="c_ssid" maxlength="63" style="margin-top:6px;width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box" placeholder="或手动输入 SSID">
        <small id="c_scanHint" style="color:var(--muted);font-size:11px;min-height:14px;display:block;margin-top:2px"></small>
      </div>
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">WiFi 密码 <small id="c_hasPass"></small></label>
        <input id="c_pass" type="password" maxlength="63" style="width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box" placeholder="留空不修改">
      </div>
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">pushplus token</label>
        <input id="c_token" maxlength="63" style="width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box">
      </div>
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">pushplus topic <small style="color:var(--muted)">(空 = 个人推送)</small></label>
        <input id="c_topic" maxlength="63" style="width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box">
      </div>
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">OTA 用户名</label>
        <input id="c_otaUser" maxlength="31" style="width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box">
      </div>
      <div>
        <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px">OTA 密码 <small id="c_hasOtaPass"></small></label>
        <input id="c_otaPass" type="password" maxlength="31" style="width:100%;background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px;font-size:13px;box-sizing:border-box" placeholder="留空不修改">
      </div>
    </div>

    <div style="display:flex;align-items:center;justify-content:space-between;margin-top:14px;padding:12px;background:var(--card2);border:1px solid var(--border);border-radius:8px;gap:12px">
      <div>
        <div style="font-size:13px;font-weight:500">开机推送</div>
        <div style="font-size:11px;color:var(--muted);margin-top:2px">关闭后启动时不再推送通知;短信转发不受影响</div>
      </div>
      <label style="position:relative;display:inline-block;width:46px;height:26px;flex-shrink:0">
        <input type="checkbox" id="c_bp" onchange="toggleBp(this.checked)" style="opacity:0;width:0;height:0;position:absolute">
        <span style="position:absolute;inset:0;background:#3a4654;border-radius:999px;transition:background .2s;cursor:pointer" onclick="document.getElementById('c_bp').click()"></span>
        <span style="position:absolute;top:2px;left:2px;width:22px;height:22px;background:#f1f5f9;border-radius:50%;box-shadow:0 2px 4px rgba(0,0,0,.3);transition:transform .2s" id="c_bpKnob"></span>
      </label>
    </div>

    <div id="c_status" class="status-line"></div>
    <div style="margin-top:8px;font-size:12px;color:var(--muted)">保存后 WiFi / token 改动需重启生效</div>
  </div>

  <div class="card danger">
    <h2>危险操作</h2>
    <div class="danger-row">
      <div class="label">
        <span class="t">恢复出厂设置</span>
        <span class="h">擦除整个 NVS 分区 (WiFi / Token / OTA / 历史), 设备重启进入 AP 配网模式</span>
      </div>
      <button class="btn danger" onclick="doFactory()">⚠ 恢复出厂</button>
    </div>
    <div id="f_status" class="status-line"></div>
  </div>
</div>
<script>
async function doFactory() {
  if (!confirm('⚠ 真的要恢复出厂?\\n\\n将擦除:\\n  · WiFi 凭据\\n  · pushplus token\\n  · OTA 用户密码\\n  · 开机推送设置\\n  · 最近 32 条发送历史\\n\\n设备将重启并进入 AP 配网模式。\\n\\n此操作不可撤销,确认继续?')) return;
  if (!confirm('再次确认: 真的要恢复出厂?')) return;
  const s = document.getElementById('f_status');
  s.className = 'status-line'; s.textContent = '正在擦除 NVS, 设备约 3s 后重启…';
  try {
    const r = await fetch('/api/factory', {method:'POST'});
    if (!r.ok) throw new Error('HTTP '+r.status);
    s.className = 'status-line ok';
    s.textContent = '已发送重启指令, 设备将进入 AP 模式 (热点 SMS-Forwarder-XXXXXX)';
  } catch (e) {
    s.className = 'status-line err';
    s.textContent = '失败: ' + e.message + ' — 设备可能已重启, 刷新试试';
  }
}
// v4.0.6 P24: 配网表单从 dashboard 搬来 (/config 页)
async function loadCfg() {
  const s = document.getElementById('c_status');
  s.className = 'status-line'; s.textContent = '加载中…';
  try {
    const r = await fetch('/api/cfg');
    if (!r.ok) throw new Error('HTTP '+r.status);
    const j = await r.json();
    document.getElementById('c_ssid').value    = j.ssid || '';
    document.getElementById('c_ssidSel').innerHTML = '<option value="">— 选择扫描到的网络 —</option>';
    document.getElementById('c_token').value   = j.token || '';
    document.getElementById('c_topic').value   = j.topic || '';
    document.getElementById('c_otaUser').value = j.otaUser || '';
    document.getElementById('c_hasPass').textContent    = j.hasPass    ? '(已设置)' : '(未设置)';
    document.getElementById('c_hasOtaPass').textContent = j.hasOtaPass ? '(已设置)' : '(未设置)';
    document.getElementById('c_pass').value    = '';
    document.getElementById('c_otaPass').value = '';
    document.getElementById('c_bp').checked    = !!j.bootPush;
    updateBpKnob();
    if (j.ssid) document.getElementById('c_curSsid').textContent = '当前: ' + j.ssid;
    s.className = 'status-line ok'; s.textContent = '已加载';
  } catch (e) {
    s.className = 'status-line err'; s.textContent = '加载失败: ' + e.message;
  }
  markClean();
}
// v4.0.7 P2: dirty 检测 — 表单变化显示"未保存"
function markDirty(){
  const d = document.getElementById('c_dirty');
  if (d) d.style.display = 'inline';
}
function markClean(){
  const d = document.getElementById('c_dirty');
  if (d) d.style.display = 'none';
}
['c_ssid','c_pass','c_token','c_topic','c_otaUser','c_otaPass'].forEach(id=>{
  const el = document.getElementById(id);
  if (el) el.addEventListener('input', markDirty);
});
document.getElementById('c_bp').addEventListener('change', markDirty);
// v4.0.7 P2: status-line 变 err 时自动滚到视口
new MutationObserver(records => {
  records.forEach(r => {
    const el = r.target;
    if (el.classList && el.classList.contains('err')) {
      el.scrollIntoView({behavior:'smooth', block:'center'});
    }
  });
}).observe(document.body, {subtree:true, attributes:true, attributeFilter:['class']});
async function saveCfg() {
  const s = document.getElementById('c_status');
  const body = {
    ssid:    document.getElementById('c_ssid').value.trim(),
    token:   document.getElementById('c_token').value.trim(),
    topic:   document.getElementById('c_topic').value.trim(),
    otaUser: document.getElementById('c_otaUser').value.trim(),
    bootPush:document.getElementById('c_bp').checked,
  };
  const pw  = document.getElementById('c_pass').value;
  const opw = document.getElementById('c_otaPass').value;
  if (pw)  body.pass    = pw;
  if (opw) body.otaPass = opw;
  s.className = 'status-line'; s.textContent = '保存中…';
  try {
    const r = await fetch('/api/cfg', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify(body)
    });
    const j = await r.json();
    if (!r.ok || !j.ok) throw new Error('HTTP '+r.status);
    s.className = 'status-line ok';
    s.textContent = '已保存' + (j.restart ? ' · WiFi / token 改动需重启' : '');
    document.getElementById('c_pass').value    = '';
    document.getElementById('c_otaPass').value = '';
    markClean();
  } catch (e) {
    s.className = 'status-line err'; s.textContent = '保存失败: ' + e.message;
  }
}
async function scanWifi() {
  const btn = document.getElementById('c_scanBtn');
  const hint = document.getElementById('c_scanHint');
  const sel = document.getElementById('c_ssidSel');
  btn.disabled = true; btn.textContent = '…扫描中';
  hint.style.color = 'var(--muted)';
  hint.textContent = '扫描中 (~3s, 阻塞)';
  try {
    const r = await fetch('/api/scan');
    if (!r.ok) throw new Error('HTTP '+r.status);
    const arr = await r.json();
    const cur = document.getElementById('c_ssid').value;
    sel.innerHTML = '<option value="">— 选择扫描到的网络 —</option>'
      + arr.map(n => {
        const tag = n.current ? ' ✓ 已连' : (n.secured ? ' 🔒' : ' 开放');
        return '<option value="' + n.ssid.replace(/"/g,'&quot;') + '"' + (n.ssid===cur?' selected':'') + '>'
          + n.ssid + ' · ' + n.rssi + ' dBm' + tag + '</option>';
      }).join('');
    sel.onchange = () => { if (sel.value) document.getElementById('c_ssid').value = sel.value; };
    hint.style.color = 'var(--accent)';
    hint.textContent = '找到 ' + arr.length + ' 个网络 (按信号强度排序)';
  } catch (e) {
    hint.style.color = 'var(--err)';
    hint.textContent = '扫描失败: ' + e.message;
  } finally {
    btn.disabled = false; btn.textContent = '🔍 扫描';
  }
}
async function toggleBp(on) {
  updateBpKnob();
  try {
    const r = await fetch('/api/bootPush', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({on: on})
    });
    if (!r.ok) { alert('保存失败'); loadCfg(); return; }
  } catch (e) {
    alert('网络错误: ' + e.message); loadCfg();
  }
}
function updateBpKnob() {
  const on = document.getElementById('c_bp').checked;
  const track = document.querySelector('#c_bp').parentElement.children[1];
  const knob  = document.querySelector('#c_bp').parentElement.children[2];
  track.style.background = on ? 'var(--accent)' : '#3a4654';
  knob.style.transform   = on ? 'translateX(20px)' : 'none';
}
loadCfg();
</script><script>if(self!==top){['page-head','page-nav'].forEach(function(id){var h=document.getElementById(id); if(h)h.style.display='none';});}</script></body></html>
)HTML";
