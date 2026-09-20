import React from 'react';
import { Terminal } from 'lucide-react';

// Diagnostics, hidden behind developer mode. There is currently no log
// streaming endpoint wired up on the daemon or ESP HTTP surface (the old
// LiveLogs.jsx never actually received real logs either - `logs` was always
// an empty array in the old App.jsx). Rather than fake activity, this is an
// honest placeholder until a real log-streaming channel exists.
export default function DevLogs({ snapshot }) {
  return (
    <div className="dev-logs">
      <div className="dev-logs-header">
        <Terminal size={14} /> Diagnostics
      </div>
      <pre className="dev-logs-body">{JSON.stringify(snapshot, null, 2)}</pre>
      <p className="dev-logs-note">
        No live log-streaming channel exists yet between the daemon and this dashboard -
        this shows the raw WebSocket state snapshot instead of fabricated log lines.
      </p>
    </div>
  );
}
