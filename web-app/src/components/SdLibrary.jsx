import React, { useState, useEffect } from 'react';
import { RefreshCw, Play, Trash2, FileAudio, Search } from 'lucide-react';
import { getMusicLibrary, scanMusicLibrary, deleteFromLibrary, playLocalOnEsp } from '../lib/api';
import { resolveTrackInfo, needsResolution } from '../lib/trackMetadata';

function formatDuration(seconds) {
  if (!seconds) return '--:--';
  const m = Math.floor(seconds / 60);
  const s = Math.floor(seconds % 60);
  return `${m}:${s.toString().padStart(2, '0')}`;
}
function formatSize(bytes) {
  if (!bytes) return '';
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export default function SdLibrary({ onPlayLocal, disabled, onLibraryCount }) {
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
      onLibraryCount?.(list.length);

      list.filter(needsResolution).forEach(async (t) => {
        const meta = await resolveTrackInfo(t.id);
        if (meta) {
          setTracks((prev) => prev.map((item) => (item.id === t.id ? { ...item, ...meta } : item)));
        }
      });
    } catch (err) {
      setMsg(`Failed to load SD library: ${err.message}`);
    } finally {
      setIsLoading(false);
    }
  };

  useEffect(() => { fetchLibrary(); }, []);

  const handleScan = async () => {
    setIsScanning(true);
    setMsg('Scanning /sdcard/music...');
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

  const handlePlay = async (track) => {
    setMsg(`Playing "${track.title}" from SD card...`);
    try {
      await playLocalOnEsp(track.id || track.filePath);
      onPlayLocal?.(track);
      setMsg(`Now playing: ${track.title}`);
    } catch (err) {
      setMsg(`Playback failed: ${err.message}`);
    }
  };

  const handleDelete = async (track) => {
    if (!window.confirm(`Delete "${track.title}" from SD card?`)) return;
    try {
      await deleteFromLibrary(track.id);
      setMsg(`Deleted "${track.title}".`);
      fetchLibrary();
    } catch (err) {
      setMsg(`Delete failed: ${err.message}`);
    }
  };

  return (
    <div className="library-view">
      <div className="library-toolbar">
        <div className="search-field">
          <Search size={15} />
          <input
            type="text"
            value={filter}
            onChange={(e) => { setFilter(e.target.value); fetchLibrary(e.target.value); }}
            placeholder="Filter cached songs..."
          />
        </div>
        <button className="btn-secondary" onClick={handleScan} disabled={isScanning}>
          <RefreshCw size={15} className={isScanning ? 'spin' : ''} /> Scan & sync
        </button>
        <button className="btn-ghost" onClick={() => fetchLibrary()} disabled={isLoading}>Refresh</button>
      </div>

      <div className="library-meta">{tracks.length} tracks cached on SD card</div>

      {msg && <div className="status-banner">{msg}</div>}

      <div className="track-list">
        {tracks.length > 0 ? (
          tracks.map((t) => (
            <div key={t.id} className="track-row">
              <div className="track-left">
                {t.id && t.id.length === 11 ? (
                  <img
                    className="track-thumb"
                    src={`https://i.ytimg.com/vi/${t.id}/default.jpg`}
                    alt=""
                    onError={(e) => { e.target.style.visibility = 'hidden'; }}
                  />
                ) : (
                  <span className="track-icon"><FileAudio size={20} /></span>
                )}
                <div className="track-text">
                  <span className="track-title" title={t.title}>{t.title}</span>
                  <span className="track-artist">{t.artist || 'Local track'}</span>
                </div>
              </div>
              <div className="track-right">
                <span className="mono-value">{t.format || 'opus'}</span>
                {t.sizeBytes > 0 && <span className="mono-value">{formatSize(t.sizeBytes)}</span>}
                <span className="mono-value">{formatDuration(t.durationSeconds)}</span>
                <button className="icon-btn" onClick={() => handlePlay(t)} disabled={disabled} title="Play from SD">
                  <Play size={16} fill="currentColor" />
                </button>
                <button className="icon-btn icon-btn-danger" onClick={() => handleDelete(t)} title="Delete">
                  <Trash2 size={16} />
                </button>
              </div>
            </div>
          ))
        ) : (
          <div className="empty-state">
            <h3>SD card library is empty</h3>
            <p>Songs cached while streaming show up here, or place files on the SD card and scan.</p>
          </div>
        )}
      </div>
    </div>
  );
}
