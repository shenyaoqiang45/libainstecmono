import { useState } from 'react';
import { Ros, Service, ServiceRequest } from 'roslib';

export default function CapturePanel({ ros }: { ros: Ros | null }) {
  const [result, setResult] = useState<string | null>(null);
  const [ok, setOk] = useState<boolean | null>(null);
  const [busy, setBusy] = useState(false);

  const capture = () => {
    if (!ros || busy) return;
    setBusy(true);
    const service = new Service({
      ros,
      name: '/ainstec_camera/capture',
      serviceType: 'std_srvs/srv/Trigger',
    });
    service.callService(
      new ServiceRequest({}),
      (resp: any) => {
        setBusy(false);
        setOk(!!resp.success);
        setResult(resp.message || (resp.success ? 'OK' : 'capture failed'));
      },
      (err: any) => {
        setBusy(false);
        setOk(false);
        setResult(typeof err === 'string' ? err : JSON.stringify(err));
      }
    );
  };

  return (
    <div className="panel-body">
      <div>
        <button onClick={capture} disabled={!ros || busy}>
          {busy ? 'Capturing…' : 'Capture'}
        </button>
      </div>
      {result && <p className={ok ? 'ok-text' : 'error-text'}>{result}</p>}
      {!ros && <p className="muted">Connect to rosbridge first.</p>}
    </div>
  );
}
