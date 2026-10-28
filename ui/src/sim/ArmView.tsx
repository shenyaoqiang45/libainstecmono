import { useEffect, useRef, useState } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { SENSOR } from './ainstecCamera';
import { SensorRig } from './core/sensorRig';
import { MOUNTS } from './core/mounts';
import { IDEAL, MAT, markMaterial, type ChallengeState } from './core/challenge';
import ChallengePanel from './ChallengePanel';

const TABLE_Y = 0.74;
const A1 = 0.36;
const A2 = 0.3;
const TOOL = 0.09;
const GRIP_X = 0.072;
const SHOULDER_H = 0.16;

const BASE = new THREE.Vector3(-0.48, TABLE_Y, 0);
const BIN = new THREE.Vector3(0.02, TABLE_Y, 0.24);

type Phase = 'observe' | 'above' | 'down' | 'close' | 'lift' | 'carry' | 'open' | 'home' | 'done';

const PHASE_LABEL: Record<Phase, string> = {
  observe: '观察',
  above: '对准',
  down: '下降',
  close: '闭合',
  lift: '抬起',
  carry: '搬运',
  open: '放开',
  home: '回位',
  done: '完成',
};

interface Piece {
  mesh: THREE.Mesh;
  name: string;
  half: number;
  home: THREE.Vector3;
  grasped: boolean;
  placed: boolean;
}

interface Joints {
  q1: number;
  q2: number;
  q3: number;
  q4: number;
}

interface Stats {
  phase: string;
  target: string;
  x: number;
  y: number;
  z: number;
  depthValid: number;
  centerMm: number | null;
  placed: number;
  total: number;
}

const EMPTY: Stats = {
  phase: PHASE_LABEL.observe,
  target: '—',
  x: 0,
  y: 0,
  z: 0,
  depthValid: 0,
  centerMm: null,
  placed: 0,
  total: 3,
};

export default function ArmView() {
  const mountRef = useRef<HTMLDivElement>(null);
  const colorRef = useRef<HTMLCanvasElement>(null);
  const depthRef = useRef<HTMLCanvasElement>(null);
  const resetRef = useRef(false);
  const challengeRef = useRef<ChallengeState>(IDEAL);
  const [challenge, setChallenge] = useState<ChallengeState>(IDEAL);
  const [stats, setStats] = useState<Stats>(EMPTY);

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
    scene.background = new THREE.Color(0x12161c);
    scene.add(new THREE.HemisphereLight(0xf4f7ff, 0x3e463c, 1.25));
    const sun = new THREE.DirectionalLight(0xffffff, 1.45);
    sun.position.set(1.2, 3.2, 1.4);
    scene.add(sun);

    const view = new THREE.PerspectiveCamera(48, (mount.clientWidth || 960) / (mount.clientHeight || 640), 0.05, 20);
    view.position.set(1.15, 1.55, 1.25);
    const controls = new OrbitControls(view, renderer.domElement);
    controls.target.set(-0.05, 0.85, 0);
    controls.update();

    addBox(scene, 4, 0.04, 4, 0, -0.02, 0, 0x2a3038);
    addBox(scene, 1.2, 0.04, 0.72, 0, TABLE_Y - 0.02, 0, 0xc4a574);
    addBox(scene, 0.06, TABLE_Y, 0.06, -0.52, TABLE_Y / 2, -0.28, 0x6b5344);
    addBox(scene, 0.06, TABLE_Y, 0.06, 0.52, TABLE_Y / 2, -0.28, 0x6b5344);
    addBox(scene, 0.06, TABLE_Y, 0.06, -0.52, TABLE_Y / 2, 0.28, 0x6b5344);
    addBox(scene, 0.06, TABLE_Y, 0.06, 0.52, TABLE_Y / 2, 0.28, 0x6b5344);
    addBin(scene, BIN.x, BIN.z);

    const pieces = makePieces(scene);
    markMaterial(pieces[0].mesh, MAT.DARK);
    markMaterial(pieces[1].mesh, MAT.METAL);

    const yaw = new THREE.Group();
    yaw.position.copy(BASE);
    scene.add(yaw);
    addBox(yaw, 0.12, 0.08, 0.12, 0, 0.04, 0, 0x2c3138);

    const shoulder = new THREE.Group();
    shoulder.position.y = SHOULDER_H;
    yaw.add(shoulder);
    shoulder.add(link(A1, 0.035, 0xd5d8de));

    const elbow = new THREE.Group();
    elbow.position.x = A1;
    shoulder.add(elbow);
    elbow.add(link(A2, 0.028, 0xc5cad2));

    const wrist = new THREE.Group();
    wrist.position.x = A2;
    elbow.add(wrist);

    const tool = new THREE.Group();
    wrist.add(tool);
    addBox(tool, 0.03, 0.028, 0.04, 0.01, 0.02, 0, 0x1a1d22);
    const leftFinger = addBox(tool, 0.028, 0.012, 0.012, 0.072, 0, 0.02, 0x3a3f46);
    const rightFinger = addBox(tool, 0.028, 0.012, 0.012, 0.072, 0, -0.02, 0x3a3f46);

    const rig = new SensorRig(MOUNTS.wrist_r);
    rig.camera.rotation.y = -Math.PI / 2; // local -Z onto tool +X
    tool.add(rig.camera);

    const cloudGeom = new THREE.BufferGeometry();
    const cloud = new THREE.Points(
      cloudGeom,
      new THREE.PointsMaterial({ color: 0xffb020, size: 4, sizeAttenuation: false })
    );
    cloud.frustumCulled = false;
    scene.add(cloud);

    let joints = solve(0.0, TABLE_Y + 0.32 + TOOL, 0.0) ?? { q1: 0.3, q2: 0.6, q3: -1.4, q4: -0.8 };
    let goal: Joints = { ...joints };
    let grip = 0.046;
    let gripGoal = 0.046;
    let phase: Phase = 'observe';
    let phaseTime = 0;
    let target: Piece | null = null;
    let captureIn = 0;
    let statsIn = 0;
    let depthValid = 0;
    let centerMm: number | null = null;
    const toolPos = new THREE.Vector3();

    const apply = () => {
      yaw.rotation.y = joints.q1;
      shoulder.rotation.z = joints.q2;
      elbow.rotation.z = joints.q3;
      wrist.rotation.z = joints.q4;
      leftFinger.position.z = grip / 2;
      rightFinger.position.z = -grip / 2;
    };
    apply();

    const settled = () =>
      Math.abs(deltaAngle(joints.q1, goal.q1)) < 0.04 &&
      Math.abs(joints.q2 - goal.q2) < 0.04 &&
      Math.abs(joints.q3 - goal.q3) < 0.04 &&
      Math.abs(joints.q4 - goal.q4) < 0.04 &&
      Math.abs(grip - gripGoal) < 0.003;

    const aim = (x: number, y: number, z: number) => {
      const next = solve(x, y + TOOL, z);
      if (!next) return false;
      goal = next;
      return true;
    };

    const resetPieces = () => {
      for (const piece of pieces) {
        scene.attach(piece.mesh);
        piece.mesh.position.copy(piece.home);
        piece.mesh.rotation.set(0, 0, 0);
        piece.grasped = false;
        piece.placed = false;
      }
      target = null;
      gripGoal = 0.046;
      aim(0, TABLE_Y + 0.32, 0);
      phase = 'observe';
      phaseTime = 0;
    };

    const capture = () => {
      yaw.updateWorldMatrix(true, true);
      const currentChallenge = challengeRef.current;
      const result = rig.capture(renderer, scene, {
        collectPoints: true,
        hidden: [yaw, cloud],
        depthSkip: [],
        challenge: currentChallenge,
        noiseBoost: currentChallenge.interference ? 2.5 : 1,
      });

      colorCtx.putImageData(rig.colorImage, 0, 0);
      depthCtx.putImageData(rig.depthImage, 0, 0);

      depthValid = result.valid;
      centerMm = result.centerMm;

      const seen: number[] = [];
      if (result.points && result.pointCount > 0) {
        const points = result.points;
        const world = new THREE.Vector3();
        for (let i = 0; i < result.pointCount; i++) {
          world.set(points[i * 3], points[i * 3 + 1], points[i * 3 + 2]);
          if (
            world.y > TABLE_Y + 0.012 &&
            world.y < TABLE_Y + 0.14 &&
            world.x > -0.42 &&
            world.x < 0.5 &&
            Math.abs(world.z) < 0.32 &&
            (Math.abs(world.x - BIN.x) > 0.12 || Math.abs(world.z - BIN.z) > 0.09)
          ) {
            seen.push(world.x, world.y, world.z);
          }
        }
      }
      cloudGeom.setAttribute('position', new THREE.BufferAttribute(new Float32Array(seen), 3));
      cloudGeom.computeBoundingSphere();
      return seen;
    };

    const detect = (seen: number[]) => {
      let best: Piece | null = null;
      let bestN = 12;
      for (const piece of pieces) {
        if (piece.placed || piece.grasped) continue;
        let n = 0;
        const px = piece.mesh.position.x;
        const pz = piece.mesh.position.z;
        for (let i = 0; i < seen.length; i += 3) {
          if (Math.hypot(seen[i] - px, seen[i + 2] - pz) < 0.055) n++;
        }
        if (n > bestN) {
          bestN = n;
          best = piece;
        }
      }
      return best;
    };

    const gripPoint = new THREE.Vector3();
    const graspAt = () => {
      gripPoint.set(GRIP_X, 0, 0);
      tool.localToWorld(gripPoint);
      return gripPoint;
    };

    const tryGrasp = () => {
      if (!target || target.placed) return false;
      const gripAtWorld = graspAt();
      const p = target.mesh.position;
      if (Math.hypot(p.x - gripAtWorld.x, p.z - gripAtWorld.z) > 0.04) return false;
      if (Math.abs(p.y - gripAtWorld.y) > 0.035) return false;
      tool.attach(target.mesh);
      target.grasped = true;
      return true;
    };

    const drop = () => {
      if (!target?.grasped) return;
      const slot = pieces.filter((piece) => piece.placed).length;
      scene.attach(target.mesh);
      target.mesh.position.set(BIN.x + (slot - 1) * 0.07, TABLE_Y + 0.031 + target.half, BIN.z);
      target.mesh.rotation.set(0, 0, 0);
      target.grasped = false;
      target.placed = true;
    };

    const nextPiece = () => pieces.find((piece) => !piece.placed && !piece.grasped) ?? null;

    aim(0, TABLE_Y + 0.32, 0);

    let raf = 0;
    const clock = new THREE.Clock();
    const animate = () => {
      raf = requestAnimationFrame(animate);
      const dt = Math.min(clock.getDelta(), 0.05);
      if (resetRef.current) {
        resetRef.current = false;
        resetPieces();
      }

      const max = 1.35 * dt;
      joints.q1 = approachAngle(joints.q1, goal.q1, max);
      joints.q2 = moveTo(joints.q2, goal.q2, max);
      joints.q3 = moveTo(joints.q3, goal.q3, max);
      joints.q4 = moveTo(joints.q4, goal.q4, max);
      grip = moveTo(grip, gripGoal, 0.09 * dt);
      apply();
      phaseTime += dt;

      const ready = settled();
      if (phase === 'observe' && phaseTime > 0.45 && ready) {
        const seen = capture();
        const found = detect(seen);
        if (found && aim(found.mesh.position.x, TABLE_Y + 0.18, found.mesh.position.z)) {
          target = found;
          phase = 'above';
          phaseTime = 0;
        }
      } else if (phase === 'above' && ready && target) {
        aim(target.mesh.position.x, TABLE_Y + target.half - (TOOL - GRIP_X), target.mesh.position.z);
        phase = 'down';
        phaseTime = 0;
      } else if (phase === 'down' && ready) {
        gripGoal = 0.018;
        phase = 'close';
        phaseTime = 0;
      } else if (phase === 'close' && ready) {
        if (tryGrasp() && target) {
          target.mesh.getWorldPosition(toolPos);
          aim(toolPos.x, TABLE_Y + 0.22, toolPos.z);
          phase = 'lift';
        } else {
          gripGoal = 0.046;
          aim(0, TABLE_Y + 0.32, 0);
          phase = 'home';
        }
        phaseTime = 0;
      } else if (phase === 'lift' && ready && target) {
        aim(BIN.x, TABLE_Y + 0.22, BIN.z);
        phase = 'carry';
        phaseTime = 0;
      } else if (phase === 'carry' && ready) {
        gripGoal = 0.046;
        phase = 'open';
        phaseTime = 0;
      } else if (phase === 'open' && ready) {
        drop();
        target = null;
        aim(0, TABLE_Y + 0.32, 0);
        phase = nextPiece() ? 'home' : 'done';
        phaseTime = 0;
      } else if (phase === 'home' && ready) {
        phase = 'observe';
        phaseTime = 0;
      }

      captureIn += dt;
      if (captureIn > 0.4 && phase !== 'observe') {
        captureIn = 0;
        capture();
      }

      controls.update();
      renderer.setRenderTarget(null);
      renderer.setClearColor(0x12161c, 1);
      renderer.render(scene, view);

      statsIn += dt;
      if (statsIn > 0.25) {
        statsIn = 0;
        tool.getWorldPosition(toolPos);
        setStats({
          phase: PHASE_LABEL[phase],
          target: target?.name ?? '—',
          x: toolPos.x,
          y: toolPos.y,
          z: toolPos.z,
          depthValid,
          centerMm,
          placed: pieces.filter((piece) => piece.placed).length,
          total: pieces.length,
        });
      }
    };
    animate();

    const resizeObserver = new ResizeObserver(() => {
      const w = mount.clientWidth;
      const h = mount.clientHeight;
      if (!w || !h) return;
      view.aspect = w / h;
      view.updateProjectionMatrix();
      renderer.setSize(w, h);
    });
    resizeObserver.observe(mount);

    return () => {
      resizeObserver.disconnect();
      cancelAnimationFrame(raf);
      controls.dispose();
      rig.dispose();
      cloudGeom.dispose();
      (cloud.material as THREE.Material).dispose();
      renderer.dispose();
      mount.removeChild(renderer.domElement);
    };
  }, []);

  return (
    <div className="sim">
      <section className="panel">
        <h2>桌面 · 协作臂 · 腕部相机</h2>
        <div ref={mountRef} className="sim-mount" />
        <p className="muted">拖拽旋转视角。腕部相机朝下，夹爪沿相机光轴闭合。</p>
      </section>
      <section className="panel sim-side">
        <h2>腕部 Ainstec 相机</h2>
        <p className="muted">彩色</p>
        <canvas ref={colorRef} className="sim-preview" width={SENSOR.width} height={SENSOR.height} />
        <p className="muted">深度（近处偏蓝，远处偏红）</p>
        <canvas ref={depthRef} className="sim-preview" width={SENSOR.width} height={SENSOR.height} />
        <ChallengePanel challenge={challenge} onChange={setChallenge} />
        <div className="sim-actions">
          <button type="button" className="secondary" onClick={() => { resetRef.current = true; }}>
            复位
          </button>
        </div>
        <p>阶段 {stats.phase} · 目标 {stats.target}</p>
        <p>
          夹爪 x={stats.x.toFixed(2)} y={stats.y.toFixed(2)} z={stats.z.toFixed(2)} m
        </p>
        <p>
          有效深度 {stats.depthValid}/{SENSOR.width * SENSOR.height}
          {stats.centerMm != null ? ` · 中心 ${stats.centerMm.toFixed(0)} mm` : ''}
        </p>
        <p>料盒 {stats.placed}/{stats.total}</p>
        <p className="muted">
          内参与 mock 相同：{SENSOR.width}×{SENSOR.height}，fx=fy={SENSOR.fx}。深度沿光轴，单位毫米。
          高出桌面的点作为物体，对准后下降、闭合、抬起，再放进料盒。
        </p>
      </section>
    </div>
  );
}

function solve(x: number, wristY: number, z: number): Joints | null {
  const dx = x - BASE.x;
  const dz = z - BASE.z;
  const r = Math.hypot(dx, dz);
  const h = wristY - (BASE.y + SHOULDER_H);
  const d2 = r * r + h * h;
  const reach = A1 + A2;
  const minReach = Math.abs(A1 - A2) + 0.02;
  const d = Math.sqrt(d2);
  if (d < minReach || d > reach - 0.01 || r < 0.05) return null;
  const c3 = (d2 - A1 * A1 - A2 * A2) / (2 * A1 * A2);
  if (c3 < -1 || c3 > 1) return null;
  const q3 = -Math.acos(c3);
  const k1 = A1 + A2 * Math.cos(q3);
  const k2 = A2 * Math.sin(q3);
  const q2 = Math.atan2(h, r) - Math.atan2(k2, k1);
  const q4 = -Math.PI / 2 - q2 - q3;
  const q1 = Math.atan2(-dz, dx);
  return { q1, q2, q3, q4 };
}

function makePieces(scene: THREE.Scene): Piece[] {
  const specs: Array<{ name: string; color: number; x: number; z: number; size: number; tall?: boolean }> = [
    { name: '红块', color: 0xd2564a, x: -0.08, z: 0.06, size: 0.05 },
    { name: '蓝柱', color: 0x3d7ea6, x: 0.06, z: -0.1, size: 0.032, tall: true },
    { name: '绿盒', color: 0x3f8f6b, x: -0.2, z: -0.12, size: 0.045 },
  ];
  return specs.map((spec) => {
    const height = spec.tall ? spec.size * 2.2 : spec.size;
    const mesh = new THREE.Mesh(
      spec.tall
        ? new THREE.CylinderGeometry(spec.size, spec.size, height, 20)
        : new THREE.BoxGeometry(spec.size, height, spec.size),
      new THREE.MeshStandardMaterial({ color: spec.color, roughness: 0.55 })
    );
    const half = height / 2;
    mesh.position.set(spec.x, TABLE_Y + half, spec.z);
    scene.add(mesh);
    return {
      mesh,
      name: spec.name,
      half,
      home: mesh.position.clone(),
      grasped: false,
      placed: false,
    };
  });
}

function addBin(scene: THREE.Scene, x: number, z: number) {
  const y = TABLE_Y + 0.025;
  addBox(scene, 0.22, 0.012, 0.16, x, y, z, 0x4d5560);
  addBox(scene, 0.012, 0.04, 0.16, x - 0.1, y + 0.02, z, 0x6a7380);
  addBox(scene, 0.012, 0.04, 0.16, x + 0.1, y + 0.02, z, 0x6a7380);
  addBox(scene, 0.2, 0.04, 0.012, x, y + 0.02, z - 0.074, 0x6a7380);
  addBox(scene, 0.2, 0.04, 0.012, x, y + 0.02, z + 0.074, 0x6a7380);
}

function link(length: number, radius: number, color: number) {
  const mesh = new THREE.Mesh(
    new THREE.CylinderGeometry(radius, radius * 0.85, length, 18),
    new THREE.MeshStandardMaterial({ color, roughness: 0.4, metalness: 0.45 })
  );
  mesh.rotation.z = Math.PI / 2;
  mesh.position.x = length / 2;
  return mesh;
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
    new THREE.MeshStandardMaterial({ color, roughness: 0.72 })
  );
  mesh.position.set(x, y, z);
  parent.add(mesh);
  return mesh;
}

function moveTo(current: number, target: number, maxStep: number) {
  const d = target - current;
  if (Math.abs(d) <= maxStep) return target;
  return current + Math.sign(d) * maxStep;
}

function approachAngle(current: number, target: number, maxStep: number) {
  return current + moveTo(0, deltaAngle(current, target), maxStep);
}

function deltaAngle(current: number, target: number) {
  let d = target - current;
  while (d > Math.PI) d -= Math.PI * 2;
  while (d < -Math.PI) d += Math.PI * 2;
  return d;
}
