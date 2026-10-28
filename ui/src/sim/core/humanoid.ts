// Parametric humanoid for the demo scenes. Kinematic only: two 4-DOF arms
// (shoulder yaw/pitch, elbow, wrist pitch) with analytic IK solved in the
// torso frame, a pan/tilt head, crouch + torso pitch for floor-level work,
// and camera mount groups for head/chest/left wrist/right wrist sensors.
// Conventions: root sits on the floor and faces -Z; arm links extend along
// local +X; the gripper closes along local Z.

import * as THREE from 'three';
import type { MountId } from './mounts';

const A1 = 0.34;
const A2 = 0.32;
const TOOL = 0.1;
const GRIP_X = 0.075;
const HIPS_REST_Y = 0.95;
const SHOULDER_X = 0.24;
const SHOULDER_Y = 0.43; // torso-local, above the hip pivot
const HEAD_Y = 0.6; // torso-local
const TORSO_X = 0.24;

export type ToolDir = 'down' | 'forward';

export interface ArmGoal {
  yaw: number;
  pitch: number;
  elbow: number;
  wrist: number;
}

export class Arm {
  readonly side: 'l' | 'r';
  readonly yaw: THREE.Group;
  readonly pitch: THREE.Group;
  readonly elbow: THREE.Group;
  readonly wrist: THREE.Group;
  readonly tool: THREE.Group;
  readonly links: THREE.Object3D[] = [];
  readonly mountPoint: THREE.Group;
  current: ArmGoal;
  goal: ArmGoal;
  grip = 0.05;
  gripGoal = 0.05;
  held: THREE.Mesh | null = null;
  private readonly leftFinger: THREE.Mesh;
  private readonly rightFinger: THREE.Mesh;

  constructor(side: 'l' | 'r', torso: THREE.Group) {
    this.side = side;
    const sx = side === 'r' ? SHOULDER_X : -SHOULDER_X;
    this.yaw = new THREE.Group();
    this.yaw.position.set(sx, SHOULDER_Y, 0);
    torso.add(this.yaw);

    this.pitch = new THREE.Group();
    this.yaw.add(this.pitch);
    const upper = limbMesh(A1, 0.045, 0xd5d8de);
    this.pitch.add(upper);
    this.links.push(upper);

    this.elbow = new THREE.Group();
    this.elbow.position.x = A1;
    this.pitch.add(this.elbow);
    const fore = limbMesh(A2, 0.038, 0xc5cad2);
    this.elbow.add(fore);
    this.links.push(fore);

    this.wrist = new THREE.Group();
    this.wrist.position.x = A2;
    this.elbow.add(this.wrist);

    this.tool = new THREE.Group();
    this.wrist.add(this.tool);
    const palm = box(0.05, 0.03, 0.06, 0.01, 0, 0, 0x1a1d22);
    this.tool.add(palm);
    this.leftFinger = box(0.055, 0.014, 0.014, GRIP_X, 0, 0.025, 0x3a3f46);
    this.rightFinger = box(0.055, 0.014, 0.014, GRIP_X, 0, -0.025, 0x3a3f46);
    this.tool.add(this.leftFinger, this.rightFinger);

    this.mountPoint = new THREE.Group();
    this.mountPoint.position.set(0.02, 0.02, 0);
    this.mountPoint.rotation.y = -Math.PI / 2; // sensor -Z onto tool +X
    this.tool.add(this.mountPoint);

    // Arms rest hanging down with a slight elbow bend.
    this.current = {
      yaw: side === 'r' ? -0.35 : 0.35,
      pitch: -1.3,
      elbow: -0.35,
      wrist: -Math.PI / 2 + 1.3 + 0.35,
    };
    this.goal = { ...this.current };
    this.apply();
  }

  private apply() {
    this.yaw.rotation.y = this.current.yaw;
    this.pitch.rotation.z = this.current.pitch;
    this.elbow.rotation.z = this.current.elbow;
    this.wrist.rotation.z = this.current.wrist;
    this.leftFinger.position.z = this.grip / 2;
    this.rightFinger.position.z = -this.grip / 2;
  }

  // Analytic IK in the torso frame. `target` is where the grasp point
  // (GRIP_X along the tool) should end up; toolDir picks the tool attitude.
  aimLocal(target: THREE.Vector3, toolDir: ToolDir = 'down'): boolean {
    const sx = this.side === 'r' ? SHOULDER_X : -SHOULDER_X;
    const vx = target.x - sx;
    const vz = target.z - 0;
    const yaw = Math.atan2(-vz, vx);
    const r = Math.hypot(vx, vz);
    // Wrist pivot sits one tool length back along the tool direction.
    const up = toolDir === 'down' ? 1 : 0;
    const h = target.y + up * TOOL - SHOULDER_Y;
    const rr = Math.max(0.06, r - (1 - up) * TOOL);
    const d = Math.hypot(rr, h);
    const reach = A1 + A2;
    const minReach = Math.abs(A1 - A2) + 0.03;
    if (d < minReach || d > reach - 0.01) return false;
    const c3 = (d * d - A1 * A1 - A2 * A2) / (2 * A1 * A2);
    if (c3 < -1 || c3 > 1) return false;
    const q3 = -Math.acos(c3);
    const q2 =
      Math.atan2(h, rr) - Math.atan2(A2 * Math.sin(q3), A1 + A2 * Math.cos(q3));
    const planeAngle = toolDir === 'down' ? -Math.PI / 2 : 0;
    const q4 = planeAngle - q2 - q3;
    this.goal = { yaw, pitch: q2, elbow: q3, wrist: q4 };
    return true;
  }

  home() {
    this.goal = {
      yaw: this.side === 'r' ? -0.35 : 0.35,
      pitch: -1.3,
      elbow: -0.35,
      wrist: -Math.PI / 2 + 1.3 + 0.35,
    };
    this.gripGoal = 0.05;
  }

  open() {
    this.gripGoal = 0.05;
  }

  close() {
    this.gripGoal = 0.024;
  }

  gripPointWorld(out: THREE.Vector3): THREE.Vector3 {
    out.set(GRIP_X, 0, 0);
    return this.tool.localToWorld(out);
  }

  grasp(mesh: THREE.Mesh) {
    this.tool.attach(mesh);
    this.held = mesh;
  }

  release(parent: THREE.Object3D, worldPos: THREE.Vector3) {
    if (!this.held) return;
    parent.attach(this.held);
    this.held.position.copy(parent.worldToLocal(worldPos.clone()));
    this.held.rotation.set(0, 0, 0);
    this.held = null;
  }

  settled(): boolean {
    return (
      Math.abs(deltaAngle(this.current.yaw, this.goal.yaw)) < 0.04 &&
      Math.abs(this.current.pitch - this.goal.pitch) < 0.04 &&
      Math.abs(this.current.elbow - this.goal.elbow) < 0.04 &&
      Math.abs(this.current.wrist - this.goal.wrist) < 0.04 &&
      Math.abs(this.grip - this.gripGoal) < 0.004
    );
  }

  update(dt: number) {
    const step = 1.6 * dt;
    this.current.yaw = approachAngle(this.current.yaw, this.goal.yaw, step);
    this.current.pitch = moveTo(this.current.pitch, this.goal.pitch, step);
    this.current.elbow = moveTo(this.current.elbow, this.goal.elbow, step);
    this.current.wrist = moveTo(this.current.wrist, this.goal.wrist, step);
    this.grip = moveTo(this.grip, this.gripGoal, 0.12 * dt);
    this.apply();
  }
}

export class Humanoid {
  readonly root: THREE.Group;
  readonly hips: THREE.Group;
  readonly torso: THREE.Group;
  readonly headYaw: THREE.Group;
  readonly headPitch: THREE.Group;
  readonly armL: Arm;
  readonly armR: Arm;
  readonly mounts: Record<MountId, THREE.Object3D>;
  crouch = 0;
  crouchGoal = 0;
  lean = 0;
  leanGoal = 0;
  headPanGoal = 0;
  headTiltGoal = 0;
  private walkPhase = 0;
  private readonly thighL: THREE.Group;
  private readonly thighR: THREE.Group;
  private readonly shinL: THREE.Group;
  private readonly shinR: THREE.Group;

  constructor() {
    this.root = new THREE.Group();
    this.root.name = 'humanoid';

    this.hips = new THREE.Group();
    this.hips.position.y = HIPS_REST_Y;
    this.root.add(this.hips);

    this.torso = new THREE.Group();
    this.hips.add(this.torso);
    const pelvis = box(0.3, 0.2, 0.19, 0, 0.02, 0, 0x2c3e50);
    this.torso.add(pelvis);
    const chest = box(0.36, 0.46, 0.2, 0, 0.3, 0, 0x3ec1ff);
    this.torso.add(chest);

    this.headYaw = new THREE.Group();
    this.headYaw.position.set(0, HEAD_Y, 0);
    this.torso.add(this.headYaw);
    this.headPitch = new THREE.Group();
    this.headYaw.add(this.headPitch);
    const skull = new THREE.Mesh(
      new THREE.SphereGeometry(0.115, 20, 16),
      new THREE.MeshStandardMaterial({ color: 0xd8c3a5, roughness: 0.6 })
    );
    skull.position.y = 0.06;
    this.headPitch.add(skull);
    const visor = box(0.16, 0.05, 0.06, 0, 0.06, -0.09, 0x111418);
    this.headPitch.add(visor);

    this.armL = new Arm('l', this.torso);
    this.armR = new Arm('r', this.torso);

    const headMount = new THREE.Group();
    headMount.position.set(0, 0.08, -0.13);
    this.headPitch.add(headMount);
    const chestMount = new THREE.Group();
    chestMount.position.set(0, 0.32, -0.13);
    chestMount.rotation.x = 0.5; // tilt down over the work volume
    this.torso.add(chestMount);
    this.mounts = {
      head: headMount,
      chest: chestMount,
      wrist_l: this.armL.mountPoint,
      wrist_r: this.armR.mountPoint,
    };

    this.thighL = this.leg(-0.1);
    this.thighR = this.leg(0.1);
    this.shinL = this.thighL.children[0] as THREE.Group;
    this.shinR = this.thighR.children[0] as THREE.Group;
  }

  private leg(x: number): THREE.Group {
    const thigh = new THREE.Group();
    thigh.position.set(x, 0, 0);
    this.hips.add(thigh);
    const thighMesh = box(0.11, 0.46, 0.12, 0, -0.23, 0, 0x2c3e50);
    thigh.add(thighMesh);
    const shin = new THREE.Group();
    shin.position.set(0, -0.46, 0);
    thigh.add(shin);
    const shinMesh = box(0.09, 0.44, 0.1, 0, -0.22, 0, 0x3d5a80);
    shin.add(shinMesh);
    const foot = box(0.1, 0.05, 0.2, 0, -0.45, -0.04, 0x1a1d22);
    shin.add(foot);
    return thigh;
  }

  // Squat depth 0..1: hips drop up to 0.5 m, knees fold, feet stay planted-ish.
  setCrouch(s: number) {
    this.crouchGoal = THREE.MathUtils.clamp(s, 0, 1);
  }

  // Forward bend around the hip pivot, radians.
  setLean(rad: number) {
    this.leanGoal = THREE.MathUtils.clamp(rad, 0, 1.1);
  }

  setHead(pan: number, tilt: number) {
    this.headPanGoal = THREE.MathUtils.clamp(pan, -1.4, 1.4);
    this.headTiltGoal = THREE.MathUtils.clamp(tilt, -0.6, 0.9);
  }

  faceYawTo(wx: number, wz: number): number {
    return Math.atan2(-(wx - this.root.position.x), -(wz - this.root.position.z));
  }

  faceTowards(wx: number, wz: number) {
    this.root.rotation.y = this.faceYawTo(wx, wz);
  }

  // Walk on the floor toward (wx, wz). Returns true when arrived. Turns in
  // place first, then advances; legs swing while moving.
  moveToward(wx: number, wz: number, dt: number, speed = 0.55): boolean {
    const dx = wx - this.root.position.x;
    const dz = wz - this.root.position.z;
    const dist = Math.hypot(dx, dz);
    if (dist < 0.02) return true;
    const yawGoal = Math.atan2(-dx, -dz);
    const dyaw = deltaAngle(this.root.rotation.y, yawGoal);
    if (Math.abs(dyaw) > 0.15) {
      this.root.rotation.y += Math.sign(dyaw) * Math.min(Math.abs(dyaw), 2.2 * dt);
      this.walkPhase += dt * 5;
      this.applyGait();
      return false;
    }
    const step = Math.min(dist, speed * dt);
    this.root.position.x += (dx / dist) * step;
    this.root.position.z += (dz / dist) * step;
    this.walkPhase += dt * 6;
    this.applyGait();
    return dist - step < 0.02;
  }

  private applyGait() {
    const s = Math.sin(this.walkPhase) * 0.4;
    const c = this.crouch;
    this.thighL.rotation.x = -0.5 * c + s * (1 - c);
    this.thighR.rotation.x = -0.5 * c - s * (1 - c);
    this.shinL.rotation.x = 1.9 * c + Math.max(0, -s) * 0.6;
    this.shinR.rotation.x = 1.9 * c + Math.max(0, s) * 0.6;
  }

  hideFor(mount: MountId): THREE.Object3D[] {
    if (mount === 'wrist_l') return this.armL.links;
    if (mount === 'wrist_r') return this.armR.links;
    return [];
  }

  update(dt: number) {
    this.crouch = moveTo(this.crouch, this.crouchGoal, 0.9 * dt);
    this.lean = moveTo(this.lean, this.leanGoal, 1.2 * dt);
    this.hips.position.y = HIPS_REST_Y - 0.5 * this.crouch;
    this.torso.rotation.x = -this.lean; // pitch forward (root faces -Z)
    this.headYaw.rotation.y = moveTo(this.headYaw.rotation.y, this.headPanGoal, 2.5 * dt);
    this.headPitch.rotation.x = moveTo(this.headPitch.rotation.x, this.headTiltGoal, 2.5 * dt);
    this.armL.update(dt);
    this.armR.update(dt);
    this.applyGait();
  }

  settled(): boolean {
    return (
      this.armL.settled() &&
      this.armR.settled() &&
      Math.abs(this.crouch - this.crouchGoal) < 0.02 &&
      Math.abs(this.lean - this.leanGoal) < 0.03 &&
      Math.abs(this.headYaw.rotation.y - this.headPanGoal) < 0.05 &&
      Math.abs(this.headPitch.rotation.x - this.headTiltGoal) < 0.05
    );
  }
}

function box(
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
    new THREE.MeshStandardMaterial({ color, roughness: 0.7 })
  );
  mesh.position.set(x, y, z);
  return mesh;
}

function limbMesh(length: number, radius: number, color: number): THREE.Mesh {
  const mesh = new THREE.Mesh(
    new THREE.CylinderGeometry(radius, radius * 0.85, length, 18),
    new THREE.MeshStandardMaterial({ color, roughness: 0.4, metalness: 0.45 })
  );
  mesh.rotation.z = Math.PI / 2;
  mesh.position.x = length / 2;
  return mesh;
}

export function moveTo(current: number, target: number, maxStep: number): number {
  const d = target - current;
  if (Math.abs(d) <= maxStep) return target;
  return current + Math.sign(d) * maxStep;
}

export function approachAngle(current: number, target: number, maxStep: number): number {
  return current + moveTo(0, deltaAngle(current, target), maxStep);
}

export function deltaAngle(current: number, target: number): number {
  let d = target - current;
  while (d > Math.PI) d -= Math.PI * 2;
  while (d < -Math.PI) d += Math.PI * 2;
  return d;
}

export { A1, A2, TOOL, GRIP_X, SHOULDER_Y, TORSO_X };
