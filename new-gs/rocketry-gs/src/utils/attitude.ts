// Attitude frame conversion for the onboard transmitters.
//
// Each transmitter reports its estimated attitude as a quaternion [w, x, y, z]
// that rotates a vector from the BODY frame into NED:
//   body: X forward (nose), Y right, Z down   (aerospace convention)
//   NED:  X north, Y east, Z down
//
// The 3D scene works in a Y-up display world that matches the compass on the
// ground plane: North = -Z, East = +X, Up = +Y (Down = -Y). A loaded rocket
// section is built so its airframe (nose) points along local +Y.
//
// nedQuatToDisplay() maps a body->NED quaternion to the section-local -> world
// quaternion the scene applies. displayToNedQuat() is its exact inverse, so the
// demo can author orientations in the easy-to-reason display frame and store
// them as realistic body->NED quaternions that round-trip perfectly.

import * as THREE from 'three';

// NED basis vectors expressed in the display world.
const Q_NED_TO_WORLD = new THREE.Quaternion().setFromRotationMatrix(
  new THREE.Matrix4().makeBasis(
    new THREE.Vector3(0, 0, -1), // North -> -Z
    new THREE.Vector3(1, 0, 0),  // East  -> +X
    new THREE.Vector3(0, -1, 0), // Down  -> -Y
  ),
);
const Q_WORLD_TO_NED = Q_NED_TO_WORLD.clone().invert();

// Aerospace body axes expressed in section-local coordinates (nose = +Y).
const Q_BODY_TO_LOCAL = new THREE.Quaternion().setFromRotationMatrix(
  new THREE.Matrix4().makeBasis(
    new THREE.Vector3(0, 1, 0),  // X forward (nose) -> +Y
    new THREE.Vector3(1, 0, 0),  // Y right          -> +X
    new THREE.Vector3(0, 0, -1), // Z down           -> -Z
  ),
);
const Q_LOCAL_TO_BODY = Q_BODY_TO_LOCAL.clone().invert();

export type Quat = [number, number, number, number]; // [w, x, y, z]

// body->NED quaternion -> section-local -> world quaternion for the scene.
export function nedQuatToDisplay([w, x, y, z]: Quat): THREE.Quaternion {
  const bodyToNed = new THREE.Quaternion(x, y, z, w);
  return Q_NED_TO_WORLD.clone().multiply(bodyToNed).multiply(Q_LOCAL_TO_BODY);
}

// Inverse: a display (section-local -> world) quaternion -> body->NED [w,x,y,z].
export function displayToNedQuat(display: THREE.Quaternion): Quat {
  const bodyToNed = Q_WORLD_TO_NED.clone().multiply(display).multiply(Q_BODY_TO_LOCAL);
  return [bodyToNed.w, bodyToNed.x, bodyToNed.y, bodyToNed.z];
}
