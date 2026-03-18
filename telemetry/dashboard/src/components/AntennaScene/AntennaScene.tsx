import { useRef } from 'react';
import { Canvas, useFrame } from '@react-three/fiber';
import { OrbitControls, Grid, Line, Html } from '@react-three/drei';
import * as THREE from 'three';

import { useTelemetryStore } from '../../store/telemetryStore';
import { BEAM_HALF_ANGLE_DEG } from '../../config';
import styles from './AntennaScene.module.css';

const DEG = Math.PI / 180;

// ── Beam cone geometry (constant, based on config half-angle) ──────────────────
// Matches the Cesium cone: N spokes from apex + N ring segments.
// CONE_RANGE is the visual reach of the beam line in Three.js scene units.
const N_CONE     = 16;
const CONE_RANGE = 2.05;  // same end-point as the pointing ray (z = 1.15 + 2.05 ≈ 3.2)
const CONE_HA    = BEAM_HALF_ANGLE_DEG * DEG;
const CONE_H     = CONE_RANGE * Math.cos(CONE_HA);
const CONE_R     = CONE_RANGE * Math.sin(CONE_HA);
const CONE_APEX: [number, number, number] = [0, 0, 1.15];

// Pre-built flat segment list: each consecutive pair = one segment (for <Line segments>)
const CONE_PTS = (() => {
  const ring = Array.from({ length: N_CONE }, (_, i) => {
    const phi = (i * 2 * Math.PI) / N_CONE;
    return [CONE_R * Math.cos(phi), CONE_R * Math.sin(phi), 1.15 + CONE_H] as [number, number, number];
  });
  const pts: [number, number, number][] = [];
  for (let i = 0; i < N_CONE; i++) {
    pts.push(CONE_APEX, ring[i]);                        // spoke
    pts.push(ring[i], ring[(i + 1) % N_CONE]);           // ring segment
  }
  return pts;
})();

// ── Azimuth/elevation maths ────────────────────────────────────────────────────
// Three.js: Y-up, +X east, +Z south (toward viewer), -Z north.
// Boom extends in +Z local space.
// rotation.y = π - az·DEG  → az=0 points -Z (north), az=90 points +X (east)
// rotation.x = -el·DEG     → el=0 horizontal, el=90 points +Y (zenith)

function Tracker() {
  const azRef       = useRef<THREE.Group>(null);
  const elRef       = useRef<THREE.Group>(null);
  const targetAzRef = useRef<THREE.Group>(null);
  const targetElRef = useRef<THREE.Group>(null);

  useFrame(() => {
    const antenna = useTelemetryStore.getState().antenna;
    const az = antenna?.actual_az ?? 0;
    const el = antenna?.actual_el ?? 0;
    if (azRef.current) azRef.current.rotation.y = Math.PI - az * DEG;
    if (elRef.current) elRef.current.rotation.x = -el * DEG;

    // Target direction — shown only when target az/el are present
    const hasTarget = antenna?.target_az != null && antenna?.target_el != null;
    if (targetAzRef.current) {
      targetAzRef.current.visible    = hasTarget;
      targetAzRef.current.rotation.y = Math.PI - (antenna?.target_az ?? az) * DEG;
    }
    if (targetElRef.current) {
      targetElRef.current.rotation.x = -(antenna?.target_el ?? el) * DEG;
    }
  });

  return (
    <group>
      {/* ── Tripod legs ─────────────────────────────────────────────────── */}
      {[0, 120, 240].map((a) => (
        <group key={a} rotation={[0, a * DEG, 0]}>
          <mesh position={[0, -0.435, 0.18]} rotation={[2.28, 0, 0]}>
            <cylinderGeometry args={[0.022, 0.014, 0.48, 8]} />
            <meshStandardMaterial color="#2a2a2a" />
          </mesh>
        </group>
      ))}

      {/* ── Vertical mast ──────────────────────────────────────────────── */}
      <mesh position={[0, 0.12, 0]}>
        <cylinderGeometry args={[0.028, 0.034, 0.84, 12]} />
        <meshStandardMaterial color="#1a1a1a" />
      </mesh>

      {/* ── Azimuth group (actual direction) ───────────────────────────── */}
      <group ref={azRef} position={[0, 0.56, 0]}>
        {/* Bearing disk */}
        <mesh>
          <cylinderGeometry args={[0.1, 0.1, 0.06, 20]} />
          <meshStandardMaterial color="#861F41" />
        </mesh>

        {/* Fork arms */}
        {([-0.12, 0.12] as const).map((x) => (
          <mesh key={x} position={[x, 0.18, 0]}>
            <boxGeometry args={[0.034, 0.34, 0.034]} />
            <meshStandardMaterial color="#861F41" />
          </mesh>
        ))}

        {/* Elevation axle — polished metal */}
        <mesh position={[0, 0.35, 0]} rotation={[0, 0, Math.PI / 2]}>
          <cylinderGeometry args={[0.02, 0.02, 0.29, 12]} />
          <meshStandardMaterial color="#bbbbbb" metalness={0.9} roughness={0.1} />
        </mesh>

        {/* ── Elevation group ─────────────────────────────────────────── */}
        <group ref={elRef} position={[0, 0.35, 0]}>
          {/* Boom — extends in +Z */}
          <mesh position={[0, 0, 0.55]}>
            <boxGeometry args={[0.02, 0.02, 1.1]} />
            <meshStandardMaterial color="#111111" />
          </mesh>

          {/* Reflector — longest, behind driven element */}
          <mesh position={[0, 0, 0.02]}>
            <boxGeometry args={[0.5, 0.012, 0.012]} />
            <meshStandardMaterial color="#333333" />
          </mesh>

          {/* Driven element — orange accent */}
          <mesh position={[0, 0, 0.17]}>
            <boxGeometry args={[0.45, 0.018, 0.018]} />
            <meshStandardMaterial color="#CA4F00" emissive="#CA4F00" emissiveIntensity={0.2} />
          </mesh>

          {/* Directors — progressively shorter toward the front */}
          {[0.33, 0.49, 0.65, 0.79, 0.93, 1.06].map((z, i) => (
            <mesh key={z} position={[0, 0, z]}>
              <boxGeometry args={[Math.max(0.18, 0.41 - i * 0.026), 0.012, 0.012]} />
              <meshStandardMaterial color="#2a2a2a" />
            </mesh>
          ))}

          {/* Pointing direction ray */}
          <Line
            points={[[0, 0, 1.15], [0, 0, 3.2]]}
            color="#CA4F00"
            lineWidth={1.5}
            dashed
            dashSize={0.11}
            gapSize={0.08}
          />

          {/* Beam cone — spokes + ring matching the Cesium visualization */}
          <Line points={CONE_PTS} color="#CA4F00" lineWidth={0.8} segments />
        </group>
      </group>

      {/* ── Target direction (maroon dashed) — mirrors actual az/el pivot ─ */}
      <group ref={targetAzRef} position={[0, 0.56, 0]}>
        <group ref={targetElRef} position={[0, 0.35, 0]}>
          <Line
            points={[[0, 0, 1.15], [0, 0, 3.2]]}
            color="#861F41"
            lineWidth={2}
            dashed
            dashSize={0.15}
            gapSize={0.1}
          />
        </group>
      </group>
    </group>
  );
}

// ── Compass rose on the ground plane ──────────────────────────────────────────
function Compass() {
  const y = -0.59;
  const r = 1.0;
  const dirs: [string, number, number, number][] = [
    ['N', 0,  y, -r],
    ['E', r,  y,  0],
    ['S', 0,  y,  r],
    ['W', -r, y,  0],
  ];
  return (
    <group>
      {/* North indicator — maroon, starts 0.3 m from centre to avoid looking like a tripod leg */}
      <Line points={[[0, y, -0.3], [0, y, -0.78]]} color="#861F41" lineWidth={2} />
      <mesh position={[0, y, -0.88]} rotation={[Math.PI / 2, 0, Math.PI]}>
        <coneGeometry args={[0.045, 0.18, 6]} />
        <meshStandardMaterial color="#861F41" />
      </mesh>

      {/* Cardinal labels */}
      {dirs.map(([label, x, ly, z]) => (
        <Html key={label} position={[x, ly + 0.08, z]} center>
          <span style={{
            fontFamily: 'Courier New, monospace',
            fontSize: '11px',
            fontWeight: 'bold',
            color: label === 'N' ? '#861F41' : '#75787B',
            userSelect: 'none',
            pointerEvents: 'none',
          }}>
            {label}
          </span>
        </Html>
      ))}
    </group>
  );
}

// ── Scene ─────────────────────────────────────────────────────────────────────
export function AntennaScene() {
  const antenna = useTelemetryStore((s) => s.antenna);
  const az = antenna?.actual_az ?? 0;
  const el = antenna?.actual_el ?? 0;

  return (
    <div className={styles.wrapper}>
      <Canvas
        camera={{ position: [2.4, 1.8, 3.2], fov: 45 }}
        style={{ background: 'white' }}
      >
        <ambientLight intensity={0.85} />
        <directionalLight position={[5, 8, 4]} intensity={1.0} castShadow />
        <directionalLight position={[-4, 3, -4]} intensity={0.3} />

        <Grid
          args={[8, 8]}
          cellSize={0.5}
          cellColor="#d8d8d8"
          sectionSize={1}
          sectionColor="#bbbbbb"
          position={[0, -0.59, 0]}
          fadeDistance={8}
          infiniteGrid
        />

        <Tracker />
        <Compass />

        <OrbitControls
          enablePan={false}
          minDistance={2}
          maxDistance={9}
          target={[0, 0.3, 0]}
        />
      </Canvas>

      {/* ── AZ / EL readout ──────────────────────────────────────────── */}
      <div className={styles.readout}>
        <span className={styles.metric}>AZ</span>
        <span className={styles.value}>{az.toFixed(1)}°</span>
        <span className={styles.dot}>·</span>
        <span className={styles.metric}>EL</span>
        <span className={styles.value}>{el.toFixed(1)}°</span>
      </div>
    </div>
  );
}
