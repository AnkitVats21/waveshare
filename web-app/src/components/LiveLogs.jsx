import React, { useRef, useEffect } from 'react';
import { Terminal, Trash2 } from 'lucide-react';

export default function LiveLogs({ logs, onClearLogs, autoScroll, onToggleAutoScroll }) {
  const terminalRef = useRef(null);

  useEffect(() => {
    if (autoScroll && terminalRef.current) {
      terminalRef.current.scrollTop = terminalRef.current.scrollHeight;
    }
  }, [logs, autoScroll]);

  return (
    <div className="terminal-container">
      <div className="terminal-toolbar">
        <span className="term-title">
          <Terminal size={14} style={{ display: 'inline', marginRight: 6 }} />
          ESP-IDF Kernel Stream (Embedded SysDb & LogRouter)
        </span>
        <div className="term-actions">
          <button className="btn-ghost" onClick={onClearLogs}>
            <Trash2 size={14} /> Clear
          </button>
          <label className="auto-scroll-label">
            <input
              type="checkbox"
              checked={autoScroll}
              onChange={(e) => onToggleAutoScroll(e.target.checked)}
            />{' '}
            Auto-scroll
          </label>
        </div>
      </div>
      <pre ref={terminalRef} className="terminal-body">
        {logs.length > 0
          ? logs.join('\n')
          : 'Connecting to ESP32 live stream log router...\n'}
      </pre>
    </div>
  );
}
