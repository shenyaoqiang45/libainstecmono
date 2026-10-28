// Camera mount points on the humanoid. Each mount carries a virtual Ainstec
// sensor whose intrinsics match the mock backend (see ../ainstecCamera.ts);
// only the near/far range differs per mounting position.

export type MountId = 'head' | 'chest' | 'wrist_l' | 'wrist_r';

export interface MountPreset {
  id: MountId;
  label: string;
  near: number;
  far: number;
}

export const MOUNTS: Record<MountId, MountPreset> = {
  head: { id: 'head', label: '头部', near: 0.08, far: 8 },
  chest: { id: 'chest', label: '胸部', near: 0.08, far: 3 },
  wrist_l: { id: 'wrist_l', label: '左腕', near: 0.03, far: 1.5 },
  wrist_r: { id: 'wrist_r', label: '右腕', near: 0.03, far: 1.5 },
};

export const MOUNT_ORDER: MountId[] = ['head', 'chest', 'wrist_l', 'wrist_r'];
