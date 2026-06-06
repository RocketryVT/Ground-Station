// ============================================================================
// Ground-station physical model
// ----------------------------------------------------------------------------
// Hand-editable description of the antenna tracker: the kinematic chain (base →
// azimuth/yaw platform → elevation/zenith bar → antennas), the sensor boards and
// the individual sensor chips on them, and the diagnostic frames we visualize.
//
// Units are metres. Coordinate convention matches AntennaScene: Y up, +X east,
// −Z north. In each part's LOCAL frame, +Z is forward / boresight (where the
// antennas point), +Y is up, and +X is LEFT (so −X is right) — this matches the
// azimuth joint (rotation.y = π − az·DEG maps local +Z → world north at az=0).
//
// "Standing behind the tracker, antennas facing forward":
//   forward = +Z, back = −Z, left = +X, right = −X, up = +Y.
//
// This file is pure data + types. Rendering lives in AhrsFrameScene.tsx.
// ============================================================================

export type Vec3 = [number, number, number];
export type EulerDeg = [number, number, number];

// --- Geometry primitives -----------------------------------------------------

export interface BoxGeo {
  kind: 'box';
  size: Vec3;
  position?: Vec3;
  rotation?: EulerDeg;
  color: string;
  emissive?: string;
  metalness?: number;
  roughness?: number;
}

export interface CylinderGeo {
  kind: 'cylinder';
  radiusTop: number;
  radiusBottom: number;
  height: number;
  radialSegments?: number;
  position?: Vec3;
  rotation?: EulerDeg;
  color: string;
  metalness?: number;
  roughness?: number;
}

export type Geo = BoxGeo | CylinderGeo;

// --- Yagi antenna (boom + elements) ------------------------------------------
// The boom runs along the part's local +Z (boresight). Elements are crossbars
// at a z-offset along the boom, spanning the element length along local X.

export interface YagiElement {
  z: number;        // offset along the boom (local +Z), from the part origin
  length: number;   // element span along local X
  role?: 'reflector' | 'driven' | 'director';
}

export interface Yagi {
  id: string;
  label: string;
  band: string;             // e.g. '433 MHz'
  position?: Vec3;          // boom origin relative to the part
  rotation?: EulerDeg;
  boomLength: number;
  boomThickness?: number;
  elements: YagiElement[];
  color: string;            // boom / director colour
  accent: string;           // driven-element colour
}

// --- Sensor boards and chips -------------------------------------------------
// A board is one physical PCB mounted on a node. A chip is one logical sensor
// on that board. The yaw board carries two chips (IMU + mag) on one PCB; the
// zenith system is TWO separate boards (IMU board + mag board).

export interface SensorChip {
  id: string;
  label: string;        // e.g. 'LSM6DSOX'
  kind: 'imu' | 'mag';
  position: Vec3;       // relative to the board origin
  size?: Vec3;
  color: string;
  // Fusion sensor→body remap note, for the calibration overlay (Phase 4).
  remap?: string;
}

export interface SensorBoard {
  id: string;
  label: string;
  node: string;             // id of the PartNode this board is mounted on
  contributesTo: FrameSource; // which fused frame this board feeds
  position: Vec3;           // relative to the node origin
  rotation?: EulerDeg;
  size: Vec3;               // PCB dimensions
  color: string;
  chips: SensorChip[];
}

// --- Kinematic chain ---------------------------------------------------------

export interface Joint {
  // azimuth   → rotation about world up, driven by antenna.actual_az
  // elevation → rotation about the local X axis, driven by antenna.actual_el
  type: 'azimuth' | 'elevation';
}

export interface PartNode {
  id: string;
  label: string;
  position: Vec3;           // relative to parent origin
  rotation?: EulerDeg;      // static mounting rotation, applied before the joint
  joint?: Joint;            // optional driven joint
  geo: Geo[];
  yagis?: Yagi[];
  children?: PartNode[];
}

// --- Diagnostic frames -------------------------------------------------------

export type FrameSource = 'yaw_q' | 'bar_q' | 'bar_rel_q' | 'q' | 'heading';

export interface FrameSpec {
  key: string;
  label: string;
  source: FrameSource;
  // Where to draw the axis triad (absolute scene position; gizmos render in the
  // world frame oriented by the absolute earth→body quaternion, so they are NOT
  // parented to the moving mechanical joints).
  anchor: Vec3;
  color: string;
  // bar_rel_q is relative to the yaw platform: render it inside a group oriented
  // by yaw_q so a consistent system overlaps bar_q.
  relativeToYaw?: boolean;
  description: string;
}

export interface GroundStationModel {
  root: PartNode;
  boards: SensorBoard[];
  frames: FrameSpec[];
}

// --- Sensor → body remap -----------------------------------------------------
// Mirrors Fusion's FusionRemapAlignment names (e.g. "PYPXNZ" = body [X,Y,Z] =
// sensor [+Y,+X,−Z]). Used by the calibration overlay to map raw sensor-frame
// accel/mag into the body frame the AHRS quaternions are expressed in.

const AXIS_INDEX: Record<string, 0 | 1 | 2> = { X: 0, Y: 1, Z: 2 };

export function remapToBody(remap: string, v: Vec3): Vec3 {
  const tokens = [remap.slice(0, 2), remap.slice(2, 4), remap.slice(4, 6)];
  const out = tokens.map((tok) => {
    const sign = tok[0] === 'N' ? -1 : 1;
    return sign * v[AXIS_INDEX[tok[1]] ?? 0];
  });
  return [out[0], out[1], out[2]];
}

// Earth-frame magnetic field magnitude gate used by the firmware (fusion_task.cpp
// FUSION_MAG_MIN/MAX_FIELD_UT). Outside this band the mag trim is rejected.
export const MAG_FIELD_MIN_UT = 20;
export const MAG_FIELD_MAX_UT = 75;

// ============================================================================
// Default model — measured from the real ground station
// ============================================================================

const IN = 0.0254;
const FT = 0.3048;

const ORANGE = '#CA4F00';   // 433 MHz accent
const BLUE = '#56a8ff';     // 915 MHz accent
const ALU = '#c2c8d0';      // aluminium extrusion / bar
const STEEL = '#9aa3ad';    // gears / shafts
const WOOD = '#7a5a32';     // wooden pole
const PCB_GREEN = '#1f6b46';
const SERVO = '#2b2f36';

// --- Dimensions (metric) -----------------------------------------------------
const POLE_W = 3.5 * IN;        // 0.0889 m square
const POLE_H = 8 * FT;          // 2.4384 m
const FRAME_W = 12.787 * IN;    // 0.32479 m (left-right, X)
const FRAME_D = 12.787 * IN;    // 0.32479 m (fore-aft, Z)
const FRAME_H = 7.575 * IN;     // 0.19240 m
const EXT = 0.02;               // 20 mm extrusion bar
const BAR_LEN = 3 * FT;         // 0.9144 m (left-right, X)
const BAR_HALF = BAR_LEN / 2;   // 0.4572 m
const YAGI_915_LEN = 30 * IN;   // 0.762 m
const YAGI_433_LEN = 51.5 * IN; // 1.30810 m

// Aluminium-extrusion cube frame (12 edges), origin at the frame's bottom centre.
function frameExtrusions(): Geo[] {
  const hx = FRAME_W / 2;
  const hz = FRAME_D / 2;
  const post = (x: number, z: number): Geo => ({
    kind: 'box', size: [EXT, FRAME_H, EXT], position: [x, FRAME_H / 2, z], color: ALU, metalness: 0.6, roughness: 0.35,
  });
  const railX = (y: number, z: number): Geo => ({
    kind: 'box', size: [FRAME_W, EXT, EXT], position: [0, y, z], color: ALU, metalness: 0.6, roughness: 0.35,
  });
  const railZ = (y: number, x: number): Geo => ({
    kind: 'box', size: [EXT, EXT, FRAME_D], position: [x, y, 0], color: ALU, metalness: 0.6, roughness: 0.35,
  });
  return [
    post(-hx, -hz), post(hx, -hz), post(-hx, hz), post(hx, hz),
    railX(0, -hz), railX(0, hz), railX(FRAME_H, -hz), railX(FRAME_H, hz),
    railZ(0, -hx), railZ(0, hx), railZ(FRAME_H, -hx), railZ(FRAME_H, hx),
  ];
}

export const GROUND_STATION: GroundStationModel = {
  root: {
    id: 'base',
    label: 'Pole + bearing',
    position: [0, 0, 0],
    geo: [
      // 8 ft, 3.5"×3.5" wooden pole, descending below the bearing (top at y=0).
      { kind: 'box', size: [POLE_W, POLE_H, POLE_W], position: [0, -POLE_H / 2, 0], color: WOOD, roughness: 0.85 },
      // Bearing that carries the whole frame.
      { kind: 'cylinder', radiusTop: 0.062, radiusBottom: 0.07, height: 0.05, radialSegments: 24, position: [0, 0.02, 0], color: STEEL, metalness: 0.85, roughness: 0.2 },
      // Fixed azimuth ring gear (32T) the platform pinion rides on.
      { kind: 'cylinder', radiusTop: 0.088, radiusBottom: 0.088, height: 0.018, radialSegments: 36, position: [0, 0.05, 0], color: STEEL, metalness: 0.8, roughness: 0.25 },
    ],
    children: [
      {
        id: 'az_platform',
        label: 'Azimuth / Yaw platform (frame)',
        position: [0, 0.06, 0],
        joint: { type: 'azimuth' },
        geo: [
          ...frameExtrusions(),

          // Azimuth servo + 10:1 gearbox, front-right, output shaft pointing UP.
          // front = +Z, right = −X.
          { kind: 'box', size: [0.045, 0.08, 0.045], position: [-0.11, 0.055, 0.11], color: SERVO },
          { kind: 'cylinder', radiusTop: 0.026, radiusBottom: 0.026, height: 0.05, radialSegments: 16, position: [-0.11, 0.12, 0.11], color: SERVO },
          // 14T azimuth pinion (rides on the fixed 32T ring below).
          { kind: 'cylinder', radiusTop: 0.017, radiusBottom: 0.017, height: 0.02, radialSegments: 18, position: [-0.11, 0.155, 0.11], color: STEEL, metalness: 0.8, roughness: 0.25 },

          // Zenith servo + 10:1 gearbox, back-centre, on top of the frame, output
          // shaft pointing LEFT (+X). Drives the elevation bar via a belt.
          { kind: 'box', size: [0.085, 0.046, 0.046], position: [0.0, 0.215, -0.12], color: SERVO },
          // 14T output pulley (axis along X), set in ~2" from the left edge.
          { kind: 'cylinder', radiusTop: 0.02, radiusBottom: 0.02, height: 0.02, radialSegments: 18, position: [0.085, 0.215, -0.12], rotation: [0, 0, 90], color: STEEL, metalness: 0.8, roughness: 0.25 },
        ],
        children: [
          {
            id: 'el_bar',
            label: 'Elevation / Zenith bar',
            // Pivot just above the frame top, inline with the zenith servo.
            position: [0, 0.21, 0],
            joint: { type: 'elevation' },
            geo: [
              // 3 ft zenith bar, left-right along X.
              { kind: 'box', size: [BAR_LEN, 0.035, 0.05], position: [0, 0, 0], color: ALU, metalness: 0.55, roughness: 0.4 },
              // 20T driven gear, coaxial with the elevation axis so it wraps around
              // the bar (axis along X, centred on the bar at y=0, z=0). The belt spans
              // the z offset back to the servo pulley.
              { kind: 'cylinder', radiusTop: 0.032, radiusBottom: 0.032, height: 0.018, radialSegments: 24, position: [0.085, 0, 0], rotation: [0, 0, 90], color: STEEL, metalness: 0.8, roughness: 0.25 },
            ],
            yagis: [
              // 915 MHz on the LEFT edge (+X). 30", 8 elements, ~15 dBi.
              // Mounted at the boom centre on the bar (z = −L/2 → spans ∓L/2).
              {
                id: 'yagi_915',
                label: 'Yagi 915 MHz',
                band: '915 MHz',
                position: [BAR_HALF, 0.045, -YAGI_915_LEN / 2],
                boomLength: YAGI_915_LEN,
                boomThickness: 0.014,
                color: '#3a3f48',
                accent: BLUE,
                elements: [
                  { z: 0.04, length: 0.170, role: 'reflector' },
                  { z: 0.13, length: 0.160, role: 'driven' },
                  { z: 0.24, length: 0.150, role: 'director' },
                  { z: 0.35, length: 0.145, role: 'director' },
                  { z: 0.46, length: 0.140, role: 'director' },
                  { z: 0.57, length: 0.135, role: 'director' },
                  { z: 0.66, length: 0.130, role: 'director' },
                  { z: 0.74, length: 0.125, role: 'director' },
                ],
              },
              // 433 MHz on the RIGHT edge (−X). 51.5", 8 elements, ~15 dBi.
              {
                id: 'yagi_433',
                label: 'Yagi 433 MHz',
                band: '433 MHz',
                position: [-BAR_HALF, 0.045, -YAGI_433_LEN / 2],
                boomLength: YAGI_433_LEN,
                boomThickness: 0.018,
                color: '#2a2e35',
                accent: ORANGE,
                elements: [
                  { z: 0.05, length: 0.360, role: 'reflector' },
                  { z: 0.20, length: 0.345, role: 'driven' },
                  { z: 0.40, length: 0.330, role: 'director' },
                  { z: 0.60, length: 0.320, role: 'director' },
                  { z: 0.80, length: 0.310, role: 'director' },
                  { z: 0.98, length: 0.300, role: 'director' },
                  { z: 1.14, length: 0.290, role: 'director' },
                  { z: 1.27, length: 0.280, role: 'director' },
                ],
              },
            ],
          },
        ],
      },
    ],
  },

  boards: [
    // Yaw board (LSM6DSOX + LIS3MDL, one PCB): bottom-left-forward of the frame,
    // as far from the steppers as possible. left = +X, forward = +Z.
    {
      id: 'yaw_board',
      label: 'Yaw board',
      node: 'az_platform',
      contributesTo: 'yaw_q',
      position: [0.12, 0.03, 0.12],
      size: [0.05, 0.014, 0.035],
      color: PCB_GREEN,
      chips: [
        { id: 'yaw_imu', label: 'LSM6DSOX', kind: 'imu', position: [-0.011, 0.013, 0], size: [0.022, 0.011, 0.022], color: '#4dff9a', remap: 'PYPXNZ' },
        { id: 'yaw_mag', label: 'LIS3MDL', kind: 'mag', position: [0.014, 0.013, 0], size: [0.016, 0.009, 0.016], color: '#f0a03d', remap: 'PYPXNZ' },
      ],
    },
    // Zenith system = TWO separate boards (no longer stacked), reoriented so the
    // sensor +X faces forward and +Y faces left → remap PXNYNZ. The ISM330DLC sits
    // at the intersection of the 433 antenna line (−X) and the zenith bar; the
    // LIS3MDL mag is ~1" forward of it.
    {
      id: 'zenith_imu_board',
      label: 'Zenith IMU',
      node: 'el_bar',
      contributesTo: 'bar_q',
      position: [-BAR_HALF, 0.062, 0], // inline with both the 433 antenna and the zenith bar
      size: [0.04, 0.012, 0.034],
      color: PCB_GREEN,
      chips: [
        { id: 'bar_imu', label: 'ISM330DLC', kind: 'imu', position: [0, 0.011, 0], size: [0.024, 0.011, 0.024], color: BLUE, remap: 'PXNYNZ' },
      ],
    },
    {
      id: 'zenith_mag_board',
      label: 'Zenith mag',
      node: 'el_bar',
      contributesTo: 'bar_q',
      position: [-BAR_HALF, 0.062, 1 * IN], // ~1" forward of the ISM
      size: [0.036, 0.012, 0.03],
      color: PCB_GREEN,
      chips: [
        { id: 'bar_mag', label: 'LIS3MDL', kind: 'mag', position: [0, 0.011, 0], size: [0.016, 0.009, 0.016], color: '#f0a03d', remap: 'PXNYNZ' },
      ],
    },
  ],

  frames: [
    {
      key: 'yaw',
      label: 'YAW q_EY',
      source: 'yaw_q',
      anchor: [-1.7, 0.85, 0],
      color: '#4dff9a',
      description: 'Yaw board (LSM6DSOX+LIS3MDL) global estimate of the az platform.',
    },
    {
      key: 'bar',
      label: 'ZENITH q_EB',
      source: 'bar_q',
      anchor: [-0.85, 0.85, 0],
      color: BLUE,
      description: 'Zenith boards (ISM330DLC+LIS3MDL) global estimate of the bar/antenna.',
    },
    {
      key: 'rel',
      label: 'LOCAL q_YB',
      source: 'bar_rel_q',
      anchor: [0.85, 0.85, 0],
      color: '#f0a03d',
      relativeToYaw: true,
      description: 'Bar relative to the yaw platform (rendered inside yaw_q — overlaps ZENITH when consistent).',
    },
    {
      key: 'final',
      label: 'TRACKER q',
      source: 'q',
      anchor: [1.7, 0.85, 0],
      color: '#d7dce5',
      description: 'Final fused tracker orientation (q_truth).',
    },
    {
      key: 'heading',
      label: 'HEADING-CORR',
      source: 'heading',
      anchor: [1.7, 0.15, 0],
      color: '#ff6fae',
      description: 'Display-only: tracker q with declination + heading offset applied about world up.',
    },
  ],
};
