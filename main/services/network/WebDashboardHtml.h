#pragma once

namespace Services {

static const char WEB_DASHBOARD_HTML[] = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Waveshare S3 Control Hub</title>
<style>
:root {
  --bg: #0b0f19;
  --panel: #151d30;
  --border: #24314d;
  --text: #f1f5f9;
  --text-muted: #8e9cb5;
  --primary: #38bdf8;
  --primary-hover: #0ea5e9;
  --danger: #f87171;
  --danger-hover: #ef4444;
  --success: #34d399;
  --warning: #fbbf24;
}
* { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
body { background: var(--bg); color: var(--text); min-height: 100vh; padding-bottom: 50px; }
.container { max-width: 1100px; margin: 0 auto; padding: 16px; }
header { display: flex; flex-wrap: wrap; justify-content: space-between; align-items: center; gap: 12px; padding: 14px 0 18px 0; border-bottom: 1px solid var(--border); margin-bottom: 18px; }
h1 { font-size: 1.4rem; font-weight: 700; color: #fff; display: flex; align-items: center; gap: 10px; }
.badge { display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: 9999px; font-size: 0.775rem; font-weight: 600; }
.badge-wifi { background: rgba(56,189,248,0.15); color: var(--primary); border: 1px solid rgba(56,189,248,0.3); }
.badge-rec { background: rgba(248,113,113,0.2); color: var(--danger); border: 1px solid var(--danger); animation: pulse 1.5s infinite; }
@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }

/* Navigation Tabs */
.tabs-nav { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 20px; border-bottom: 1px solid var(--border); padding-bottom: 10px; }
.tab-btn { background: transparent; color: var(--text-muted); border: 1px solid transparent; padding: 8px 16px; border-radius: 8px; font-size: 0.9rem; font-weight: 600; cursor: pointer; transition: 0.2s; display: flex; align-items: center; gap: 6px; }
.tab-btn:hover { color: var(--text); background: rgba(255,255,255,0.04); }
.tab-btn.active { color: #0b0f19; background: var(--primary); border-color: var(--primary); }

/* Common UI */
.tab-pane { display: none; }
.tab-pane.active { display: block; }
.card { background: var(--panel); border: 1px solid var(--border); border-radius: 12px; padding: 20px; margin-bottom: 20px; box-shadow: 0 4px 12px rgba(0,0,0,0.25); }
.card h2 { font-size: 1.1rem; margin-bottom: 14px; font-weight: 600; color: #fff; display: flex; align-items: center; gap: 8px; }
.grid-2 { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 16px; }
.grid-3 { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 16px; }
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; padding: 8px 16px; border-radius: 6px; font-size: 0.875rem; font-weight: 500; cursor: pointer; border: none; transition: 0.2s; text-decoration: none; color: #fff; }
.btn-primary { background: var(--primary); color: #0b0f19; }
.btn-primary:hover { background: var(--primary-hover); }
.btn-secondary { background: #24314d; }
.btn-secondary:hover { background: #33446b; }
.btn-danger { background: rgba(248,113,113,0.15); color: var(--danger); border: 1px solid rgba(248,113,113,0.3); }
.btn-danger:hover { background: var(--danger); color: #fff; }
.btn-success { background: rgba(52,211,153,0.2); color: var(--success); border: 1px solid var(--success); }
.btn-success:hover { background: var(--success); color: #0b0f19; }
.btn-sm { padding: 4px 8px; font-size: 0.775rem; }

/* Form elements */
.form-group { margin-bottom: 16px; }
.form-group label { display: flex; justify-content: space-between; font-size: 0.85rem; font-weight: 500; color: var(--text-muted); margin-bottom: 6px; }
input[type="range"] { width: 100%; height: 6px; border-radius: 3px; background: #24314d; outline: none; -webkit-appearance: none; }
input[type="range"]::-webkit-slider-thumb { -webkit-appearance: none; width: 18px; height: 18px; border-radius: 50%; background: var(--primary); cursor: pointer; }
select, textarea, input[type="text"] { width: 100%; background: #0b0f19; border: 1px solid var(--border); border-radius: 6px; color: var(--text); padding: 8px 12px; font-size: 0.9rem; outline: none; }
select:focus, textarea:focus, input[type="text"]:focus { border-color: var(--primary); }
textarea { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 0.825rem; line-height: 1.4; resize: vertical; }

/* Storage & Metrics progress */
.progress-bar-bg { height: 8px; background: #24314d; border-radius: 4px; overflow: hidden; margin-top: 6px; }
.progress-bar-fill { height: 100%; background: linear-gradient(90deg, #38bdf8, #818cf8); width: 0%; transition: width 0.3s ease; }

/* File Manager */
.breadcrumbs { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; font-size: 0.875rem; margin-bottom: 14px; padding: 8px 12px; background: #0b0f19; border-radius: 6px; border: 1px solid var(--border); }
.crumb { color: var(--primary); cursor: pointer; text-decoration: none; }
.crumb:hover { text-decoration: underline; }
.crumb-sep { color: var(--text-muted); }
.drop-zone { border: 2px dashed var(--border); border-radius: 8px; padding: 20px; text-align: center; color: var(--text-muted); cursor: pointer; transition: 0.2s; margin-bottom: 16px; }
.drop-zone.active { border-color: var(--primary); background: rgba(56,189,248,0.05); color: var(--primary); }
table { width: 100%; border-collapse: collapse; text-align: left; }
th, td { padding: 10px 12px; border-bottom: 1px solid var(--border); font-size: 0.85rem; }
th { color: var(--text-muted); font-weight: 600; font-size: 0.75rem; text-transform: uppercase; }
tr:hover { background: rgba(255,255,255,0.02); }
.item-link { color: var(--text); text-decoration: none; display: flex; align-items: center; gap: 8px; font-weight: 500; cursor: pointer; }
.item-link:hover { color: var(--primary); }
.actions-cell { display: flex; gap: 6px; justify-content: flex-end; }

/* Color presets */
.color-presets { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
.color-dot { width: 28px; height: 28px; border-radius: 50%; cursor: pointer; border: 2px solid #24314d; transition: transform 0.2s; }
.color-dot:hover { transform: scale(1.15); border-color: #fff; }

/* Terminal Console */
.terminal { background: #070a12; border: 1px solid var(--border); border-radius: 8px; padding: 12px; height: 420px; overflow-y: auto; font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 0.8rem; line-height: 1.45; white-space: pre-wrap; word-break: break-all; }
.log-line { margin-bottom: 2px; }
.log-info { color: #34d399; }
.log-warn { color: #fbbf24; }
.log-err { color: #f87171; }
.log-debug { color: #94a3b8; }

/* Toast */
.toast { position: fixed; bottom: 20px; right: 20px; padding: 10px 18px; border-radius: 8px; background: #151d30; border: 1px solid var(--border); color: #fff; box-shadow: 0 10px 15px -3px rgba(0,0,0,0.5); z-index: 999; opacity: 0; transform: translateY(10px); transition: 0.3s; pointer-events: none; }
.toast.show { opacity: 1; transform: translateY(0); }
.toast-success { border-left: 4px solid var(--success); }
.toast-error { border-left: 4px solid var(--danger); }

/* Audio Player Dock */
.player-dock { position: fixed; bottom: 0; left: 0; right: 0; background: #151d30; border-top: 1px solid var(--border); padding: 10px 16px; display: none; align-items: center; justify-content: space-between; z-index: 50; box-shadow: 0 -4px 12px rgba(0,0,0,0.3); }
.player-dock.active { display: flex; }
audio { outline: none; height: 36px; width: 340px; max-width: 60%; }
</style>
</head>
<body>
<div class="container">
  <header>
    <h1>🎛 Waveshare S3 Control Hub</h1>
    <div style="display:flex; align-items:center; gap:8px; flex-wrap:wrap;">
      <span id="header-cpu-badge" class="badge" style="background:rgba(129,140,248,0.15); color:#a5b4fc; border:1px solid rgba(129,140,248,0.3);">CPU: 0% / 0%</span>
      <span id="header-rec-badge" class="badge badge-rec" style="display:none;">● RECORDING</span>
      <span id="header-wifi-badge" class="badge badge-wifi">Wi-Fi: ...</span>
    </div>
  </header>

  <!-- Tabs Navigation -->
  <div class="tabs-nav">
    <button class="tab-btn active" onclick="switchTab('files')">📁 Files</button>
    <button class="tab-btn" onclick="switchTab('controls')">🎛 Controls</button>
    <button class="tab-btn" onclick="switchTab('telemetry')">📊 Telemetry</button>
    <button class="tab-btn" onclick="switchTab('config')">⚙ Config</button>
    <button class="tab-btn" onclick="switchTab('logs')">📜 Live Console</button>
    <button class="tab-btn" onclick="switchTab('ota')">🚀 Firmware (OTA)</button>
  </div>

  <!-- Tab 1: File Manager -->
  <div id="tab-files" class="tab-pane active">
    <div class="card">
      <div style="display:flex; justify-content:space-between; font-size:0.85rem; color:var(--text-muted); margin-bottom:6px;">
        <span id="storage-status">Checking Storage...</span>
        <span id="storage-details">-</span>
      </div>
      <div class="progress-bar-bg">
        <div id="storage-progress" class="progress-bar-fill"></div>
      </div>
    </div>

    <div class="card">
      <div style="display:flex; flex-wrap:wrap; gap:10px; margin-bottom:14px;">
        <button class="btn btn-secondary" id="btn-up" onclick="goUp()">⬆ Parent Folder</button>
        <button class="btn btn-secondary" onclick="promptMkdir()">📁 New Folder</button>
        <button class="btn btn-secondary" onclick="refreshCurrent()">🔄 Refresh</button>
        <button class="btn btn-primary" onclick="document.getElementById('file-input').click()">📤 Upload File</button>
        <input type="file" id="file-input" style="display: none;" onchange="handleFileSelected(this.files)">
      </div>

      <div class="breadcrumbs" id="breadcrumbs">
        <span class="crumb" onclick="navigateTo('/sdcard')">root</span>
      </div>

      <div class="drop-zone" id="drop-zone">
        <div>📁 Drag & drop files here to upload to current directory</div>
        <div id="upload-status" style="margin-top:6px; display:none; color:var(--primary);">Uploading...</div>
      </div>

      <div style="overflow-x:auto;">
        <table>
          <thead>
            <tr>
              <th>Name</th>
              <th>Type</th>
              <th>Size</th>
              <th>Modified</th>
              <th style="text-align:right;">Actions</th>
            </tr>
          </thead>
          <tbody id="file-table-body">
            <tr><td colspan="5" style="text-align:center; color:var(--text-muted);">Loading files...</td></tr>
          </tbody>
        </table>
      </div>
    </div>
  </div>

  <!-- Tab 2: Controls (Audio & LEDs) -->
  <div id="tab-controls" class="tab-pane">
    <div class="grid-2">
      <!-- Audio Controls -->
      <div class="card">
        <h2>🔊 Audio Controls</h2>
        <div class="form-group">
          <label><span>Speaker Volume</span><span id="val-volume">80%</span></label>
          <input type="range" id="slider-volume" min="0" max="100" value="80" oninput="onVolumeChange(this.value)">
        </div>
        <div class="form-group">
          <label><span>Mic Gain</span><span id="val-mic-gain">60.0 dB</span></label>
          <input type="range" id="slider-mic-gain" min="0" max="60" step="1" value="60" oninput="onMicGainChange(this.value)">
        </div>
        <div style="display:flex; flex-wrap:wrap; gap:10px; margin-top:16px;">
          <button class="btn btn-secondary" id="btn-mute" onclick="toggleMicMute()">🎙 Mic Enabled</button>
          <button class="btn btn-secondary" onclick="playTestAlert()">🔔 Test Chime</button>
        </div>

        <hr style="border:none; border-top:1px solid var(--border); margin:20px 0;">

        <h2>🎙 Audio Recorder</h2>
        <p style="font-size:0.85rem; color:var(--text-muted); margin-bottom:12px;">Capture mic array feed to SD card (`/sdcard/recordings`)</p>
        <div style="display:flex; align-items:center; gap:12px;">
          <button class="btn btn-danger" id="btn-record-toggle" onclick="toggleRecording()">🔴 Start Recording</button>
          <span id="rec-status-text" style="font-size:0.85rem; color:var(--text-muted);">Idle</span>
        </div>
      </div>

      <!-- LED Controls -->
      <div class="card">
        <h2>💡 RGB LED Strip</h2>
        <div class="form-group">
          <label>Mode</label>
          <select id="led-mode" onchange="applyLedSettings()">
            <option value="solid">Solid Color</option>
            <option value="breath">Breathing Effect</option>
            <option value="rainbow">Rainbow Wave</option>
            <option value="blink">Blink</option>
            <option value="off">Turn Off</option>
          </select>
        </div>
        <div class="form-group">
          <label>Custom Color Picker</label>
          <input type="color" id="led-color" value="#00ff00" onchange="applyLedSettings()" style="width:100%; height:40px; cursor:pointer; background:none; border:none;">
          <div class="color-presets">
            <div class="color-dot" style="background:#ff0000;" onclick="setPresetColor('#ff0000')"></div>
            <div class="color-dot" style="background:#00ff00;" onclick="setPresetColor('#00ff00')"></div>
            <div class="color-dot" style="background:#0088ff;" onclick="setPresetColor('#0088ff')"></div>
            <div class="color-dot" style="background:#ffff00;" onclick="setPresetColor('#ffff00')"></div>
            <div class="color-dot" style="background:#a855f7;" onclick="setPresetColor('#a855f7')"></div>
            <div class="color-dot" style="background:#f97316;" onclick="setPresetColor('#f97316')"></div>
            <div class="color-dot" style="background:#ec4899;" onclick="setPresetColor('#ec4899')"></div>
            <div class="color-dot" style="background:#ffffff;" onclick="setPresetColor('#ffffff')"></div>
          </div>
        </div>
        <div class="form-group">
          <label><span>Animation Speed</span><span id="val-led-speed">500 ms</span></label>
          <input type="range" id="led-speed" min="100" max="2000" step="50" value="500" oninput="document.getElementById('val-led-speed').innerText = this.value + ' ms'" onchange="applyLedSettings()">
        </div>
      </div>
    </div>
  </div>

  <!-- Tab 3: Telemetry & Metrics -->
  <div id="tab-telemetry" class="tab-pane">
    <div class="grid-2" style="margin-bottom:16px;">
      <div class="card">
        <h2>⚡ Core 0 (Network / System)</h2>
        <div style="font-size:1.4rem; font-weight:700;" id="metric-cpu0">0%</div>
        <div style="font-size:0.8rem; color:var(--text-muted);" id="metric-cpu0-detail">Wi-Fi, HTTP Server, MQTT, SysDb</div>
        <div class="progress-bar-bg"><div id="metric-cpu0-bar" class="progress-bar-fill" style="width:0%;"></div></div>
      </div>
      <div class="card">
        <h2>🎛 Core 1 (Audio DSP / Pipeline)</h2>
        <div style="font-size:1.4rem; font-weight:700;" id="metric-cpu1">0%</div>
        <div style="font-size:0.8rem; color:var(--text-muted);" id="metric-cpu1-detail">I2S DMA, DSP AFE, Wake Word, Synthesizer</div>
        <div class="progress-bar-bg"><div id="metric-cpu1-bar" class="progress-bar-fill" style="width:0%;"></div></div>
      </div>
    </div>

    <div class="grid-3">
      <div class="card">
        <h2>🧠 Internal SRAM</h2>
        <div style="font-size:1.4rem; font-weight:700;" id="metric-sram-free">-</div>
        <div style="font-size:0.8rem; color:var(--text-muted);" id="metric-sram-detail">-</div>
        <div class="progress-bar-bg"><div id="metric-sram-bar" class="progress-bar-fill"></div></div>
      </div>
      <div class="card">
        <h2>⚡ Octal PSRAM</h2>
        <div style="font-size:1.4rem; font-weight:700;" id="metric-psram-free">-</div>
        <div style="font-size:0.8rem; color:var(--text-muted);" id="metric-psram-detail">-</div>
        <div class="progress-bar-bg"><div id="metric-psram-bar" class="progress-bar-fill"></div></div>
      </div>
      <div class="card">
        <h2>📶 Wi-Fi Connection</h2>
        <div style="font-size:1.4rem; font-weight:700;" id="metric-wifi-rssi">-</div>
        <div style="font-size:0.8rem; color:var(--text-muted);" id="metric-wifi-detail">-</div>
      </div>
    </div>

    <div class="grid-2">
      <div class="card">
        <h2>⚙ System Overview</h2>
        <table style="font-size:0.85rem;">
          <tr><td style="color:var(--text-muted);">Uptime</td><td id="metric-uptime">-</td></tr>
          <tr><td style="color:var(--text-muted);">Board Model</td><td id="metric-board">ESP32-S3 (Waveshare)</td></tr>
          <tr><td style="color:var(--text-muted);">Firmware Version</td><td id="metric-version">-</td></tr>
          <tr><td style="color:var(--text-muted);">Compile Date/Time</td><td id="metric-compile-time">-</td></tr>
          <tr><td style="color:var(--text-muted);">Active Partition</td><td id="metric-running-part">-</td></tr>
          <tr><td style="color:var(--text-muted);">Boot Reset Reason</td><td id="metric-reset-reason">-</td></tr>
        </table>
      </div>
      <div class="card">
        <h2>🎙 Audio Pipeline Metrics</h2>
        <table style="font-size:0.85rem;">
          <tr><td style="color:var(--text-muted);">Sample Rate</td><td id="metric-sample-rate">32000 Hz</td></tr>
          <tr><td style="color:var(--text-muted);">Speaker Volume</td><td id="metric-speaker-vol">80%</td></tr>
          <tr><td style="color:var(--text-muted);">Microphone Gain</td><td id="metric-mic-gain">60.0 dB</td></tr>
          <tr><td style="color:var(--text-muted);">Active Recorder</td><td id="metric-recorder-state">Idle</td></tr>
        </table>
      </div>
    </div>
  </div>

  <!-- Tab 4: Config Editor -->
  <div id="tab-config" class="tab-pane">
    <div class="grid-2">
      <div class="card">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:12px;">
          <h2>📄 settings.txt</h2>
          <button class="btn btn-primary btn-sm" onclick="saveConfigSettings()">💾 Save</button>
        </div>
        <textarea id="editor-settings" rows="18" placeholder="Loading settings.txt..."></textarea>
      </div>
      <div class="card">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:12px;">
          <h2>🤖 gemini_config.json</h2>
          <button class="btn btn-primary btn-sm" onclick="saveConfigGemini()">💾 Save</button>
        </div>
        <textarea id="editor-gemini" rows="18" placeholder="Loading gemini_config.json..."></textarea>
      </div>
    </div>
  </div>

  <!-- Tab 5: Live Console -->
  <div id="tab-logs" class="tab-pane">
    <div class="card">
      <div style="display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:12px;">
        <h2>📜 Live Log Console</h2>
        <div style="display:flex; gap:10px; align-items:center;">
          <input type="text" id="log-filter" placeholder="Filter tags/text..." oninput="renderLogs()" style="width:160px; padding:4px 8px; font-size:0.8rem;">
          <label style="font-size:0.8rem; color:var(--text-muted); cursor:pointer;"><input type="checkbox" id="log-autoscroll" checked> Auto-Scroll</label>
          <button class="btn btn-secondary btn-sm" onclick="clearLogs()">Clear</button>
        </div>
      </div>
      <div class="terminal" id="log-terminal">Waiting for log stream...</div>
    </div>
  </div>

  <!-- Tab 6: OTA Web Flasher -->
  <div id="tab-ota" class="tab-pane">
    <div class="card" style="max-width:680px; margin:0 auto;">
      <h2>🚀 Over-The-Air (OTA) Firmware Flasher</h2>
      <p style="font-size:0.85rem; color:var(--text-muted); margin-bottom:16px;">Upload a compiled <code>waveshare.bin</code> firmware binary to update this device over Wi-Fi.</p>
      
      <div class="form-group">
        <div style="background:#0b0f19; border:1px solid var(--border); border-radius:8px; padding:12px; font-size:0.85rem; margin-bottom:16px;">
          <div><strong>Running Partition:</strong> <span id="ota-running-part">-</span></div>
          <div style="margin-top:4px;"><strong>Target OTA Partition:</strong> <span id="ota-target-part">-</span></div>
        </div>
      </div>

      <div class="drop-zone" id="ota-drop-zone">
        <div>📥 Click or drag & drop <code>waveshare.bin</code> here</div>
        <input type="file" id="ota-file-input" accept=".bin" style="display:none;" onchange="handleOtaSelected(this.files)">
      </div>

      <div id="ota-progress-card" style="display:none; margin-top:16px;">
        <div style="display:flex; justify-content:space-between; font-size:0.85rem; margin-bottom:6px;">
          <span id="ota-status-label">Flashing firmware...</span>
          <span id="ota-percent">0%</span>
        </div>
        <div class="progress-bar-bg">
          <div id="ota-progress-bar" class="progress-bar-fill" style="width:0%;"></div>
        </div>
      </div>

      <hr style="border:none; border-top:1px solid var(--border); margin:24px 0;">
      <div style="display:flex; justify-content:space-between; align-items:center;">
        <div>
          <div style="font-size:0.9rem; font-weight:600;">System Reboot</div>
          <div style="font-size:0.8rem; color:var(--text-muted);">Restart the ESP32-S3 immediately</div>
        </div>
        <button class="btn btn-danger" onclick="rebootDevice()">🔄 Reboot Device</button>
      </div>
    </div>
  </div>
</div>

<!-- Audio Player Dock -->
<div class="player-dock" id="player-dock">
  <div style="display:flex; align-items:center; gap:10px; overflow:hidden;">
    <span style="font-size:1.2rem;">🎵</span>
    <div style="overflow:hidden; text-overflow:ellipsis; white-space:nowrap; max-width:280px;">
      <div id="player-title" style="font-size:0.85rem; font-weight:600;">track.wav</div>
      <div id="player-sub" style="font-size:0.75rem; color:var(--text-muted);">SD Card Audio</div>
    </div>
  </div>
  <audio id="audio-element" controls preload="none"></audio>
  <button class="btn btn-secondary btn-sm" onclick="document.getElementById('player-dock').classList.remove('active')">✕</button>
</div>

<div id="toast" class="toast"></div>

<script>
let currentTab = 'files';
let currentPath = '/sdcard';
let micMuted = false;
let isRecordingActive = false;
let logEntries = [];
let lastLogSeq = 0;
let telemetryTimer = null;
let logsTimer = null;

function formatBytes(bytes) {
  if (!bytes || bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

function showToast(msg, isError = false) {
  const toast = document.getElementById('toast');
  toast.innerText = msg;
  toast.className = 'toast ' + (isError ? 'toast-error' : 'toast-success') + ' show';
  setTimeout(() => { toast.className = 'toast'; }, 3500);
}

function switchTab(tabId) {
  currentTab = tabId;
  document.querySelectorAll('.tab-btn').forEach(btn => btn.classList.remove('active'));
  document.querySelectorAll('.tab-pane').forEach(pane => pane.classList.remove('active'));
  
  const targetBtn = Array.from(document.querySelectorAll('.tab-btn')).find(b => b.getAttribute('onclick').includes(tabId));
  if (targetBtn) targetBtn.classList.add('active');
  document.getElementById('tab-' + tabId).classList.add('active');

  if (tabId === 'telemetry') loadTelemetry();
  if (tabId === 'config') loadConfigs();
  if (tabId === 'ota') loadOtaInfo();
}

// ─────────────────────────────────────────────────────────────────────────────
// File Manager
// ─────────────────────────────────────────────────────────────────────────────
async function loadStorageInfo() {
  try {
    const res = await fetch('/api/storage/info');
    if (!res.ok) return;
    const data = await res.json();
    if (!data.mounted) {
      document.getElementById('storage-status').innerText = 'SD Card: Not Mounted';
      return;
    }
    const used = data.total_bytes - data.free_bytes;
    const pct = data.total_bytes > 0 ? ((used / data.total_bytes) * 100).toFixed(1) : 0;
    document.getElementById('storage-status').innerText = `SD Card: ${pct}% used`;
    document.getElementById('storage-details').innerText = `${formatBytes(used)} / ${formatBytes(data.total_bytes)} (Free: ${formatBytes(data.free_bytes)})`;
    document.getElementById('storage-progress').style.width = pct + '%';
  } catch (err) {}
}

function renderBreadcrumbs(path) {
  const container = document.getElementById('breadcrumbs');
  container.innerHTML = '';
  const parts = path.split('/').filter(p => p.length > 0);

  const rootSpan = document.createElement('span');
  rootSpan.className = 'crumb';
  rootSpan.innerText = 'root';
  rootSpan.onclick = () => navigateTo('/sdcard');
  container.appendChild(rootSpan);

  let currentSub = '';
  for (let i = 0; i < parts.length; i++) {
    const p = parts[i];
    if (i === 0 && p === 'sdcard') { currentSub = '/sdcard'; continue; }
    currentSub += '/' + p;
    const target = currentSub;

    const sep = document.createElement('span');
    sep.className = 'crumb-sep';
    sep.innerText = '/';
    container.appendChild(sep);

    const crumb = document.createElement('span');
    crumb.className = 'crumb';
    crumb.innerText = p;
    crumb.onclick = () => navigateTo(target);
    container.appendChild(crumb);
  }
}

async function navigateTo(path) {
  currentPath = path;
  renderBreadcrumbs(currentPath);
  document.getElementById('btn-up').disabled = (currentPath === '/sdcard');

  const tbody = document.getElementById('file-table-body');
  tbody.innerHTML = '<tr><td colspan="5" style="text-align:center; color:var(--text-muted);">Loading files...</td></tr>';

  try {
    const res = await fetch('/api/files?path=' + encodeURIComponent(currentPath));
    if (!res.ok) throw new Error('Error loading directory');
    const data = await res.json();
    tbody.innerHTML = '';

    if (!data.entries || data.entries.length === 0) {
      tbody.innerHTML = '<tr><td colspan="5" style="text-align:center; color:var(--text-muted);">Directory is empty</td></tr>';
      return;
    }

    data.entries.forEach(entry => {
      const tr = document.createElement('tr');
      const itemPath = currentPath + '/' + entry.name;
      const isAudio = entry.name.endsWith('.wav') || entry.name.endsWith('.mp3');

      let icon = entry.is_dir ? '📁' : (isAudio ? '🎵' : (entry.name.endsWith('.json') || entry.name.endsWith('.txt') ? '📝' : '📄'));
      let typeLabel = entry.is_dir ? 'Folder' : (isAudio ? 'Audio' : 'File');

      const tdName = document.createElement('td');
      const link = document.createElement('span');
      link.className = 'item-link';
      link.innerHTML = `<span>${icon}</span> <span>${escapeHtml(entry.name)}</span>`;
      if (entry.is_dir) {
        link.onclick = () => navigateTo(itemPath);
      } else if (isAudio) {
        link.onclick = () => playAudio(itemPath, entry.name);
      } else {
        link.onclick = () => downloadFile(itemPath);
      }
      tdName.appendChild(link);

      const tdType = document.createElement('td');
      tdType.innerText = typeLabel;
      tdType.style.color = 'var(--text-muted)';

      const tdSize = document.createElement('td');
      tdSize.innerText = entry.is_dir ? '-' : formatBytes(entry.size);

      const tdMod = document.createElement('td');
      tdMod.innerText = entry.mtime ? new Date(entry.mtime * 1000).toISOString().replace('T',' ').substring(0,19) : '-';
      tdMod.style.color = 'var(--text-muted)';

      const tdActions = document.createElement('td');
      tdActions.className = 'actions-cell';

      if (!entry.is_dir) {
        if (isAudio) {
          const btnPlay = document.createElement('button');
          btnPlay.className = 'btn btn-secondary btn-sm';
          btnPlay.innerText = '▶ Play';
          btnPlay.onclick = () => playAudio(itemPath, entry.name);
          tdActions.appendChild(btnPlay);
        }
        const btnDl = document.createElement('button');
        btnDl.className = 'btn btn-secondary btn-sm';
        btnDl.innerText = '⬇';
        btnDl.title = 'Download';
        btnDl.onclick = () => downloadFile(itemPath);
        tdActions.appendChild(btnDl);
      }

      const btnRen = document.createElement('button');
      btnRen.className = 'btn btn-secondary btn-sm';
      btnRen.innerText = '✏';
      btnRen.title = 'Rename';
      btnRen.onclick = () => promptRename(entry.name);
      tdActions.appendChild(btnRen);

      const btnDel = document.createElement('button');
      btnDel.className = 'btn btn-danger btn-sm';
      btnDel.innerText = '🗑';
      btnDel.title = 'Delete';
      btnDel.onclick = () => confirmDelete(itemPath, entry.name, entry.is_dir);
      tdActions.appendChild(btnDel);

      tr.appendChild(tdName);
      tr.appendChild(tdType);
      tr.appendChild(tdSize);
      tr.appendChild(tdMod);
      tr.appendChild(tdActions);
      tbody.appendChild(tr);
    });
  } catch (err) {
    tbody.innerHTML = `<tr><td colspan="5" style="text-align:center; color:var(--danger);">${err.message}</td></tr>`;
  }
}

function escapeHtml(str) {
  return str.replace(/[&<>'"]/g, tag => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' }[tag] || tag));
}

function goUp() {
  if (currentPath === '/sdcard') return;
  const idx = currentPath.lastIndexOf('/');
  navigateTo(idx <= 0 ? '/sdcard' : currentPath.substring(0, idx));
}

function refreshCurrent() {
  loadStorageInfo();
  navigateTo(currentPath);
}

function downloadFile(filePath) {
  window.location.href = '/api/files/download?path=' + encodeURIComponent(filePath);
}

function playAudio(path, filename) {
  const dock = document.getElementById('player-dock');
  const audio = document.getElementById('audio-element');
  document.getElementById('player-title').innerText = filename;
  document.getElementById('player-sub').innerText = path;
  audio.src = '/api/files/download?path=' + encodeURIComponent(path);
  dock.classList.add('active');
  audio.play();
}

async function promptMkdir() {
  const name = prompt('Folder name:');
  if (!name || !name.trim()) return;
  try {
    const res = await fetch('/api/files/mkdir?path=' + encodeURIComponent(currentPath + '/' + name.trim()), { method: 'POST' });
    if (!res.ok) throw new Error('Failed to create folder');
    showToast('Folder created');
    refreshCurrent();
  } catch (err) { showToast(err.message, true); }
}

async function promptRename(oldName) {
  const newName = prompt('New name:', oldName);
  if (!newName || !newName.trim() || newName === oldName) return;
  try {
    const res = await fetch('/api/files/rename', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ old_path: currentPath + '/' + oldName, new_path: currentPath + '/' + newName.trim() })
    });
    if (!res.ok) throw new Error('Failed to rename');
    showToast('Renamed');
    refreshCurrent();
  } catch (err) { showToast(err.message, true); }
}

async function confirmDelete(fullPath, name, isDir) {
  if (!confirm(`Delete ${isDir ? 'folder' : 'file'} "${name}"?`)) return;
  try {
    const res = await fetch('/api/files?path=' + encodeURIComponent(fullPath), { method: 'DELETE' });
    if (!res.ok) throw new Error('Failed to delete');
    showToast('Deleted');
    refreshCurrent();
  } catch (err) { showToast(err.message, true); }
}

async function uploadFile(file) {
  const uploadStatus = document.getElementById('upload-status');
  uploadStatus.style.display = 'block';
  uploadStatus.innerText = `Uploading "${file.name}"...`;
  try {
    const res = await fetch('/api/files/upload?path=' + encodeURIComponent(currentPath + '/' + file.name), {
      method: 'POST',
      body: file
    });
    if (!res.ok) throw new Error('Upload failed');
    showToast(`Uploaded ${file.name}`);
    uploadStatus.style.display = 'none';
    refreshCurrent();
  } catch (err) {
    uploadStatus.innerText = err.message;
    showToast(err.message, true);
  }
}

function handleFileSelected(files) { if (files.length > 0) uploadFile(files[0]); }

// ─────────────────────────────────────────────────────────────────────────────
// Controls (Audio & LEDs)
// ─────────────────────────────────────────────────────────────────────────────
let volDebounce = null;
function onVolumeChange(val) {
  document.getElementById('val-volume').innerText = val + '%';
  clearTimeout(volDebounce);
  volDebounce = setTimeout(() => {
    fetch('/api/audio/volume?value=' + val, { method: 'POST' });
  }, 250);
}

let micDebounce = null;
function onMicGainChange(val) {
  document.getElementById('val-mic-gain').innerText = parseFloat(val).toFixed(1) + ' dB';
  clearTimeout(micDebounce);
  micDebounce = setTimeout(() => {
    fetch('/api/audio/mic_gain?value=' + val, { method: 'POST' });
  }, 250);
}

async function toggleMicMute() {
  micMuted = !micMuted;
  const btn = document.getElementById('btn-mute');
  btn.innerText = micMuted ? '🔇 Mic Muted' : '🎙 Mic Enabled';
  btn.className = micMuted ? 'btn btn-danger' : 'btn btn-secondary';
  await fetch('/api/audio/mic_mute?muted=' + micMuted, { method: 'POST' });
}

async function playTestAlert() {
  await fetch('/api/audio/alert', { method: 'POST' });
  showToast('Playing chime alert');
}

async function toggleRecording() {
  const btn = document.getElementById('btn-record-toggle');
  const txt = document.getElementById('rec-status-text');
  const badge = document.getElementById('header-rec-badge');
  if (!isRecordingActive) {
    const res = await fetch('/api/audio/record/start', { method: 'POST' });
    if (res.ok) {
      isRecordingActive = true;
      btn.innerText = '⏹ Stop Recording';
      btn.className = 'btn btn-secondary';
      txt.innerText = 'Recording Active (RAW 32kHz)';
      badge.style.display = 'inline-flex';
      showToast('Recording started');
    }
  } else {
    await fetch('/api/audio/record/stop', { method: 'POST' });
    isRecordingActive = false;
    btn.innerText = '🔴 Start Recording';
    btn.className = 'btn btn-danger';
    txt.innerText = 'Idle';
    badge.style.display = 'none';
    showToast('Recording stopped');
    refreshCurrent();
  }
}

function setPresetColor(hex) {
  document.getElementById('led-color').value = hex;
  applyLedSettings();
}

async function applyLedSettings() {
  const mode = document.getElementById('led-mode').value;
  const hex = document.getElementById('led-color').value;
  const speed = parseInt(document.getElementById('led-speed').value);
  const r = parseInt(hex.substr(1,2), 16);
  const g = parseInt(hex.substr(3,2), 16);
  const b = parseInt(hex.substr(5,2), 16);

  await fetch('/api/led/set', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ mode, r, g, b, speed_ms: speed })
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Telemetry & Real-Time Delta Stream
// ─────────────────────────────────────────────────────────────────────────────
let sysInfo = { internal_total: 311931, psram_total: 7864320 };

async function initSystem() {
  try {
    const res = await fetch('/api/system/init');
    if (!res.ok) return;
    const init = await res.json();
    sysInfo = init;

    if (document.getElementById('metric-board')) document.getElementById('metric-board').innerText = init.board || 'ESP32-S3';
    if (document.getElementById('metric-version')) document.getElementById('metric-version').innerText = init.version || '-';
    if (document.getElementById('metric-compile-time')) document.getElementById('metric-compile-time').innerText = `${init.compile_date} ${init.compile_time}`;
    if (document.getElementById('metric-running-part')) document.getElementById('metric-running-part').innerText = `${init.running_partition} (Target: ${init.target_partition})`;
    if (document.getElementById('metric-reset-reason')) document.getElementById('metric-reset-reason').innerText = init.reset_reason || 'Normal Boot';

    if (init.state) applyState(init.state);
  } catch (err) {}
}

function applyState(st) {
  if (st.speaker_volume !== undefined) {
    document.getElementById('val-volume').innerText = st.speaker_volume + '%';
    document.getElementById('slider-volume').value = st.speaker_volume;
    document.getElementById('metric-speaker-vol').innerText = st.speaker_volume + '%';
  }
  if (st.mic_gain_db !== undefined) {
    document.getElementById('val-mic-gain').innerText = Number(st.mic_gain_db).toFixed(1) + ' dB';
    document.getElementById('slider-mic-gain').value = st.mic_gain_db;
    document.getElementById('metric-mic-gain').innerText = Number(st.mic_gain_db).toFixed(1) + ' dB';
  }
  if (st.sample_rate !== undefined) {
    document.getElementById('metric-sample-rate').innerText = st.sample_rate + ' Hz';
  }
  if (st.is_recording !== undefined && st.is_recording !== isRecordingActive) {
    isRecordingActive = st.is_recording;
    document.getElementById('btn-record-toggle').innerText = isRecordingActive ? '⏹ Stop Recording' : '🔴 Start Recording';
    document.getElementById('header-rec-badge').style.display = isRecordingActive ? 'inline-flex' : 'none';
    document.getElementById('metric-recorder-state').innerText = isRecordingActive ? 'Recording' : 'Idle';
  }
}

async function fetchDelta() {
  try {
    const res = await fetch('/api/system/delta?log_seq=' + lastLogSeq);
    if (!res.ok) return;
    const d = await res.json();

    // 1. Real-Time CPU%
    const c0 = d.c0 || 0;
    const c1 = d.c1 || 0;
    document.getElementById('header-cpu-badge').innerText = `CPU: C0: ${c0}% | C1: ${c1}%`;
    if (document.getElementById('metric-cpu0')) {
      document.getElementById('metric-cpu0').innerText = c0 + '%';
      document.getElementById('metric-cpu0-bar').style.width = c0 + '%';
      document.getElementById('metric-cpu1').innerText = c1 + '%';
      document.getElementById('metric-cpu1-bar').style.width = c1 + '%';
    }

    // 2. High-churn Memory
    const sramTotal = sysInfo.internal_total || 311931;
    const sramUsed = sramTotal - d.sram;
    const sramPct = ((sramUsed / sramTotal) * 100).toFixed(1);
    document.getElementById('metric-sram-free').innerText = formatBytes(d.sram) + ' Free';
    document.getElementById('metric-sram-detail').innerText = `${formatBytes(sramUsed)} / ${formatBytes(sramTotal)} (Min: ${formatBytes(d.min_sram)})`;
    document.getElementById('metric-sram-bar').style.width = sramPct + '%';

    const psramTotal = sysInfo.psram_total || 7864320;
    if (psramTotal > 0) {
      const psramUsed = psramTotal - d.psram;
      const psramPct = ((psramUsed / psramTotal) * 100).toFixed(1);
      document.getElementById('metric-psram-free').innerText = formatBytes(d.psram) + ' Free';
      document.getElementById('metric-psram-detail').innerText = `${formatBytes(psramUsed)} / ${formatBytes(psramTotal)}`;
      document.getElementById('metric-psram-bar').style.width = psramPct + '%';
    }

    // 3. Wi-Fi
    const rssi = d.rssi;
    document.getElementById('metric-wifi-rssi').innerText = rssi ? `${rssi} dBm` : 'Offline';
    document.getElementById('header-wifi-badge').innerText = `Wi-Fi: ${rssi ? rssi + ' dBm' : 'Off'}`;

    // 4. Uptime
    const upSec = d.up || 0;
    const days = Math.floor(upSec / 86400);
    const h = Math.floor((upSec % 86400) / 3600);
    const m = Math.floor((upSec % 3600) / 60);
    const s = upSec % 60;
    document.getElementById('metric-uptime').innerText = `${days > 0 ? days+'d ' : ''}${h}h ${m}m ${s}s`;

    // 5. Delta State
    if (d.state) applyState(d.state);

    // 6. Delta Logs
    if (d.logs && d.logs.length > 0) {
      logEntries.push(...d.logs);
      if (logEntries.length > 300) logEntries.splice(0, logEntries.length - 300);
      lastLogSeq = d.latest_seq;
      if (currentTab === 'logs') renderLogs();
    } else if (d.latest_seq !== undefined) {
      lastLogSeq = d.latest_seq;
    }
  } catch (err) {}
}

// ─────────────────────────────────────────────────────────────────────────────
// Config Management
// ─────────────────────────────────────────────────────────────────────────────
async function loadConfigs() {
  try {
    const resSet = await fetch('/api/config/settings');
    if (resSet.ok) document.getElementById('editor-settings').value = await resSet.text();
    const resGem = await fetch('/api/config/gemini');
    if (resGem.ok) document.getElementById('editor-gemini').value = await resGem.text();
  } catch (err) {}
}

async function saveConfigSettings() {
  const content = document.getElementById('editor-settings').value;
  try {
    const res = await fetch('/api/config/settings', { method: 'POST', body: content });
    if (!res.ok) throw new Error('Save failed');
    showToast('Saved settings.txt');
  } catch (err) { showToast(err.message, true); }
}

async function saveConfigGemini() {
  const content = document.getElementById('editor-gemini').value;
  try {
    const res = await fetch('/api/config/gemini', { method: 'POST', body: content });
    if (!res.ok) throw new Error('Save failed');
    showToast('Saved gemini_config.json');
  } catch (err) { showToast(err.message, true); }
}

// ─────────────────────────────────────────────────────────────────────────────
// Live Console
// ─────────────────────────────────────────────────────────────────────────────
async function pollLogs() {
  if (currentTab !== 'logs') return;
  try {
    const res = await fetch('/api/logs?since=' + lastLogSeq);
    if (!res.ok) return;
    const data = await res.json();
    if (data.logs && data.logs.length > 0) {
      logEntries.push(...data.logs);
      if (logEntries.length > 250) logEntries.splice(0, logEntries.length - 250);
      lastLogSeq = data.latest_seq;
      renderLogs();
    }
  } catch (err) {}
}

function renderLogs() {
  const filter = (document.getElementById('log-filter').value || '').toLowerCase();
  const term = document.getElementById('log-terminal');
  const filtered = filter ? logEntries.filter(l => l.toLowerCase().includes(filter)) : logEntries;
  
  term.innerHTML = filtered.map(l => {
    let cls = 'log-debug';
    if (l.includes('I (')) cls = 'log-info';
    else if (l.includes('W (')) cls = 'log-warn';
    else if (l.includes('E (')) cls = 'log-err';
    return `<div class="log-line ${cls}">${escapeHtml(l)}</div>`;
  }).join('');

  if (document.getElementById('log-autoscroll').checked) {
    term.scrollTop = term.scrollHeight;
  }
}

function clearLogs() {
  logEntries = [];
  renderLogs();
}

// ─────────────────────────────────────────────────────────────────────────────
// OTA Web Flasher
// ─────────────────────────────────────────────────────────────────────────────
async function loadOtaInfo() {
  try {
    const res = await fetch('/api/ota/status');
    if (!res.ok) return;
    const data = await res.json();
    document.getElementById('ota-running-part').innerText = `${data.running_partition} (v${data.version || 'unknown'})`;
    document.getElementById('ota-target-part').innerText = data.target_partition;
  } catch (err) {}
}

async function uploadOta(file) {
  if (!confirm(`Flash firmware binary "${file.name}" (${formatBytes(file.size)})? Device will reboot upon completion.`)) return;

  const card = document.getElementById('ota-progress-card');
  const bar = document.getElementById('ota-progress-bar');
  const lbl = document.getElementById('ota-status-label');
  const pct = document.getElementById('ota-percent');
  card.style.display = 'block';
  bar.style.width = '0%';
  lbl.innerText = 'Uploading and writing to OTA flash...';

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota', true);

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const p = ((e.loaded / e.total) * 100).toFixed(0);
      bar.style.width = p + '%';
      pct.innerText = p + '%';
    }
  };

  xhr.onload = () => {
    if (xhr.status === 200) {
      lbl.innerText = 'Flash successful! Rebooting device in 3 seconds...';
      bar.style.width = '100%';
      pct.innerText = '100%';
      showToast('Firmware flashed! Device rebooting...');
      setTimeout(() => { window.location.reload(); }, 6000);
    } else {
      lbl.innerText = 'OTA Flash Error: ' + xhr.responseText;
      showToast('OTA Flash failed', true);
    }
  };

  xhr.onerror = () => {
    lbl.innerText = 'Network connection interrupted during OTA upload';
    showToast('OTA network failure', true);
  };

  xhr.send(file);
}

function handleOtaSelected(files) { if (files.length > 0) uploadOta(files[0]); }

async function rebootDevice() {
  if (!confirm('Reboot ESP32-S3 now?')) return;
  await fetch('/api/system/reboot', { method: 'POST' });
  showToast('Rebooting...');
  setTimeout(() => { window.location.reload(); }, 5000);
}

// Drag & Drop Setup
const dropZone = document.getElementById('drop-zone');
dropZone.onclick = () => document.getElementById('file-input').click();
['dragenter', 'dragover'].forEach(n => dropZone.addEventListener(n, e => { e.preventDefault(); dropZone.classList.add('active'); }));
['dragleave', 'drop'].forEach(n => dropZone.addEventListener(n, e => { e.preventDefault(); dropZone.classList.remove('active'); }));
dropZone.addEventListener('drop', e => { if (e.dataTransfer.files.length > 0) uploadFile(e.dataTransfer.files[0]); });

const otaDrop = document.getElementById('ota-drop-zone');
otaDrop.onclick = () => document.getElementById('ota-file-input').click();
['dragenter', 'dragover'].forEach(n => otaDrop.addEventListener(n, e => { e.preventDefault(); otaDrop.classList.add('active'); }));
['dragleave', 'drop'].forEach(n => otaDrop.addEventListener(n, e => { e.preventDefault(); otaDrop.classList.remove('active'); }));
otaDrop.addEventListener('drop', e => { if (e.dataTransfer.files.length > 0) uploadOta(e.dataTransfer.files[0]); });

// Boot
loadStorageInfo();
navigateTo('/sdcard');
initSystem();
fetchDelta();
setInterval(fetchDelta, 1000); // 1 single unified 1Hz delta & log stream
</script>
</body>
</html>
)rawliteral";

} // namespace Services
