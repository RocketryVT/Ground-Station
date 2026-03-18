/**
 * useDemoMode — feeds synthetic telemetry into the store at 10 Hz.
 *
 * Flight profile (repeats every CYCLE_S seconds):
 *   0 –  3 s  motor burn   fast climb, high acceleration
 *   3 – 12 s  coast        deceleration, rotation slows
 *  12 – 14 s  apogee       ~3 000 m, near-zero velocity
 *  14 – 28 s  descent      drogue → main chute, slow fall
 *  28 – 30 s  landed       stationary on ground
 */

import { useEffect, useRef } from 'react';
import { useTelemetryStore } from '../store/telemetryStore';

// ── Launch-pad coordinates (White Sands, NM) ────────────────────────────────
const LAT0 =  32.9503;
const LON0 = -106.9750;
const ALT0 =  1220;   // m MSL (pad elevation)

const MAX_ALT   = 3200;   // m MSL at apogee
const CYCLE_S   = 30;     // seconds per simulated flight
const HZ        = 10;
const DT        = 1 / HZ;
const BURN_END_ALT_FRAC = 0.7; // fraction of apogee reached at t=3 s

// Altitude profile — returns MSL altitude for t ∈ [0, CYCLE_S]
function altProfile(t: number): number {
  const deltaAlt = MAX_ALT - ALT0;
  const burnEndAlt = ALT0 + deltaAlt * BURN_END_ALT_FRAC;

  if (t < 3) {
    // Powered ascent: accelerating climb.
    return ALT0 + (burnEndAlt - ALT0) * (t / 3) ** 2;
  }

  if (t < 14) {
    // Coast to apogee: decelerating climb, continuous at t=3 and t=14.
    const u = (t - 3) / 11; // 0 -> 1
    return burnEndAlt + (MAX_ALT - burnEndAlt) * (1 - (1 - u) ** 2);
  }

  if (t < 28)      return ALT0 + (MAX_ALT - ALT0) * Math.max(0, 1 - ((t - 14) / 14) ** 0.6); // descent
  return ALT0;
}

// ── Chase-car offset from pad (degrees) ────────────────────────────────────
const CHASE_DLAT =  0.0012;
const CHASE_DLON = -0.0008;

export function useDemoMode(enabled: boolean) {
  const { addTelemetry, setAntenna, updateNode, setConnected, clearFlight } =
    useTelemetryStore();

  const tRef       = useRef(0);
  // Simulate motor lag: actual position lags behind setpoint with a ~1.5 s time constant.
  const actualAzRef = useRef(0);
  const actualElRef = useRef(0);

  useEffect(() => {
    if (!enabled) return;

    clearFlight();
    setConnected(true);
    tRef.current = 0;
    actualAzRef.current = 0;
    actualElRef.current = 0;

    // Seeded horizontal drift direction so the trajectory looks intentional
    const driftBearing = 42; // degrees from north

    const id = setInterval(() => {
      const t = tRef.current % CYCLE_S;
      tRef.current += DT;

      // ── Position ───────────────────────────────────────────────────────
      const alt  = altProfile(t);
      const frac = Math.min(t, 28) / 28;            // 0→1 over the flight

      // Horizontal drift — monotonic so the trajectory is a true arc, not a loop.
      // Rocket drifts ~700 m downrange by landing.
      const downrange = 700 * frac;
      const bearRad   = (driftBearing * Math.PI) / 180;
      const dlat = (downrange * Math.cos(bearRad)) / 111_320;
      const dlon = (downrange * Math.sin(bearRad)) / (111_320 * Math.cos(LAT0 * Math.PI / 180));
      const lat  = LAT0 + dlat;
      const lon  = LON0 + dlon;

      // ── Velocity (numerical derivative of alt + drift) ──────────────
      const altNext = altProfile(Math.min(t + DT, 28));
      const vel_d   = -(altNext - alt) / DT;                    // positive = falling

      const fracNext = Math.min(t + DT, 28) / 28;
      const downNext = 700 * fracNext;
      const vel_horiz   = (downNext - downrange) / DT;
      const vel_n       = vel_horiz * Math.cos(bearRad);
      const vel_e       = vel_horiz * Math.sin(bearRad);

      // ── Attitude ────────────────────────────────────────────────────
      // During burn: pitched slightly toward drift; during descent: stable
      const pitch = t < 3
        ? 5 + 3 * Math.sin(t * 2)
        : t < 14
          ? 2 + 4 * Math.sin(t * 0.4)
          : 1 * Math.sin(t * 0.2);

      const roll = t < 14
        ? 360 * (t / 14) * 0.5 + 8 * Math.sin(t * 1.3)  // slow spin
        : 5 * Math.sin(t * 0.3);

      const yaw = (driftBearing + 5 * Math.sin(t * 0.7)) % 360;

      // ── Signal ──────────────────────────────────────────────────────
      const slantRange = Math.sqrt(
        ((lat - LAT0) * 111_320) ** 2 +
        ((lon - LON0) * 111_320 * Math.cos(LAT0 * Math.PI / 180)) ** 2 +
        (alt - ALT0) ** 2,
      );
      const rssi = Math.round(-60 - 20 * Math.log10(Math.max(1, slantRange / 100)));
      const snr  = parseFloat((15 - slantRange / 500).toFixed(1));

      addTelemetry({
        timestamp: Date.now(),
        lat, lon, alt_m: alt,
        vel_n, vel_e, vel_d,
        roll, pitch, yaw,
        rssi, snr,
      });

      // ── Antenna tracking (az/el from GS node position, not pad) ─────────
      // The beam is drawn from the GS node, so angles must be referenced there.
      const gsLat = LAT0 + CHASE_DLAT;
      const gsLon = LON0 + CHASE_DLON;
      const dx_m = (lat - gsLat) * 111_320;
      const dy_m = (lon - gsLon) * 111_320 * Math.cos(gsLat * Math.PI / 180);
      const horiz = Math.sqrt(dx_m ** 2 + dy_m ** 2);
      const el    = Math.atan2(alt - ALT0, Math.max(1, horiz)) * (180 / Math.PI);
      const az    = ((Math.atan2(dy_m, dx_m) * (180 / Math.PI)) + 360) % 360;

      // Motor lag: exponential smoothing toward setpoint (~1.5 s time constant)
      const ALPHA = 1 - Math.exp(-DT / 1.5);
      actualAzRef.current += (az - actualAzRef.current) * ALPHA;
      actualElRef.current += (el - actualElRef.current) * ALPHA;

      setAntenna({
        timestamp: Date.now(),
        actual_az: actualAzRef.current,
        actual_el: actualElRef.current,
        target_az: az,
        target_el: el,
      });

      // ── Ground nodes ─────────────────────────────────────────────────
      updateNode({
        id:        'gs',
        name:      'GS',
        lat:       LAT0 + CHASE_DLAT,
        lon:       LON0 + CHASE_DLON,
        timestamp: Date.now(),
      });
    }, 1000 / HZ);

    return () => {
      clearInterval(id);
      setConnected(false);
    };
  }, [enabled, addTelemetry, setAntenna, updateNode, setConnected, clearFlight]);
}
