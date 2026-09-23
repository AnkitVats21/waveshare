#pragma once

namespace Services {

static const char CAPTIVE_PORTAL_HTML[] = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Waveshare Assistant - Wi-Fi Setup</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #0f172a; color: #f8fafc; min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 1rem; }
  .card { background: #1e293b; border: 1px solid #334155; border-radius: 1rem; padding: 1.75rem; max-width: 420px; width: 100%; box-shadow: 0 20px 25px -5px rgba(0,0,0,0.5); }
  .header { text-align: center; margin-bottom: 1.5rem; }
  .header h1 { font-size: 1.35rem; font-weight: 700; color: #38bdf8; margin-top: 0.5rem; }
  .header p { font-size: 0.85rem; color: #94a3b8; margin-top: 0.25rem; }
  .badge { display: inline-block; background: #0284c7; color: white; padding: 0.25rem 0.6rem; border-radius: 9999px; font-size: 0.75rem; font-weight: 600; text-transform: uppercase; }
  .form-group { margin-bottom: 1.25rem; }
  label { display: block; font-size: 0.85rem; font-weight: 600; margin-bottom: 0.4rem; color: #cbd5e1; }
  select, input[type="text"], input[type="password"] { width: 100%; padding: 0.65rem 0.85rem; background: #0f172a; border: 1px solid #475569; border-radius: 0.5rem; color: white; font-size: 0.95rem; outline: none; transition: border-color 0.2s; }
  select:focus, input:focus { border-color: #38bdf8; }
  .btn { width: 100%; padding: 0.75rem; border: none; border-radius: 0.5rem; font-weight: 600; font-size: 0.95rem; cursor: pointer; transition: all 0.2s; }
  .btn-primary { background: #0284c7; color: white; margin-top: 0.5rem; }
  .btn-primary:hover { background: #0369a1; }
  .btn-primary:disabled { background: #475569; cursor: not-allowed; opacity: 0.6; }
  .btn-secondary { background: transparent; border: 1px solid #475569; color: #94a3b8; font-size: 0.8rem; padding: 0.4rem 0.75rem; margin-top: 0.5rem; display: flex; align-items: center; justify-content: center; gap: 0.4rem; }
  .btn-secondary:hover { color: white; border-color: #94a3b8; }
  .status-box { margin-top: 1.25rem; padding: 0.85rem; border-radius: 0.5rem; font-size: 0.85rem; display: none; line-height: 1.4; }
  .status-box.info { display: block; background: #0369a1; color: #e0f2fe; border: 1px solid #0284c7; }
  .status-box.success { display: block; background: #065f46; color: #d1fae5; border: 1px solid #059669; }
  .status-box.error { display: block; background: #881337; color: #ffe4e6; border: 1px solid #e11d48; }
  .toggle-pwd { display: flex; align-items: center; gap: 0.4rem; margin-top: 0.4rem; font-size: 0.8rem; color: #94a3b8; cursor: pointer; user-select: none; }
  .hidden { display: none; }
</style>
</head>
<body>
<div class="card">
  <div class="header">
    <span class="badge">Wi-Fi Setup Mode</span>
    <h1>Waveshare Assistant</h1>
    <p>Select your wireless network to connect</p>
  </div>
  <form id="wifiForm">
    <div class="form-group">
      <label for="ssid">Wireless Network (SSID)</label>
      <select id="ssid" required>
        <option value="" disabled selected>Scanning nearby networks...</option>
      </select>
      <input type="text" id="manualSsid" class="hidden" placeholder="Enter network name" style="margin-top:0.4rem;">
      <button type="button" id="scanBtn" class="btn btn-secondary">🔄 Rescan Networks</button>
    </div>
    <div class="form-group">
      <label for="password">Network Password</label>
      <input type="password" id="password" placeholder="Enter password (leave empty if open)">
      <label class="toggle-pwd">
        <input type="checkbox" id="showPass"> Show Password
      </label>
    </div>
    <button type="submit" id="submitBtn" class="btn btn-primary">Connect Device</button>
  </form>
  <div id="statusBox" class="status-box"></div>
</div>
<script>
const ssidSelect = document.getElementById('ssid');
const manualSsid = document.getElementById('manualSsid');
const scanBtn = document.getElementById('scanBtn');
const wifiForm = document.getElementById('wifiForm');
const submitBtn = document.getElementById('submitBtn');
const statusBox = document.getElementById('statusBox');
const showPass = document.getElementById('showPass');
const passwordInput = document.getElementById('password');

showPass.addEventListener('change', () => {
  passwordInput.type = showPass.checked ? 'text' : 'password';
});

ssidSelect.addEventListener('change', () => {
  if (ssidSelect.value === '__manual__') {
    manualSsid.classList.remove('hidden');
    manualSsid.required = true;
    manualSsid.focus();
  } else {
    manualSsid.classList.add('hidden');
    manualSsid.required = false;
  }
});

async function scanNetworks() {
  scanBtn.disabled = true;
  scanBtn.innerText = 'Scanning...';
  ssidSelect.innerHTML = '<option disabled selected>Scanning nearby networks...</option>';
  try {
    const res = await fetch('/api/wifi/scan');
    const nets = await res.json();
    ssidSelect.innerHTML = '<option value="" disabled selected>Choose a network</option>';
    nets.forEach(n => {
      const opt = document.createElement('option');
      opt.value = n.ssid;
      const lock = n.auth === 'OPEN' ? '🔓' : '🔒';
      opt.innerText = `${lock} ${n.ssid} (${n.rssi} dBm)`;
      ssidSelect.appendChild(opt);
    });
    const manualOpt = document.createElement('option');
    manualOpt.value = '__manual__';
    manualOpt.innerText = '➕ Other / Hidden Network...';
    ssidSelect.appendChild(manualOpt);
  } catch(e) {
    ssidSelect.innerHTML = '<option value="" disabled selected>Scan failed</option><option value="__manual__">➕ Enter manually...</option>';
  } finally {
    scanBtn.disabled = false;
    scanBtn.innerText = '🔄 Rescan Networks';
  }
}

scanBtn.addEventListener('click', scanNetworks);
window.addEventListener('DOMContentLoaded', scanNetworks);

wifiForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  const ssid = (ssidSelect.value === '__manual__') ? manualSsid.value.trim() : ssidSelect.value;
  const password = passwordInput.value;

  if (!ssid) return;

  submitBtn.disabled = true;
  submitBtn.innerText = 'Connecting...';
  statusBox.className = 'status-box info';
  statusBox.innerText = `Connecting to "${ssid}"... please wait.`;

  try {
    const res = await fetch('/api/wifi/configure', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ssid, password })
    });
    if (!res.ok) throw new Error('Failed to send configuration');

    pollStatus(ssid);
  } catch(err) {
    statusBox.className = 'status-box error';
    statusBox.innerText = `Error: ${err.message}`;
    submitBtn.disabled = false;
    submitBtn.innerText = 'Connect Device';
  }
});

function pollStatus(targetSsid) {
  let attempts = 0;
  const interval = setInterval(async () => {
    attempts++;
    try {
      const res = await fetch('/api/wifi/status');
      const data = await res.json();
      if (data.network_state === 'Connected') {
        clearInterval(interval);
        statusBox.className = 'status-box success';
        statusBox.innerText = `🎉 Successfully connected to ${targetSsid}! Assistant is now online. SoftAP will close.`;
      } else if (data.network_state === 'Failed' || (attempts > 15 && data.network_state === 'PortalActive')) {
        clearInterval(interval);
        statusBox.className = 'status-box error';
        statusBox.innerText = `❌ Connection failed. Please check password and try again.`;
        submitBtn.disabled = false;
        submitBtn.innerText = 'Connect Device';
      }
    } catch(e) {
      if (attempts > 15) {
        clearInterval(interval);
        statusBox.className = 'status-box error';
        statusBox.innerText = 'Connection timed out or SoftAP closed.';
        submitBtn.disabled = false;
        submitBtn.innerText = 'Connect Device';
      }
    }
  }, 1500);
}
</script>
</body>
</html>
)rawliteral";

} // namespace Services
