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
    <header className="sticky top-0 z-40 bg-slate-950/85 backdrop-blur-xl border-b border-slate-800/80 px-4 sm:px-8 py-3.5 flex flex-wrap items-center justify-between gap-4">
      <div className="flex flex-wrap items-center gap-4 sm:gap-6">
        <div className="flex items-center gap-2.5">
          <div className="w-8 h-8 rounded-lg bg-teal-500/10 border border-teal-500/30 flex items-center justify-center text-teal-300 shadow-[0_0_12px_rgba(45,212,191,0.25)]">
            <Zap size={18} />
          </div>
          <span className="font-bold text-base tracking-tight text-white">
            WAVESHARE<span className="text-teal-400">.CORE</span>
          </span>
          <span className="hidden sm:inline-block text-[10px] font-mono font-medium px-2 py-0.5 rounded-full bg-slate-800 text-slate-300 border border-slate-700">
            React • Vite
          </span>
        </div>

        <form
          className="flex items-center gap-2 bg-slate-900/90 border border-slate-800 rounded-lg px-3 py-1.5 focus-within:border-teal-500/50 transition-colors shadow-inner"
          onSubmit={handleSubmit}
        >
          <span
            className={`w-2 h-2 rounded-full shrink-0 ${
              isOnline
                ? 'bg-emerald-400 shadow-[0_0_8px_#34d399]'
                : 'bg-rose-500 shadow-[0_0_8px_#f43f5e]'
            }`}
            title={isOnline ? 'ESP32 Connected' : 'ESP32 Offline'}
          />
          <input
            type="text"
            className="bg-transparent text-xs sm:text-sm text-slate-200 placeholder-slate-500 outline-none w-28 sm:w-36 font-mono"
            value={localHost}
            onChange={(e) => setLocalHost(e.target.value)}
            placeholder="ESP32 IP"
            title="ESP32 IP address or hostname"
          />
          <button
            type="submit"
            className="text-slate-400 hover:text-teal-300 transition-colors p-0.5"
            title="Save & Connect"
          >
            <Link size={15} />
          </button>
        </form>
      </div>

      <div className="flex items-center gap-2 overflow-x-auto max-w-full pb-1 sm:pb-0">
        <div className="flex items-center gap-1.5 px-2.5 py-1 rounded-md bg-slate-900/80 border border-slate-800 text-xs font-mono shrink-0" title="Core 0 (Network & HTTP)">
          <span className="text-slate-500 font-semibold">C0 NET</span>
          <span className="text-teal-400 font-bold">{telemetry.c0 !== undefined ? `${telemetry.c0}%` : '--%'}</span>
        </div>
        <div className="flex items-center gap-1.5 px-2.5 py-1 rounded-md bg-slate-900/80 border border-slate-800 text-xs font-mono shrink-0" title="Core 1 (Audio DSP & Codec)">
          <span className="text-slate-500 font-semibold">C1 DSP</span>
          <span className="text-emerald-400 font-bold">{telemetry.c1 !== undefined ? `${telemetry.c1}%` : '--%'}</span>
        </div>
        <div className="flex items-center gap-1.5 px-2.5 py-1 rounded-md bg-slate-900/80 border border-slate-800 text-xs font-mono shrink-0" title="Free Internal SRAM">
          <span className="text-slate-500 font-semibold">SRAM</span>
          <span className="text-violet-400 font-bold">{formatKb(telemetry.sram)}</span>
        </div>
        <div className="flex items-center gap-1.5 px-2.5 py-1 rounded-md bg-slate-900/80 border border-slate-800 text-xs font-mono shrink-0" title="Free External PSRAM">
          <span className="text-slate-500 font-semibold">PSRAM</span>
          <span className="text-slate-200 font-bold">{formatMb(telemetry.psram)}</span>
        </div>
        <div className="flex items-center gap-1.5 px-2.5 py-1 rounded-md bg-slate-900/80 border border-slate-800 text-xs font-mono shrink-0" title="Wi-Fi Signal Strength">
          <span className="text-slate-500 font-semibold">RSSI</span>
          <span className="text-slate-300 font-bold">{telemetry.rssi ? `${telemetry.rssi} dBm` : '-- dBm'}</span>
        </div>
      </div>
    </header>
  );
}
