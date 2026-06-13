// Shared renderer for the ground-station physical model (groundStationModel.ts).
// Used by both the AHRS diagnostic scene (AhrsFrameScene) and the main mission
// view (AntennaScene). The azimuth/elevation joints are driven by mechanical
// actual_az / actual_el. Convention matches AntennaScene: Y up, +X east, −Z north;
// az=0 → boom north (rotation.y = π − az·DEG), el=0 → horizon (rotation.x = −el·DEG).

import { Html } from '@react-three/drei';

import type {
  Geo,
  GroundStationModel,
  PartNode,
  SensorBoard,
  Yagi,
} from './groundStationModel';
import styles from './AhrsFrameScene.module.css';

const DEG = Math.PI / 180;

export function eulerToRad(deg?: [number, number, number]): [number, number, number] {
  return deg ? [deg[0] * DEG, deg[1] * DEG, deg[2] * DEG] : [0, 0, 0];
}

function GeoMesh({ geo }: { geo: Geo }) {
  const position = geo.position ?? [0, 0, 0];
  const rotation = eulerToRad(geo.rotation);
  if (geo.kind === 'box') {
    return (
      <mesh position={position} rotation={rotation}>
        <boxGeometry args={geo.size} />
        <meshStandardMaterial
          color={geo.color}
          emissive={geo.emissive ?? '#000000'}
          emissiveIntensity={geo.emissive ? 0.2 : 0}
          metalness={geo.metalness ?? 0.1}
          roughness={geo.roughness ?? 0.6}
        />
      </mesh>
    );
  }
  return (
    <mesh position={position} rotation={rotation}>
      <cylinderGeometry args={[geo.radiusTop, geo.radiusBottom, geo.height, geo.radialSegments ?? 16]} />
      <meshStandardMaterial color={geo.color} metalness={geo.metalness ?? 0.1} roughness={geo.roughness ?? 0.6} />
    </mesh>
  );
}

function YagiMesh({ yagi }: { yagi: Yagi }) {
  const t = yagi.boomThickness ?? 0.02;
  const elementT = yagi.elementThickness ?? t * 0.8;
  return (
    <group position={yagi.position ?? [0, 0, 0]} rotation={eulerToRad(yagi.rotation)}>
      <mesh position={[0, 0, yagi.boomLength / 2]}>
        <boxGeometry args={[t, t, yagi.boomLength]} />
        <meshStandardMaterial color={yagi.color} />
      </mesh>
      {yagi.elements.map((el, i) => {
        const driven = el.role === 'driven';
        return (
          <mesh key={i} position={[0, 0, el.z]}>
            <boxGeometry args={[el.length, elementT, elementT]} />
            <meshStandardMaterial
              color={driven ? yagi.accent : yagi.color}
              emissive={driven ? yagi.accent : '#000000'}
              emissiveIntensity={driven ? 0.25 : 0}
            />
          </mesh>
        );
      })}
      <Html position={[0, t + 0.05, 0]} center>
        <div className={styles.tag}>{yagi.label}</div>
      </Html>
    </group>
  );
}

function BoardMesh({ board }: { board: SensorBoard }) {
  return (
    <group position={board.position} rotation={eulerToRad(board.rotation)}>
      <mesh>
        <boxGeometry args={board.size} />
        <meshStandardMaterial color={board.color} roughness={0.5} />
      </mesh>
      {board.chips.map((chip) => (
        <group key={chip.id}>
          <mesh position={chip.position}>
            <boxGeometry args={chip.size ?? [0.02, 0.01, 0.02]} />
            <meshStandardMaterial color={chip.color} emissive={chip.color} emissiveIntensity={0.25} />
          </mesh>
          <Html position={[chip.position[0], chip.position[1] + 0.035, chip.position[2]]} center>
            <div className={styles.chipTag}>{chip.label}</div>
          </Html>
        </group>
      ))}
    </group>
  );
}

export function PartGroup({
  node, az, el, showBoards, boards,
}: {
  node: PartNode;
  az: number;
  el: number;
  showBoards: boolean;
  boards: SensorBoard[];
}) {
  const jointRot: [number, number, number] =
    node.joint?.type === 'azimuth'   ? [0, Math.PI - az * DEG, 0] :
    node.joint?.type === 'elevation' ? [-el * DEG, 0, 0] :
    [0, 0, 0];
  const nodeBoards = boards.filter((b) => b.node === node.id);

  return (
    <group position={node.position} rotation={eulerToRad(node.rotation)}>
      <group rotation={jointRot}>
        {node.geo.map((g, i) => <GeoMesh key={i} geo={g} />)}
        {node.yagis?.map((y) => <YagiMesh key={y.id} yagi={y} />)}
        {showBoards && nodeBoards.map((b) => <BoardMesh key={b.id} board={b} />)}
        {node.children?.map((child) => (
          <PartGroup key={child.id} node={child} az={az} el={el} showBoards={showBoards} boards={boards} />
        ))}
      </group>
    </group>
  );
}

export function GroundStationModelView({
  model, az, el, showBoards = false,
}: {
  model: GroundStationModel;
  az: number;
  el: number;
  showBoards?: boolean;
}) {
  return <PartGroup node={model.root} az={az} el={el} showBoards={showBoards} boards={model.boards} />;
}
