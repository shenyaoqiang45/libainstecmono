import { useEffect, useRef, useState } from 'react';
import { Ros, Topic } from 'roslib';
import { base64ToBytes, turbo } from '../codec';

// 32FC1 depth image (meters) -> per-frame min/max normalize -> turbo colormap.
export default function DepthView({ ros }: { ros: Ros | null }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [range, setRange] = useState<{ min: number; max: number } | null>(null);

  useEffect(() => {
    if (!ros) return;
    const topic = new Topic({
      ros,
      name: '/ainstec_camera/depth/image_raw',
      messageType: 'sensor_msgs/Image',
      throttle_rate: 500,
      queue_size: 1,
      queue_length: 1,
    });

    topic.subscribe((msg: any) => {
      const canvas = canvasRef.current;
      if (!canvas) return;
      const w = msg.width as number;
      const h = msg.height as number;
      if (!w || !h) return;
      if (canvas.width !== w) canvas.width = w;
      if (canvas.height !== h) canvas.height = h;

      const bytes = base64ToBytes(msg.data);
      const step = (msg.step as number) || w * 4;
      if (bytes.length < step * h) return;
      const floats = new Float32Array(bytes.buffer, bytes.byteOffset, Math.floor(bytes.length / 4));
      const stride = step / 4; // step is a multiple of 4 for 32FC1

      let min = Infinity;
      let max = -Infinity;
      for (let y = 0; y < h; y++) {
        const row = y * stride;
        for (let x = 0; x < w; x++) {
          const v = floats[row + x];
          if (!Number.isFinite(v) || v <= 0) continue;
          if (v < min) min = v;
          if (v > max) max = v;
        }
      }
      if (!(min < max)) return;

      const ctx = canvas.getContext('2d');
      if (!ctx) return;
      const img = ctx.createImageData(w, h);
      const px = img.data;
      const span = max - min;
      for (let y = 0; y < h; y++) {
        const row = y * stride;
        for (let x = 0; x < w; x++) {
          const v = floats[row + x];
          const t = Number.isFinite(v) && v > 0 ? (v - min) / span : 0;
          const [r, g, b] = turbo(t);
          const o = (y * w + x) * 4;
          px[o] = r;
          px[o + 1] = g;
          px[o + 2] = b;
          px[o + 3] = 255;
        }
      }
      ctx.putImageData(img, 0, 0);
      setRange({ min, max });
    });

    return () => topic.unsubscribe();
  }, [ros]);

  return (
    <div className="panel-body view-body">
      <canvas ref={canvasRef} />
      <p className="muted">
        {range ? `min ${range.min.toFixed(3)} m · max ${range.max.toFixed(3)} m` : 'waiting for frames…'}
      </p>
    </div>
  );
}
