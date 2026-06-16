import { useMemo, useState } from 'react';
import { Canvas } from '@react-three/fiber';
import { Grid, Html, Line, OrbitControls } from '@react-three/drei';

import { useTelemetryStore } from '../../store/telemetryStore';
import type { GroundImuState } from '../../types/telemetry';
import {
  GROUND_STATION,
  MAG_FIELD_MAX_UT,
  MAG_FIELD_MIN_UT,
  remapToBody,
  type FrameSpec,
  type Vec3,
} from './groundStationModel';
import { GroundStationModelView } from './GroundStationModelView';
import styles from './AhrsFrameScene.module.css';

type Quat4 = [number, number, number, number];

interface Props {
  imu: GroundImuState | null;
}

const DEG = Math.PI / 180;

const BODY_AXES: Array<{ label: string; body: Vec3; color: string; scale: number }> = [
  { label: '+X', body: [1, 0, 0], color: '#ff4d5e', scale: 0.46 }, // forward / boresight
  { label: '+Y', body: [0, 1, 0], color: '#4dff9a', scale: 0.38 }, // right
  { label: '+Z', body: [0, 0, 1], color: '#56a8ff', scale: 0.32 }, // down
];

// ---- vector / quaternion maths ----------------------------------------------

function quatRotate(q: Quat4, v: Vec3): Vec3 {
  const [w, x, y, z] = q;
  const [vx, vy, vz] = v;
  const tx = 2 * (y * vz - z * vy);
  const ty = 2 * (z * vx - x * vz);
  const tz = 2 * (x * vy - y * vx);
  return [
    vx + w * tx + (y * tz - z * ty),
    vy + w * ty + (z * tx - x * tz),
    vz + w * tz + (x * ty - y * tx),
  ];
}

function quatMul(a: Quat4, b: Quat4): Quat4 {
  const [aw, ax, ay, az] = a;
  const [bw, bx, by, bz] = b;
  return [
    aw * bw - ax * bx - ay * by - az * bz,
    aw * bx + ax * bw + ay * bz - az * by,
    aw * by - ax * bz + ay * bw + az * bx,
    aw * bz + ax * by - ay * bx + az * bw,
  ];
}

// NED (x=N, y=E, z=Down) → Three.js (x=E, y=Up, z=South). Matches AntennaScene.
function nedToThree(v: Vec3): Vec3 {
  return [v[1], -v[2], -v[0]];
}

function rotateY(v: Vec3, a: number): Vec3 {
  const c = Math.cos(a);
  const s = Math.sin(a);
  return [v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c];
}

function add(a: Vec3, b: Vec3): Vec3 {
  return [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
}

function scale(v: Vec3, s: number): Vec3 {
  return [v[0] * s, v[1] * s, v[2] * s];
}

function quatToEulerDeg(q?: Quat4): string {
  if (!q) return '--';
  const [w, x, y, z] = q;
  const roll = Math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y));
  const sinPitch = 2 * (w * y - z * x);
  const pitch = Math.asin(Math.max(-1, Math.min(1, sinPitch)));
  const yaw = Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
  return `${(roll / DEG).toFixed(1)} / ${(pitch / DEG).toFixed(1)} / ${(yaw / DEG).toFixed(1)}`;
}

// Resolve the absolute earth→body quaternion to draw for a frame spec.
function resolveQuat(spec: FrameSpec, imu: GroundImuState | null): Quat4 | undefined {
  if (!imu) return undefined;
  switch (spec.source) {
    case 'yaw_q': return imu.yaw_q;
    case 'bar_q': return imu.bar_q;
    case 'q':
    case 'heading': return imu.q;
    case 'bar_rel_q':
      if (spec.relativeToYaw && imu.yaw_q && imu.bar_rel_q) return quatMul(imu.yaw_q, imu.bar_rel_q);
      return imu.bar_rel_q;
    default: return undefined;
  }
}

// ---- calibration analysis ---------------------------------------------------

const RAD2DEG = 180 / Math.PI;

function vlen(v: Vec3): number {
  return Math.hypot(v[0], v[1], v[2]);
}

function normalize(v: Vec3): Vec3 {
  const n = vlen(v);
  return n > 1e-9 ? [v[0] / n, v[1] / n, v[2] / n] : [0, 0, 0];
}

function norm360(deg: number): number {
  return ((deg % 360) + 360) % 360;
}

// Smallest signed difference a − b, wrapped to [−180, 180].
function angleDiff(a: number, b: number): number {
  let d = (a - b) % 360;
  if (d > 180) d -= 360;
  if (d < -180) d += 360;
  return d;
}

// Heading (0 = north, CW) from an earth(NED)→body quaternion.
function headingFromQuat(q: Quat4): number {
  const [w, x, y, z] = q;
  return norm360(Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) * RAD2DEG);
}

// Boresight (body +X) elevation above horizon, from an earth(NED)→body quaternion.
function boresightElFromQuat(q: Quat4): number {
  const f = quatRotate(q, [1, 0, 0]); // [N, E, D]
  return Math.atan2(-f[2], Math.hypot(f[0], f[1])) * RAD2DEG;
}

// Magnetic heading from an earth-frame field vector [N, E, D].
function magHeading(earthMag: Vec3): number {
  return norm360(Math.atan2(earthMag[1], earthMag[0]) * RAD2DEG);
}

interface BoardDiag {
  q?: Quat4;
  anchor: Vec3;
  color: string;
  accelEarth?: Vec3;   // unit, earth frame
  magEarth?: Vec3;     // unit, earth frame
  fieldUt?: number;    // raw field magnitude
  tiltDeg?: number;    // accel tilt from level (0 = perfectly level)
  fieldState?: 'OK' | 'LOW' | 'HIGH';
}

function buildBoardDiag(
  q: Quat4 | undefined,
  anchor: Vec3,
  color: string,
  remap: string,
  accelSensor?: Vec3,
  magSensor?: Vec3,
): BoardDiag {
  const diag: BoardDiag = { q, anchor, color };
  if (accelSensor && vlen(accelSensor) > 1e-6) {
    const aBody = remapToBody(remap, accelSensor);
    const aUnit = normalize(aBody);
    // Gravity reaction at rest points along body −Z (up); tilt is the angle off that.
    diag.tiltDeg = Math.acos(Math.max(-1, Math.min(1, -aUnit[2]))) * RAD2DEG;
    if (q) diag.accelEarth = normalize(quatRotate(q, aBody));
  }
  if (magSensor && vlen(magSensor) > 1e-6) {
    diag.fieldUt = vlen(magSensor);
    diag.fieldState = diag.fieldUt < MAG_FIELD_MIN_UT ? 'LOW' : diag.fieldUt > MAG_FIELD_MAX_UT ? 'HIGH' : 'OK';
    const mBody = remapToBody(remap, magSensor);
    if (q) diag.magEarth = normalize(quatRotate(q, mBody));
  }
  return diag;
}

// ---- frame gizmos -----------------------------------------------------------

function FrameGizmo({ spec, imu, headingDeg }: { spec: FrameSpec; imu: GroundImuState | null; headingDeg: number }) {
  const q = resolveQuat(spec, imu);
  const origin = spec.anchor;

  if (!q) {
    return (
      <Html position={[origin[0], origin[1] + 0.1, origin[2]]} center>
        <div className={styles.label} style={{ borderColor: spec.color }}>{spec.label} · --</div>
      </Html>
    );
  }

  return (
    <group>
      <mesh position={origin}>
        <boxGeometry args={[0.12, 0.04, 0.1]} />
        <meshStandardMaterial color={spec.color} metalness={0.15} roughness={0.4} />
      </mesh>
      {BODY_AXES.map((axis) => {
        let dir = nedToThree(quatRotate(q, axis.body));
        if (spec.source === 'heading') dir = rotateY(dir, -headingDeg * DEG);
        const end = add(origin, scale(dir, axis.scale));
        return (
          <group key={axis.label}>
            <Line points={[origin, end]} color={axis.color} lineWidth={2.4} />
            <mesh position={end}>
              <sphereGeometry args={[0.024, 12, 8]} />
              <meshStandardMaterial color={axis.color} />
            </mesh>
          </group>
        );
      })}
      <Html position={[origin[0], origin[1] + 0.2, origin[2]]} center>
        <div className={styles.label} style={{ borderColor: spec.color }}>{spec.label}</div>
      </Html>
    </group>
  );
}

// ---- sensor vector overlay --------------------------------------------------

const GRAVITY_COLOR = '#ffe14d';
const MAG_COLOR = '#ff4df0';

function SensorVectors({ diag }: { diag: BoardDiag }) {
  const origin = diag.anchor;
  return (
    <group>
      {diag.accelEarth && (
        <group>
          <Line points={[origin, add(origin, scale(nedToThree(diag.accelEarth), 0.55))]} color={GRAVITY_COLOR} lineWidth={2.6} />
          <mesh position={add(origin, scale(nedToThree(diag.accelEarth), 0.55))}>
            <sphereGeometry args={[0.03, 12, 8]} />
            <meshStandardMaterial color={GRAVITY_COLOR} emissive={GRAVITY_COLOR} emissiveIntensity={0.3} />
          </mesh>
        </group>
      )}
      {diag.magEarth && (
        <group>
          <Line points={[origin, add(origin, scale(nedToThree(diag.magEarth), 0.55))]} color={MAG_COLOR} lineWidth={2.6} dashed dashSize={0.06} gapSize={0.04} />
          <mesh position={add(origin, scale(nedToThree(diag.magEarth), 0.55))}>
            <sphereGeometry args={[0.03, 12, 8]} />
            <meshStandardMaterial color={MAG_COLOR} emissive={MAG_COLOR} emissiveIntensity={0.3} />
          </mesh>
        </group>
      )}
    </group>
  );
}

// ---- scene ------------------------------------------------------------------

// Board mounting remap (imu chip) for the calibration overlay, pulled from the model.
function boardRemap(boardId: string): string {
  const board = GROUND_STATION.boards.find((b) => b.id === boardId);
  return board?.chips.find((c) => c.kind === 'imu')?.remap
    ?? board?.chips[0]?.remap
    ?? 'PYPXNZ';
}

const fmtDeg = (n: number | null): string => (n == null ? '--' : `${n.toFixed(1)}°`);
const residColor = (n: number | null, warn: number): string =>
  n == null ? '#aeb6c2' : Math.abs(n) > warn ? '#ff6b6b' : '#4dff9a';
const residColorField = (state?: 'OK' | 'LOW' | 'HIGH'): string =>
  state === 'OK' ? '#4dff9a' : state == null ? '#aeb6c2' : '#ff6b6b';

export function AhrsFrameScene({ imu }: Props) {
  const antenna = useTelemetryStore((s) => s.antenna);
  const rawYawImuArr = useTelemetryStore((s) => s.rawYawImu);
  const rawImuArr = useTelemetryStore((s) => s.rawImu);
  const rawMagArr = useTelemetryStore((s) => s.rawMag);
  const az = antenna?.actual_az ?? 0;
  const el = antenna?.actual_el ?? 0;
  const visualEl = Math.abs(el);

  const [showModel, setShowModel] = useState(true);
  const [showBoards, setShowBoards] = useState(true);
  const [showVectors, setShowVectors] = useState(false);
  const [showCal, setShowCal] = useState(false);
  const [headingDeg, setHeadingDeg] = useState(-8.53);
  const [visible, setVisible] = useState<Record<string, boolean>>(
    () => Object.fromEntries(GROUND_STATION.frames.map((f) => [f.key, true])),
  );

  const frames = GROUND_STATION.frames;
  const hasAny = useMemo(() => frames.some((f) => resolveQuat(f, imu)), [frames, imu]);

  // ---- per-board calibration diagnostics ----
  const { yawDiag, zenDiag, resid } = useMemo(() => {
    const yawFrame = frames.find((f) => f.key === 'yaw');
    const barFrame = frames.find((f) => f.key === 'bar');
    const ry = rawYawImuArr.at(-1);
    const ri = rawImuArr.at(-1);
    const rm = rawMagArr.at(-1);

    const yaw = buildBoardDiag(
      imu?.yaw_q,
      yawFrame?.anchor ?? [0, 0, 0],
      yawFrame?.color ?? '#4dff9a',
      boardRemap('yaw_board'),
      ry ? [ry.ax, ry.ay, ry.az] : undefined,
      ry && ry.mag_valid !== false ? [ry.mx_ut, ry.my_ut, ry.mz_ut] : undefined,
    );
    const zen = buildBoardDiag(
      imu?.bar_q,
      barFrame?.anchor ?? [0, 0, 0],
      barFrame?.color ?? '#56a8ff',
      boardRemap('zenith_imu_board'),
      ri ? [ri.ax, ri.ay, ri.az] : undefined,
      rm ? [rm.mx, rm.my, rm.mz] : undefined,
    );

    const yawHeading = imu?.yaw_q ? headingFromQuat(imu.yaw_q) : null;
    const barEl = imu?.bar_q ? boresightElFromQuat(imu.bar_q) : null;
    const magHdg = yaw.magEarth ? magHeading(yaw.magEarth) : null;
    const relEuler = imu?.bar_rel_q
      ? (() => {
          const [w, x, y, z] = imu.bar_rel_q!;
          return {
            roll: Math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) * RAD2DEG,
            yaw: Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) * RAD2DEG,
          };
        })()
      : null;

    return {
      yawDiag: yaw,
      zenDiag: zen,
      resid: {
        yawHeading,
        barEl,
        magHdg,
        magPull: magHdg != null && yawHeading != null ? angleDiff(magHdg, yawHeading) : null,
        azResid: yawHeading != null && antenna ? angleDiff(yawHeading, az) : null,
        elResid: barEl != null && antenna ? barEl - el : null,
        relYaw: relEuler?.yaw ?? null,
        relRoll: relEuler?.roll ?? null,
      },
    };
  }, [imu, antenna, rawYawImuArr, rawImuArr, rawMagArr, frames, az, el]);

  return (
    <div className={styles.wrapper}>
      <Canvas camera={{ position: [3.6, 1.9, 4.2], fov: 46 }} style={{ background: '#ffffff' }}>
        <ambientLight intensity={0.9} />
        <directionalLight position={[3, 5, 4]} intensity={1.05} />
        <directionalLight position={[-4, 2, -3]} intensity={0.4} />
        <Grid
          args={[10, 10]}
          cellSize={0.5}
          cellColor="#d8d8d8"
          sectionSize={1}
          sectionColor="#bbbbbb"
          position={[0, -2.42, 0]}
          fadeDistance={16}
          infiniteGrid
        />

        {/* North marker (−Z): the antennas point north at az=0 */}
        <Line points={[[0, -0.05, -0.7], [0, -0.05, -1.7]]} color="#861F41" lineWidth={2} />
        <Html position={[0, 0.0, -1.85]} center>
          <div className={styles.northTag}>N</div>
        </Html>

        {showModel && (
          <GroundStationModelView model={GROUND_STATION} az={az} el={visualEl} showBoards={showBoards} />
        )}

        {frames.filter((f) => visible[f.key]).map((f) => (
          <FrameGizmo key={f.key} spec={f} imu={imu} headingDeg={headingDeg} />
        ))}

        {showVectors && visible.yaw && <SensorVectors diag={yawDiag} />}
        {showVectors && visible.bar && <SensorVectors diag={zenDiag} />}

        <OrbitControls enablePan minDistance={1.2} maxDistance={16} target={[0, 0.25, 0]} />
      </Canvas>

      {/* Controls */}
      <div className={styles.controls}>
        <button
          className={`${styles.toggleBtn} ${showModel ? styles.on : ''}`}
          onClick={() => setShowModel((v) => !v)}
        >MODEL</button>
        <button
          className={`${styles.toggleBtn} ${showBoards ? styles.on : ''}`}
          onClick={() => setShowBoards((v) => !v)}
        >BOARDS</button>
        <button
          className={`${styles.toggleBtn} ${showVectors ? styles.on : ''}`}
          title="Gravity (solid) + magnetic (dashed) vectors per board, mapped sensor→body→earth"
          onClick={() => setShowVectors((v) => !v)}
        >VECTORS</button>
        <button
          className={`${styles.toggleBtn} ${showCal ? styles.on : ''}`}
          title="Calibration residuals: mag health, AHRS-vs-mechanical, frame consistency"
          onClick={() => setShowCal((v) => !v)}
        >CAL</button>
        {frames.map((f) => (
          <button
            key={f.key}
            className={`${styles.toggleBtn} ${visible[f.key] ? styles.on : ''}`}
            style={visible[f.key] ? { borderColor: f.color, color: f.color } : undefined}
            title={f.description}
            onClick={() => setVisible((v) => ({ ...v, [f.key]: !v[f.key] }))}
          >{f.label}</button>
        ))}
        <label className={styles.headingCtl} title="Display-only heading correction (declination + offset) applied to the HEADING-CORR frame">
          HDG°
          <input
            type="number"
            step="0.5"
            value={headingDeg}
            onChange={(e) => setHeadingDeg(Number(e.target.value))}
          />
        </label>
        <span className={styles.azel}>AZ {az.toFixed(1)}° · EL {el.toFixed(1)}°</span>
      </div>

      {!hasAny && <div className={styles.empty}>WAITING FOR AHRS QUATERNIONS</div>}

      {/* Calibration residual panel */}
      {showCal && (
        <div className={styles.calPanel}>
          <div className={styles.calTitle}>CALIBRATION</div>
          <div className={styles.calGrid}>
            <span>Mag field · yaw</span>
            <strong style={{ color: residColorField(yawDiag.fieldState) }}>
              {yawDiag.fieldUt != null ? `${yawDiag.fieldUt.toFixed(1)} µT ${yawDiag.fieldState}` : '--'}
            </strong>
            <span>Mag field · zenith</span>
            <strong style={{ color: residColorField(zenDiag.fieldState) }}>
              {zenDiag.fieldUt != null ? `${zenDiag.fieldUt.toFixed(1)} µT ${zenDiag.fieldState}` : '--'}
            </strong>
            <span>Mag pull (mag−AHRS yaw)</span>
            <strong style={{ color: residColor(resid.magPull, 10) }}>{fmtDeg(resid.magPull)}</strong>
            <span>AZ resid (yaw_q−mech)</span>
            <strong style={{ color: residColor(resid.azResid, 5) }}>{fmtDeg(resid.azResid)}</strong>
            <span>EL resid (bar_q−mech)</span>
            <strong style={{ color: residColor(resid.elResid, 5) }}>{fmtDeg(resid.elResid)}</strong>
            <span>Bar rel yaw (→0)</span>
            <strong style={{ color: residColor(resid.relYaw, 5) }}>{fmtDeg(resid.relYaw)}</strong>
            <span>Bar rel roll (→0)</span>
            <strong style={{ color: residColor(resid.relRoll, 5) }}>{fmtDeg(resid.relRoll)}</strong>
            <span>Tilt · yaw board</span>
            <strong>{fmtDeg(yawDiag.tiltDeg ?? null)}</strong>
            <span>Tilt · zenith board</span>
            <strong>{fmtDeg(zenDiag.tiltDeg ?? null)}</strong>
          </div>
        </div>
      )}

      {/* Per-frame Euler readout */}
      <div className={styles.readout}>
        {frames.map((f) => (
          <div key={f.key} className={styles.readoutItem} style={{ opacity: visible[f.key] ? 1 : 0.4 }}>
            <span style={{ color: f.color }}>{f.label}</span>
            <strong>{quatToEulerDeg(resolveQuat(f, imu))}</strong>
          </div>
        ))}
      </div>
    </div>
  );
}
