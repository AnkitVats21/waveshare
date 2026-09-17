import React from 'react';
import { Play, Pause, SkipBack, SkipForward, Square, Repeat, Radio, HardDrive, Volume2, VolumeX } from 'lucide-react';
import { controlPlayback, setSpeakerVolume } from '../api';

export default function PlayerDock({
  currentTrack,
  playbackState,
  repeatMode,
  setRepeatMode,
  autoplay,
  setAutoplay,
  caching,
  setCaching,
  volume,
  setVolume,
}) {
  const isPlaying = playbackState === 'STREAMING' || playbackState === 'LOCAL';

  const handleTogglePlayPause = async () => {
    try {
      await controlPlayback(isPlaying ? 'pause' : 'resume');
    } catch (err) {
      console.error(err);
    }
  };

  const handleNext = async () => {
    try {
      await controlPlayback('next');
    } catch (err) {
      console.error(err);
    }
  };

  const handlePrev = async () => {
    try {
      await controlPlayback('prev');
    } catch (err) {
      console.error(err);
    }
  };

  const handleStop = async () => {
    try {
      await controlPlayback('stop');
    } catch (err) {
      console.error(err);
    }
  };

  const cycleRepeat = async () => {
    // 0 = Off, 1 = One, 2 = All
    const next = (repeatMode + 1) % 3;
    setRepeatMode(next);
    try {
      await controlPlayback('repeat', next);
    } catch (err) {
      console.error(err);
    }
  };

  const toggleAutoplay = async () => {
    const next = !autoplay;
    setAutoplay(next);
    try {
      await controlPlayback('autoplay', next);
    } catch (err) {
      console.error(err);
    }
  };

  const toggleCaching = async () => {
    const next = !caching;
    setCaching(next);
    try {
      await controlPlayback('caching', next);
    } catch (err) {
      console.error(err);
    }
  };

  const handleVolChange = (val) => {
    setVolume(val);
    setSpeakerVolume(val).catch(console.error);
  };

  const getRepeatLabel = () => {
    if (repeatMode === 1) return '🔁 Repeat 1';
    if (repeatMode === 2) return '🔁 Repeat All';
    return '🔁 Off';
  };

  const videoId = currentTrack?.id || currentTrack?.videoId;
  const thumbUrl = videoId
    ? `https://i.ytimg.com/vi/${videoId}/default.jpg`
    : "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='60' height='60' viewBox='0 0 60 60'%3E%3Crect width='60' height='60' fill='%23181824'/%3E%3Ctext x='50%25' y='50%25' font-size='24' text-anchor='middle' alignment-baseline='central' fill='%23555'%3E🎵%3C/text%3E%3C/svg%3E";

  return (
    <footer className="player-dock">
      {/* Track Info */}
      <div className="dock-track-info">
        <img
          src={thumbUrl}
          alt="Album Art"
          className="dock-art"
          onError={(e) => {
            e.target.src = "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='60' height='60' viewBox='0 0 60 60'%3E%3Crect width='60' height='60' fill='%23181824'/%3E%3Ctext x='50%25' y='50%25' font-size='24' text-anchor='middle' alignment-baseline='central' fill='%23555'%3E🎵%3C/text%3E%3C/svg%3E";
          }}
        />
        <div className="dock-text">
          <div className="dock-title-marquee">
            <span className="dock-title">
              {currentTrack?.title || 'No Active Playback'}
            </span>
          </div>
          <div className="dock-meta">
            <span className="dock-artist">
              {currentTrack?.artist || 'ESP32-S3 Idle'}
            </span>
            <span className="dock-state-badge">
              {playbackState || 'IDLE'}
            </span>
          </div>
        </div>
      </div>

      {/* Center Controls */}
      <div className="dock-center">
        <div className="dock-controls">
          <button className="btn-dock-ctrl" onClick={handlePrev} title="Previous Track">
            <SkipBack size={20} />
          </button>
          <button className="btn-dock-play" onClick={handleTogglePlayPause} title={isPlaying ? 'Pause' : 'Play'}>
            {isPlaying ? <Pause size={20} fill="#0b0c14" /> : <Play size={20} fill="#0b0c14" />}
          </button>
          <button className="btn-dock-ctrl" onClick={handleNext} title="Next Track">
            <SkipForward size={20} />
          </button>
          <button className="btn-dock-ctrl" onClick={handleStop} title="Stop Playback">
            <Square size={18} />
          </button>
        </div>

        <div className="dock-playback-toggles">
          <button
            className={`toggle-pill ${repeatMode > 0 ? 'active' : ''}`}
            onClick={cycleRepeat}
            title="Repeat Mode"
          >
            {getRepeatLabel()}
          </button>
          <button
            className={`toggle-pill ${autoplay ? 'active' : ''}`}
            onClick={toggleAutoplay}
            title="Autoplay Endless Mix"
          >
            📻 Autoplay
          </button>
          <button
            className={`toggle-pill ${caching ? 'active' : ''}`}
            onClick={toggleCaching}
            title="Cache Songs to SD Card"
          >
            💾 Cache to SD
          </button>
        </div>
      </div>

      {/* Right Controls */}
      <div className="dock-right">
        <div className="dock-vol">
          <span className="vol-icon">
            {volume > 0 ? <Volume2 size={18} /> : <VolumeX size={18} />}
          </span>
          <input
            type="range"
            min="0"
            max="100"
            value={volume}
            onChange={(e) => handleVolChange(Number(e.target.value))}
            className="styled-range mini"
          />
          <span className="vol-percent">{volume}%</span>
        </div>
      </div>
    </footer>
  );
}
