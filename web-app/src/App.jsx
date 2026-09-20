import React, { useState, useEffect, useCallback } from 'react';
import { Play, Sliders } from 'lucide-react';
import Header from './components/Header';
import PlayTab from './components/PlayTab';
import DeviceTab from './components/DeviceTab';
import NowPlayingDock from './components/NowPlayingDock';
import DevLogs from './components/DevLogs';
import { useStarSocket } from './hooks/useStarSocket';
import { usePendingValue } from './hooks/usePendingValue';
import { getEspHost, setEspHost } from './lib/api';

const ledEqual = (a, b) =>
  a.mode === b.mode && a.speed_ms === b.speed_ms &&
  a.color.r === b.color.r && a.color.g === b.color.g && a.color.b === b.color.b;

// Optimistic track shown the instant a user hits "Play", before the daemon's
// next snapshot confirms it (which can take a few seconds - stream
// resolution + device round trip). Cleared once the snapshot's track id
// matches, or after a grace period.
const TRACK_OPTIMISM_MS = 5000;

export default function App() {
  const [espHost, setHostState] = useState(getEspHost());
  const [activeTab, setActiveTab] = useState('play');
  const [devMode, setDevMode] = useState(false);
  const [libraryCount, setLibraryCount] = useState(0);
  const [optimisticTrack, setOptimisticTrack] = useState(null);

  const { snapshot, online, send } = useStarSocket();

  useEffect(() => {
    if (!optimisticTrack) return;
    if (snapshot.music.current_track?.id === optimisticTrack.id) {
      setOptimisticTrack(null);
      return;
    }
    const t = setTimeout(() => setOptimisticTrack(null), TRACK_OPTIMISM_MS);
    return () => clearTimeout(t);
  }, [optimisticTrack, snapshot.music.current_track?.id]);

  const handleHostChange = (newHost) => setHostState(setEspHost(newHost));

  const volume = usePendingValue(snapshot.state.speaker_volume, (v) => send('volume', { value: v }));
  const micGain = usePendingValue(snapshot.state.mic_gain_db, (v) => send('mic_gain', { value: v }));
  const micMuted = usePendingValue(!snapshot.state.mic_enabled, (v) => send('mic_mute', { value: v }));
  const repeat = usePendingValue(snapshot.music.repeat_mode, (v) => send('action', { action: 'repeat', value: v }));
  const autoplay = usePendingValue(snapshot.music.autoplay, (v) => send('action', { action: 'autoplay', value: v }));
  const caching = usePendingValue(snapshot.music.caching, (v) => send('action', { action: 'caching', value: v }));
  const led = usePendingValue(snapshot.led, (v) => send('led', {
    mode: ['off', 'solid', 'blink', 'breath', 'rainbow'][v.mode] || 'off',
    color: v.color,
    speed_ms: v.speed_ms,
  }), ledEqual);

  const handlePlay = useCallback((track, streamUrl) => {
    setOptimisticTrack(track);
    send('action', { action: 'play', data: streamUrl });
  }, [send]);

  const handlePlayLocal = useCallback((track) => {
    setOptimisticTrack({ id: track.id, title: track.title, artist: track.artist, duration: track.durationSeconds });
  }, []);

  const displayMusic = {
    ...snapshot.music,
    current_track: optimisticTrack || snapshot.music.current_track,
  };

  return (
    <div className="app-shell">
      <Header
        espHost={espHost}
        onHostChange={handleHostChange}
        online={online}
        deviceConnected={snapshot.connected}
        devMode={devMode}
        onToggleDevMode={() => setDevMode((v) => !v)}
      />

      <main className="app-main">
        {!devMode && (
          <nav className="tab-bar">
            <button className={activeTab === 'play' ? 'active' : ''} onClick={() => setActiveTab('play')}>
              <Play size={15} /> Play
            </button>
            <button className={activeTab === 'device' ? 'active' : ''} onClick={() => setActiveTab('device')}>
              <Sliders size={15} /> Device
            </button>
          </nav>
        )}

        {devMode ? (
          <DevLogs snapshot={snapshot} />
        ) : activeTab === 'play' ? (
          <PlayTab
            onPlay={handlePlay}
            onPlayLocal={handlePlayLocal}
            disabled={!online}
            libraryCount={libraryCount}
            onLibraryCount={setLibraryCount}
          />
        ) : (
          <DeviceTab
            online={online}
            send={send}
            volume={volume}
            micGain={micGain}
            micMuted={micMuted}
            led={led}
          />
        )}
      </main>

      <NowPlayingDock
        music={displayMusic}
        online={online}
        send={send}
        volume={volume}
        repeat={repeat}
        autoplay={autoplay}
        caching={caching}
      />
    </div>
  );
}
