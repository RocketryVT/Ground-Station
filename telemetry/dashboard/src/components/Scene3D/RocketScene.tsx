import { useRef } from 'react';
import { Canvas, useFrame } from '@react-three/fiber';
import { OrbitControls, Grid, Line, Html } from '@react-three/drei';
import * as THREE from 'three';
import { useTelemetryStore } from '../../store/telemetryStore';
import styles from './RocketScene.module.css';

// ── Rocket geometry (procedural — swap for a GLTF if you have a model) ─────────
function RocketModel() {
  const groupRef = useRef<THREE.Group>(null);

  useFrame(() => {
    const latest = useTelemetryStore.getState().latest;
    if (!groupRef.current || !latest) return;
    groupRef.current.rotation.set(
      THREE.MathUtils.degToRad(latest.pitch),
      THREE.MathUtils.degToRad(latest.yaw),
      THREE.MathUtils.degToRad(latest.roll),
    );
  });

  return (
    <group ref={groupRef}>
      {/* Body */}
      <mesh>
        <cylinderGeometry args={[0.08, 0.08, 1.6, 24]} />
        <meshStandardMaterial color="#cccccc" metalness={0.8} roughness={0.25} />
      </mesh>

      {/* Nose cone */}
      <mesh position={[0, 0.95, 0]}>
        <coneGeometry args={[0.08, 0.5, 24]} />
        <meshStandardMaterial color="#dd3333" metalness={0.5} roughness={0.3} />
      </mesh>

      {/* 4 fins */}
      {[0, 90, 180, 270].map((deg) => (
        <mesh
          key={deg}
          position={[
            Math.sin(THREE.MathUtils.degToRad(deg)) * 0.12,
            -0.55,
            Math.cos(THREE.MathUtils.degToRad(deg)) * 0.12,
          ]}
          rotation={[0, THREE.MathUtils.degToRad(deg), 0]}
        >
          <boxGeometry args={[0.28, 0.35, 0.015]} />
          <meshStandardMaterial color="#999999" metalness={0.7} roughness={0.3} />
        </mesh>
      ))}

      {/* Motor nozzle */}
      <mesh position={[0, -0.85, 0]}>
        <cylinderGeometry args={[0.04, 0.07, 0.12, 16]} />
        <meshStandardMaterial color="#555555" metalness={0.9} roughness={0.2} />
      </mesh>
    </group>
  );
}

// ── Body-frame axis indicators ─────────────────────────────────────────────────
function BodyAxes() {
  const groupRef = useRef<THREE.Group>(null);

  useFrame(() => {
    const latest = useTelemetryStore.getState().latest;
    if (!groupRef.current || !latest) return;
    groupRef.current.rotation.set(
      THREE.MathUtils.degToRad(latest.pitch),
      THREE.MathUtils.degToRad(latest.yaw),
      THREE.MathUtils.degToRad(latest.roll),
    );
  });

  return (
    <group ref={groupRef}>
      {/* +Y = up (body axis) */}
      <Line points={[[0,0,0],[0,1.4,0]]} color="#861F41" lineWidth={1.5} />
      {/* +X = right */}
      <Line points={[[0,0,0],[0.6,0,0]]} color="#CA4F00" lineWidth={1.5} />
      {/* +Z = forward */}
      <Line points={[[0,0,0],[0,0,0.6]]} color="#75787B" lineWidth={1.5} />
    </group>
  );
}

// ── Label overlay ──────────────────────────────────────────────────────────────
function AttitudeReadout() {
  const latest = useTelemetryStore((s) => s.latest);
  if (!latest) return null;
  return (
    <div className={styles.readout}>
      <div className={styles.row}><span>ROLL</span><span>{latest.roll.toFixed(1)}°</span></div>
      <div className={styles.row}><span>PITCH</span><span>{latest.pitch.toFixed(1)}°</span></div>
      <div className={styles.row}><span>YAW</span><span>{latest.yaw.toFixed(1)}°</span></div>
    </div>
  );
}

// ── Compass rose on the ground plane ──────────────────────────────────────────
function Compass() {
  const y = -1.59;
  const r = 1.0;
  const dirs: [string, number, number, number][] = [
    ['N', 0,  y, -r],
    ['E', r,  y,  0],
    ['S', 0,  y,  r],
    ['W', -r, y,  0],
  ];
  return (
    <group>
      {/* North indicator — maroon */}
      <Line points={[[0, y, 0], [0, y, -0.85]]} color="#861F41" lineWidth={2} />
      <mesh position={[0, y, -0.95]} rotation={[Math.PI / 2, 0, Math.PI]}>
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

export function RocketScene() {
  return (
    <div className={styles.wrapper}>
      <div className={styles.panelLabel}>ORIENTATION</div>
      <Canvas camera={{ position: [0, 0.5, 4], fov: 45 }}>
        <color attach="background" args={['#ffffff']} />
        <ambientLight intensity={0.7} />
        <directionalLight position={[5, 8, 5]} intensity={1.2} />
        <pointLight position={[-3, -2, -3]} intensity={0.4} />
        <RocketModel />
        <BodyAxes />
        <Grid
          args={[8, 8]}
          cellSize={0.5}
          cellColor="#D7D2CB"
          sectionColor="#E5E1E6"
          fadeDistance={6}
          infiniteGrid
        />

        <Compass />

        <OrbitControls enablePan={false} minDistance={2} maxDistance={8} />
      </Canvas>
      <AttitudeReadout />
    </div>
  );
}
