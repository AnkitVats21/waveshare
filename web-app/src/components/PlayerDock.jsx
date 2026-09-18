import React from 'react';
import {
  Play,
  Pause,
  SkipBack,
  SkipForward,
  Square,
  Repeat,
  Repeat1,
  Radio,
  HardDrive,
  Volume2,
  Volume1,
  VolumeX,
} from 'lucide-react';
import { controlPlayback, setSpeakerVolume } from '../api';

const metadataCache = new Map();

async function resolveTrackInfo(id) {
  if (!id || id.length !== 11) return null;
  if (metadataCache.has(id)) return metadataCache.get(id);
  try {
    const res = await fetch(`https://noembed.com/embed?url=https://www.youtube.com/watch?v=${encodeURIComponent(id)}`);
    if (res.ok) {
      const data = await res.json();
      if (data && data.title) {
        const info = {
          title: data.title,
          artist: data.author_name || 'YouTube',
        };
        metadataCache.set(id, info);
        return info;
      }
    }
  } catch (e) {
    // Ignore network fetch errors
  }
  return null;
}

export default function PlayerDock({
  currentTrack,
  playbackState,
  positionMs = 0,
  durationMs = 0,
  seekable = false,
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
  const [isDragging, setIsDragging] = React.useState(false);
  const [dragPos, setDragPos] = React.useState(0);
  const [localPos, setLocalPos] = React.useState(0);
  const [prevVolume, setPrevVolume] = React.useState(volume || 80);
  const [resolvedTitle, setResolvedTitle] = React.useState('');
  const [resolvedArtist, setResolvedArtist] = React.useState('');

  const videoId = currentTrack?.id || currentTrack?.videoId;

  React.useEffect(() => {
    if (!videoId) {
      setResolvedTitle('');
      setResolvedArtist('');
      return;
    }
    const needsResolution = !currentTrack.artist || currentTrack.artist === 'Local Storage' || currentTrack.title === videoId;
    if (needsResolution && videoId.length === 11) {
      resolveTrackInfo(videoId).then((meta) => {
        if (meta) {
          setResolvedTitle(meta.title);
          setResolvedArtist(meta.artist);
        }
      });
    } else {
      setResolvedTitle('');
      setResolvedArtist('');
    }
  }, [videoId, currentTrack?.title, currentTrack?.artist]);

  // Sync with incoming position from ESP32 when not dragging
  React.useEffect(() => {
    if (!isDragging) {
      setLocalPos(positionMs || 0);
    }
  }, [positionMs, isDragging]);

  // Smooth local increment ticker when actively playing
  React.useEffect(() => {
    if (!isPlaying || isDragging) return;
    const interval = setInterval(() => {
      setLocalPos((prev) => {
        const total = durationMs || (currentTrack?.duration ? currentTrack.duration * 1000 : 0);
        if (total > 0 && prev >= total) return prev;
        return prev + 250;
      });
    }, 250);
    return () => clearInterval(interval);
  }, [isPlaying, isDragging, durationMs, currentTrack]);

  const totalDurMs = durationMs || (currentTrack?.duration ? currentTrack.duration * 1000 : 0);
  const hasDuration = totalDurMs > 0;
  const displayPos = isDragging ? dragPos : Math.min(localPos, hasDuration ? totalDurMs : localPos);
  const progressPct = hasDuration ? Math.min(100, Math.max(0, (displayPos / totalDurMs) * 100)) : 0;

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

  const handleSeekCommit = async () => {
    if (!hasDuration) return;
    setIsDragging(false);
    setLocalPos(dragPos);
    try {
      await controlPlayback('seek', dragPos);
    } catch (err) {
      console.error('Seek error:', err);
    }
  };

  const formatTime = (ms) => {
    if (!ms || isNaN(ms) || ms < 0) return '0:00';
    const totalSec = Math.floor(ms / 1000);
    const min = Math.floor(totalSec / 60);
    const sec = totalSec % 60;
    return `${min}:${sec < 10 ? '0' : ''}${sec}`;
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

  const getRepeatTitle = () => {
    if (repeatMode === 1) return 'Repeat: Current Track (Click to cycle)';
    if (repeatMode === 2) return 'Repeat: All Tracks (Click to cycle)';
    return 'Repeat: Off (Click to cycle)';
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
    if (val > 0) setPrevVolume(val);
    setSpeakerVolume(val).catch(console.error);
  };

  const handleToggleMute = () => {
    if (volume > 0) {
      setPrevVolume(volume);
      handleVolChange(0);
    } else {
      handleVolChange(prevVolume > 0 ? prevVolume : 80);
    }
  };

  const thumbUrl = videoId
    ? `https://i.ytimg.com/vi/${videoId}/default.jpg`
    : "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='60' height='60' viewBox='0 0 60 60'%3E%3Crect width='60' height='60' fill='%23181824'/%3E%3Ctext x='50%25' y='50%25' font-size='24' text-anchor='middle' alignment-baseline='central' fill='%23555'%3E🎵%3C/text%3E%3C/svg%3E";

  const getStateBadgeClass = () => {
    if (playbackState === 'STREAMING') return 'bg-teal-500/15 text-teal-300 border-teal-500/30 shadow-[0_0_8px_rgba(45,212,191,0.2)]';
    if (playbackState === 'LOCAL') return 'bg-emerald-500/15 text-emerald-300 border-emerald-500/30 shadow-[0_0_8px_rgba(16,185,129,0.2)]';
    if (playbackState === 'PAUSED') return 'bg-amber-500/15 text-amber-300 border-amber-500/30';
    return 'bg-slate-800 text-slate-400 border-slate-700';
  };

  return (
    <footer className="fixed bottom-0 left-0 right-0 h-20 bg-slate-950/95 backdrop-blur-2xl border-t border-slate-800/80 shadow-[0_-8px_32px_rgba(0,0,0,0.6)] flex items-center justify-between px-4 sm:px-8 z-50">
      {/* Track Info */}
      <div className="flex items-center gap-3 w-56 sm:w-72 min-w-0">
        <img
          src={thumbUrl}
          alt="Album Art"
          className="w-12 h-12 rounded-lg object-cover border border-white/10 shadow-md shrink-0"
          onError={(e) => {
            e.target.src = "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='60' height='60' viewBox='0 0 60 60'%3E%3Crect width='60' height='60' fill='%23181824'/%3E%3Ctext x='50%25' y='50%25' font-size='24' text-anchor='middle' alignment-baseline='central' fill='%23555'%3E🎵%3C/text%3E%3C/svg%3E";
          }}
        />
        <div className="flex flex-col min-w-0 overflow-hidden gap-0.5">
          <div className="overflow-hidden text-ellipsis whitespace-nowrap">
            <span
              className="text-sm font-semibold text-slate-100 truncate block"
              title={resolvedTitle || currentTrack?.title || 'No Active Playback'}
            >
              {resolvedTitle || currentTrack?.title || 'No Active Playback'}
            </span>
          </div>
          <div className="flex items-center gap-2">
            <span
              className="text-xs text-slate-400 truncate block max-w-[140px]"
              title={resolvedArtist || currentTrack?.artist || 'ESP32-S3 Idle'}
            >
              {resolvedArtist || currentTrack?.artist || 'ESP32-S3 Idle'}
            </span>
            <span className={`text-[10px] font-mono font-bold tracking-wider px-1.5 py-0.5 rounded-full uppercase border shrink-0 ${getStateBadgeClass()}`}>
              {playbackState || 'IDLE'}
            </span>
          </div>
        </div>
      </div>

      {/* Center Controls & Scrubber */}
      <div className="flex flex-col items-center gap-1.5 w-full max-w-xl mx-2 sm:mx-6">
        <div className="flex items-center gap-3 sm:gap-4">
          {/* Repeat */}
          <button
            className={`relative flex items-center justify-center w-8 h-8 rounded-full transition-all ${
              repeatMode > 0
                ? 'text-teal-300 bg-teal-500/10 hover:bg-teal-500/20'
                : 'text-slate-400 hover:text-white hover:bg-white/10'
            }`}
            onClick={cycleRepeat}
            title={getRepeatTitle()}
          >
            {repeatMode === 1 ? <Repeat1 size={17} /> : <Repeat size={17} />}
            {repeatMode > 0 && <span className="absolute bottom-1 left-1/2 -translate-x-1/2 w-1 h-1 rounded-full bg-teal-400 shadow-[0_0_6px_#2dd4bf]" />}
          </button>

          {/* Prev */}
          <button
            className="flex items-center justify-center w-8 h-8 rounded-full text-slate-300 hover:text-white hover:bg-white/10 transition-all hover:scale-105 active:scale-95"
            onClick={handlePrev}
            title="Previous"
          >
            <SkipBack size={18} />
          </button>

          {/* Play/Pause Hero */}
          <button
            className="flex items-center justify-center w-10 h-10 rounded-full bg-white text-slate-950 shadow-md hover:shadow-white/20 hover:scale-105 active:scale-95 transition-all"
            onClick={handleTogglePlayPause}
            title={isPlaying ? 'Pause' : 'Play'}
          >
            {isPlaying ? (
              <Pause size={19} fill="#0b0c14" />
            ) : (
              <Play size={19} fill="#0b0c14" className="ml-0.5" />
            )}
          </button>

          {/* Next */}
          <button
            className="flex items-center justify-center w-8 h-8 rounded-full text-slate-300 hover:text-white hover:bg-white/10 transition-all hover:scale-105 active:scale-95"
            onClick={handleNext}
            title="Next"
          >
            <SkipForward size={18} />
          </button>

          {/* Stop */}
          <button
            className="flex items-center justify-center w-8 h-8 rounded-full text-slate-300 hover:text-white hover:bg-white/10 transition-all hover:scale-105 active:scale-95"
            onClick={handleStop}
            title="Stop"
          >
            <Square size={15} />
          </button>

          <div className="w-px h-4 bg-white/15 mx-1" />

          {/* Autoplay Toggle */}
          <button
            className={`relative flex items-center justify-center w-8 h-8 rounded-full transition-all ${
              autoplay
                ? 'text-teal-300 bg-teal-500/10 hover:bg-teal-500/20'
                : 'text-slate-400 hover:text-white hover:bg-white/10'
            }`}
            onClick={toggleAutoplay}
            title={autoplay ? 'Autoplay: ON (Continuous Mix)' : 'Autoplay: OFF'}
          >
            <Radio size={17} />
            {autoplay && <span className="absolute bottom-1 left-1/2 -translate-x-1/2 w-1 h-1 rounded-full bg-teal-400 shadow-[0_0_6px_#2dd4bf]" />}
          </button>

          {/* Cache to SD Toggle */}
          <button
            className={`relative flex items-center justify-center w-8 h-8 rounded-full transition-all ${
              caching
                ? 'text-teal-300 bg-teal-500/10 hover:bg-teal-500/20'
                : 'text-slate-400 hover:text-white hover:bg-white/10'
            }`}
            onClick={toggleCaching}
            title={caching ? 'Auto-Cache to SD: ON' : 'Auto-Cache to SD: OFF'}
          >
            <HardDrive size={17} />
            {caching && <span className="absolute bottom-1 left-1/2 -translate-x-1/2 w-1 h-1 rounded-full bg-teal-400 shadow-[0_0_6px_#2dd4bf]" />}
          </button>
        </div>

        {/* Scrubber Bar */}
        <div className="flex items-center gap-2.5 w-full max-w-md">
          <span className="font-mono text-[11px] text-slate-400 w-10 text-center select-none shrink-0">
            {formatTime(displayPos)}
          </span>
          <div className="scrubber-slider-wrap">
            <input
              type="range"
              min="0"
              max={hasDuration ? totalDurMs : 100}
              value={hasDuration ? Math.min(displayPos, totalDurMs) : 0}
              disabled={!hasDuration || (!seekable && !isPlaying)}
              style={{ '--slider-fill': `${progressPct}%` }}
              onMouseDown={() => {
                if (hasDuration) {
                  setIsDragging(true);
                  setDragPos(displayPos);
                }
              }}
              onTouchStart={() => {
                if (hasDuration) {
                  setIsDragging(true);
                  setDragPos(displayPos);
                }
              }}
              onChange={(e) => setDragPos(Number(e.target.value))}
              onMouseUp={handleSeekCommit}
              onTouchEnd={handleSeekCommit}
              className="styled-range scrubber-range"
              title={hasDuration ? 'Seek playback' : 'Streaming (Live position)'}
            />
          </div>
          <span className="font-mono text-[11px] text-slate-400 w-10 text-center select-none shrink-0">
            {hasDuration ? formatTime(totalDurMs) : '--:--'}
          </span>
        </div>
      </div>

      {/* Right Controls */}
      <div className="hidden md:flex items-center justify-end w-56 sm:w-72">
        <div className="flex items-center gap-2 w-44">
          <button
            className="flex items-center justify-center w-7 h-7 rounded-full text-slate-400 hover:text-white hover:bg-white/10 transition-all shrink-0"
            onClick={handleToggleMute}
            title={volume === 0 ? 'Unmute' : 'Mute'}
          >
            {volume === 0 ? (
              <VolumeX size={17} />
            ) : volume < 50 ? (
              <Volume1 size={17} />
            ) : (
              <Volume2 size={17} />
            )}
          </button>
          <div className="vol-slider-wrap">
            <input
              type="range"
              min="0"
              max="100"
              value={volume}
              style={{ '--slider-fill': `${volume}%` }}
              onChange={(e) => handleVolChange(Number(e.target.value))}
              className="styled-range mini"
              title="Speaker Volume"
            />
          </div>
          <span className="font-mono text-xs text-slate-400 w-9 text-right select-none shrink-0">
            {volume}%
          </span>
        </div>
      </div>
    </footer>
  );
}
