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
    <div className="app-container">
      <Header
        espHost={espHost}
        onHostChange={handleHostChange}
        isOnline={isOnline}
        telemetry={telemetry}
      />

      <main className="app-layout">
        <nav className="sub-nav">
          <button
            className={`nav-tab ${activeTab === 'search' ? 'active' : ''}`}
            onClick={() => setActiveTab('search')}
          >
            <Search size={16} /> YouTube Streamer
          </button>
          <button
            className={`nav-tab ${activeTab === 'library' ? 'active' : ''}`}
            onClick={() => setActiveTab('library')}
          >
            <HardDrive size={16} /> SD Card Library{' '}
            <span className="badge">{libraryCount}</span>
          </button>
          <button
            className={`nav-tab ${activeTab === 'controls' ? 'active' : ''}`}
            onClick={() => setActiveTab('controls')}
          >
            <Sliders size={16} /> Audio & LED Controls
          </button>
          <button
            className={`nav-tab ${activeTab === 'logs' ? 'active' : ''}`}
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
