import { useState } from 'react';
import { connect, disconnect, getUrl, useRosState } from '../ros';

export default function ConnectionBar() {
  const { status } = useRosState();
  const [url, setUrl] = useState(getUrl());

  const dotClass = status === 'connected' ? 'dot ok' : status === 'connecting' ? 'dot warn' : 'dot err';

  return (
    <div className="connection-bar">
      <span className={dotClass} title={status} />
      <span className="status-text">{status}</span>
      <input
        value={url}
        onChange={(e) => setUrl(e.target.value)}
        spellCheck={false}
        aria-label="rosbridge url"
      />
      {status === 'connected' || status === 'connecting' ? (
        <button onClick={() => disconnect()}>Disconnect</button>
      ) : (
        <button onClick={() => connect(url)}>Connect</button>
      )}
    </div>
  );
}
