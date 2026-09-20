import React, { useState } from 'react';
import { Bell, Mic, Volume2 } from 'lucide-react';
import CommitSlider from './CommitSlider';
import { playAlertSound } from '../lib/api';

const PRESET_COLORS = [
  { name: 'Neon Mint', hex: '#00f5d4' },
  { name: 'Cyber Violet', hex: '#8b5cf6' },
  { name: 'Electric Blue', hex: '#00b4d8' },
  { name: 'Neon Pink', hex: '#f43f5e' },
  { name: 'Warm Amber', hex: '#f59e0b' },
  { name: 'Pure White', hex: '#ffffff' },
];

const LED_MODES = ['off', 'solid', 'blink', 'breath', 'rainbow'];

function hexToRgb(hex) {
  return {
    r: parseInt(hex.slice(1, 3), 16) || 0,
    g: parseInt(hex.slice(3, 5), 16) || 0,
    b: parseInt(hex.slice(5, 7), 16) || 0,
  };
}
function rgbToHex({ r, g, b }) {
  const h = (n) => n.toString(16).padStart(2, '0');
  return `#${h(r)}${h(g)}${h(b)}`;
}

export default function DeviceTab({ online, send, volume, micGain, micMuted, led }) {
  const disabled = !online;
  const [chimeStatus, setChimeStatus] = useState('');

  const ledColorHex = rgbToHex(led.value.color);
  const ledModeName = LED_MODES[led.value.mode] || 'off';

  const setLedMode = (name) => {
    if (disabled) return;
    const modeIdx = LED_MODES.indexOf(name);
    led.commit({ ...led.value, mode: modeIdx });
  };
  const setLedColorHex = (hex) => {
    if (disabled) return;
    led.commit({ ...led.value, mode: ledModeName === 'off' ? LED_MODES.indexOf('solid') : led.value.mode, color: hexToRgb(hex) });
  };
  const setLedSpeed = (ms) => {
    if (disabled) return;
    led.commit({ ...led.value, speed_ms: ms });
  };

  const handlePlayChime = async () => {
    setChimeStatus('Playing chime...');
    try {
      await playAlertSound();
      setChimeStatus('Chime played');
      setTimeout(() => setChimeStatus(''), 2000);
    } catch (err) {
      setChimeStatus(`Chime error: ${err.message}`);
    }
  };

  return (
    <div className={`device-tab ${disabled ? 'section-disabled' : ''}`}>
      <div className="panel">
        <div className="panel-header">
          <h3><Volume2 size={16} /> Speaker</h3>
          <span className="panel-tag">ES8311 DAC</span>
        </div>
        <div className="control-row">
          <label>Master volume</label>
          <span className="mono-value">{volume.value}%</span>
        </div>
        <CommitSlider min={0} max={100} value={volume.value} disabled={disabled} onCommit={volume.commit} />
        <div className="pill-row">
          {[20, 40, 60, 80, 100].map((v) => (
            <button key={v} className="pill-btn" disabled={disabled} onClick={() => volume.commit(v)}>{v}%</button>
          ))}
        </div>
      </div>

      <div className="panel">
        <div className="panel-header">
          <h3><Mic size={16} /> Microphone</h3>
          <span className="panel-tag">ES7210 ADC</span>
        </div>
        <div className="control-row">
          <label>PGA gain</label>
          <span className="mono-value">{micGain.value.toFixed(1)} dB</span>
        </div>
        <CommitSlider min={0} max={60} step={0.5} value={micGain.value} disabled={disabled} onCommit={micGain.commit} />
        <div className="toggle-row">
          <span>Mic state</span>
          <button
            className={`toggle-btn ${micMuted.value ? 'toggle-muted' : 'toggle-on'}`}
            disabled={disabled}
            onClick={() => micMuted.commit(!micMuted.value)}
          >
            {micMuted.value ? 'Muted' : 'Active'}
          </button>
        </div>

        <div className="panel-divider" />
        <div className="panel-header">
          <h3><Bell size={16} /> Alert chime</h3>
        </div>
        <button className="btn-secondary btn-block" onClick={handlePlayChime}>
          <Bell size={16} /> Play wake / confirm chime
        </button>
        {chimeStatus && <div className="status-banner">{chimeStatus}</div>}
      </div>

      <div className="panel">
        <div className="panel-header">
          <h3>WS2812 lighting</h3>
          <span className="panel-tag">GPIO 38</span>
        </div>

        <div className="color-dots">
          {PRESET_COLORS.map((c) => (
            <button
              key={c.hex}
              className="color-dot"
              style={{ backgroundColor: c.hex }}
              title={c.name}
              disabled={disabled}
              onClick={() => setLedColorHex(c.hex)}
            />
          ))}
          <input
            type="color"
            className="color-dot color-dot-custom"
            value={ledColorHex}
            disabled={disabled}
            onChange={(e) => setLedColorHex(e.target.value)}
            title="Custom color"
          />
        </div>

        <div className="mode-buttons">
          {LED_MODES.map((m) => (
            <button
              key={m}
              className={`mode-btn ${ledModeName === m ? 'mode-btn-active' : ''}`}
              disabled={disabled}
              onClick={() => setLedMode(m)}
            >
              {m.toUpperCase()}
            </button>
          ))}
        </div>

        <div className="control-row" style={{ marginTop: 14 }}>
          <label>Animation period</label>
          <span className="mono-value">{led.value.speed_ms} ms</span>
        </div>
        <CommitSlider min={100} max={2000} step={50} value={led.value.speed_ms} disabled={disabled} onCommit={setLedSpeed} />
      </div>
    </div>
  );
}
