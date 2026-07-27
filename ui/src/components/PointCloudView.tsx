import { useEffect, useRef, useState } from 'react';
import { Ros, Topic } from 'roslib';
import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { base64ToBytes } from '../codec';

interface SceneCtx {
  renderer: THREE.WebGLRenderer;
  scene: THREE.Scene;
  camera: THREE.PerspectiveCamera;
  controls: OrbitControls;
  points: THREE.Points;
}

// PointCloud2 (xyz float32, millimeters from this vendor) -> THREE.Points in meters.
export default function PointCloudView({ ros }: { ros: Ros | null }) {
  const mountRef = useRef<HTMLDivElement>(null);
  const ctxRef = useRef<SceneCtx | null>(null);
  const [count, setCount] = useState(0);

  // Build the three.js scene once.
  useEffect(() => {
    const mount = mountRef.current;
    if (!mount) return;

    const renderer = new THREE.WebGLRenderer({ antialias: true });
    renderer.setPixelRatio(window.devicePixelRatio);
    renderer.setSize(mount.clientWidth || 640, mount.clientHeight || 480);
    mount.appendChild(renderer.domElement);

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x0b0e13);

    const camera = new THREE.PerspectiveCamera(
      60,
      (mount.clientWidth || 640) / (mount.clientHeight || 480),
      0.01,
      100
    );
    camera.position.set(0, -0.7, 0.1);

    const controls = new OrbitControls(camera, renderer.domElement);
    controls.target.set(0, 0, 1.05); // the mock scene is a plane at z ~ 1 m
    controls.update();

    const geometry = new THREE.BufferGeometry();
    const material = new THREE.PointsMaterial({
      color: 0x4fc3f7,
      size: 1.5,
      sizeAttenuation: false, // size in pixels
    });
    const points = new THREE.Points(geometry, material);
    points.frustumCulled = false;
    scene.add(points);
    scene.add(new THREE.AxesHelper(0.3));

    let raf = 0;
    const animate = () => {
      raf = requestAnimationFrame(animate);
      controls.update();
      renderer.render(scene, camera);
    };
    animate();

    const onResize = () => {
      const w = mount.clientWidth;
      const h = mount.clientHeight;
      if (!w || !h) return;
      camera.aspect = w / h;
      camera.updateProjectionMatrix();
      renderer.setSize(w, h);
    };
    const resizeObserver = new ResizeObserver(onResize);
    resizeObserver.observe(mount);

    ctxRef.current = { renderer, scene, camera, controls, points };

    return () => {
      resizeObserver.disconnect();
      cancelAnimationFrame(raf);
      ctxRef.current = null;
      controls.dispose();
      geometry.dispose();
      material.dispose();
      renderer.dispose();
      mount.removeChild(renderer.domElement);
    };
  }, []);

  // Subscribe to the decimated preview cloud while connected.
  useEffect(() => {
    if (!ros) return;
    const topic = new Topic({
      ros,
      name: '/ainstec_camera/points_preview',
      messageType: 'sensor_msgs/PointCloud2',
      throttle_rate: 500,
      queue_size: 1,
      queue_length: 1,
    });

    topic.subscribe((msg: any) => {
      const ctx = ctxRef.current;
      if (!ctx) return;
      const width = msg.width as number;
      const height = (msg.height as number) || 1;
      const pointStep = (msg.point_step as number) || 12;
      const n = width * height;
      if (!n) return;

      const bytes = base64ToBytes(msg.data);
      if (bytes.length < n * pointStep) return;
      const positions = new Float32Array(n * 3);
      if (pointStep === 12 && (bytes.byteOffset & 3) === 0) {
        // Fast path: tightly packed xyz float32.
        const src = new Float32Array(bytes.buffer, bytes.byteOffset, n * 3);
        for (let i = 0; i < n * 3; i++) positions[i] = src[i] * 0.001; // mm -> m
      } else {
        const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        for (let i = 0; i < n; i++) {
          const o = i * pointStep;
          positions[i * 3] = dv.getFloat32(o, true) * 0.001;
          positions[i * 3 + 1] = dv.getFloat32(o + 4, true) * 0.001;
          positions[i * 3 + 2] = dv.getFloat32(o + 8, true) * 0.001;
        }
      }

      const geo = ctx.points.geometry;
      geo.setAttribute('position', new THREE.BufferAttribute(positions, 3));
      geo.computeBoundingSphere();
      setCount(n);
    });

    return () => topic.unsubscribe();
  }, [ros]);

  return (
    <div className="panel-body view-body cloud-body">
      <div ref={mountRef} className="cloud-mount" />
      <p className="muted">{count > 0 ? `${count.toLocaleString()} points` : 'waiting for frames…'}</p>
    </div>
  );
}
