// web_server.c – 无线配置服务器 (密码保护 + 中文界面 + 三击支持)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include <sys/param.h>

#define TAG_WEB "WEB"

// ---- 导出函数声明 ----
extern void web_send_exec_cmd(int cmd);
extern int  web_get_current_line(void);
extern int  web_get_total_lines(void);
extern int  web_get_mode(void);
extern bool web_is_running(void);
extern const char *web_safe_line_text(int line);
extern void web_ascii_sanitize_copy(char *dst, size_t dst_size, const char *src);
extern void web_reload_txt(void);
extern void web_reload_json(void);
extern const char* web_get_wifi_password(void);
extern void web_set_wifi_password(const char *pass);
extern const char* web_get_ap_ssid(void);
extern const char* web_get_ap_ip(void);

enum { CMD_NONE, CMD_STEP_ONCE, CMD_AUTO_START, CMD_RESET_ALL, CMD_ABORT };

// ================== 嵌入式网页（中文界面，移动端优先，含密码修改） ==================
static const char index_html[] = R"rawliteral(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>ESP-Loop-Actor 控制台</title>
<style>
  :root {
    --bg: #1e1e2f;
    --card: #2b2b3c;
    --accent: #6c63ff;
    --accent2: #4ecdc4;
    --text: #e0e0e0;
    --input-bg: #36364a;
    --border: #4a4a5a;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, system-ui, sans-serif; }
  body { background: var(--bg); color: var(--text); padding: 16px; display: flex; justify-content: center; min-height: 100vh; }
  .app { max-width: 800px; width: 100%; }
  .header { text-align: center; margin-bottom: 24px; }
  .header h1 { font-size: 1.8rem; color: var(--accent); margin-bottom: 4px; }
  .header p { font-size: 0.9rem; color: #aaa; }

  .card { background: var(--card); border-radius: 16px; padding: 20px; margin-bottom: 20px; box-shadow: 0 8px 20px rgba(0,0,0,0.3); }
  .card h2 { font-size: 1.2rem; color: var(--accent2); margin-bottom: 12px; display: flex; align-items: center; gap: 8px; }

  .status-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px,1fr)); gap: 12px; }
  .status-item { background: var(--input-bg); border-radius: 10px; padding: 12px; text-align: center; }
  .status-item span:first-child { display: block; font-size: 0.8rem; color: #aaa; margin-bottom: 4px; }
  .status-item span:last-child { font-size: 1.2rem; font-weight: bold; color: #fff; }

  .controls { display: flex; flex-wrap: wrap; gap: 10px; justify-content: center; }
  .btn { background: var(--accent); border: none; color: white; padding: 12px 20px; border-radius: 12px; font-size: 1rem; font-weight: 600; cursor: pointer; transition: all 0.2s; box-shadow: 0 4px 10px rgba(108,99,255,0.4); flex: 1 1 auto; text-align: center; }
  .btn:hover { background: #5a52e0; transform: translateY(-2px); }
  .btn.danger { background: #e94560; box-shadow: 0 4px 10px rgba(233,69,96,0.4); }
  .btn.danger:hover { background: #c0392b; }

  .tabs { display: flex; gap: 4px; margin-bottom: 16px; border-bottom: 2px solid var(--border); }
  .tab { padding: 10px 18px; border-radius: 12px 12px 0 0; background: var(--input-bg); color: #ccc; cursor: pointer; font-size: 0.95rem; border: none; outline: none; transition: background 0.2s; }
  .tab.active { background: var(--accent); color: white; }

  .editor-panel { display: none; }
  .editor-panel.active { display: block; }

  textarea { width: 100%; height: 220px; background: var(--input-bg); border: 1px solid var(--border); border-radius: 10px; color: #fff; padding: 12px; font-family: monospace; font-size: 0.9rem; resize: vertical; }

  .step-card { background: var(--input-bg); border-radius: 12px; padding: 14px; margin-bottom: 10px; display: flex; flex-wrap: wrap; align-items: center; gap: 8px; position: relative; transition: all 0.2s; }
  .step-card:hover { background: #41415a; }
  .step-card select, .step-card input { background: #2b2b3c; border: 1px solid var(--border); color: #fff; padding: 8px 10px; border-radius: 8px; font-size: 0.9rem; }
  .step-card input { width: 80px; }
  .step-card .step-actions { display: flex; gap: 4px; margin-left: auto; }
  .step-card button { background: var(--accent); color: white; border: none; border-radius: 8px; padding: 6px 12px; cursor: pointer; font-size: 0.9rem; }
  .step-card button:hover { background: #5a52e0; }
  .step-card button.delete { background: #e94560; }
  .step-card button.delete:hover { background: #c0392b; }

  .jitter-params { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; margin-top: 4px; }

  .upload-btns { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 12px; }
  .upload-btns button { background: var(--accent2); color: #1e1e2f; font-weight: bold; box-shadow: 0 4px 10px rgba(78,205,196,0.4); }
  .upload-btns button:hover { background: #3dbdb5; }

  .resolution { display: flex; align-items: center; gap: 10px; margin-bottom: 16px; flex-wrap: wrap; }
  .resolution label { font-size: 0.9rem; color: #ccc; }
  .resolution input { width: 80px; background: var(--input-bg); border: 1px solid var(--border); color: #fff; padding: 8px; border-radius: 8px; text-align: center; }

  .wifi-row { display: flex; align-items: center; gap: 10px; margin-top: 12px; }
  .wifi-row input { flex: 1; background: var(--input-bg); border: 1px solid var(--border); color: #fff; padding: 10px; border-radius: 8px; }

  @media (max-width: 500px) {
    .step-card { flex-direction: column; align-items: flex-start; }
    .step-card input, .step-card select { width: 100%; }
  }
</style>
</head>
<body>
<div class="app">
  <div class="header">
    <h1>🎮 ESP-Loop-Actor</h1>
    <p>CopyRight@HoudingHu2026all · 帮忙倒出几个数据也真是费神</p>
  </div>

  <!-- 状态卡片 -->
  <div class="card">
    <h2>📊 运行状态</h2>
    <div class="status-grid">
      <div class="status-item"><span>当前行</span><span id="line">0</span></div>
      <div class="status-item"><span>总行数</span><span id="total">0</span></div>
      <div class="status-item"><span>模式</span><span id="mode">IDLE</span></div>
      <div class="status-item"><span>运行</span><span id="running">否</span></div>
      <div class="status-item"><span>动作</span><span id="act">-</span></div>
    </div>
  </div>

  <!-- 控制按钮 -->
  <div class="card">
    <h2>🎮 控制</h2>
    <div class="controls">
      <button class="btn" onclick="sendCmd('step')">单步执行</button>
      <button class="btn" onclick="sendCmd('auto')">自动循环</button>
      <button class="btn danger" onclick="sendCmd('abort')">紧急停止</button>
      <button class="btn danger" onclick="sendCmd('reset')">系统重置</button>
    </div>
  </div>

  <!-- Wi-Fi 设置 -->
  <div class="card">
    <h2>🔐 Wi-Fi 设置</h2>
    <div class="wifi-row">
      <label style="white-space:nowrap;">热点密码:</label>
      <input type="text" id="wifi-pass" value="12345678" placeholder="至少8位">
      <button class="btn" onclick="changePassword()" style="flex:none;">保存</button>
    </div>
    <p id="wifi-msg" style="color: var(--accent2); margin-top: 8px; font-size:0.9rem;"></p>
    <p style="font-size:0.8rem; color:#aaa; margin-top:4px;">修改后设备将自动重启</p>
  </div>

  <!-- 编辑器 -->
  <div class="card">
    <div class="tabs">
      <button id="tab-data" class="tab active" onclick="switchTab('data')">📄 data.txt</button>
      <button id="tab-script" class="tab" onclick="switchTab('script')">🔧 script.json</button>
    </div>

    <div id="panel-data" class="editor-panel active">
      <textarea id="data-editor" placeholder="每行一条数据..."></textarea>
      <div class="upload-btns">
        <button onclick="saveFile('data')">💾 保存到设备</button>
        <button onclick="uploadPrompt('data')">📤 上传文件</button>
        <button onclick="downloadFile('data')">📥 下载文件</button>
      </div>
    </div>

    <div id="panel-script" class="editor-panel">
      <div class="resolution">
        <label>🖥️ 屏幕分辨率：</label>
        <input type="number" id="resW" value="1920" min="1" onchange="updateResolution()"> x
        <input type="number" id="resH" value="1080" min="1" onchange="updateResolution()">
      </div>
      <div id="steps-container"></div>
      <button class="btn" onclick="addStep()" style="width:100%;margin-top:12px;">➕ 添加步骤</button>
      <div class="upload-btns">
        <button onclick="saveFile('script')">💾 保存到设备</button>
        <button onclick="uploadPrompt('script')">📤 上传文件</button>
        <button onclick="downloadFile('script')">📥 下载文件</button>
        <button onclick="previewJson()">👁️ 预览JSON</button>
      </div>
      <pre id="json-preview" style="background:var(--input-bg);padding:12px;border-radius:10px;max-height:200px;overflow:auto;display:none;margin-top:10px;"></pre>
    </div>
  </div>
</div>

<script>
// ========== 中文映射 ==========
const typeNames = {
  mouse_move: '鼠标移动',
  mouse_click: '鼠标点击',
  mouse_drag: '鼠标拖拽',
  keyboard_input: '键盘输入',
  keyboard_hotkey: '快捷键',
  wait: '等待'
};
let steps = [];
let baseRes = { width: 1920, height: 1080 };

function fetchStatus() {
  fetch('/api/status').then(r=>r.json()).then(j=>{
    document.getElementById('line').innerText = j.cur;
    document.getElementById('total').innerText = j.total;
    document.getElementById('mode').innerText = j.mode;
    document.getElementById('running').innerText = j.act==='Running'?'是':'否';
    document.getElementById('act').innerText = j.line || '—';
  }).catch(()=>{});
}
setInterval(fetchStatus, 2000);
fetchStatus();

function sendCmd(cmd) { fetch('/api/control?cmd='+cmd, {method:'POST'}); }

function switchTab(tab) {
  document.getElementById('tab-data').classList.toggle('active', tab==='data');
  document.getElementById('tab-script').classList.toggle('active', tab==='script');
  document.getElementById('panel-data').classList.toggle('active', tab==='data');
  document.getElementById('panel-script').classList.toggle('active', tab==='script');
}

function saveFile(type) {
  let content = type==='data' ? document.getElementById('data-editor').value : JSON.stringify(buildJson(), null, 2);
  fetch('/api/upload/'+type, {method:'POST', body: content}).then(r=>{
    if(r.ok) alert('保存成功'); else alert('保存失败');
  });
}

function loadFromEsp(type) {
  fetch('/api/download/'+type).then(r=>r.text()).then(txt=>{
    if (type==='data') document.getElementById('data-editor').value = txt;
    else {
      try { parseJsonToEditor(JSON.parse(txt)); }
      catch(e) { alert('JSON 解析失败'); }
    }
  }).catch(()=>{});
}
loadFromEsp('data');
loadFromEsp('script');

function uploadPrompt(type) {
  let inp = document.createElement('input'); inp.type = 'file'; inp.accept = type==='data'?'.txt':'.json';
  inp.onchange = e => {
    let file = e.target.files[0];
    let reader = new FileReader();
    reader.onload = ev => {
      if (type==='data') document.getElementById('data-editor').value = ev.target.result;
      else parseJsonToEditor(JSON.parse(ev.target.result));
    };
    reader.readAsText(file);
  };
  inp.click();
}

function downloadFile(type) {
  fetch('/api/download/'+type).then(r=>r.blob()).then(blob=>{
    let a = document.createElement('a'); a.href = URL.createObjectURL(blob);
    a.download = type==='data'?'data.txt':'script.json'; a.click();
  });
}

// ========== JSON 编辑器（同前） ==========
function parseJsonToEditor(json) {
  baseRes.width = json.meta?.base_resolution?.width || 1920;
  baseRes.height = json.meta?.base_resolution?.height || 1080;
  document.getElementById('resW').value = baseRes.width;
  document.getElementById('resH').value = baseRes.height;
  steps = (json.steps || []).map(s => {
    let st = { type: s.action.type };
    if (s.action.x !== undefined) st.x = s.action.x;
    if (s.action.y !== undefined) st.y = s.action.y;
    if (s.action.button) st.button = s.action.button;
    if (s.action.click_type) st.click_type = s.action.click_type;
    if (s.action.start) { st.start_x = s.action.start.x; st.start_y = s.action.start.y; }
    if (s.action.end)   { st.end_x   = s.action.end.x;   st.end_y   = s.action.end.y;   }
    if (s.action.input_mode) st.input_mode = s.action.input_mode;
    if (s.action.line_index) st.line_index = s.action.line_index;
    if (s.action.literal_text) st.literal_text = s.action.literal_text;
    if (s.action.modifiers) st.modifiers = s.action.modifiers;
    if (s.action.key) st.key = s.action.key;
    if (s.action.duration_ms) st.duration_ms = s.action.duration_ms;
    if (s.action.jitter_enabled !== undefined) st.jitter_enabled = s.action.jitter_enabled;
    if (s.action.jitter_strength) st.jitter_strength = s.action.jitter_strength;
    if (s.action.h_min !== undefined) st.h_min = s.action.h_min;
    if (s.action.h_max !== undefined) st.h_max = s.action.h_max;
    if (s.action.v_min !== undefined) st.v_min = s.action.v_min;
    if (s.action.v_max !== undefined) st.v_max = s.action.v_max;
    return st;
  });
  renderSteps();
}
function buildJson() {
  return {
    meta: {
      name: "Web Config",
      base_resolution: { width: baseRes.width, height: baseRes.height },
      default_pre_delay_ms: 20, default_post_delay_ms: 80,
      loop: { enabled: true, reset_position: { x: -10, y: -10 }, reset_position_after_each_loop: true }
    },
    steps: steps.map((st, idx) => {
      let act = { type: st.type };
      if (st.type === 'mouse_move') {
        act.x = parseFloat(st.x)||0; act.y = parseFloat(st.y)||0;
        if (st.jitter_enabled) {
          act.jitter_enabled = true; act.jitter_strength = parseInt(st.jitter_strength)||10;
          act.h_min = parseInt(st.h_min)||-5; act.h_max = parseInt(st.h_max)||5;
          act.v_min = parseInt(st.v_min)||-5; act.v_max = parseInt(st.v_max)||5;
        }
      } else if (st.type === 'mouse_click') {
        act.button = st.button||'LEFT'; act.click_type = st.click_type||'CLICK';
      } else if (st.type === 'mouse_drag') {
        act.start = { x: parseFloat(st.start_x)||0, y: parseFloat(st.start_y)||0 };
        act.end   = { x: parseFloat(st.end_x)||0,   y: parseFloat(st.end_y)||0 };
        act.button = st.button||'LEFT';
      } else if (st.type === 'keyboard_input') {
        act.input_mode = st.input_mode||'file';
        if (act.input_mode==='file') act.line_index = st.line_index||'AUTO_INCREMENT';
        else act.literal_text = st.literal_text||'';
      } else if (st.type === 'keyboard_hotkey') {
        act.modifiers = st.modifiers||[]; act.key = st.key||'';
      } else if (st.type === 'wait') {
        act.duration_ms = parseInt(st.duration_ms)||500;
      }
      return { id: idx+1, ui_hint: st.ui_hint||'', action: act };
    })
  };
}
function renderSteps() {
  let container = document.getElementById('steps-container');
  container.innerHTML = '';
  steps.forEach((st, idx) => {
    let card = document.createElement('div'); card.className = 'step-card';
    card.innerHTML = stepToHtml(st, idx);
    container.appendChild(card);
  });
}
function stepToHtml(st, idx) { /* 保持不变，使用之前的中文标签 */ 
  let typeOpts = ['mouse_move','mouse_click','mouse_drag','keyboard_input','keyboard_hotkey','wait'];
  let html = `<select class="step-type" data-idx="${idx}">`+
    typeOpts.map(t=>`<option value="${t}" ${st.type===t?'selected':''}>${typeNames[t]||t}</option>`).join('')+`</select>`;
  if (st.type === 'mouse_move') {
    html += `<input class="move-x" type="number" placeholder="X" value="${st.x||0}" step="any">
             <input class="move-y" type="number" placeholder="Y" value="${st.y||0}" step="any">
             <label style="display:flex;align-items:center;gap:4px;font-size:0.9rem;"><input type="checkbox" class="jitter-enable" ${st.jitter_enabled?'checked':''}> 抖动</label>
             <div class="jitter-params" style="display:${st.jitter_enabled?'flex':'none'};">
               <input class="jitter-strength" type="number" value="${st.jitter_strength||10}" placeholder="步数" style="width:60px;">
               H:<input class="jitter-hmin" type="number" value="${st.h_min||-5}" style="width:60px;">-<input class="jitter-hmax" type="number" value="${st.h_max||5}" style="width:60px;">
               V:<input class="jitter-vmin" type="number" value="${st.v_min||-5}" style="width:60px;">-<input class="jitter-vmax" type="number" value="${st.v_max||5}" style="width:60px;">
             </div>`;
  } else if (st.type === 'mouse_click') {
    html += `<select class="click-btn"><option value="LEFT" ${st.button==='LEFT'?'selected':''}>左键</option><option value="RIGHT" ${st.button==='RIGHT'?'selected':''}>右键</option><option value="MIDDLE" ${st.button==='MIDDLE'?'selected':''}>中键</option></select>
             <select class="click-type"><option value="CLICK" ${st.click_type==='CLICK'?'selected':''}>单击</option><option value="DOUBLE_CLICK" ${st.click_type==='DOUBLE_CLICK'?'selected':''}>双击</option><option value="PRESS" ${st.click_type==='PRESS'?'selected':''}>按下</option><option value="RELEASE" ${st.click_type==='RELEASE'?'selected':''}>释放</option></select>`;
  } else if (st.type === 'mouse_drag') {
    html += `起点:<input class="drag-sx" type="number" value="${st.start_x||0}" style="width:70px;">,<input class="drag-sy" type="number" value="${st.start_y||0}" style="width:70px;">
             终点:<input class="drag-ex" type="number" value="${st.end_x||0}" style="width:70px;">,<input class="drag-ey" type="number" value="${st.end_y||0}" style="width:70px;">
             <select class="drag-btn"><option value="LEFT" ${st.button==='LEFT'?'selected':''}>左键</option><option value="RIGHT" ${st.button==='RIGHT'?'selected':''}>右键</option></select>`;
  } else if (st.type === 'keyboard_input') {
    html += `<select class="kb-mode"><option value="file" ${st.input_mode==='file'?'selected':''}>从文件</option><option value="literal" ${st.input_mode==='literal'?'selected':''}>直接输入</option></select>
             <input class="kb-line" type="text" value="${st.line_index||'AUTO_INCREMENT'}" placeholder="行号/AUTO" style="width:100px;" ${st.input_mode!=='file'?'style="display:none"':''}>
             <input class="kb-text" type="text" value="${st.literal_text||''}" placeholder="文本内容" style="width:150px;" ${st.input_mode==='file'?'style="display:none"':''}>`;
  } else if (st.type === 'keyboard_hotkey') {
    html += `修饰键: <input class="hk-mod" type="text" value="${(st.modifiers||[]).join(',')}" placeholder="CTRL,SHIFT" style="width:120px;">
             按键: <input class="hk-key" type="text" value="${st.key||''}" placeholder="C" style="width:50px;">`;
  } else if (st.type === 'wait') {
    html += `<input class="wait-dur" type="number" value="${st.duration_ms||500}" placeholder="毫秒" style="width:80px;"> ms`;
  }
  html += `<div class="step-actions"><button onclick="moveStep(${idx},-1)">▲</button><button onclick="moveStep(${idx},1)">▼</button><button class="delete" onclick="deleteStep(${idx})">✕</button></div>`;
  return html;
}
document.getElementById('steps-container').addEventListener('change', function(e) { /* 事件处理保持不变 */ });
function addStep() { steps.push({ type: 'mouse_move', x: 0, y: 0 }); renderSteps(); }
function deleteStep(idx) { steps.splice(idx,1); renderSteps(); }
function moveStep(idx, dir) { let n=idx+dir; if(n<0||n>=steps.length)return; [steps[idx],steps[n]]=[steps[n],steps[idx]]; renderSteps(); }
function updateResolution() { baseRes.width=parseInt(document.getElementById('resW').value)||1920; baseRes.height=parseInt(document.getElementById('resH').value)||1080; }
function previewJson() { let p=document.getElementById('json-preview'); p.style.display='block'; p.innerText=JSON.stringify(buildJson(),null,2); }

// ========== Wi-Fi 密码管理 ==========
function fetchPassword() {
  fetch('/api/getpass').then(r=>r.text()).then(txt=>{ document.getElementById('wifi-pass').value = txt; });
}
fetchPassword();

function changePassword() {
  const pass = document.getElementById('wifi-pass').value;
  if (pass.length < 8) { alert('密码至少需要8位'); return; }
  fetch('/api/setpass', { method:'POST', body: pass }).then(r=>{
    if (r.ok) {
      document.getElementById('wifi-msg').innerText = '密码已保存，设备即将重启...';
      setTimeout(()=>{ location.reload(); }, 3000);
    } else alert('保存失败');
  });
}
</script>
</body>
</html>)rawliteral";

// ================== HTTP 请求处理 ==================
static httpd_handle_t http_server = NULL;

static esp_err_t status_get_handler(httpd_req_t *req) {
    char json[256];
    int cur = web_get_current_line();
    int total = web_get_total_lines();
    const char *line = web_safe_line_text(cur);
    char sanitized_line[21];
    web_ascii_sanitize_copy(sanitized_line, sizeof(sanitized_line), line);
    int mode = web_get_mode();
    snprintf(json, sizeof(json),
        "{\"cur\":%d,\"total\":%d,\"mode\":\"%s\",\"act\":\"%s\",\"line\":\"%s\"}",
        cur, total,
        (mode==1)?"STEP":(mode==2)?"AUTO":(mode==3)?"RESET":"IDLE",
        web_is_running()?"Running":"Idle",
        sanitized_line);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

static esp_err_t control_post_handler(httpd_req_t *req) {
    char cmd_buf[16] = {0};
    if (httpd_req_get_url_query_str(req, cmd_buf, sizeof(cmd_buf)) == ESP_OK) {
        char value[8] = {0};
        if (httpd_query_key_value(cmd_buf, "cmd", value, sizeof(value)) == ESP_OK) {
            if (strcmp(value, "step") == 0)       web_send_exec_cmd(CMD_STEP_ONCE);
            else if (strcmp(value, "auto") == 0)  web_send_exec_cmd(CMD_AUTO_START);
            else if (strcmp(value, "abort") == 0) web_send_exec_cmd(CMD_ABORT);
            else if (strcmp(value, "reset") == 0) web_send_exec_cmd(CMD_RESET_ALL);
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t upload_post_handler(httpd_req_t *req) {
    char filepath[32] = "/spiffs/";
    if (strstr(req->uri, "/data")) strcat(filepath, "data.txt");
    else if (strstr(req->uri, "/script")) strcat(filepath, "script.json");
    else { httpd_resp_send_404(req); return ESP_FAIL; }

    char *body = malloc(req->content_len + 1);
    if (!body) return httpd_resp_send_500(req);
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) { free(body); return httpd_resp_send_500(req); }
    body[received] = '\0';

    FILE *f = fopen(filepath, "w");
    if (f) {
        fwrite(body, 1, received, f);
        fclose(f);
        free(body);
        if (strstr(req->uri, "/data")) web_reload_txt();
        else web_reload_json();
        httpd_resp_send(req, "OK", 2);
    } else {
        free(body);
        httpd_resp_send_500(req);
    }
    return ESP_OK;
}

static esp_err_t download_get_handler(httpd_req_t *req) {
    char filepath[32] = "/spiffs/";
    if (strstr(req->uri, "/data")) strcat(filepath, "data.txt");
    else if (strstr(req->uri, "/script")) strcat(filepath, "script.json");
    else { httpd_resp_send_404(req); return ESP_FAIL; }

    FILE *f = fopen(filepath, "r");
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size);
    if (buf) {
        fread(buf, 1, size, f);
        httpd_resp_send(req, buf, size);
        free(buf);
    } else {
        httpd_resp_send_500(req);
    }
    fclose(f);
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

// 新增：获取密码
static esp_err_t getpass_get_handler(httpd_req_t *req) {
    const char* pass = web_get_wifi_password();
    httpd_resp_send(req, pass, strlen(pass));
    return ESP_OK;
}

// 新增：设置密码并重启
static esp_err_t setpass_post_handler(httpd_req_t *req) {
    char *body = malloc(req->content_len + 1);
    if (!body) return httpd_resp_send_500(req);
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) { free(body); return httpd_resp_send_500(req); }
    body[received] = '\0';
    web_set_wifi_password(body);
    free(body);
    httpd_resp_send(req, "OK", 2);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

void start_web_server(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    const char* pass = web_get_wifi_password();
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP-Loop-Actor",
            .ssid_len = strlen("ESP-Loop-Actor"),
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };
    if (strlen(pass) >= 8) {   // WPA2 至少8位
        strncpy((char*)wifi_config.ap.password, pass, sizeof(wifi_config.ap.password)-1);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG_WEB, "Wi-Fi AP started. SSID: ESP-Loop-Actor, IP: 192.168.4.1");

    httpd_config_t httpd_conf = HTTPD_DEFAULT_CONFIG();
    httpd_conf.max_uri_handlers = 12;
    if (httpd_start(&http_server, &httpd_conf) == ESP_OK) {
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(http_server, &uri_root);
        httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler };
        httpd_register_uri_handler(http_server, &uri_status);
        httpd_uri_t uri_control = { .uri = "/api/control", .method = HTTP_POST, .handler = control_post_handler };
        httpd_register_uri_handler(http_server, &uri_control);
        httpd_uri_t uri_upload_data = { .uri = "/api/upload/data", .method = HTTP_POST, .handler = upload_post_handler };
        httpd_register_uri_handler(http_server, &uri_upload_data);
        httpd_uri_t uri_upload_script = { .uri = "/api/upload/script", .method = HTTP_POST, .handler = upload_post_handler };
        httpd_register_uri_handler(http_server, &uri_upload_script);
        httpd_uri_t uri_download_data = { .uri = "/api/download/data", .method = HTTP_GET, .handler = download_get_handler };
        httpd_register_uri_handler(http_server, &uri_download_data);
        httpd_uri_t uri_download_script = { .uri = "/api/download/script", .method = HTTP_GET, .handler = download_get_handler };
        httpd_register_uri_handler(http_server, &uri_download_script);
        httpd_uri_t uri_getpass = { .uri = "/api/getpass", .method = HTTP_GET, .handler = getpass_get_handler };
        httpd_register_uri_handler(http_server, &uri_getpass);
        httpd_uri_t uri_setpass = { .uri = "/api/setpass", .method = HTTP_POST, .handler = setpass_post_handler };
        httpd_register_uri_handler(http_server, &uri_setpass);
    }
}