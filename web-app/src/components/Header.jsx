import React, { useState } from 'react';
import { Zap, Link, Activity, Wifi, Cpu, Database } from 'lucide-react';

export default function Header({ espHost, onHostChange, isOnline, telemetry }) {
  const [localHost, setLocalHost] = useState(espHost);

  const handleSubmit = (e) => {
    e.preventDefault();
    onHostChange(localHost);
  };

  const formatKb = (bytes) => {
    if (!bytes && bytes !== 0) return '-- KB';
    return `${(bytes / 1024).toFixed(1)} KB`;
  };

  const formatMb = (bytes) => {
    if (!bytes && bytes !== 0) return '-- MB';
    return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
  };

  return (
    <header className="top-nav">
      <div className="nav-left">
        <div className="brand-badge">
          <span className="brand-icon"><Zap size={22} /></span>
          <span className="brand-title">WAVESHARE<span className="accent-text">.CORE</span></span>
          <span className="react-badge">React • Vite</span>
        </div>

        <form className="connection-box" onSubmit={handleSubmit}>
          <span className={`status-indicator ${isOnline ? 'online' : ''}`} title={isOnline ? 'ESP32 Connected' : 'ESP32 Offline'} />
          <input
            type="text"
            className="host-input"
            value={localHost}
            onChange={(e) => setLocalHost(e.target.value)}
            placeholder="ESP32 IP"
            title="ESP32 IP address or hostname"
          />
          <button type="submit" className="btn-icon" title="Save & Connect">
            <Link size={16} />
          </button>
        </form>
      </div>

      <div className="telemetry-pill-group">
        <div className="tele-pill" title="Core 0 (Network & HTTP)">
          <span className="tele-label">C0 NET</span>
          <span className="tele-val highlight-cyan">{telemetry.c0 !== undefined ? `${telemetry.c0}%` : '--%'}</span>
        </div>
        <div className="tele-pill" title="Core 1 (Audio DSP & Codec)">
          <span className="tele-label">C1 DSP</span>
          <span className="tele-val highlight-emerald">{telemetry.c1 !== undefined ? `${telemetry.c1}%` : '--%'}</span>
        </div>
        <div className="tele-pill" title="Free Internal SRAM">
          <span className="tele-label">SRAM</span>
          <span className="tele-val highlight-violet">{formatKb(telemetry.sram)}</span>
        </div>
        <div className="tele-pill" title="Free External PSRAM">
          <span className="tele-label">PSRAM</span>
          <span className="tele-val">{formatMb(telemetry.psram)}</span>
        </div>
        <div className="tele-pill" title="Wi-Fi Signal Strength">
          <span className="tele-label">RSSI</span>
          <span className="tele-val">{telemetry.rssi ? `${telemetry.rssi} dBm` : '-- dBm'}</span>
        </div>
      </div>
    </header>
  );
}
