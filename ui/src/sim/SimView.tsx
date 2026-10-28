import { useEffect, useRef, useState } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { SENSOR } from './ainstecCamera';
import { Humanoid } from './core/humanoid';
import { SensorRig } from './core/sensorRig';
import { MOUNTS } from './core/mounts';
import { IDEAL, MAT, markMaterial, type ChallengeState } from './core/challenge';
import ChallengePanel from './ChallengePanel';

interface Stats {
  x: number;
  z: number;
  yawDeg: number;
  mapPoints: number;
  depthValid: number;
  depthCount: number;
  centerMm: number | null;
}

const EMPTY_STATS: Stats = {
  x: 0,
  z: 0,
  yawDeg: 0,
  mapPoints: 0,
  depthValid: 0,
  depthCount: SENSOR.width * SENSOR.height,
  centerMm: null,
};

interface Box {
  minX: number;
  maxX: number;
  minZ: number;
  maxZ: number;
}

const ROBOT_RADIUS = 0.28;
const WALK_LIMIT_X = 3.85;
const WALK_LIMIT_Z = 2.85;
const VOXEL = 0.04;

export default function SimView() {
  const mountRef = useRef<HTMLDivElement>(null);
  const colorRef = useRef<HTMLCanvasElement>(null);
  const depthRef = useRef<HTMLCanvasElement>(null);
  const autoRef = useRef(true);
  const challengeRef = useRef<ChallengeState>(IDEAL);
  const nudgeRef = useRef({ forward: 0, yaw: 0 });
  const clearRef = useRef(false);
  const [auto, setAuto] = useState(true);
  const [challenge, setChallenge] = useState<ChallengeState>(IDEAL);
  const [stats, setStats] = useState<Stats>(EMPTY_STATS);

  useEffect(() => {
    autoRef.current = auto;
  }, [auto]);

  useEffect(() => {
    challengeRef.current = challenge;
  }, [challenge]);

  useEffect(() => {
    const mount = mountRef.current;
    const colorCanvas = colorRef.current;
    const depthCanvas = depthRef.current;
    if (!mount || !colorCanvas || !depthCanvas) return;

    const colorCtx = colorCanvas.getContext('2d');
    const depthCtx = depthCanvas.getContext('2d');
    if (!colorCtx || !depthCtx) return;

    const renderer = new THREE.WebGLRenderer({ antialias: true });
    renderer.setPixelRatio(window.devicePixelRatio);
    renderer.setSize(mount.clientWidth || 960, mount.clientHeight || 640);
    mount.appendChild(renderer.domElement);

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x0b0e13);

    const viewCamera = new THREE.PerspectiveCamera(
      50,
      (mount.clientWidth || 960) / (mount.clientHeight || 640),
      0.05,
      80
    );
    viewCamera.position.set(0.4, 8.2, 7.6);
    const controls = new OrbitControls(viewCamera, renderer.domElement);
    controls.target.set(0, 0.4, 0);
    controls.update();

    scene.add(new THREE.HemisphereLight(0xf2f6ff, 0x4a5248, 1.35));
    const sun = new THREE.DirectionalLight(0xffffff, 1.6);
    sun.position.set(4, 8, 2);
    scene.add(sun);

    const room = new THREE.Group();
    scene.add(room);
    const colliders: Box[] = [];
    const gizmos = new THREE.Group();
    scene.add(gizmos);

    addBox(room, 8.6, 0.08, 6.6, 0, -0.04, 0, 0x6b737c);
    addBox(room, 8.6, 2.5, 0.16, 0, 1.25, -3.2, 0xa7b3c2);
    addBox(room, 8.6, 2.5, 0.16, 0, 1.25, 3.2, 0x8ea0b3);
    addBox(room, 0.16, 2.5, 6.6, -4.2, 1.25, 0, 0x97a6b8);
    addBox(room, 0.16, 2.5, 6.6, 4.2, 1.25, 0, 0x97a6b8);
    addObstacle(room, colliders, 1.3, 0.75, 0.7, 1.3, 0.375, -0.9, 0x8d6a45);
    const darkBox = addObstacle(room, colliders, 0.55, 1.15, 0.4, -1.6, 0.575, 1.15, 0x6d7d8a);
    const metalPillar = addObstacle(room, colliders, 0.45, 2.2, 0.45, 0.15, 1.1, 1.45, 0xc4b08a);
    const glassBox = addBox(room, 0.5, 0.5, 0.5, -0.5, 0.25, -1.55, 0xb5523a);
    markMaterial(darkBox, MAT.DARK);
    markMaterial(metalPillar, MAT.METAL);
    markMaterial(glassBox, MAT.GLASS);

    const grid = new THREE.GridHelper(8, 16, 0x3a4450, 0x242b33);
    gizmos.add(grid);

    const humanoid = new Humanoid();
    humanoid.root.position.set(-2.2, 0, 2.0);
    scene.add(humanoid.root);

    const rig = new SensorRig(MOUNTS.head);
    humanoid.mounts.head.add(rig.camera);

    const mapGeom = new THREE.BufferGeometry();
    const mapMat = new THREE.PointsMaterial({
      size: 3.5,
      sizeAttenuation: false,
      vertexColors: true,
    });
    const mapPoints = new THREE.Points(mapGeom, mapMat);
    mapPoints.frustumCulled = false;
    scene.add(mapPoints);

    let xyz = new Float32Array(65536 * 3);
    let rgb = new Float32Array(65536 * 3);
    let mapCount = 0;
    const voxels = new Set<number>();
    let captureIn = 0;
    let statsIn = 0;
    let depthValid = 0;
    let centerMm: number | null = null;
    const keys = new Set<string>();

    const uploadMap = () => {
      mapGeom.setAttribute('position', new THREE.BufferAttribute(xyz, 3));
      mapGeom.setAttribute('color', new THREE.BufferAttribute(rgb, 3));
      mapGeom.setDrawRange(0, mapCount);
      mapGeom.computeBoundingSphere();
    };

    const clearMap = () => {
      mapCount = 0;
      voxels.clear();
      mapGeom.setDrawRange(0, 0);
    };

    const pushVoxel = (x: number, y: number, z: number) => {
      const ix = Math.round(x / VOXEL);
      const iy = Math.round(y / VOXEL);
      const iz = Math.round(z / VOXEL);
      const key = (ix + 1024) * 4096 * 4096 + (iy + 1024) * 4096 + (iz + 1024);
      if (voxels.has(key)) return false;
      voxels.add(key);
      return true;
    };

    const capture = () => {
      humanoid.root.updateWorldMatrix(true, true);
      const currentChallenge = challengeRef.current;
      const result = rig.capture(renderer, scene, {
        collectPoints: true,
        hidden: [humanoid.root, gizmos, mapPoints],
        depthSkip: [glassBox],
        challenge: currentChallenge,
        noiseBoost: currentChallenge.interference ? 2.5 : 1,
      });

      colorCtx.putImageData(rig.colorImage, 0, 0);
      depthCtx.putImageData(rig.depthImage, 0, 0);

      depthValid = result.valid;
      centerMm = result.centerMm;
      if (result.points && result.pointCount > 0) {
        let added = false;
        const points = result.points;
        for (let i = 0; i < result.pointCount; i++) {
          const x = points[i * 3];
          const y = points[i * 3 + 1];
          const z = points[i * 3 + 2];
          if (pushVoxel(x, y, z)) {
            if (mapCount >= xyz.length / 3) {
              xyz = grow(xyz);
              rgb = grow(rgb);
            }
            const height = Math.min(1, Math.max(0, y / 2.2));
            xyz[mapCount * 3] = x;
            xyz[mapCount * 3 + 1] = y;
            xyz[mapCount * 3 + 2] = z;
            rgb[mapCount * 3] = 0.15 + 0.85 * height;
            rgb[mapCount * 3 + 1] = 0.82;
            rgb[mapCount * 3 + 2] = 1 - 0.75 * height;
            mapCount++;
            added = true;
          }
        }
        if (added) uploadMap();
      }
    };

    const clearance = (yawOffset: number) => {
      const origin = new THREE.Vector3();
      rig.camera.getWorldPosition(origin);
      const dir = new THREE.Vector3(0, 0, -1)
        .applyQuaternion(humanoid.root.quaternion)
        .applyAxisAngle(new THREE.Vector3(0, 1, 0), yawOffset);
      const ray = new THREE.Raycaster(origin, dir, 0.05, 8);
      const hits = ray.intersectObjects(room.children, false);
      return hits.length > 0 ? hits[0].distance : 8;
    };

    const moveHumanoid = (distance: number, dt: number) => {
      if (distance === 0) return false;
      const dir = new THREE.Vector3(0, 0, -1).applyQuaternion(humanoid.root.quaternion);
      const tx = humanoid.root.position.x + dir.x * distance;
      const tz = humanoid.root.position.z + dir.z * distance;
      if (blocked(tx, tz, colliders)) return false;
      // moveToward both advances the root and animates the gait.
      humanoid.moveToward(tx, tz, dt, 1.0);
      return true;
    };

    let turnSign = 1;
    let turnHold = 0;
    const clock = new THREE.Clock();
    let raf = 0;
    const animate = () => {
      raf = requestAnimationFrame(animate);
      const dt = Math.min(clock.getDelta(), 0.05);
      let forward = nudgeRef.current.forward;
      let yaw = nudgeRef.current.yaw;
      nudgeRef.current.forward = 0;
      nudgeRef.current.yaw = 0;
      const manual =
        keys.has('ArrowUp') ||
        keys.has('KeyW') ||
        keys.has('ArrowDown') ||
        keys.has('KeyS') ||
        keys.has('ArrowLeft') ||
        keys.has('KeyA') ||
        keys.has('ArrowRight') ||
        keys.has('KeyD') ||
        forward !== 0 ||
        yaw !== 0;
      if (keys.has('ArrowUp') || keys.has('KeyW')) forward += 0.7 * dt;
      if (keys.has('ArrowDown') || keys.has('KeyS')) forward -= 0.7 * dt;
      if (keys.has('ArrowLeft') || keys.has('KeyA')) yaw += 1.4 * dt;
      if (keys.has('ArrowRight') || keys.has('KeyD')) yaw -= 1.4 * dt;

      if (manual) {
        humanoid.root.rotation.y += yaw;
        moveHumanoid(forward, dt);
      } else if (autoRef.current) {
        const ahead = clearance(0);
        if (ahead > 1.05) {
          moveHumanoid(0.65 * dt, dt);
          turnHold = 0;
        } else {
          if (turnHold <= 0) {
            turnSign = clearance(0.9) >= clearance(-0.9) ? 1 : -1;
            turnHold = 0.8;
          }
          humanoid.root.rotation.y += turnSign * 1.6 * dt;
          turnHold -= dt;
        }
      }

      humanoid.setHead(0, 0.15);
      humanoid.update(dt);

      if (clearRef.current) {
        clearRef.current = false;
        clearMap();
      }

      captureIn += dt;
      if (captureIn >= 0.45) {
        captureIn = 0;
        capture();
      }

      controls.update();
      renderer.setRenderTarget(null);
      renderer.setClearColor(0x0b0e13, 1);
      renderer.render(scene, viewCamera);

      statsIn += dt;
      if (statsIn >= 0.25) {
        statsIn = 0;
        setStats({
          x: humanoid.root.position.x,
          z: humanoid.root.position.z,
          yawDeg: (humanoid.root.rotation.y * 180) / Math.PI,
          mapPoints: mapCount,
          depthValid,
          depthCount: SENSOR.width * SENSOR.height,
          centerMm,
        });
      }
    };
    animate();

    const onKey = (event: KeyboardEvent, down: boolean) => {
      const watched = ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'KeyW', 'KeyA', 'KeyS', 'KeyD'];
      if (!watched.includes(event.code)) return;
      if (down) keys.add(event.code);
      else keys.delete(event.code);
      event.preventDefault();
    };
    const down = (event: KeyboardEvent) => onKey(event, true);
    const up = (event: KeyboardEvent) => onKey(event, false);
    window.addEventListener('keydown', down);
    window.addEventListener('keyup', up);

    const resizeObserver = new ResizeObserver(() => {
      const w = mount.clientWidth;
      const h = mount.clientHeight;
      if (!w || !h) return;
      viewCamera.aspect = w / h;
      viewCamera.updateProjectionMatrix();
      renderer.setSize(w, h);
    });
    resizeObserver.observe(mount);

    return () => {
      resizeObserver.disconnect();
      window.removeEventListener('keydown', down);
      window.removeEventListener('keyup', up);
      cancelAnimationFrame(raf);
      controls.dispose();
      rig.dispose();
      mapGeom.dispose();
      mapMat.dispose();
      renderer.dispose();
      mount.removeChild(renderer.domElement);
    };
  }, []);

  // React to challenge toggles: the next capture() picks up the new state.
  // No extra effect needed because challenge is read from closure in animate().

  const step = (forward: number, yaw: number) => {
    nudgeRef.current.forward += forward;
    nudgeRef.current.yaw += yaw;
  };

  return (
    <div className="sim">
      <section className="panel">
        <h2>房间 · 人形 · 累积点云</h2>
        <div ref={mountRef} className="sim-mount" />
        <p className="muted">拖拽旋转视角。方向键或 WASD 走动时暂停自动探索，松开后若勾选则继续。</p>
      </section>
      <section className="panel sim-side">
        <h2>头部 Ainstec 相机</h2>
        <p className="muted">彩色</p>
        <canvas ref={colorRef} className="sim-preview" width={SENSOR.width} height={SENSOR.height} />
        <p className="muted">深度（近处偏蓝，远处偏红）</p>
        <canvas ref={depthRef} className="sim-preview" width={SENSOR.width} height={SENSOR.height} />
        <ChallengePanel challenge={challenge} onChange={setChallenge} />
        <label className="sim-check">
          <input
            type="checkbox"
            checked={auto}
            onChange={(event) => setAuto(event.target.checked)}
          />
          自动探索
        </label>
        <div className="sim-actions">
          <button type="button" className="secondary" onClick={() => step(0.35, 0)}>
            前进
          </button>
          <button type="button" className="secondary" onClick={() => step(-0.35, 0)}>
            后退
          </button>
          <button type="button" className="secondary" onClick={() => step(0, 0.4)}>
            左转
          </button>
          <button type="button" className="secondary" onClick={() => step(0, -0.4)}>
            右转
          </button>
          <button type="button" className="secondary" onClick={() => { clearRef.current = true; }}>
            清空地图
          </button>
        </div>
        <p>
          位姿 x={stats.x.toFixed(2)} m，z={stats.z.toFixed(2)} m，朝向 {stats.yawDeg.toFixed(0)}°
        </p>
        <p>
          地图 {stats.mapPoints} 点 · 有效深度 {stats.depthValid}/{stats.depthCount}
          {stats.centerMm != null ? ` · 中心 ${stats.centerMm.toFixed(0)} mm` : ''}
        </p>
        <p className="muted">
          内参与 mock 相同：{SENSOR.width}×{SENSOR.height}，fx=fy={SENSOR.fx}，主点在图像中心。
          深度沿光轴，单位毫米；点云在世界系，单位米，4 cm 体素。
        </p>
      </section>
    </div>
  );
}

function addBox(
  parent: THREE.Object3D,
  w: number,
  h: number,
  d: number,
  x: number,
  y: number,
  z: number,
  color: number
): THREE.Mesh {
  const mesh = new THREE.Mesh(
    new THREE.BoxGeometry(w, h, d),
    new THREE.MeshStandardMaterial({ color, roughness: 0.82 })
  );
  mesh.position.set(x, y, z);
  parent.add(mesh);
  return mesh;
}

function addObstacle(
  parent: THREE.Object3D,
  colliders: Box[],
  w: number,
  h: number,
  d: number,
  x: number,
  y: number,
  z: number,
  color: number
): THREE.Mesh {
  const mesh = addBox(parent, w, h, d, x, y, z, color);
  colliders.push({
    minX: x - w / 2,
    maxX: x + w / 2,
    minZ: z - d / 2,
    maxZ: z + d / 2,
  });
  return mesh;
}

function blocked(x: number, z: number, colliders: Box[]) {
  if (Math.abs(x) > WALK_LIMIT_X || Math.abs(z) > WALK_LIMIT_Z) return true;
  for (const box of colliders) {
    const cx = Math.max(box.minX, Math.min(x, box.maxX));
    const cz = Math.max(box.minZ, Math.min(z, box.maxZ));
    const dx = x - cx;
    const dz = z - cz;
    if (dx * dx + dz * dz < ROBOT_RADIUS * ROBOT_RADIUS) return true;
  }
  return false;
}

function grow(src: Float32Array) {
  const next = new Float32Array(src.length * 2);
  next.set(src);
  return next;
}
