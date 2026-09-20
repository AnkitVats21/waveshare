import React, { useState, useEffect } from 'react';
import {
  Play, Pause, SkipBack, SkipForward, Square, Repeat, Repeat1, Radio, HardDrive,
  Volume2, Volume1, VolumeX,
} from 'lucide-react';
import CommitSlider from './CommitSlider';
import { resolveTrackInfo, needsResolution } from '../lib/trackMetadata';

const FALLBACK_ART = "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='60' height='60' viewBox='0 0 60 60'%3E%3Crect width='60' height='60' fill='%23181824'/%3E%3Ctext x='50%25' y='50%25' font-size='24' text-anchor='middle' alignment-baseline='central' fill='%23555'%3E%F0%9F%8E%B5%3C/text%3E%3C/svg%3E";

function formatTime(ms) {
  if (!ms || Number.isNaN(ms) || ms < 0) return '0:00';
  const totalSec = Math.floor(ms / 1000);
  const min = Math.floor(totalSec / 60);
  const sec = totalSec % 60;
  return `${min}:${sec < 10 ? '0' : ''}${sec}`;
}

export default function NowPlayingDock({ music, online, send, volume, repeat, autoplay, caching }) {
  const track = music.current_track || {};
  const videoId = track.id;
  const isPlaying = music.state === 'PLAYING';
  const isBusy = music.state === 'RESOLVING' || music.state === 'BUFFERING';

  const [resolved, setResolved] = useState(null);
  useEffect(() => {
    if (needsResolution(track)) {
      let cancelled = false;
      resolveTrackInfo(videoId).then((meta) => {
        if (!cancelled && meta) setResolved(meta);
      });
      return () => { cancelled = true; };
    }
    setResolved(null);
  }, [videoId, track.title, track.artist]);

  const displayTitle = resolved?.title || track.title || 'No Active Playback';
  const displayArtist = resolved?.artist || track.artist || 'ESP32-S3 Idle';

  // Local scrub position, ticking while playing, synced from server when not dragging.
  const [localPos, setLocalPos] = useState(music.position_ms || 0);
  const [dragging, setDragging] = useState(false);
  useEffect(() => {
    if (!dragging) setLocalPos(music.position_ms || 0);
  }, [music.position_ms, dragging]);
  useEffect(() => {
    if (!isPlaying || dragging) return;
    const id = setInterval(() => {
      setLocalPos((prev) => (music.duration_ms > 0 && prev >= music.duration_ms ? prev : prev + 250));
    }, 250);
    return () => clearInterval(id);
  }, [isPlaying, dragging, music.duration_ms]);

  const hasDuration = music.duration_ms > 0;

  const disabled = !online;

  const [prevVolume, setPrevVolume] = useState(volume.value || 80);

  const cycleRepeat = () => {
    if (disabled) return;
    repeat.commit((repeat.value + 1) % 3);
  };
  const toggleAutoplay = () => { if (!disabled) autoplay.commit(!autoplay.value); };
  const toggleCaching = () => { if (!disabled) caching.commit(!caching.value); };

  const act = (action, value) => { if (!disabled) send('action', { action, ...(value !== undefined ? { value } : {}) }); };

  const handleVolChange = (v) => {
    if (disabled) return;
    if (v > 0) setPrevVolume(v);
    volume.commit(v);
  };
  const toggleMute = () => {
    if (disabled) return;
    if (volume.value > 0) {
      setPrevVolume(volume.value);
      volume.commit(0);
    } else {
      volume.commit(prevVolume > 0 ? prevVolume : 80);
    }
  };

  const repeatTitle = repeat.value === 1 ? 'Repeat: current track' : repeat.value === 2 ? 'Repeat: all tracks' : 'Repeat: off';

  return (
    <footer className={`now-playing-dock ${disabled ? 'dock-disabled' : ''}`}>
      <div className="dock-track">
        <img
          src={videoId ? `https://i.ytimg.com/vi/${videoId}/default.jpg` : FALLBACK_ART}
          alt=""
          className="dock-art"
          onError={(e) => { e.target.src = FALLBACK_ART; }}
        />
        <div className="dock-track-text">
          <span className="dock-title" title={displayTitle}>{displayTitle}</span>
          <div className="dock-subline">
            <span className="dock-artist" title={displayArtist}>{displayArtist}</span>
            <span className={`state-pill state-${music.state?.toLowerCase() || 'idle'}`}>{music.state || 'IDLE'}</span>
          </div>
        </div>
      </div>

      <div className="dock-center">
        <div className="dock-transport">
          <button
            className={`icon-btn ${repeat.value > 0 ? 'icon-btn-active' : ''}`}
            onClick={cycleRepeat}
            title={repeatTitle}
            disabled={disabled}
          >
            {repeat.value === 1 ? <Repeat1 size={18} /> : <Repeat size={18} />}
          </button>
          <button className="icon-btn" onClick={() => act('prev')} title="Previous" disabled={disabled}>
            <SkipBack size={19} />
          </button>
          <button
            className="icon-btn icon-btn-hero"
            onClick={() => act(isPlaying ? 'pause' : 'resume')}
            title={isPlaying ? 'Pause' : 'Play'}
            disabled={disabled}
          >
            {isPlaying ? <Pause size={20} fill="currentColor" /> : <Play size={20} fill="currentColor" />}
          </button>
          <button className="icon-btn" onClick={() => act('next')} title="Next" disabled={disabled}>
            <SkipForward size={19} />
          </button>
          <button className="icon-btn" onClick={() => act('stop')} title="Stop" disabled={disabled}>
            <Square size={16} />
          </button>
          <div className="dock-sep" />
          <button
            className={`icon-btn ${autoplay.value ? 'icon-btn-active' : ''}`}
            onClick={toggleAutoplay}
            title={autoplay.value ? 'Autoplay: on' : 'Autoplay: off'}
            disabled={disabled}
          >
            <Radio size={18} />
          </button>
          <button
            className={`icon-btn ${caching.value ? 'icon-btn-active' : ''}`}
            onClick={toggleCaching}
            title={caching.value ? 'Cache to SD: on' : 'Cache to SD: off'}
            disabled={disabled}
          >
            <HardDrive size={18} />
          </button>
        </div>

        <div className="dock-scrub">
          <span className="mono-time">{formatTime(dragging ? localPos : localPos)}</span>
          <CommitSlider
            min={0}
            max={hasDuration ? music.duration_ms : 100}
            value={hasDuration ? Math.min(localPos, music.duration_ms) : 0}
            disabled={disabled || (!hasDuration || (!music.seekable && !isPlaying))}
            className="scrub-slider"
            onMouseDown={() => hasDuration && setDragging(true)}
            onTouchStart={() => hasDuration && setDragging(true)}
            onCommit={(v) => {
              setDragging(false);
              setLocalPos(v);
              act('seek', v);
            }}
          />
          <span className="mono-time">{hasDuration ? formatTime(music.duration_ms) : '--:--'}</span>
        </div>
      </div>

      <div className="dock-volume">
        <button className="icon-btn" onClick={toggleMute} title={volume.value === 0 ? 'Unmute' : 'Mute'} disabled={disabled}>
          {volume.value === 0 ? <VolumeX size={18} /> : volume.value < 50 ? <Volume1 size={18} /> : <Volume2 size={18} />}
        </button>
        <CommitSlider
          min={0}
          max={100}
          value={volume.value}
          disabled={disabled}
          className="volume-slider"
          onCommit={handleVolChange}
        />
        <span className="mono-time vol-pct">{volume.value}%</span>
        {isBusy && <span className="busy-dot" title={music.state} />}
      </div>
    </footer>
  );
}
