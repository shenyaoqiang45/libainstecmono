import { useEffect, useState } from 'react';
import { Ros } from 'roslib';
import { connect, useRosState } from './ros';
import ConnectionBar from './components/ConnectionBar';
import DevicePanel from './components/DevicePanel';
import CapturePanel from './components/CapturePanel';
import DepthView from './components/DepthView';
import IrView from './components/IrView';
import RgbView from './components/RgbView';
import ParamsPanel from './components/ParamsPanel';
import PointCloudView from './components/PointCloudView';
import SimView from './sim/SimView';
import ArmView from './sim/ArmView';

export default function App() {
  const { status, ros } = useRosState();
  const [mode, setMode] = useState<'camera' | 'sim' | 'arm'>('camera');

  useEffect(() => {
    connect();
  }, []);

  const activeRos: Ros | null = status === 'connected' ? ros : null;

  return (
    <div className="app">
      <header className="app-header">
        <h1>Ainstec Camera Viewer</h1>
        <div className="mode-switch">
          <button
            type="button"
            className={mode === 'camera' ? '' : 'secondary'}
            onClick={() => setMode('camera')}
          >
            相机
          </button>
          <button
            type="button"
            className={mode === 'sim' ? '' : 'secondary'}
            onClick={() => setMode('sim')}
          >
            仿真
          </button>
          <button
            type="button"
            className={mode === 'arm' ? '' : 'secondary'}
            onClick={() => setMode('arm')}
          >
            抓取
          </button>
        </div>
        <ConnectionBar />
      </header>
      {mode === 'sim' ? (
        <SimView />
      ) : mode === 'arm' ? (
        <ArmView />
      ) : (
      <main className="grid">
        <section className="panel area-cloud">
          <h2>Point Cloud (preview)</h2>
          <PointCloudView ros={activeRos} />
        </section>
        <section className="panel area-depth">
          <h2>Depth</h2>
          <DepthView ros={activeRos} />
        </section>
        <section className="panel area-ir">
          <h2>IR</h2>
          <IrView ros={activeRos} />
        </section>
        <section className="panel area-rgb">
          <h2>RGB</h2>
          <RgbView ros={activeRos} />
        </section>
        <section className="panel area-device">
          <h2>Devices</h2>
          <DevicePanel ros={activeRos} />
        </section>
        <section className="panel area-capture">
          <h2>Capture</h2>
          <CapturePanel ros={activeRos} />
        </section>
        <section className="panel area-params">
          <h2>Parameters (attr.*)</h2>
          <ParamsPanel ros={activeRos} />
        </section>
      </main>
      )}
    </div>
  );
}
