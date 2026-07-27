import { useState } from 'react';
import { Ros, Service, ServiceRequest } from 'roslib';

interface Device {
  name: string;
  model: string;
  ip: string;
  serial: string;
  mac: string;
}

export default function DevicePanel({ ros }: { ros: Ros | null }) {
  const [devices, setDevices] = useState<Device[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [searched, setSearched] = useState(false);

  const discover = () => {
    if (!ros || busy) return;
    setBusy(true);
    setError(null);
    const service = new Service({
      ros,
      name: '/ainstec_camera/discover',
      serviceType: 'ainstec_interfaces/srv/Discover',
    });
    service.callService(
      new ServiceRequest({ timeout_ms: 800 }),
      (resp: any) => {
        setBusy(false);
        setSearched(true);
        try {
          const parsed = JSON.parse(resp.devices_json);
          setDevices(Array.isArray(parsed) ? parsed : []);
        } catch (e) {
          setDevices([]);
          setError(`failed to parse devices_json: ${String(e)}`);
        }
      },
      (err: any) => {
        setBusy(false);
        setSearched(true);
        setError(typeof err === 'string' ? err : JSON.stringify(err));
      }
    );
  };

  return (
    <div className="panel-body">
      <div>
        <button onClick={discover} disabled={!ros || busy}>
          {busy ? 'Discovering…' : 'Discover'}
        </button>
      </div>
      {error && <p className="error-text">{error}</p>}
      {devices.length > 0 && (
        <table>
          <thead>
            <tr>
              <th>Name</th>
              <th>Model</th>
              <th>IP</th>
              <th>Serial</th>
              <th>MAC</th>
            </tr>
          </thead>
          <tbody>
            {devices.map((d, i) => (
              <tr key={d.serial || i}>
                <td>{d.name}</td>
                <td>{d.model}</td>
                <td>{d.ip}</td>
                <td>{d.serial}</td>
                <td>{d.mac}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      {!error && !busy && searched && devices.length === 0 && <p className="muted">No devices found.</p>}
      {!ros && <p className="muted">Connect to rosbridge first.</p>}
    </div>
  );
}
