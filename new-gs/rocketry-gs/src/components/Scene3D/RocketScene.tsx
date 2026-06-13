import { useEffect, useMemo, useRef, useState, type RefObject } from 'react';
import { Canvas, useFrame } from '@react-three/fiber';
import { OrbitControls, Grid, Line, Html } from '@react-three/drei';
import * as THREE from 'three';
import { MTLLoader } from 'three/examples/jsm/loaders/MTLLoader.js';
import { OBJLoader } from 'three/examples/jsm/loaders/OBJLoader.js';
import { useTelemetryStore } from '../../store/telemetryStore';
import { formatFeet } from '../../utils/units';
import { phaseFromState, isSeparated, isMainChute, PHASE_LABEL } from '../../utils/flightPhase';
import { nedQuatToDisplay } from '../../utils/attitude';
import type { TxId, TxTelemetry } from '../../types/telemetry';
import styles from './RocketScene.module.css';

const FLAP_MAX_ANGLE_DEG = 60;
const OPENROCKET_BASE = '/models/openrocket/';
const OPENROCKET_OBJ = 'full_rocket.obj';
const OPENROCKET_MTL = 'full_rocket.mtl';
const OPENROCKET_DISPLAY_LENGTH = 2.2;
const ACTIVE_DRAG_Y = -0.57;

// The OBJ is decomposed into 46 named parts ("1_Nose Cone" … "46_Boattail").
// For descent we split it into three sections that separate at the real joints:
//   nose  = part 1        (separates from the upper body when the drogue deploys)
//   upper = parts 2..23   (payload + main chute + e-bay)
//   lower = parts >= 24   (drogue, ADS, motor, fins, boattail)
const NOSE_MAX = 1;
const UPPER_MAX = 23;

type Section = { pivot: THREE.Group; half: number };
type Loaded = { whole: THREE.Group; nose: Section; upper: Section; lower: Section };

function partNumber(name: string): number {
  const n = parseInt(name, 10);
  return Number.isFinite(n) ? n : 0;
}

function sectionFor(num: number): 'nose' | 'upper' | 'lower' {
  if (num <= NOSE_MAX) return 'nose';
  if (num <= UPPER_MAX) return 'upper';
  return 'lower';
}

// Pull a set of parts into their own pivot group: meshes recentered about the
// section's own centroid, oriented so the airframe (local +Z) points up (+Y)
// and scaled to display size. The returned pivot can then be freely posed.
function buildSection(children: THREE.Object3D[], scale: number): Section {
  const inner = new THREE.Group();
  for (const child of children) inner.add(child);
  const box = new THREE.Box3().setFromObject(inner);
  const center = box.getCenter(new THREE.Vector3());
  const lenZ = box.getSize(new THREE.Vector3()).z;
  inner.position.copy(center).multiplyScalar(-1);

  const oriented = new THREE.Group();
  oriented.add(inner);
  oriented.rotation.x = -Math.PI / 2;
  oriented.scale.setScalar(scale);

  const pivot = new THREE.Group();
  pivot.add(oriented);
  return { pivot, half: (lenZ * scale) / 2 };
}

async function loadRocket(): Promise<Loaded> {
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
  const size = box.getSize(new THREE.Vector3());
  const center = box.getCenter(new THREE.Vector3());
  const scale = OPENROCKET_DISPLAY_LENGTH / Math.max(size.z, 0.001);

  // Whole rocket for ascent — clone first so the split below can reuse the meshes.
  const wholeInner = assembly.clone(true);
  wholeInner.position.sub(center);
  const whole = new THREE.Group();
  whole.add(wholeInner);
  whole.scale.setScalar(scale);
  whole.rotation.x = -Math.PI / 2;

  // Split the original into the three separable sections.
  const buckets: Record<'nose' | 'upper' | 'lower', THREE.Object3D[]> =
    { nose: [], upper: [], lower: [] };
  for (const child of [...assembly.children]) {
    buckets[sectionFor(partNumber(child.name))].push(child);
  }

  return {
    whole,
    nose: buildSection(buckets.nose, scale),
    upper: buildSection(buckets.upper, scale),
    lower: buildSection(buckets.lower, scale),
  };
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
function Parachute({ radius, y, attachY, color, showLines = true }: {
  radius: number;
  y: number;       // canopy rim height (open bottom of the dome)
  attachY: number; // confluence point the shroud lines run down to
  color: string;
  showLines?: boolean;
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
      {showLines && <Line points={lines} segments color="#9aa3ad" lineWidth={1} />}
    </group>
  );
}

// -- Descent: each separated half is a rigid body of stacked sections, hung
// from a fixed point and oriented LIVE by its own transmitter's attitude.
//   915 (nose)  drives the nose-side body — nose section, + upper once main is out
//   433 (ads)   drives the aft body       — upper+lower under drogue, lower under main
const HANG_TOP = 0.88;       // height the canopy lines attach to each body
const DR_HALF_X = 0.17;      // half-spacing of the two bodies under drogue (∀)
const MN_HALF_X = 0.34;      // half-spacing under main (∩)

// One hanging rigid body: its member sections stacked along the airframe
// (nose = local +Y), attached at one end, swinging from `groupRef` about the
// attach point. Orientation is applied to groupRef per-frame from telemetry.
function HangBody({ sections, attachAft, x, groupRef }: {
  sections: Section[];          // ordered nose -> tail
  attachAft: boolean;           // true: hang from the aft end (nose-down body)
  x: number;
  groupRef: RefObject<THREE.Group | null>;
}) {
  const total = sections.reduce((sum, s) => sum + 2 * s.half, 0);
  // stack centered on the body origin, nose (+Y) at the top
  let top = total / 2;
  const placed = sections.map((s) => {
    const y = top - s.half;
    top -= 2 * s.half;
    return { s, y };
  });
  // shift so the attach end sits at the swing pivot (group origin)
  const offsetY = attachAft ? total / 2 : -total / 2;

  return (
    <group ref={groupRef} position={[x, HANG_TOP, 0]}>
      <group position={[0, offsetY, 0]}>
        {placed.map(({ s, y }, i) => (
          <primitive key={i} object={s.pivot} position={[0, y, 0]} rotation={[0, 0, 0]} />
        ))}
      </group>
    </group>
  );
}

function DescentRig({ loaded, main }: { loaded: Loaded; main: boolean }) {
  const { nose, upper, lower } = loaded;
  const noseRef = useRef<THREE.Group>(null); // driven by the 915 (nose) transmitter
  const adsRef = useRef<THREE.Group>(null);  // driven by the 433 (ads) transmitter

  // Orient each half from its transmitter's reported attitude, smoothed.
  useFrame(() => {
    const tx = useTelemetryStore.getState().tx;
    if (tx.nose && noseRef.current) {
      noseRef.current.quaternion.slerp(nedQuatToDisplay(tx.nose.quat), 0.25);
    }
    if (tx.ads && adsRef.current) {
      adsRef.current.quaternion.slerp(nedQuatToDisplay(tx.ads.quat), 0.25);
    }
  });

  const halfX = main ? MN_HALF_X : DR_HALF_X;
  // nose-side body: just the nose under drogue, nose + upper once main is out
  const noseSections = main ? [nose, upper] : [nose];
  // aft body: upper + lower under drogue, lower alone once main is out
  const adsSections = main ? [lower] : [upper, lower];

  return (
    <group>
      <HangBody sections={noseSections} attachAft x={-halfX} groupRef={noseRef} />
      <HangBody sections={adsSections} attachAft={false} x={halfX} groupRef={adsRef} />
      {main ? (
        <>
          {/* large main canopy bridging the two bodies (∩) */}
          <Parachute radius={0.62} y={HANG_TOP + 1.0} attachY={HANG_TOP + 0.14} color="#CA4F00" />
          <Line points={[[0, HANG_TOP + 0.14, 0], [-MN_HALF_X, HANG_TOP, 0]]} color="#5a5a5a" lineWidth={1} />
          <Line points={[[0, HANG_TOP + 0.14, 0], [ MN_HALF_X, HANG_TOP, 0]]} color="#5a5a5a" lineWidth={1} />
        </>
      ) : (
        <>
          {/* small drogue over the two close bodies (∀) */}
          <Parachute radius={0.27} y={HANG_TOP + 0.40} attachY={HANG_TOP + 0.06} color="#861F41" />
          <Line points={[[0, HANG_TOP + 0.06, 0], [-DR_HALF_X, HANG_TOP, 0]]} color="#5a5a5a" lineWidth={1} />
          <Line points={[[0, HANG_TOP + 0.06, 0], [ DR_HALF_X, HANG_TOP, 0]]} color="#5a5a5a" lineWidth={1} />
        </>
      )}
    </group>
  );
}

// Whichever transmitter we heard from most recently — before separation both
// ride the one airframe, so we drive the whole rocket from the freshest of the
// two (we may receive only one at a time, or both).
function freshestTx(tx: Record<TxId, TxTelemetry | null>): TxTelemetry | null {
  const { nose, ads } = tx;
  if (!nose) return ads;
  if (!ads) return nose;
  return nose.timestamp >= ads.timestamp ? nose : ads;
}

// -- Rocket: whole model on ascent, split sections under canopy on descent -----
function RocketModel() {
  const groupRef = useRef<THREE.Group>(null);
  const [loaded, setLoaded] = useState<Loaded | null>(null);
  const latest = useTelemetryStore((s) => s.latest);
  const phase = phaseFromState(latest?.state);
  const separated = isSeparated(phase);
  const flapAngle = latest?.flap_angle_deg
    ?? (latest?.flap_deployment_percent != null ? latest.flap_deployment_percent * 0.6 : 0);

  useEffect(() => {
    let mounted = true;
    loadRocket()
      .then((l) => { if (mounted) setLoaded(l); })
      .catch((error) => console.error('Failed to load OpenRocket OBJ model', error));
    return () => { mounted = false; };
  }, []);

  useFrame(() => {
    const store = useTelemetryStore.getState();
    const l = store.latest;
    if (!groupRef.current || !l) return;
    if (isSeparated(phaseFromState(l.state))) {
      // Each half orients itself from its own transmitter (see DescentRig), so
      // the outer group stays in the world frame.
      groupRef.current.rotation.set(0, 0, 0);
      return;
    }
    // Ascent: drive the whole airframe from the most-recent transmitter, falling
    // back to the fused Euler attitude if no transmitter data has arrived yet.
    const best = freshestTx(store.tx);
    if (best) {
      groupRef.current.quaternion.slerp(nedQuatToDisplay(best.quat), 0.3);
    } else {
      groupRef.current.rotation.set(
        THREE.MathUtils.degToRad(l.pitch),
        THREE.MathUtils.degToRad(l.yaw),
        THREE.MathUtils.degToRad(l.roll),
      );
    }
  });

  if (!loaded) return null;
  return (
    <group ref={groupRef}>
      {separated ? (
        <DescentRig loaded={loaded} main={isMainChute(phase)} />
      ) : (
        <>
          <primitive object={loaded.whole} />
          {[0, 90, 180, 270].map((deg) => (
            <ActiveDragFlap key={deg} angleDeg={deg} deployDeg={flapAngle} />
          ))}
        </>
      )}
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
const TX_LABEL: Record<TxId, string> = { nose: 'NOSE 915', ads: 'ADS 433' };

function AttitudeReadout() {
  const latest = useTelemetryStore((s) => s.latest);
  const tx = useTelemetryStore((s) => s.tx);
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
      {(['nose', 'ads'] as TxId[]).map((id) => {
        const t = tx[id];
        if (!t) return null;
        return (
          <div key={id} className={styles.row}>
            <span>{TX_LABEL[id]}</span>
            <span>{t.rssi ?? '--'} dBm · {t.have_gps ? 'GPS' : 'no fix'}</span>
          </div>
        );
      })}
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
