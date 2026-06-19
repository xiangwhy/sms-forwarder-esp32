// v4.0.14: OTA_HTML 物理抽 (main.cpp:2477-2592), 0 改内容
static const char OTA_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>OTA · SMS Forwarder</title>
<style>
:root{--bg:#0f1419;--card:#1a2028;--card2:#232b35;--border:#2a3440;--text:#e6edf3;--muted:#8b95a5;--accent:#4ade80;--warn:#fbbf24;--err:#f87171;--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 12px rgba(0,0,0,.25)}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;font-size:14px;line-height:1.5}
.container{max-width:880px;margin:0 auto;padding:8px 16px 48px}  /* v4.0.11.21: padding-top 24→8 跟 .app-nav margin-bottom 16→8 配对, 总间距 40→16px (用户: 外层和内层之间的宽度调小一些) */
header{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:24px;gap:16px}
header h1{margin:0;font-size:22px;font-weight:600;letter-spacing:-0.01em}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px;box-shadow:var(--shadow);margin-bottom:16px}
.card h2{margin:0 0 14px;font-size:14px;font-weight:600;color:var(--text)}
.file-row{display:flex;align-items:center;gap:12px;padding:14px;background:var(--card2);border:1px dashed var(--border);border-radius:10px;margin-bottom:14px}
.file-row input[type=file]{flex:1;color:var(--text);font-size:13px;font-family:inherit}
.file-row input[type=file]::file-selector-button{background:var(--card2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:6px 12px;font-size:12px;cursor:pointer;font-family:inherit;margin-right:10px}
.file-row input[type=file]::file-selector-button:hover{background:#2c3744}
.actions{display:flex;gap:8px;align-items:center}
.btn{display:inline-flex;align-items:center;gap:6px;padding:10px 18px;border-radius:8px;font-size:13px;font-weight:500;text-decoration:none;color:var(--text);background:var(--card2);border:1px solid var(--border);cursor:pointer;font-family:inherit;transition:background .15s}
.btn:hover{background:#2c3744;border-color:#384454}
.btn.primary{background:var(--accent);color:#0a1014;border-color:transparent;font-weight:600}
.btn.primary:hover{background:#5be692}
.btn.primary:disabled{background:var(--muted);cursor:not-allowed}
.warn-text{color:var(--warn);font-size:12px;margin-top:10px}
.progress-wrap{margin-top:14px;background:var(--card2);border:1px solid var(--border);border-radius:8px;overflow:hidden;height:18px;position:relative}
.progress-bar{height:100%;background:var(--accent);width:0%;transition:width .2s}
.progress-text{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;font-size:11px;color:var(--text);font-family:ui-monospace,monospace}
.status-line{margin-top:14px;font-size:12px;color:var(--muted);min-height:16px}
.status-line.ok{color:var(--accent)}
.status-line.err{color:var(--err)}
footer{margin-top:24px;text-align:center;font-size:12px;color:var(--muted)}
</style></head><body>
<div class="container">
  <header id="page-head">
    <h1>OTA 升级</h1>
    <span style="font-size:12px;color:var(--muted)">选择 .bin 文件, &lt; 3MB</span>
  </header>

  <div class="card">
    <h2>上传固件</h2>
    <div class="file-row">
      <input type="file" id="fwFile" accept=".bin" required>
    </div>
    <div class="actions">
      <button class="btn primary" id="upBtn" onclick="upload()">⤴ 上传并烧录</button>
      <span class="status-line" id="upStatus" style="margin-top:0"></span>
    </div>
    <div class="progress-wrap" id="pwrap" style="display:none">
      <div class="progress-bar" id="pbar"></div>
      <div class="progress-text" id="ptext">0%</div>
    </div>
    <p class="warn-text">⚠ 烧录中请勿断电;成功后设备将自动重启,约 5s 后可用</p>
  </div>
</div>
<script>
async function upload() {
  const f = document.getElementById('fwFile').files[0];
  if (!f) { alert('请先选 .bin 文件'); return; }
  if (f.size > 3*1024*1024) { alert('文件太大 (>3MB)'); return; }
  const btn = document.getElementById('upBtn');
  const st  = document.getElementById('upStatus');
  const pw  = document.getElementById('pwrap');
  const pb  = document.getElementById('pbar');
  const pt  = document.getElementById('ptext');
  btn.disabled = true; btn.textContent = '上传中…';
  st.className = 'status-line'; st.textContent = '上传中…';
  pw.style.display = 'block'; pb.style.width = '0%'; pt.textContent = '0%';
  try {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/update', true);
    xhr.upload.onprogress = e => {
      if (e.lengthComputable) {
        const p = Math.round(e.loaded/e.total*100);
        pb.style.width = p + '%'; pt.textContent = p + '%';
      }
    };
    xhr.onload = () => {
      if (xhr.status === 200) {
        pb.style.width = '100%'; pt.textContent = '100%';
        st.className = 'status-line ok';
        st.textContent = '✓ 烧录成功,设备即将重启…';
        btn.textContent = '✓ 完成';
      } else {
        st.className = 'status-line err';
        st.textContent = '烧录失败: HTTP ' + xhr.status;
        btn.disabled = false; btn.textContent = '⤴ 上传并烧录';
      }
    };
    xhr.onerror = () => {
      st.className = 'status-line err';
      st.textContent = '网络错误 — 设备可能已重启,稍候刷新';
      btn.disabled = false; btn.textContent = '⤴ 上传并烧录';
    };
    const fd = new FormData();
    fd.append('firmware', f);
    xhr.send(fd);
  } catch (e) {
    st.className = 'status-line err';
    st.textContent = '失败: ' + e.message;
    btn.disabled = false; btn.textContent = '⤴ 上传并烧录';
  }
}
</script>
<script>if(self!==top){['page-head','page-nav'].forEach(function(id){var h=document.getElementById(id); if(h)h.style.display='none';});}</script>
</body></html>
)HTML";
