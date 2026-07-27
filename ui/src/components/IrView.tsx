import { useEffect, useRef, useState } from 'react';
import { Ros, Topic } from 'roslib';
import { base64ToBytes } from '../codec';

// mono8 IR image -> grayscale canvas.
export default function IrView({ ros }: { ros: Ros | null }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [frames, setFrames] = useState(0);

  useEffect(() => {
    if (!ros) return;
    const topic = new Topic({
      ros,
      name: '/ainstec_camera/ir/image_raw',
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
      const stride = (msg.step as number) || w;
      if (bytes.length < stride * h) return;

      const ctx = canvas.getContext('2d');
      if (!ctx) return;
      const img = ctx.createImageData(w, h);
      const px = img.data;
      for (let y = 0; y < h; y++) {
        const row = y * stride;
        for (let x = 0; x < w; x++) {
          const v = bytes[row + x];
          const o = (y * w + x) * 4;
          px[o] = v;
          px[o + 1] = v;
          px[o + 2] = v;
          px[o + 3] = 255;
        }
      }
      ctx.putImageData(img, 0, 0);
      setFrames((n) => n + 1);
    });

    return () => topic.unsubscribe();
  }, [ros]);

  return (
    <div className="panel-body view-body">
      <canvas ref={canvasRef} />
      <p className="muted">{frames > 0 ? `${frames} frames` : 'waiting for frames…'}</p>
    </div>
  );
}
