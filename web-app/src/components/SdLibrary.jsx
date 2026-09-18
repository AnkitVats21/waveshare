import React, { useState, useEffect } from 'react';
import { RefreshCw, Play, Trash2, HardDrive, FileAudio, Search } from 'lucide-react';
import { getMusicLibrary, scanMusicLibrary, deleteFromLibrary, playLocalOnEsp } from '../api';

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
    // Ignore offline/network fetch issues
  }
  return null;
}

export default function SdLibrary({ onTrackStarted, libraryCount, setLibraryCount }) {
  const [tracks, setTracks] = useState([]);
  const [filter, setFilter] = useState('');
  const [isLoading, setIsLoading] = useState(false);
  const [isScanning, setIsScanning] = useState(false);
  const [msg, setMsg] = useState('');

  const fetchLibrary = async (searchFilter = filter) => {
    setIsLoading(true);
    try {
      const data = await getMusicLibrary(searchFilter);
      const list = Array.isArray(data) ? data : (data.tracks || []);
      setTracks(list);
      if (setLibraryCount) setLibraryCount(list.length);

      // Asynchronously enrich tracks showing raw IDs with true YouTube song titles & artists
      const unnameds = list.filter((t) => !t.artist || t.artist === 'Local Storage' || t.title === t.id);
      if (unnameds.length > 0) {
        unnameds.forEach(async (t) => {
          const meta = await resolveTrackInfo(t.id);
          if (meta) {
            setTracks((prev) =>
              prev.map((item) =>
                item.id === t.id ? { ...item, title: meta.title, artist: meta.artist } : item
              )
            );
          }
        });
      }
    } catch (err) {
      console.error(err);
      setMsg(`Failed to load SD library: ${err.message}`);
    } finally {
      setIsLoading(false);
    }
  };

  useEffect(() => {
    fetchLibrary();
  }, []);

  const handleScan = async () => {
    setIsScanning(true);
    setMsg('Scanning /sdcard/music for new files and syncing catalog.db...');
    try {
      const res = await scanMusicLibrary();
      setMsg(`Scan complete: ${res.scanned_count || 0} tracks indexed.`);
      await fetchLibrary();
    } catch (err) {
      setMsg(`Scan failed: ${err.message}`);
    } finally {
      setIsScanning(false);
    }
  };

  const handlePlayLocal = async (track) => {
    setMsg(`Playing "${track.title}" directly from SD card (0ms buffer)...`);
    try {
      await playLocalOnEsp(track.id || track.filePath);
      if (onTrackStarted) onTrackStarted(track);
      setMsg(`Now playing from SD card: ${track.title}`);
    } catch (err) {
      setMsg(`Playback failed: ${err.message}`);
    }
  };

  const handleDelete = async (track) => {
    if (!confirm(`Delete "${track.title}" from SD card?`)) return;
    try {
      await deleteFromLibrary(track.id);
      setMsg(`Deleted "${track.title}" from SD card.`);
      fetchLibrary();
    } catch (err) {
      setMsg(`Delete failed: ${err.message}`);
    }
  };

  const formatDuration = (seconds) => {
    if (!seconds) return '--:--';
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    return `${m}:${s.toString().padStart(2, '0')}`;
  };

  const formatSize = (bytes) => {
    if (!bytes) return '';
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  };

  return (
    <div className="sd-library-view">
      <div className="library-toolbar">
        <div className="lib-search-box">
          <Search size={16} className="text-muted" />
          <input
            type="text"
            value={filter}
            onChange={(e) => {
              setFilter(e.target.value);
              fetchLibrary(e.target.value);
            }}
            placeholder="Filter cached songs by title or artist..."
          />
        </div>

        <div className="lib-actions">
          <button className="btn-secondary" onClick={handleScan} disabled={isScanning}>
            <RefreshCw size={16} className={isScanning ? 'animate-spin' : ''} />
            <span>Scan & Sync SD</span>
          </button>
          <button className="btn-ghost" onClick={() => fetchLibrary()} disabled={isLoading}>
            <span>Refresh</span>
          </button>
        </div>
      </div>

      <div className="lib-meta-bar">
        <span>{tracks.length} tracks cached on SD card</span>
        <span className="lib-path-hint">📁 Storage path: <code>/sdcard/music/catalog.db</code></span>
      </div>

      {msg && (
        <div className="status-banner">
          <span>{msg}</span>
        </div>
      )}

      <div className="library-container">
        <div className="track-table">
          {tracks.length > 0 ? (
            tracks.map((t) => (
              <div key={t.id} className="track-row">
                <div className="track-left">
                  {t.id && t.id.length === 11 ? (
                    <img
                      src={`https://i.ytimg.com/vi/${t.id}/default.jpg`}
                      alt=""
                      className="track-thumb-mini"
                      onError={(e) => { e.target.style.display = 'none'; }}
                    />
                  ) : (
                    <span className="track-icon">
                      <FileAudio size={22} />
                    </span>
                  )}
                  <div className="track-details">
                    <span className="track-title" title={t.title}>{t.title}</span>
                    <span className="track-artist">{t.artist || 'Local Track'}</span>
                  </div>
                </div>

                <div className="track-meta">
                  <span className="track-format-tag">{t.format || 'opus'}</span>
                  {t.sizeBytes > 0 && <span>{formatSize(t.sizeBytes)}</span>}
                  <span>{formatDuration(t.durationSeconds)}</span>
                  <div className="track-btns">
                    <button
                      className="btn-play-mini"
                      onClick={() => handlePlayLocal(t)}
                      title="Play from SD Card"
                    >
                      <Play size={14} fill="currentColor" /> Play Local
                    </button>
                    <button
                      className="btn-del-mini"
                      onClick={() => handleDelete(t)}
                      title="Delete from SD Card"
                    >
                      <Trash2 size={14} />
                    </button>
                  </div>
                </div>
              </div>
            ))
          ) : (
            <div className="empty-state">
              <div className="empty-icon">💾</div>
              <h3>SD Card Library is Empty</h3>
              <p>
                Songs streamed from the YouTube tab are automatically cached to{' '}
                <code className="code-pill">/sdcard/music/</code> when caching is enabled.
                You can also place <code className="code-pill">.opus</code> or <code className="code-pill">.wav</code> files on the SD card and click <strong>Scan & Sync SD</strong>!
              </p>
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
