import React, { useState } from 'react';
import { Volume2, Mic, Bell, Sparkles, Sliders } from 'lucide-react';
import { setSpeakerVolume, setMicGain, setMicMute, playAlertSound, setLedLighting } from '../api';

const PRESET_COLORS = [
  { name: 'Neon Mint', rgb: [0, 245, 212], hex: '#00f5d4' },
  { name: 'Cyber Violet', rgb: [139, 92, 246], hex: '#8b5cf6' },
  { name: 'Electric Blue', rgb: [0, 180, 216], hex: '#00b4d8' },
  { name: 'Neon Pink', rgb: [244, 63, 94], hex: '#f43f5e' },
  { name: 'Warm Amber', rgb: [245, 158, 11], hex: '#f59e0b' },
  { name: 'Pure White', rgb: [255, 255, 255], hex: '#ffffff' },
];

export default function Controls({ volume, onVolumeChange, micGain, onMicGainChange, micMuted, onMicMuteChange }) {
  const [ledMode, setLedMode] = useState('solid');
  const [ledColor, setLedColor] = useState('#8b5cf6');
  const [ledSpeed, setLedSpeed] = useState(500);
  const [chimeStatus, setChimeStatus] = useState('');

  const handleVolChange = (val) => {
    onVolumeChange(val);
    setSpeakerVolume(val).catch(console.error);
  };

  const handleMicGainChange = (val) => {
    onMicGainChange(val);
    setMicGain(val).catch(console.error);
  };

  const handleMicMuteToggle = () => {
    const next = !micMuted;
    onMicMuteChange(next);
    setMicMute(next).catch(console.error);
  };

  const handlePlayChime = async () => {
    setChimeStatus('Playing chime...');
    try {
      await playAlertSound();
      setChimeStatus('Chime played!');
      setTimeout(() => setChimeStatus(''), 2000);
    } catch (err) {
      setChimeStatus(`Chime error: ${err.message}`);
    }
  };

  const applyLed = (mode, hex, speed = ledSpeed) => {
    setLedMode(mode);
    setLedColor(hex);
    // Parse hex to r, g, b
    const r = parseInt(hex.slice(1, 3), 16) || 0;
    const g = parseInt(hex.slice(3, 5), 16) || 0;
    const b = parseInt(hex.slice(5, 7), 16) || 0;
    setLedLighting({ mode, r, g, b, speed_ms: speed }).catch(console.error);
  };

  return (
    <div className="controls-grid">
      {/* Audio Card */}
      <div className="control-card">
        <div className="card-header">
          <h3>🔊 Speaker & Amplifier</h3>
          <span className="card-tag">ES8311 DAC</span>
        </div>

        <div className="slider-row">
          <label>Master Volume</label>
          <span className="val-display">{volume}%</span>
        </div>
        <input
          type="range"
          min="0"
          max="100"
          value={volume}
          onChange={(e) => handleVolChange(Number(e.target.value))}
          className="styled-range"
        />

        <div className="btn-row">
          {[20, 40, 60, 80, 100].map((v) => (
            <button key={v} className="btn-pill" onClick={() => handleVolChange(v)}>
              {v}%
            </button>
          ))}
        </div>

        <div className="divider" />

        <div className="card-header">
          <h3>🎙️ Microphone Input</h3>
          <span className="card-tag">ES7210 ADC</span>
        </div>

        <div className="slider-row">
          <label>PGA Gain</label>
          <span className="val-display">{micGain.toFixed(1)} dB</span>
        </div>
        <input
          type="range"
          min="0"
          max="60"
          step="0.5"
          value={micGain}
          onChange={(e) => handleMicGainChange(Number(e.target.value))}
          className="styled-range"
        />

        <div className="toggle-row">
          <span>Mic State</span>
          <button
            className={`btn-toggle ${micMuted ? 'muted' : ''}`}
            onClick={handleMicMuteToggle}
          >
            {micMuted ? 'Muted' : 'Active / Unmuted'}
          </button>
        </div>

        <div className="divider" />

        <div className="card-header">
          <h3>🔔 Audio Chime</h3>
        </div>
        <button className="btn-secondary w-full" onClick={handlePlayChime}>
          <Bell size={16} /> Play Wake / Confirm Chime
        </button>
        {chimeStatus && <div className="status-banner" style={{ marginTop: 10 }}>{chimeStatus}</div>}
      </div>

      {/* LED WS2812 Lighting Card */}
      <div className="control-card">
        <div className="card-header">
          <h3>✨ WS2812 RGB LED</h3>
          <span className="card-tag">GPIO 38</span>
        </div>

        <div className="preset-colors">
          {PRESET_COLORS.map((c) => (
            <button
              key={c.hex}
              className="color-dot"
              style={{ backgroundColor: c.hex }}
              title={c.name}
              onClick={() => applyLed(ledMode === 'off' ? 'solid' : ledMode, c.hex)}
            />
          ))}
        </div>

        <div className="mode-selector">
          <label>Lighting Mode</label>
          <div className="mode-buttons">
            {['solid', 'breath', 'rainbow', 'blink', 'off'].map((m) => (
              <button
                key={m}
                className={`btn-mode ${ledMode === m ? 'active' : ''}`}
                onClick={() => applyLed(m, ledColor)}
              >
                {m.toUpperCase()}
              </button>
            ))}
          </div>
        </div>

        <div className="slider-row" style={{ marginTop: 16 }}>
          <label>Animation Period</label>
          <span className="val-display">{ledSpeed} ms</span>
        </div>
        <input
          type="range"
          min="100"
          max="2000"
          step="50"
          value={ledSpeed}
          onChange={(e) => {
            const spd = Number(e.target.value);
            setLedSpeed(spd);
            applyLed(ledMode, ledColor, spd);
          }}
          className="styled-range"
        />

        <div className="color-picker-row">
          <label>Custom Color Picker:</label>
          <input
            type="color"
            id="ledColorPicker"
            value={ledColor}
            onChange={(e) => applyLed(ledMode === 'off' ? 'solid' : ledMode, e.target.value)}
          />
        </div>
      </div>
    </div>
  );
}
