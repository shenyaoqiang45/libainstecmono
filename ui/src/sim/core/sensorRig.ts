// One virtual Ainstec sensor on a humanoid mount. Renders a color pass and a
// linearized depth pass into offscreen targets, reads both back, and decodes
// depth with the same convention as the mock backend: millimeters along the
// optical axis, points from the pinhole back-projection. The depth shader also
// writes the per-vertex material class into the B channel so challenge mode
// can drop or perturb pixels per surface type.

import * as THREE from 'three';
import { SENSOR, verticalFovDeg } from '../ainstecCamera';
import { MAT, hash2, type ChallengeState } from './challenge';
import type { MountPreset } from './mounts';

export interface CaptureOptions {
  depthOnly?: boolean; // skip the color pass (PiP feeds)
  collectPoints?: boolean; // backproject every 2nd pixel into world points
  hidden?: THREE.Object3D[]; // hidden in both passes (own body parts)
  depthSkip?: THREE.Object3D[]; // hidden only in the depth pass (glass)
  challenge: ChallengeState;
  noiseBoost?: number; // multiplies noise sigma (multi-cam interference)
}

export interface CaptureResult {
  valid: number;
  total: number;
  centerMm: number | null;
  points: Float32Array | null;
  pointCount: number;
}

const DROPOUT: Partial<Record<number, number>> = {
  [MAT.DARK]: 0.55,
  [MAT.METAL]: 0.35,
};

export class SensorRig {
  readonly preset: MountPreset;
  readonly camera: THREE.PerspectiveCamera;
  readonly width: number;
  readonly height: number;
  readonly colorImage: ImageData;
  readonly depthImage: ImageData;
  private readonly colorTarget: THREE.WebGLRenderTarget;
  private readonly depthTarget: THREE.WebGLRenderTarget;
  private readonly depthMaterial: THREE.ShaderMaterial;
  private readonly colorBuf: Uint8Array;
  private readonly depthBuf: Uint8Array;
  private readonly fx: number;
  private readonly fy: number;
  private readonly cx: number;
  private readonly cy: number;

  constructor(preset: MountPreset, scale = 1) {
    this.preset = preset;
    this.width = Math.round(SENSOR.width * scale);
    this.height = Math.round(SENSOR.height * scale);
    this.fx = SENSOR.fx * scale;
    this.fy = SENSOR.fy * scale;
    this.cx = (SENSOR.width * scale) / 2;
    this.cy = (SENSOR.height * scale) / 2;
    this.camera = new THREE.PerspectiveCamera(
      verticalFovDeg(),
      SENSOR.width / SENSOR.height,
      preset.near,
      preset.far
    );
    this.camera.name = `ainstec-${preset.id}`;

    this.depthTarget = new THREE.WebGLRenderTarget(this.width, this.height, {
      minFilter: THREE.NearestFilter,
      magFilter: THREE.NearestFilter,
      format: THREE.RGBAFormat,
      type: THREE.UnsignedByteType,
      depthBuffer: true,
    });
    this.depthTarget.texture.colorSpace = THREE.LinearSRGBColorSpace;
    this.colorTarget = this.depthTarget.clone();
    const far = preset.far;
    this.depthMaterial = new THREE.ShaderMaterial({
      toneMapped: false,
      vertexShader: `
        attribute float matClass;
        varying float vMeters;
        varying float vClass;
        void main() {
          vec4 viewPos = modelViewMatrix * vec4(position, 1.0);
          vMeters = -viewPos.z;
          vClass = matClass;
          gl_Position = projectionMatrix * viewPos;
        }
      `,
      fragmentShader: `
        varying float vMeters;
        varying float vClass;
        void main() {
          float d = clamp(vMeters / ${far.toFixed(2)}, 0.0, 1.0);
          gl_FragColor = vec4(d, fract(d * 255.0), vClass / 255.0, 1.0);
        }
      `,
    });
    this.colorBuf = new Uint8Array(this.width * this.height * 4);
    this.depthBuf = new Uint8Array(this.width * this.height * 4);
    this.colorImage = new ImageData(this.width, this.height);
    this.depthImage = new ImageData(this.width, this.height);
  }

  private offset(u: number, vTop: number): number {
    return (this.height - 1 - vTop) * this.width + u;
  }

  capture(
    renderer: THREE.WebGLRenderer,
    scene: THREE.Scene,
    opts: CaptureOptions
  ): CaptureResult {
    const { near, far } = this.preset;
    const challenge = opts.challenge;
    const boost = opts.noiseBoost ?? 1;
    this.camera.updateWorldMatrix(true, false);

    const restore: Array<[THREE.Object3D, boolean]> = [];
    const hide = (obj: THREE.Object3D) => {
      restore.push([obj, obj.visible]);
      obj.visible = false;
    };
    for (const obj of opts.hidden ?? []) hide(obj);

    const previousBackground = scene.background;
    scene.background = null;
    renderer.setClearColor(0x000000, 1);

    if (!opts.depthOnly) {
      renderer.setRenderTarget(this.colorTarget);
      renderer.clear();
      renderer.render(scene, this.camera);
      renderer.readRenderTargetPixels(
        this.colorTarget,
        0,
        0,
        this.width,
        this.height,
        this.colorBuf
      );
    }

    if (challenge.materialEffects) {
      for (const obj of opts.depthSkip ?? []) hide(obj);
    }
    const previousSpace = renderer.outputColorSpace;
    const previousTone = renderer.toneMapping;
    renderer.outputColorSpace = THREE.LinearSRGBColorSpace;
    renderer.toneMapping = THREE.NoToneMapping;
    scene.overrideMaterial = this.depthMaterial;
    renderer.setRenderTarget(this.depthTarget);
    renderer.clear();
    renderer.render(scene, this.camera);
    renderer.readRenderTargetPixels(
      this.depthTarget,
      0,
      0,
      this.width,
      this.height,
      this.depthBuf
    );
    renderer.outputColorSpace = previousSpace;
    renderer.toneMapping = previousTone;
    scene.overrideMaterial = null;
    renderer.setRenderTarget(null);
    scene.background = previousBackground;
    for (const [obj, vis] of restore) obj.visible = vis;

    const w = this.width;
    const h = this.height;
    if (!opts.depthOnly) {
      for (let v = 0; v < h; v++) {
        const src = (h - 1 - v) * w * 4;
        this.colorImage.data.set(this.colorBuf.subarray(src, src + w * 4), v * w * 4);
      }
    }

    const margin = (far - near) * 0.004;
    const paintRange = Math.min(far, 4);
    const centerSrc = this.offset(Math.round(this.cx), Math.round(this.cy));
    const world = new THREE.Vector3();
    let valid = 0;
    let centerMm: number | null = null;
    let points: Float32Array | null = null;
    let pointCount = 0;
    if (opts.collectPoints) {
      points = new Float32Array(((w * h) / 4) * 3);
    }
    for (let v = 0; v < h; v++) {
      for (let u = 0; u < w; u++) {
        const src = this.offset(u, v);
        const o = src * 4;
        const dst = (v * w + u) * 4;
        const hi = this.depthBuf[o] / 255;
        const lo = this.depthBuf[o + 1] / 255;
        const cls = this.depthBuf[o + 2];
        let meters = (hi + lo / 255) * far;
        let bad = hi <= 0 || !(meters > near + margin && meters < far * 0.98);
        if (!bad && challenge.materialEffects) {
          const p = DROPOUT[cls];
          if (p !== undefined && hash2(u, v) < p) bad = true;
        }
        if (bad) {
          this.depthImage.data[dst] = 0;
          this.depthImage.data[dst + 1] = 0;
          this.depthImage.data[dst + 2] = 0;
          this.depthImage.data[dst + 3] = 255;
          continue;
        }
        const sigma = (0.0005 + challenge.ambientLight * 0.004) * meters * meters * boost;
        if (sigma > 0) {
          meters += (hash2(v, u) - 0.5) * 2 * sigma;
        }
        valid++;
        if (src === centerSrc) centerMm = meters / SENSOR.depthScaleM;
        const t = Math.min(1, Math.max(0, (meters - near) / paintRange));
        this.depthImage.data[dst] = Math.round(40 + 180 * t);
        this.depthImage.data[dst + 1] = Math.round(150 * (1 - Math.abs(t - 0.5) * 2));
        this.depthImage.data[dst + 2] = Math.round(210 * (1 - t));
        this.depthImage.data[dst + 3] = 255;
        if (points && (u & 1) === 0 && (v & 1) === 0) {
          const px = ((u - this.cx) * meters) / this.fx;
          const py = -((v - this.cy) * meters) / this.fy;
          world.set(px, py, -meters).applyMatrix4(this.camera.matrixWorld);
          points[pointCount * 3] = world.x;
          points[pointCount * 3 + 1] = world.y;
          points[pointCount * 3 + 2] = world.z;
          pointCount++;
        }
      }
    }
    return { valid, total: w * h, centerMm, points, pointCount };
  }

  dispose() {
    this.colorTarget.dispose();
    this.depthTarget.dispose();
    this.depthMaterial.dispose();
  }
}
