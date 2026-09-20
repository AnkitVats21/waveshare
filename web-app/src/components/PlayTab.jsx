import React, { useState } from 'react';
import { Search, HardDrive } from 'lucide-react';
import YouTubeSearch from './YouTubeSearch';
import SdLibrary from './SdLibrary';

// "Play" merges YouTube search and the SD library behind one sub-toggle -
// they're the same mental task (pick a song, send it to the device), not
// two separate top-level concerns.
export default function PlayTab({ onPlay, onPlayLocal, disabled, libraryCount, onLibraryCount }) {
  const [mode, setMode] = useState('search');

  return (
    <div className="play-tab">
      <div className="sub-toggle">
        <button className={mode === 'search' ? 'active' : ''} onClick={() => setMode('search')}>
          <Search size={14} /> Search
        </button>
        <button className={mode === 'library' ? 'active' : ''} onClick={() => setMode('library')}>
          <HardDrive size={14} /> SD library
          {libraryCount > 0 && <span className="count-badge">{libraryCount}</span>}
        </button>
      </div>

      {mode === 'search' ? (
        <YouTubeSearch onPlay={onPlay} disabled={disabled} />
      ) : (
        <SdLibrary onPlayLocal={onPlayLocal} disabled={disabled} onLibraryCount={onLibraryCount} />
      )}
    </div>
  );
}
