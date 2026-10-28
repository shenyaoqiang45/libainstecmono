// Challenge-mode material model. Meshes marked with a material class carry a
// per-vertex `matClass` attribute that the depth pass writes into the B
// channel; the decode step then drops or perturbs pixels to mimic how active
// 3D sensors behave on dark, reflective, and transparent surfaces.

import * as THREE from 'three';

export const MAT = { NORMAL: 0, DARK: 1, METAL: 2, GLASS: 3 } as const;
export type MatClass = (typeof MAT)[keyof typeof MAT];

export interface ChallengeState {
  materialEffects: boolean; // dark/metal dropout; glass returns no depth
  ambientLight: number; // 0..1, scales axial depth noise
  interference: boolean; // dual-active wrist cameras get extra noise
}

export const IDEAL: ChallengeState = {
  materialEffects: false,
  ambientLight: 0,
  interference: false,
};

export function markMaterial(root: THREE.Object3D, cls: MatClass) {
  root.traverse((obj) => {
    if (obj instanceof THREE.Mesh) {
      const geo = obj.geometry as THREE.BufferGeometry;
      const pos = geo.getAttribute('position');
      if (pos && !geo.getAttribute('matClass')) {
        const arr = new Float32Array(pos.count).fill(cls);
        geo.setAttribute('matClass', new THREE.BufferAttribute(arr, 1));
      }
      obj.userData.matClass = cls;
    }
  });
}

export function collectGlass(root: THREE.Object3D): THREE.Object3D[] {
  const out: THREE.Object3D[] = [];
  root.traverse((obj) => {
    if (obj.userData.matClass === MAT.GLASS) out.push(obj);
  });
  return out;
}

// Deterministic per-pixel hash so dropout and noise are temporally stable.
export function hash2(u: number, v: number): number {
  const s = Math.sin(u * 12.9898 + v * 78.7873) * 43758.5453;
  return s - Math.floor(s);
}
