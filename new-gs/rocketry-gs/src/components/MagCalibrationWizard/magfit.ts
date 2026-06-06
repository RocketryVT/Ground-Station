// Magnetometer hard/soft-iron ellipsoid fit.
//
// Fits the general quadric  Xᵀ M X + 2 nᵀ X = 1  to the raw samples, then:
//   hard iron (centre)  c = -M⁻¹ n
//   k = 1 + cᵀ M c   so that (X-c)ᵀ (M/k) (X-c) = 1
//   soft iron W = sqrtm(M/k)  maps the ellipsoid to a unit sphere; scaled by the
//   mean field radius so corrected magnitudes stay in physical units (µT).
//
// The Pico applies it as  corrected = softIron * (mag - hardIron)  via
// FusionModelMagnetic, matching this convention.

export type Vec3 = [number, number, number];
export type Mat3 = [number, number, number, number, number, number, number, number, number];

export interface MagSample { x: number; y: number; z: number }

export interface FitResult {
  ok: boolean;
  reason?: string;
  hardIron: Vec3;
  softIron: Mat3;       // row-major 3x3
  fieldRadius: number;  // mean corrected magnitude (µT)
  residual: number;     // RMS of |corrected| - fieldRadius, normalised (0 = perfect sphere)
  samples: number;
}

const IDENTITY: Mat3 = [1, 0, 0, 0, 1, 0, 0, 0, 1];

// Solve A x = b for a square system via Gaussian elimination with partial pivot.
function solveLinear(A: number[][], b: number[]): number[] | null {
  const n = b.length;
  // Augmented copy.
  const m = A.map((row, i) => [...row, b[i]]);
  for (let col = 0; col < n; col++) {
    let pivot = col;
    for (let r = col + 1; r < n; r++) {
      if (Math.abs(m[r][col]) > Math.abs(m[pivot][col])) pivot = r;
    }
    if (Math.abs(m[pivot][col]) < 1e-12) return null;
    [m[col], m[pivot]] = [m[pivot], m[col]];
    const diag = m[col][col];
    for (let r = 0; r < n; r++) {
      if (r === col) continue;
      const factor = m[r][col] / diag;
      if (factor === 0) continue;
      for (let c = col; c <= n; c++) m[r][c] -= factor * m[col][c];
    }
  }
  return m.map((row, i) => row[n] / m[i][i]);
}

function mat3MulVec(M: Mat3, v: Vec3): Vec3 {
  return [
    M[0] * v[0] + M[1] * v[1] + M[2] * v[2],
    M[3] * v[0] + M[4] * v[1] + M[5] * v[2],
    M[6] * v[0] + M[7] * v[1] + M[8] * v[2],
  ];
}

function mat3Inverse(M: Mat3): Mat3 | null {
  const [a, b, c, d, e, f, g, h, i] = M;
  const A = e * i - f * h;
  const B = -(d * i - f * g);
  const C = d * h - e * g;
  const det = a * A + b * B + c * C;
  if (Math.abs(det) < 1e-18) return null;
  const inv = 1 / det;
  return [
    A * inv, (c * h - b * i) * inv, (b * f - c * e) * inv,
    B * inv, (a * i - c * g) * inv, (c * d - a * f) * inv,
    C * inv, (b * g - a * h) * inv, (a * e - b * d) * inv,
  ];
}

// Jacobi eigen-decomposition for a symmetric 3x3. Returns eigenvalues and
// eigenvectors (columns of V).
function jacobiEigen(M: Mat3): { values: Vec3; vectors: Mat3 } {
  const a = [
    [M[0], M[1], M[2]],
    [M[3], M[4], M[5]],
    [M[6], M[7], M[8]],
  ];
  const v = [
    [1, 0, 0],
    [0, 1, 0],
    [0, 0, 1],
  ];
  for (let sweep = 0; sweep < 50; sweep++) {
    let off = 0;
    for (let p = 0; p < 3; p++) for (let q = p + 1; q < 3; q++) off += a[p][q] * a[p][q];
    if (off < 1e-20) break;
    for (let p = 0; p < 3; p++) {
      for (let q = p + 1; q < 3; q++) {
        if (Math.abs(a[p][q]) < 1e-18) continue;
        const phi = 0.5 * Math.atan2(2 * a[p][q], a[q][q] - a[p][p]);
        const cs = Math.cos(phi);
        const sn = Math.sin(phi);
        for (let k = 0; k < 3; k++) {
          const akp = a[k][p];
          const akq = a[k][q];
          a[k][p] = cs * akp - sn * akq;
          a[k][q] = sn * akp + cs * akq;
        }
        for (let k = 0; k < 3; k++) {
          const apk = a[p][k];
          const aqk = a[q][k];
          a[p][k] = cs * apk - sn * aqk;
          a[q][k] = sn * apk + cs * aqk;
        }
        for (let k = 0; k < 3; k++) {
          const vkp = v[k][p];
          const vkq = v[k][q];
          v[k][p] = cs * vkp - sn * vkq;
          v[k][q] = sn * vkp + cs * vkq;
        }
      }
    }
  }
  return {
    values: [a[0][0], a[1][1], a[2][2]],
    vectors: [v[0][0], v[0][1], v[0][2], v[1][0], v[1][1], v[1][2], v[2][0], v[2][1], v[2][2]],
  };
}

// Symmetric positive-definite matrix square root via eigen-decomposition.
function sqrtmSym(M: Mat3): Mat3 | null {
  const { values, vectors } = jacobiEigen(M);
  if (values.some((l) => l <= 1e-12)) return null;
  const s: Vec3 = [Math.sqrt(values[0]), Math.sqrt(values[1]), Math.sqrt(values[2])];
  // V * diag(s) * Vᵀ
  const V = vectors;
  const out: number[] = new Array(9).fill(0);
  for (let i = 0; i < 3; i++) {
    for (let j = 0; j < 3; j++) {
      let sum = 0;
      for (let k = 0; k < 3; k++) sum += V[i * 3 + k] * s[k] * V[j * 3 + k];
      out[i * 3 + j] = sum;
    }
  }
  return out as Mat3;
}

export function fitEllipsoid(raw: MagSample[]): FitResult {
  const empty: FitResult = {
    ok: false, hardIron: [0, 0, 0], softIron: IDENTITY, fieldRadius: 0, residual: 0, samples: raw.length,
  };
  if (raw.length < 50) return { ...empty, reason: `need ≥50 samples, have ${raw.length}` };

  // Pre-normalise for conditioning: subtract mean, divide by RMS radius.
  const mean: Vec3 = [0, 0, 0];
  for (const s of raw) { mean[0] += s.x; mean[1] += s.y; mean[2] += s.z; }
  mean[0] /= raw.length; mean[1] /= raw.length; mean[2] /= raw.length;
  let scale = 0;
  for (const s of raw) {
    const dx = s.x - mean[0], dy = s.y - mean[1], dz = s.z - mean[2];
    scale += dx * dx + dy * dy + dz * dz;
  }
  scale = Math.sqrt(scale / raw.length) || 1;

  // Normal equations DᵀD p = Dᵀ1 with D row [x²,y²,z²,2xy,2xz,2yz,2x,2y,2z].
  const ATA: number[][] = Array.from({ length: 9 }, () => new Array(9).fill(0));
  const ATb: number[] = new Array(9).fill(0);
  for (const s of raw) {
    const x = (s.x - mean[0]) / scale;
    const y = (s.y - mean[1]) / scale;
    const z = (s.z - mean[2]) / scale;
    const row = [x * x, y * y, z * z, 2 * x * y, 2 * x * z, 2 * y * z, 2 * x, 2 * y, 2 * z];
    for (let i = 0; i < 9; i++) {
      ATb[i] += row[i];
      for (let j = 0; j < 9; j++) ATA[i][j] += row[i] * row[j];
    }
  }
  const p = solveLinear(ATA, ATb);
  if (!p) return { ...empty, reason: 'fit did not converge (rotate the sensor more)' };

  const [a, b, c, d, e, f, g, h, i] = p;
  const Mn: Mat3 = [a, d, e, d, b, f, e, f, c];
  const n: Vec3 = [g, h, i];
  const Minv = mat3Inverse(Mn);
  if (!Minv) return { ...empty, reason: 'degenerate fit (insufficient coverage)' };
  const cn = mat3MulVec(Minv, n).map((v) => -v) as Vec3; // centre (normalised)
  const k = 1 + (cn[0] * n[0] + cn[1] * n[1] + cn[2] * n[2]); // (X-c)ᵀ(M/k)(X-c)=1
  if (k <= 1e-9) return { ...empty, reason: 'non-ellipsoid fit (rotate through all axes)' };
  const Mk: Mat3 = Mn.map((v) => v / k) as Mat3;
  const W0 = sqrtmSym(Mk);
  if (!W0) return { ...empty, reason: 'fit not positive-definite (need fuller rotation)' };

  // Un-normalise. centre  c = mean + scale*cn ; whitening in raw units = W0/scale.
  const hardIron: Vec3 = [mean[0] + scale * cn[0], mean[1] + scale * cn[1], mean[2] + scale * cn[2]];
  const Wraw: Mat3 = W0.map((v) => v / scale) as Mat3;

  // Mean field radius so corrected stays in µT, then residual (sphericity).
  let radiusSum = 0;
  const corr: number[] = [];
  for (const s of raw) {
    const v = mat3MulVec(Wraw, [s.x - hardIron[0], s.y - hardIron[1], s.z - hardIron[2]]);
    const mag = Math.hypot(v[0], v[1], v[2]);
    corr.push(mag);
    radiusSum += mag;
  }
  const unitRadius = radiusSum / raw.length || 1; // ≈1 since W0 maps to unit sphere
  // Physical field radius = mean raw distance from centre.
  let fieldSum = 0;
  for (const s of raw) {
    fieldSum += Math.hypot(s.x - hardIron[0], s.y - hardIron[1], s.z - hardIron[2]);
  }
  const fieldRadius = fieldSum / raw.length;
  const softIron: Mat3 = Wraw.map((v) => (v / unitRadius) * fieldRadius) as Mat3;

  let resid = 0;
  for (const mag of corr) {
    const d2 = mag / unitRadius - 1;
    resid += d2 * d2;
  }
  const residual = Math.sqrt(resid / corr.length);

  return { ok: true, hardIron, softIron, fieldRadius, residual, samples: raw.length };
}

// Crude rotational-coverage metric: fraction of 26 direction bins (from sphere
// face/edge/corner directions) hit by the samples around their centroid. Used to
// tell the operator when they've rotated enough.
export function coverage(raw: MagSample[]): number {
  if (raw.length < 3) return 0;
  const mean: Vec3 = [0, 0, 0];
  for (const s of raw) { mean[0] += s.x; mean[1] += s.y; mean[2] += s.z; }
  mean[0] /= raw.length; mean[1] /= raw.length; mean[2] /= raw.length;
  const bins = new Set<number>();
  for (const s of raw) {
    const dx = s.x - mean[0], dy = s.y - mean[1], dz = s.z - mean[2];
    const r = Math.hypot(dx, dy, dz);
    if (r < 1e-6) continue;
    const bx = Math.round(dx / r), by = Math.round(dy / r), bz = Math.round(dz / r);
    bins.add((bx + 1) * 9 + (by + 1) * 3 + (bz + 1));
  }
  bins.delete(13); // the zero direction
  return Math.min(1, bins.size / 26);
}
