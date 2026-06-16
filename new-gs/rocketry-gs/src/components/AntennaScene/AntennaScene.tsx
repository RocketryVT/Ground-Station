import { Canvas } from '@react-three/fiber';
import { OrbitControls, Grid, Line, Html } from '@react-three/drei';

import { useTelemetryStore } from '../../store/telemetryStore';
import { BEAM_HALF_ANGLE_DEG } from '../../config';
import { GROUND_STATION } from '../AhrsFrameScene/groundStationModel';
import type { PartNode, Vec3, Yagi } from '../AhrsFrameScene/groundStationModel';
import { GroundStationModelView } from '../AhrsFrameScene/GroundStationModelView';
import styles from './AntennaScene.module.css';

const DEG = Math.PI / 180;

function addVec(a: Vec3, b: Vec3): Vec3 {
  return [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
}

function findNode(node: PartNode, id: string): PartNode | null {
  if (node.id === id) return node;
  for (const child of node.children ?? []) {
    const found = findNode(child, id);
    if (found) return found;
  }
  return null;
}

function findNodePosition(node: PartNode, id: string, parent: Vec3 = [0, 0, 0]): Vec3 | null {
  const here = addVec(parent, node.position);
  if (node.id === id) return here;
  for (const child of node.children ?? []) {
    const found = findNodePosition(child, id, here);
    if (found) return found;
  }
  return null;
}

function yagiForwardTip(yagi: Yagi): Vec3 {
  const p = yagi.position ?? [0, 0, 0];
  return [p[0], p[1], p[2] + yagi.boomLength];
}

function deriveBoresightOrigin(node: PartNode | null): Vec3 {
  const yagis = node?.yagis ?? [];
  if (yagis.length === 0) return [0, 0, 0.45];

  const tips = yagis.map(yagiForwardTip);
  const avgX = tips.reduce((sum, p) => sum + p[0], 0) / tips.length;
  const avgY = tips.reduce((sum, p) => sum + p[1], 0) / tips.length;
  const frontZ = Math.max(...tips.map((p) => p[2]));
  return [avgX, avgY, frontZ];
}

const EL_PIVOT: Vec3 = findNodePosition(GROUND_STATION.root, 'el_bar') ?? [0, 0.27, 0];
const BORESIGHT_ORIGIN: Vec3 = deriveBoresightOrigin(findNode(GROUND_STATION.root, 'el_bar'));

// -- Beam cone geometry (constant, based on config half-angle) ------------------
// CONE_RANGE is the visual reach of the beam line in Three.js scene units; the
// boom/boresight is local +Z, so the cone extends past the antenna tip.
const N_CONE     = 16;
const CONE_RANGE = 2.05;
const CONE_HA    = BEAM_HALF_ANGLE_DEG * DEG;
const CONE_H     = CONE_RANGE * Math.cos(CONE_HA);
const CONE_R     = CONE_RANGE * Math.sin(CONE_HA);

const CONE_PTS = (() => {
  const [x, y, z] = BORESIGHT_ORIGIN;
  const ring = Array.from({ length: N_CONE }, (_, i) => {
    const phi = (i * 2 * Math.PI) / N_CONE;
    return [x + CONE_R * Math.cos(phi), y + CONE_R * Math.sin(phi), z + CONE_H] as [number, number, number];
  });
  const pts: [number, number, number][] = [];
  for (let i = 0; i < N_CONE; i++) {
    pts.push(BORESIGHT_ORIGIN, ring[i]);                 // spoke
    pts.push(ring[i], ring[(i + 1) % N_CONE]);           // ring segment
  }
  return pts;
})();

const BORESIGHT_END: Vec3 = [
  BORESIGHT_ORIGIN[0],
  BORESIGHT_ORIGIN[1],
  BORESIGHT_ORIGIN[2] + CONE_RANGE,
];

// -- Azimuth/elevation maths ----------------------------------------------------
// Three.js: Y-up, +X east, +Z south (toward viewer), -Z north. Boom = local +Z.
// rotation.y = π - az·DEG  -> az=0 points -Z (north), az=90 points +X (east)
// rotation.x = -el·DEG     -> el=0 horizontal, el=90 points +Y (zenith)
// These match the joint rotations in GroundStationModelView, so the beam stays
// aligned with the physical model's boom.

function Tracker({ az, el, targetAz, targetEl }: {
  az: number;
  el: number;
  targetAz?: number;
  targetEl?: number;
}) {
  const hasTarget = targetAz != null && targetEl != null;
  return (
    <group>
      <GroundStationModelView model={GROUND_STATION} az={az} el={el} showBoards={false} />

      {/* Actual pointing — beam cone + ray, pivoting at the elevation axis */}
      <group position={EL_PIVOT} rotation={[0, Math.PI - az * DEG, 0]}>
        <group rotation={[-el * DEG, 0, 0]}>
          <Line
            points={[BORESIGHT_ORIGIN, BORESIGHT_END]}
            color="#CA4F00"
            lineWidth={1.5}
            dashed
            dashSize={0.11}
            gapSize={0.08}
          />
          <Line points={CONE_PTS} color="#CA4F00" lineWidth={0.8} segments />
        </group>
      </group>

      {/* Target pointing — maroon dashed ray */}
      {hasTarget && (
        <group position={EL_PIVOT} rotation={[0, Math.PI - targetAz * DEG, 0]}>
          <group rotation={[-targetEl * DEG, 0, 0]}>
            <Line
              points={[BORESIGHT_ORIGIN, BORESIGHT_END]}
              color="#861F41"
              lineWidth={2}
              dashed
              dashSize={0.15}
              gapSize={0.1}
            />
          </group>
        </group>
      )}
    </group>
  );
}

// -- Compass rose at the assembly level ----------------------------------------
function Compass() {
  const y = -0.08;
  const r = 1.6;
  const dirs: [string, number, number, number][] = [
    ['N', 0,  y, -r],
    ['E', r,  y,  0],
    ['S', 0,  y,  r],
    ['W', -r, y,  0],
  ];
  return (
    <group>
      <Line points={[[0, y, -0.5], [0, y, -r + 0.1]]} color="#861F41" lineWidth={2} />
      <mesh position={[0, y, -r]} rotation={[Math.PI / 2, 0, Math.PI]}>
        <coneGeometry args={[0.06, 0.22, 6]} />
        <meshStandardMaterial color="#861F41" />
      </mesh>
      {dirs.map(([label, x, ly, z]) => (
        <Html key={label} position={[x, ly + 0.1, z]} center>
          <span style={{
            fontFamily: 'Courier New, monospace',
            fontSize: '12px',
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

// -- Scene ---------------------------------------------------------------------
export function AntennaScene() {
  const antenna = useTelemetryStore((s) => s.antenna);
  const az = antenna?.actual_az ?? 0;
  const el = antenna?.actual_el ?? 0;
  const visualEl = Math.abs(el);

  return (
    <div className={styles.wrapper}>
      <Canvas
        camera={{ position: [3.9, 2.3, 4.7], fov: 46 }}
        style={{ background: 'white' }}
      >
        <ambientLight intensity={0.9} />
        <directionalLight position={[5, 8, 4]} intensity={1.0} castShadow />
        <directionalLight position={[-4, 3, -4]} intensity={0.3} />

        <Grid
          args={[12, 12]}
          cellSize={0.5}
          cellColor="#d8d8d8"
          sectionSize={1}
          sectionColor="#bbbbbb"
          position={[0, -2.42, 0]}
          fadeDistance={20}
          infiniteGrid
        />

        <Tracker
          az={az}
          el={visualEl}
          targetAz={antenna?.target_az}
          targetEl={antenna?.target_el == null ? undefined : Math.abs(antenna.target_el)}
        />
        <Compass />

        <OrbitControls
          enablePan={false}
          minDistance={1.5}
          maxDistance={20}
          target={[0, 0.3, 0]}
        />
      </Canvas>

      {/* -- AZ / EL readout -------------------------------------------- */}
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
