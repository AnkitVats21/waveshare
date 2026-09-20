import React, { useState, useEffect } from 'react';
import { Search, HardDrive, Sliders, Terminal } from 'lucide-react';
import Header from './components/Header';
import YouTubeSearch from './components/YouTubeSearch';
import SdLibrary from './components/SdLibrary';
import Controls from './components/Controls';
import LiveLogs from './components/LiveLogs';
import PlayerDock from './components/PlayerDock';
import {
  getEspHost,
  setEspHost,
  getDaemonHost,
  getDaemonWsUrl,
} from './api';

export default function App() {
  const [espHost, setHostState] = useState(getEspHost());
  const [daemonHost, setDaemonHostState] = useState(getDaemonHost());
  const [isOnline, setIsOnline] = useState(false);
  const [activeTab, setActiveTab] = useState('search');

  // Telemetry & Hardware state
  const [telemetry, setTelemetry] = useState({});
  const [logs, setLogs] = useState([]);
  const [autoScroll, setAutoScroll] = useState(true);

  // Audio / Hardware controls
  const [volume, setVolume] = useState(70);
  const [micGain, setMicGain] = useState(30);
  const [micMuted, setMicMuted] = useState(false);

  // Playback state
  const [playbackState, setPlaybackState] = useState('IDLE');
  const [currentTrack, setCurrentTrack] = useState(null);
  const [positionMs, setPositionMs] = useState(0);
  const [durationMs, setDurationMs] = useState(0);
  const [seekable, setSeekable] = useState(false);
  const [repeatMode, setRepeatMode] = useState(0);
  const [autoplay, setAutoplay] = useState(true);
  const [caching, setCaching] = useState(true);
  const [libraryCount, setLibraryCount] = useState(0);

  const handleHostChange = (newHost) => {
    const saved = setEspHost(newHost);
    setHostState(saved);
  };

  // WebSocket live push from star-replica-daemon
  useEffect(() => {
    let ws = null;
    let reconnectTimer = null;
    let destroyed = false;

    const applySnapshot = (data) => {
      setIsOnline(true);

      if (data.up !== undefined || data.sram !== undefined) {
        setTelemetry({
          up: data.up,
          c0: data.c0,
          c1: data.c1,
          sram: data.sram,
          min_sram: data.min_sram,
          psram: data.psram,
          rssi: data.rssi,
        });
      }

      if (data.state) {
        if (data.state.speaker_volume !== undefined) setVolume(data.state.speaker_volume);
        if (data.state.mic_gain_db !== undefined) setMicGain(data.state.mic_gain_db);
        if (data.state.mic_enabled !== undefined) setMicMuted(!data.state.mic_enabled);
      }

      if (data.music) {
        if (data.music.state) setPlaybackState(data.music.state);
        if (data.music.current_track && data.music.current_track.id) {
          setCurrentTrack(data.music.current_track);
        }
        if (data.music.position_ms !== undefined) setPositionMs(data.music.position_ms);
        if (data.music.duration_ms !== undefined) setDurationMs(data.music.duration_ms);
        if (data.music.seekable !== undefined) setSeekable(data.music.seekable);
        if (data.music.repeat_mode !== undefined) setRepeatMode(data.music.repeat_mode);
        if (data.music.autoplay !== undefined) setAutoplay(data.music.autoplay);
        if (data.music.caching !== undefined) setCaching(data.music.caching);
      }
    };

    const connect = () => {
      if (destroyed) return;
      const url = getDaemonWsUrl();
      ws = new WebSocket(url);

      ws.onopen = () => {
        if (destroyed) { ws.close(); return; }
        setIsOnline(true);
      };

      ws.onmessage = (evt) => {
        if (destroyed) return;
        try {
          const data = JSON.parse(evt.data);
          if (data.type === 'disconnected') {
            setIsOnline(false);
          } else {
            applySnapshot(data);
          }
        } catch (e) {
          console.warn('[ws] parse error', e);
        }
      };

      ws.onerror = () => {};
      ws.onclose = () => {
        setIsOnline(false);
        if (!destroyed) {
          reconnectTimer = setTimeout(connect, 3000);
        }
      };
    };

    connect();
    return () => {
      destroyed = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
      if (ws) ws.close();
    };
  }, [daemonHost]);

  const handleTrackStarted = (track) => {
    setCurrentTrack({
      id: track.videoId || track.id,
      title: track.title,
      artist: track.author || track.artist,
      duration: track.lengthSeconds || track.durationSeconds,
    });
    setPlaybackState('STREAMING');
  };

  return (
    <div className="min-h-screen bg-[#0b0c14] text-slate-100 flex flex-col font-sans pb-28">
      <Header
        espHost={espHost}
        onHostChange={handleHostChange}
        isOnline={isOnline}
        telemetry={telemetry}
      />

      <main className="max-w-7xl w-full mx-auto px-4 sm:px-6 lg:px-8 py-6 flex-1 flex flex-col">
        <nav className="flex items-center gap-2 border-b border-slate-800/80 pb-3 mb-6 overflow-x-auto">
          <button
            className={`flex items-center gap-2 px-4 py-2 rounded-lg text-sm font-medium transition-all shrink-0 cursor-pointer ${
              activeTab === 'search'
                ? 'bg-slate-800/90 text-teal-300 border border-teal-500/30 shadow-[0_0_12px_rgba(45,212,191,0.15)]'
                : 'text-slate-400 hover:text-slate-200 hover:bg-slate-800/50 border border-transparent'
            }`}
            onClick={() => setActiveTab('search')}
          >
            <Search size={16} /> YouTube Streamer
          </button>
          <button
            className={`flex items-center gap-2 px-4 py-2 rounded-lg text-sm font-medium transition-all shrink-0 cursor-pointer ${
              activeTab === 'library'
                ? 'bg-slate-800/90 text-teal-300 border border-teal-500/30 shadow-[0_0_12px_rgba(45,212,191,0.15)]'
                : 'text-slate-400 hover:text-slate-200 hover:bg-slate-800/50 border border-transparent'
            }`}
            onClick={() => setActiveTab('library')}
          >
            <HardDrive size={16} /> SD Card Library{' '}
            <span className="ml-1.5 px-1.5 py-0.5 rounded-full text-xs font-mono bg-slate-700/60 text-slate-300 border border-slate-600/40">
              {libraryCount}
            </span>
          </button>
          <button
            className={`flex items-center gap-2 px-4 py-2 rounded-lg text-sm font-medium transition-all shrink-0 cursor-pointer ${
              activeTab === 'controls'
                ? 'bg-slate-800/90 text-teal-300 border border-teal-500/30 shadow-[0_0_12px_rgba(45,212,191,0.15)]'
                : 'text-slate-400 hover:text-slate-200 hover:bg-slate-800/50 border border-transparent'
            }`}
            onClick={() => setActiveTab('controls')}
          >
            <Sliders size={16} /> Audio & LED Controls
          </button>
          <button
            className={`flex items-center gap-2 px-4 py-2 rounded-lg text-sm font-medium transition-all shrink-0 cursor-pointer ${
              activeTab === 'logs'
                ? 'bg-slate-800/90 text-teal-300 border border-teal-500/30 shadow-[0_0_12px_rgba(45,212,191,0.15)]'
                : 'text-slate-400 hover:text-slate-200 hover:bg-slate-800/50 border border-transparent'
            }`}
            onClick={() => setActiveTab('logs')}
          >
            <Terminal size={16} /> Live Logs
          </button>
        </nav>

        {activeTab === 'search' && (
          <YouTubeSearch onTrackStarted={handleTrackStarted} />
        )}

        {activeTab === 'library' && (
          <SdLibrary
            onTrackStarted={handleTrackStarted}
            libraryCount={libraryCount}
            setLibraryCount={setLibraryCount}
          />
        )}

        {activeTab === 'controls' && (
          <Controls
            volume={volume}
            onVolumeChange={setVolume}
            micGain={micGain}
            onMicGainChange={setMicGain}
            micMuted={micMuted}
            onMicMuteChange={setMicMuted}
          />
        )}

        {activeTab === 'logs' && (
          <LiveLogs
            logs={logs}
            onClearLogs={() => setLogs([])}
            autoScroll={autoScroll}
            onToggleAutoScroll={setAutoScroll}
          />
        )}
      </main>

      <PlayerDock
        currentTrack={currentTrack}
        playbackState={playbackState}
        positionMs={positionMs}
        durationMs={durationMs}
        seekable={seekable}
        repeatMode={repeatMode}
        setRepeatMode={setRepeatMode}
        autoplay={autoplay}
        setAutoplay={setAutoplay}
        caching={caching}
        setCaching={setCaching}
        volume={volume}
        setVolume={setVolume}
      />
    </div>
  );
}
