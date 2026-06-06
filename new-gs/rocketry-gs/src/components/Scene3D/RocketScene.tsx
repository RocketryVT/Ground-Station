import { useEffect, useMemo, useRef, useState } from 'react';
import { Canvas, useFrame } from '@react-three/fiber';
import { OrbitControls, Grid, Line, Html } from '@react-three/drei';
import * as THREE from 'three';
import { MTLLoader } from 'three/examples/jsm/loaders/MTLLoader.js';
import { OBJLoader } from 'three/examples/jsm/loaders/OBJLoader.js';
import { useTelemetryStore } from '../../store/telemetryStore';
import { formatFeet } from '../../utils/units';
import { phaseFromState, isSeparated, isMainChute, PHASE_LABEL } from '../../utils/flightPhase';
import styles from './RocketScene.module.css';

const FLAP_MAX_ANGLE_DEG = 60;
const OPENROCKET_BASE = '/models/openrocket/';
const OPENROCKET_OBJ = 'full_rocket.obj';
const OPENROCKET_MTL = 'full_rocket.mtl';
const OPENROCKET_DISPLAY_LENGTH = 2.2;
const ACTIVE_DRAG_Y = -0.57;

// The OBJ is decomposed into 46 named parts ("1_Nose Cone" … "46_Boattail").
// Parts numbered >= this go in the lower/aft section; below it is the forebody.
// At separation the forebody slides apart from the aft body along the airframe.
const SEP_PART_THRESHOLD = 24;
// Separation gap as a fraction of the rocket's length.
const SEP_FRACTION = 0.16;

function partNumber(name: string): number {
  const n = parseInt(name, 10);
  return Number.isFinite(n) ? n : 0;
}

function OpenRocketAssembly({ separated = false }: { separated?: boolean }) {
  const [model, setModel] = useState<THREE.Group | null>(null);
  const upperRef = useRef<THREE.Group | null>(null);
  const offsetRef = useRef(0); // assembly-local Z offset applied to the forebody

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

      // Split the real model into forebody (upper) + aft body (lower) groups so
      // they can be slid apart on separation — the airframe runs along local Z.
      const upper = new THREE.Group();
      const lower = new THREE.Group();
      for (const child of [...assembly.children]) {
        (partNumber(child.name) >= SEP_PART_THRESHOLD ? lower : upper).add(child);
      }
      assembly.add(upper, lower);

      const uc = new THREE.Box3().setFromObject(upper).getCenter(new THREE.Vector3());
      const lc = new THREE.Box3().setFromObject(lower).getCenter(new THREE.Vector3());
      const noseSign = Math.sign(uc.z - lc.z) || 1; // push the nose section away from the tail
      offsetRef.current = noseSign * SEP_FRACTION * size.z;
      upperRef.current = upper;

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

  useEffect(() => {
    if (upperRef.current) upperRef.current.position.z = separated ? offsetRef.current : 0;
  }, [separated, model]);

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

  // The flap is an airbrake that hinges radially OUTWARD (+Z = away from the body
  // axis), not sideways. The hinge is the tangential (local X) line at the flap's
  // TOP edge; deploy swings the bottom free edge out into the airflow while the
  // top stays attached. The outer group's rotation.y only positions the flap
  // around the body (0/90/180/270°).
  return (
    <group rotation={[0, THREE.MathUtils.degToRad(angleDeg), 0]} position={[0, ACTIVE_DRAG_Y, 0]}>
      {/* hinge rod along the tangential axis, at the top of the flap */}
      <mesh position={[0, 0.18, 0.078]} rotation={[0, 0, Math.PI / 2]}>
        <cylinderGeometry args={[0.008, 0.008, 0.18, 10]} />
        <meshStandardMaterial color="#30343a" metalness={0.7} roughness={0.35} />
      </mesh>
      <group position={[0, 0.18, 0.082]} rotation={[-deploy, 0, 0]}>
        <mesh position={[0, -0.18, 0]}>
          <extrudeGeometry args={[shape, { depth: 0.012, bevelEnabled: true, bevelSize: 0.003, bevelThickness: 0.003 }]} />
          <meshStandardMaterial color="#f0a03d" metalness={0.42} roughness={0.3} side={THREE.DoubleSide} />
        </mesh>
      </group>
    </group>
  );
}

// -- Parachute (dome canopy + shroud lines) ------------------------------------
function Parachute({ radius, y, attachY, color }: {
  radius: number;
  y: number;       // canopy rim height (open bottom of the dome)
  attachY: number; // confluence point the shroud lines run down to
  color: string;
}) {
  const N = 12;
  const lines = useMemo(() => {
    const pts: [number, number, number][] = [];
    for (let i = 0; i < N; i++) {
      const a = (i / N) * Math.PI * 2;
      pts.push([Math.cos(a) * radius * 0.96, y, Math.sin(a) * radius * 0.96]); // rim
      pts.push([0, attachY, 0]);                                              // confluence
    }
    return pts;
  }, [radius, y, attachY]);

  return (
    <group>
      <mesh position={[0, y, 0]}>
        {/* upper hemisphere = dome up, open bottom */}
        <sphereGeometry args={[radius, 24, 14, 0, Math.PI * 2, 0, Math.PI / 2]} />
        <meshStandardMaterial color={color} side={THREE.DoubleSide} roughness={0.85} metalness={0} />
      </mesh>
      <Line points={lines} segments color="#9aa3ad" lineWidth={1} />
    </group>
  );
}

// -- Deployed chute over the separated airframe --------------------------------
// The body itself is the real (split) OpenRocket model; this just adds the
// deployed canopy + shock cord. Drogue is small and sits just above the gap;
// main is large and rides above the nose. Heights assume the model spans
// ~±1.1 in Y with the forebody lifted by SEP_FRACTION.
function DescentChute({ main }: { main: boolean }) {
  return (
    <group>
      {/* shock cord tethering the two separated sections across the gap */}
      <Line points={[[0, 0.35, 0], [0, -0.05, 0]]} color="#5a5a5a" lineWidth={1} />
      {main
        ? <Parachute radius={0.72} y={1.95} attachY={1.45} color="#CA4F00" />
        : <Parachute radius={0.30} y={0.95} attachY={0.30} color="#861F41" />}
    </group>
  );
}

// -- Rocket: the real OpenRocket model (split apart on separation) + chute ------
function RocketModel() {
  const groupRef = useRef<THREE.Group>(null);
  const latest = useTelemetryStore((s) => s.latest);
  const phase = phaseFromState(latest?.state);
  const separated = isSeparated(phase);
  const flapAngle = latest?.flap_angle_deg
    ?? (latest?.flap_deployment_percent != null ? latest.flap_deployment_percent * 0.6 : 0);

  useFrame((state) => {
    const l = useTelemetryStore.getState().latest;
    if (!groupRef.current || !l) return;
    if (isSeparated(phaseFromState(l.state))) {
      // Hangs under canopy: keep heading (yaw) + a gentle pendulum sway rather
      // than the meaningless tumbling flight attitude.
      const t = state.clock.elapsedTime;
      groupRef.current.rotation.set(
        Math.cos(t * 0.6) * 0.04,
        THREE.MathUtils.degToRad(l.yaw),
        Math.sin(t * 0.8) * 0.05,
      );
    } else {
      groupRef.current.rotation.set(
        THREE.MathUtils.degToRad(l.pitch),
        THREE.MathUtils.degToRad(l.yaw),
        THREE.MathUtils.degToRad(l.roll),
      );
    }
  });

  return (
    <group ref={groupRef}>
      <OpenRocketAssembly separated={separated} />
      {separated
        ? <DescentChute main={isMainChute(phase)} />
        : [0, 90, 180, 270].map((deg) => (
            <ActiveDragFlap key={deg} angleDeg={deg} deployDeg={flapAngle} />
          ))}
    </group>
  );
}

// -- Body-frame axis indicators -------------------------------------------------
function BodyAxes() {
  const groupRef = useRef<THREE.Group>(null);
  const latest = useTelemetryStore((s) => s.latest);
  const separated = isSeparated(phaseFromState(latest?.state));

  useFrame(() => {
    const l = useTelemetryStore.getState().latest;
    if (!groupRef.current || !l) return;
    groupRef.current.rotation.set(
      THREE.MathUtils.degToRad(l.pitch),
      THREE.MathUtils.degToRad(l.yaw),
      THREE.MathUtils.degToRad(l.roll),
    );
  });

  // Body axes are meaningless once the rocket is hanging under a chute.
  if (separated) return null;

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
      <div className={styles.row}><span>PHASE</span><span>{PHASE_LABEL[phaseFromState(latest.state)]}</span></div>
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
