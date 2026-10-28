// Virtual Ainstec head camera. Intrinsics match the mock backend
// (640x512, fx = fy = 500, principal point at the image center) so a
// simulated capture lines up with pyainstec.Sample: depth is millimeters
// along the optical axis, points are the pinhole back-projection.

export const SENSOR = {
  width: 640,
  height: 512,
  fx: 500,
  fy: 500,
  cx: 320,
  cy: 256,
  near: 0.08,
  far: 8,
  depthScaleM: 0.001,
} as const;

export function verticalFovDeg(): number {
  return (2 * Math.atan(SENSOR.height / 2 / SENSOR.fy) * 180) / Math.PI;
}

// Inverse of the linear RG encoding written by the sensor depth pass
// (high byte + low byte, range 0..far). Returns optical-axis depth in
// meters, or null for the clear color and the far plane.
export function depthMetersAt(
  rgba: Uint8Array,
  pixelIndex: number,
  near: number = SENSOR.near,
  far: number = SENSOR.far,
  margin = 0.02
): number | null {
  const o = pixelIndex * 4;
  const hi = rgba[o] / 255;
  const lo = rgba[o + 1] / 255;
  if (hi <= 0) return null;
  const meters = (hi + lo / 255) * far;
  if (!(meters > near + margin && meters < far * 0.98)) return null;
  return meters;
}

// Image pixel (u, vTop) to a camera-frame point in meters.
// Camera frame: x right, y up, z backward (three.js), optical axis is -z.
export function backproject(
  u: number,
  vTop: number,
  meters: number
): [number, number, number] {
  const x = ((u - SENSOR.cx) * meters) / SENSOR.fx;
  const y = -((vTop - SENSOR.cy) * meters) / SENSOR.fy;
  return [x, y, -meters];
}

export function pixelOffset(u: number, vTop: number): number {
  return (SENSOR.height - 1 - vTop) * SENSOR.width + u;
}
