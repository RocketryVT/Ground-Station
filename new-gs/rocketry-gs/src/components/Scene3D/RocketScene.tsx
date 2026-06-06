import { useEffect, useMemo, useRef, useState } from 'react';
import { Canvas, useFrame } from '@react-three/fiber';
import { OrbitControls, Grid, Line, Html } from '@react-three/drei';
import * as THREE from 'three';
import { MTLLoader } from 'three/examples/jsm/loaders/MTLLoader.js';
import { OBJLoader } from 'three/examples/jsm/loaders/OBJLoader.js';
import { useTelemetryStore } from '../../store/telemetryStore';
import { formatFeet } from '../../utils/units';
import styles from './RocketScene.module.css';

const FLAP_MAX_ANGLE_DEG = 60;
const OPENROCKET_BASE = '/models/openrocket/';
const OPENROCKET_OBJ = 'full_rocket.obj';
const OPENROCKET_MTL = 'full_rocket.mtl';
const OPENROCKET_DISPLAY_LENGTH = 2.2;
const ACTIVE_DRAG_Y = -0.57;

function OpenRocketAssembly() {
  const [model, setModel] = useState<THREE.Group | null>(null);

  useEffect(() => {
    let mounted = true;

    async function loadModel() {
      const mtlLoader = new MTLLoader();
      mtlLoader.setPath(OPENROCKET_BASE);
      mtlLoader.setResourcePath(OPENROCKET_BASE);
      const materials = await mtlLoader.loadAsync(OPENROCKET_MTL);
      materials.preload();

      const objLoader = new OBJLoader();
      objLoader.setPath(OPENROCKET_BASE);
      objLoader.setMaterials(materials);
      const assembly = await objLoader.loadAsync(OPENROCKET_OBJ);
      assembly.traverse((child) => {
        if (child.name.includes('ADS Flaps')) child.visible = false;
        if (child instanceof THREE.Mesh) {
          child.castShadow = true;
          child.receiveShadow = true;
          child.material = Array.isArray(child.material)
            ? child.material.map((material) => material.clone())
            : child.material.clone();
        }
      });

      const box = new THREE.Box3().setFromObject(assembly);
      const center = box.getCenter(new THREE.Vector3());
      const size = box.getSize(new THREE.Vector3());
      assembly.position.sub(center);

      const normalized = new THREE.Group();
      normalized.add(assembly);
      normalized.scale.setScalar(OPENROCKET_DISPLAY_LENGTH / Math.max(size.z, 0.001));
      normalized.rotation.x = -Math.PI / 2;

      if (mounted) setModel(normalized);
    }

    loadModel().catch((error) => {
      console.error('Failed to load OpenRocket OBJ model', error);
    });

    return () => {
      mounted = false;
    };
  }, []);

  if (!model) return null;
  return <primitive object={model} />;
}

function ActiveDragFlap({ angleDeg, deployDeg }: { angleDeg: number; deployDeg: number }) {
  const shape = useMemo(() => {
    const s = new THREE.Shape();
    s.moveTo(-0.055, -0.18);
    s.lineTo(0.048, -0.18);
    s.quadraticCurveTo(0.085, 0, 0.048, 0.18);
    s.lineTo(-0.055, 0.18);
    s.quadraticCurveTo(-0.078, 0, -0.055, -0.18);
    return s;
  }, []);

  const deploy = THREE.MathUtils.degToRad(THREE.MathUtils.clamp(deployDeg, 0, FLAP_MAX_ANGLE_DEG));

  return (
    <group rotation={[0, THREE.MathUtils.degToRad(angleDeg), 0]} position={[0, ACTIVE_DRAG_Y, 0]}>
      <mesh position={[0, 0, 0.078]}>
        <cylinderGeometry args={[0.008, 0.008, 0.42, 10]} />
        <meshStandardMaterial color="#30343a" metalness={0.7} roughness={0.35} />
      </mesh>
      <group position={[0, 0, 0.089]} rotation={[0, deploy, 0]}>
        <mesh>
          <extrudeGeometry args={[shape, { depth: 0.012, bevelEnabled: true, bevelSize: 0.003, bevelThickness: 0.003 }]} />
          <meshStandardMaterial color="#f0a03d" metalness={0.42} roughness={0.3} side={THREE.DoubleSide} />
        </mesh>
      </group>
    </group>
  );
}

// -- Rocket geometry (procedural — swap for a GLTF if you have a model) ---------
function RocketModel() {
  const groupRef = useRef<THREE.Group>(null);
  const latest = useTelemetryStore((s) => s.latest);
  const flapAngle = latest?.flap_angle_deg
    ?? (latest?.flap_deployment_percent != null ? latest.flap_deployment_percent * 0.6 : 0);

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
      <OpenRocketAssembly />

      {/* Telemetry-driven active-drag flaps. The static OpenRocket ADS flap group is hidden. */}
      {[0, 90, 180, 270].map((deg) => (
        <ActiveDragFlap key={deg} angleDeg={deg} deployDeg={flapAngle} />
      ))}
    </group>
  );
}

// -- Body-frame axis indicators -------------------------------------------------
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

// -- Label overlay --------------------------------------------------------------
function AttitudeReadout() {
  const latest = useTelemetryStore((s) => s.latest);
  if (!latest) return null;
  const flapAngle = latest.flap_angle_deg
    ?? (latest.flap_deployment_percent != null ? latest.flap_deployment_percent * 0.6 : 0);
  return (
    <div className={styles.readout}>
      <div className={styles.row}><span>ROLL</span><span>{latest.roll.toFixed(1)}°</span></div>
      <div className={styles.row}><span>PITCH</span><span>{latest.pitch.toFixed(1)}°</span></div>
      <div className={styles.row}><span>YAW</span><span>{latest.yaw.toFixed(1)}°</span></div>
      <div className={styles.row}><span>FLAP</span><span>{flapAngle.toFixed(1)}°</span></div>
      {latest.predicted_apogee_m != null && (
        <div className={styles.row}><span>APOGEE</span><span>{formatFeet(latest.predicted_apogee_m)}</span></div>
      )}
      {latest.target_apogee_m != null && (
        <div className={styles.row}><span>TARGET</span><span>{formatFeet(latest.target_apogee_m)}</span></div>
      )}
    </div>
  );
}

// -- Compass rose on the ground plane ------------------------------------------
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
