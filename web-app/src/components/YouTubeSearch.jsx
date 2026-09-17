import React, { useState } from 'react';
import { Search, Play, Music, Radio, Loader2 } from 'lucide-react';
import { searchYouTube, resolveStreamUrl, playDirectOnEsp } from '../api';

const QUICK_VIBES = [
  { label: '🎧 Lofi Chill', query: 'Lofi hip hop beats to relax' },
  { label: '🌆 Synthwave', query: 'Synthwave retrowave radio' },
  { label: '🎸 Coldplay', query: 'Coldplay top hits' },
  { label: '🎬 Hans Zimmer', query: 'Hans Zimmer cinematic sound' },
  { label: '🎵 A.R. Rahman', query: 'A.R. Rahman all time hits' },
  { label: '⚡ Electro House', query: 'Electro house workout mix' },
];

export default function YouTubeSearch({ onTrackStarted }) {
  const [query, setQuery] = useState('');
  const [results, setResults] = useState([]);
  const [isSearching, setIsSearching] = useState(false);
  const [resolvingId, setResolvingId] = useState(null);
  const [statusMsg, setStatusMsg] = useState('');

  const handleSearch = async (searchTerm) => {
    const q = (searchTerm !== undefined ? searchTerm : query).trim();
    if (!q) return;

    setIsSearching(true);
    setStatusMsg('Searching via stream.ankitm.xyz...');
    try {
      const data = await searchYouTube(q);
      setResults(Array.isArray(data) ? data : []);
      setStatusMsg('');
    } catch (err) {
      console.error(err);
      setStatusMsg(`Search failed: ${err.message}`);
    } finally {
      setIsSearching(false);
    }
  };

  const handlePlay = async (track) => {
    setResolvingId(track.videoId);
    setStatusMsg(`Resolving Opus stream for "${track.title}"...`);
    try {
      const streamUrl = await resolveStreamUrl(track.videoId);
      setStatusMsg(`Dispatching Opus stream to ESP32...`);
      await playDirectOnEsp(track, streamUrl);
      setStatusMsg('');
      if (onTrackStarted) onTrackStarted(track);
    } catch (err) {
      console.error(err);
      setStatusMsg(`Playback error: ${err.message}`);
    } finally {
      setResolvingId(null);
    }
  };

  const formatDuration = (seconds) => {
    if (!seconds) return '0:00';
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    return `${m}:${s.toString().padStart(2, '0')}`;
  };

  return (
    <div className="search-tab-view">
      <div className="search-header-container">
        <form
          className="search-bar-form"
          onSubmit={(e) => {
            e.preventDefault();
            handleSearch();
          }}
        >
          <span className="search-prefix">⚡</span>
          <input
            type="text"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder="Search songs, artists, albums, or paste YouTube link..."
          />
          <button type="submit" className="btn-primary" disabled={isSearching}>
            {isSearching ? <Loader2 size={18} className="animate-spin" /> : <Search size={18} />}
            <span>Search</span>
          </button>
        </form>

        <div className="quick-tags">
          <span className="tag-title">Quick Vibes:</span>
          {QUICK_VIBES.map((v) => (
            <button
              key={v.query}
              className="vibe-tag"
              onClick={() => {
                setQuery(v.query);
                handleSearch(v.query);
              }}
            >
              {v.label}
            </button>
          ))}
        </div>
      </div>

      {statusMsg && (
        <div className="status-banner">
          <span className="spinner" />
          <span>{statusMsg}</span>
        </div>
      )}

      <div className="results-grid">
        {results.length > 0 ? (
          results.map((track) => (
            <div key={track.videoId} className="song-card">
              <div className="card-thumb-wrap">
                <img
                  src={`https://i.ytimg.com/vi/${track.videoId}/mqdefault.jpg`}
                  alt={track.title}
                  loading="lazy"
                  onError={(e) => {
                    e.target.src = `https://img.youtube.com/vi/${track.videoId}/0.jpg`;
                  }}
                />
                <span className="card-duration">{formatDuration(track.lengthSeconds)}</span>
              </div>
              <div className="card-body">
                <h4 className="card-title" title={track.title}>{track.title}</h4>
                <p className="card-artist">{track.author}</p>
                <div className="card-actions">
                  <button
                    className="btn-card-play"
                    onClick={() => handlePlay(track)}
                    disabled={resolvingId === track.videoId}
                  >
                    {resolvingId === track.videoId ? (
                      <>
                        <span className="spinner" /> Resolving...
                      </>
                    ) : (
                      <>
                        <Play size={16} fill="currentColor" /> Play on Waveshare
                      </>
                    )}
                  </button>
                </div>
              </div>
            </div>
          ))
        ) : (
          <div className="empty-state">
            <div className="empty-icon">🎵</div>
            <h3>Stream Any Music Directly to Waveshare ESP32-S3</h3>
            <p>
              Opus stream URLs are resolved directly by your browser via{' '}
              <code className="code-pill">stream.ankitm.xyz</code>, leaving Core 0 and Core 1 free for zero-jitter
              DMA playback.
            </p>
          </div>
        )}
      </div>
    </div>
  );
}
