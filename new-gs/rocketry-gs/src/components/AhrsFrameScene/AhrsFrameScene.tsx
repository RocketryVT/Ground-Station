import { Canvas } from '@react-three/fiber';
import { Grid, Html, Line, OrbitControls } from '@react-three/drei';

import type { GroundImuState } from '../../types/telemetry';
import styles from './AhrsFrameScene.module.css';

type Quat4 = [number, number, number, number];
type Vec3 = [number, number, number];

interface Props {
  imu: GroundImuState | null;
}

interface FrameSpec {
  key: string;
  label: string;
  q?: Quat4;
  position: Vec3;
  color: string;
}

const BODY_AXES: Array<{ label: string; body: Vec3; color: string; scale: number }> = [
  { label: '+X', body: [1, 0, 0], color: '#ff4d5e', scale: 0.46 },
  { label: '+Y', body: [0, 1, 0], color: '#4dff9a', scale: 0.38 },
  { label: '+Z', body: [0, 0, 1], color: '#56a8ff', scale: 0.32 },
];

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

function nedToThree(v: Vec3): Vec3 {
  return [v[1], -v[2], -v[0]];
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
  return `${(roll * 180 / Math.PI).toFixed(1)} / ${(pitch * 180 / Math.PI).toFixed(1)} / ${(yaw * 180 / Math.PI).toFixed(1)}`;
}

function FrameGizmo({ spec }: { spec: FrameSpec }) {
  const q = spec.q;
  const origin = spec.position;

  return (
    <group>
      <mesh position={origin}>
        <boxGeometry args={[0.16, 0.045, 0.12]} />
        <meshStandardMaterial color={spec.color} roughness={0.42} metalness={0.12} />
      </mesh>

      {q && BODY_AXES.map((axis) => {
        const end = add(origin, scale(nedToThree(quatRotate(q, axis.body)), axis.scale));
        return (
          <group key={axis.label}>
            <Line points={[origin, end]} color={axis.color} lineWidth={2.2} />
            <mesh position={end}>
              <sphereGeometry args={[0.026, 12, 8]} />
              <meshStandardMaterial color={axis.color} />
            </mesh>
          </group>
        );
      })}

      <Html position={[origin[0], origin[1] + 0.18, origin[2]]} center>
        <div className={styles.label}>{spec.label}</div>
      </Html>
    </group>
  );
}

function TrackerModel() {
  return (
    <group position={[0, -0.74, 0]}>
      <mesh position={[0, 0.12, 0]}>
        <cylinderGeometry args={[0.035, 0.045, 0.55, 16]} />
        <meshStandardMaterial color="#3a3f48" />
      </mesh>
      <mesh position={[0, 0.43, 0]}>
        <cylinderGeometry args={[0.18, 0.18, 0.08, 24]} />
        <meshStandardMaterial color="#861F41" />
      </mesh>
      <mesh position={[0, 0.58, 0.48]}>
        <boxGeometry args={[0.03, 0.03, 0.95]} />
        <meshStandardMaterial color="#d7dce5" />
      </mesh>
      {[0.1, 0.24, 0.38, 0.52, 0.66, 0.8].map((z, i) => (
        <mesh key={z} position={[0, 0.58, z]}>
          <boxGeometry args={[0.48 - i * 0.035, 0.012, 0.012]} />
          <meshStandardMaterial color={i === 1 ? '#CA4F00' : '#aeb6c2'} />
        </mesh>
      ))}
      <mesh position={[-0.26, 0.6, 0.24]}>
        <boxGeometry args={[0.13, 0.018, 0.09]} />
        <meshStandardMaterial color="#56a8ff" />
      </mesh>
      <mesh position={[0.24, 0.49, -0.04]}>
        <boxGeometry args={[0.12, 0.018, 0.09]} />
        <meshStandardMaterial color="#4dff9a" />
      </mesh>
    </group>
  );
}

export function AhrsFrameScene({ imu }: Props) {
  const frames: FrameSpec[] = [
    { key: 'yaw', label: 'YAW q_EY', q: imu?.yaw_q, position: [-1.2, 0.55, 0], color: '#4dff9a' },
    { key: 'bar', label: 'ZENITH q_EB', q: imu?.bar_q, position: [0, 0.55, 0], color: '#56a8ff' },
    { key: 'rel', label: 'LOCAL q_YB', q: imu?.bar_rel_q, position: [1.2, 0.55, 0], color: '#f0a03d' },
    { key: 'final', label: 'TRACKER q', q: imu?.q, position: [0, -0.2, 0], color: '#d7dce5' },
  ];
  const hasAny = frames.some((frame) => frame.q);

  return (
    <div className={styles.wrapper}>
      <Canvas camera={{ position: [2.7, 1.9, 3.4], fov: 44 }} style={{ background: '#0b0d12' }}>
        <ambientLight intensity={0.82} />
        <directionalLight position={[3, 5, 4]} intensity={1.15} />
        <directionalLight position={[-4, 2, -3]} intensity={0.35} />
        <Grid
          args={[5, 5]}
          cellSize={0.5}
          cellColor="#252b35"
          sectionSize={1}
          sectionColor="#3a414d"
          position={[0, -0.85, 0]}
          fadeDistance={6}
          infiniteGrid
        />
        <TrackerModel />
        {frames.map((frame) => <FrameGizmo key={frame.key} spec={frame} />)}
        <OrbitControls enablePan={false} minDistance={2.0} maxDistance={7} target={[0, 0.05, 0]} />
      </Canvas>

      {!hasAny && <div className={styles.empty}>WAITING FOR AHRS QUATERNIONS</div>}

      <div className={styles.readout}>
        {frames.map((frame) => (
          <div key={frame.key} className={styles.readoutItem}>
            <span>{frame.label}</span>
            <strong>{quatToEulerDeg(frame.q)}</strong>
          </div>
        ))}
      </div>
    </div>
  );
}
