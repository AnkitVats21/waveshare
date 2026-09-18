import React, { useState, useEffect, useRef } from 'react';
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
  getSystemDelta,
  getMusicStatus,
} from './api';

export default function App() {
  const [espHost, setHostState] = useState(getEspHost());
  const [isOnline, setIsOnline] = useState(false);
  const [activeTab, setActiveTab] = useState('search');

  // Telemetry & Hardware state
  const [telemetry, setTelemetry] = useState({});
  const [logs, setLogs] = useState([]);
  const [autoScroll, setAutoScroll] = useState(true);
  const logSeqRef = useRef(0);

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

  // 1. High-frequency Delta Polling (1 Hz) for CPU, Memory, State Delta, and Live Logs
  useEffect(() => {
    let timerId = null;
    let isSubscribed = true;

    const pollDelta = async () => {
      try {
        const delta = await getSystemDelta(logSeqRef.current);
        if (!isSubscribed) return;

        setIsOnline(true);
        setTelemetry({
          up: delta.up,
          c0: delta.c0,
          c1: delta.c1,
          sram: delta.sram,
          min_sram: delta.min_sram,
          psram: delta.psram,
          rssi: delta.rssi,
        });

        if (delta.latest_seq) {
          logSeqRef.current = delta.latest_seq;
        }

        if (delta.logs && delta.logs.length > 0) {
          setLogs((prev) => [...prev.slice(-400), ...delta.logs]);
        }

        if (delta.state) {
          if (delta.state.speaker_volume !== undefined) setVolume(delta.state.speaker_volume);
          if (delta.state.mic_gain_db !== undefined) setMicGain(delta.state.mic_gain_db);
          if (delta.state.mic_enabled !== undefined) setMicMuted(!delta.state.mic_enabled);
        }

        if (delta.music) {
          if (delta.music.state) setPlaybackState(delta.music.state);
          if (delta.music.current_track && delta.music.current_track.id) {
            setCurrentTrack(delta.music.current_track);
          }
          if (delta.music.position_ms !== undefined) setPositionMs(delta.music.position_ms);
          if (delta.music.duration_ms !== undefined) setDurationMs(delta.music.duration_ms);
          if (delta.music.seekable !== undefined) setSeekable(delta.music.seekable);
          if (delta.music.repeat_mode !== undefined) setRepeatMode(delta.music.repeat_mode);
          if (delta.music.autoplay !== undefined) setAutoplay(delta.music.autoplay);
          if (delta.music.caching !== undefined) setCaching(delta.music.caching);
        }
      } catch (err) {
        if (isSubscribed) setIsOnline(false);
      } finally {
        if (isSubscribed) {
          timerId = setTimeout(pollDelta, 1000);
        }
      }
    };

    pollDelta();
    return () => {
      isSubscribed = false;
      if (timerId) clearTimeout(timerId);
    };
  }, [espHost]);

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
