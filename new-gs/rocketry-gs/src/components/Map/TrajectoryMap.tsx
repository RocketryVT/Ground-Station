import { useEffect, useRef } from 'react';
import * as Cesium from 'cesium';
import 'cesium/Build/Cesium/Widgets/widgets.css';

import { CESIUM_ION_TOKEN, BEAM_HALF_ANGLE_DEG, BEAM_RANGE_M } from '../../config';
import { useTelemetryStore } from '../../store/telemetryStore';
import type { RocketTelemetry } from '../../types/telemetry';
import styles from './TrajectoryMap.module.css';

Cesium.Ion.defaultAccessToken = CESIUM_ION_TOKEN;

const MAROON = Cesium.Color.fromCssColorString('#861F41');
const ORANGE = Cesium.Color.fromCssColorString('#CA4F00');
const PANEL  = Cesium.Color.fromCssColorString('#861F41').withAlpha(0.85);

const N_CONE_EDGES = 16;

// -- Build cone edge positions in ECEF ----------------------------------------
function coneRingPositions(
  gsEcef: Cesium.Cartesian3,
  azDeg: number,
  elDeg: number,
  halfAngleDeg: number,
  rangeM: number,
): { tip: Cesium.Cartesian3; ring: Cesium.Cartesian3[] } {
  const az = (azDeg  * Math.PI) / 180;
  const el = (elDeg  * Math.PI) / 180;
  const ha = (halfAngleDeg * Math.PI) / 180;

  const m = Cesium.Transforms.eastNorthUpToFixedFrame(gsEcef);

  const central = new Cesium.Cartesian3(
    Math.sin(az) * Math.cos(el),
    Math.cos(az) * Math.cos(el),
    Math.sin(el),
  );

  const tipLocal = Cesium.Cartesian3.multiplyByScalar(central, rangeM, new Cesium.Cartesian3());
  const tip = Cesium.Matrix4.multiplyByPoint(m, tipLocal, new Cesium.Cartesian3());

  let perp1 = Cesium.Cartesian3.normalize(
    new Cesium.Cartesian3(-central.y, central.x, 0),
    new Cesium.Cartesian3(),
  );
  if (Cesium.Cartesian3.magnitude(perp1) < 1e-6) {
    perp1 = Cesium.Cartesian3.normalize(
      new Cesium.Cartesian3(0, -central.z, central.y),
      new Cesium.Cartesian3(),
    );
  }
  const perp2 = Cesium.Cartesian3.cross(central, perp1, new Cesium.Cartesian3());

  const ring: Cesium.Cartesian3[] = [];
  for (let i = 0; i < N_CONE_EDGES; i++) {
    const phi = (i * 2 * Math.PI) / N_CONE_EDGES;
    const px = perp1.x * Math.cos(phi) + perp2.x * Math.sin(phi);
    const py = perp1.y * Math.cos(phi) + perp2.y * Math.sin(phi);
    const pz = perp1.z * Math.cos(phi) + perp2.z * Math.sin(phi);
    const edgeDir = new Cesium.Cartesian3(
      central.x * Math.cos(ha) + px * Math.sin(ha),
      central.y * Math.cos(ha) + py * Math.sin(ha),
      central.z * Math.cos(ha) + pz * Math.sin(ha),
    );
    const edgeLocal = Cesium.Cartesian3.multiplyByScalar(edgeDir, rangeM, new Cesium.Cartesian3());
    ring.push(Cesium.Matrix4.multiplyByPoint(m, edgeLocal, new Cesium.Cartesian3()));
  }

  return { tip, ring };
}

export function TrajectoryMap() {
  const containerRef     = useRef<HTMLDivElement>(null);
  const viewerRef        = useRef<Cesium.Viewer | null>(null);

  // Trajectory: PolylineCollection primitive — positions set in-place, no ConstantProperty
  const trackPolyRef     = useRef<Cesium.Polyline | null>(null);

  // Rocket: entity so heightReference works; props updated in-place via setValue()
  const rocketRef        = useRef<Cesium.Entity | null>(null);
  const rocketPosPropRef = useRef<Cesium.ConstantPositionProperty | null>(null);
  const rocketTextPropRef = useRef<Cesium.ConstantProperty | null>(null);

  // Nodes: entities — slow path, rarely changes
  const nodeRefs         = useRef<Map<string, Cesium.Entity>>(new Map());

  // Beam polylines: pre-allocated
  const beamLinesRef     = useRef<Cesium.PolylineCollection | null>(null);
  const beamPolylinesRef = useRef<Cesium.Polyline[]>([]);

  // Camera tracking
  const trackedRef       = useRef(false);

  // Change detection — only rebuild trajectory when history reference changes
  const lastHistoryRef   = useRef<RocketTelemetry[]>([]);

  // Pad location + MSL altitude: captured from first GPS fix.
  // Used every frame to compute the MSL->ellipsoidal offset via globe.getHeight().
  const padAltRef        = useRef<number | null>(null);
  const padLatRef        = useRef<number | null>(null);
  const padLonRef        = useRef<number | null>(null);

  // Last offset used to build the trajectory — triggers a rebuild when it changes
  // (offset refines as terrain tiles load at higher LOD).
  const lastOffsetRef    = useRef<number | null>(null);

  // Scratch Cartesian3 reused each preRender — setValue() clones it internally
  const posScratch       = useRef(new Cesium.Cartesian3());

  // These subscriptions drive only the DOM coords readout (cheap, outside Canvas)
  const nodes  = useTelemetryStore((s) => s.nodes);
  const latest = useTelemetryStore((s) => s.latest);

  // -- Init viewer + ALL primitives + preRender listener --------------------
  useEffect(() => {
    if (!containerRef.current) return;

    const viewer = new Cesium.Viewer(containerRef.current, {
      timeline:             false,
      animation:            false,
      homeButton:           false,
      sceneModePicker:      false,
      baseLayerPicker:      false,
      navigationHelpButton: false,
      geocoder:             false,
      fullscreenButton:     false,
      infoBox:              false,
      selectionIndicator:   false,
    });

    // -- Performance: fewer terrain tiles, capped frame rate --------------
    viewer.scene.globe.tileCacheSize          = 25;  // default 100
    viewer.scene.globe.maximumScreenSpaceError = 4;  // default 2 — fewer/coarser tiles
    viewer.targetFrameRate                    = 60;  // cap render loop at 30 fps
    viewer.scene.debugShowFramesPerSecond     = true;

    // -- Trajectory (PolylineCollection — positions mutated in-place) ------
    const trackColl = new Cesium.PolylineCollection();
    viewer.scene.primitives.add(trackColl);
    trackPolyRef.current = trackColl.add({
      show:      false,
      positions: [],
      width:     4,
      material:  Cesium.Material.fromType('Color', { color: Cesium.Color.WHITE }),
    });

    // -- Rocket entity — HeightReference.NONE; altitude corrected via offset --
    // offset = globe.getHeight(pad) - padAltMSL is recomputed every frame so
    // the rocket always matches the rendered tile mesh, just like CLAMP_TO_GROUND.
    const rocketPosProp  = new Cesium.ConstantPositionProperty(Cesium.Cartesian3.ZERO);
    const rocketTextProp = new Cesium.ConstantProperty('');
    rocketPosPropRef.current  = rocketPosProp;
    rocketTextPropRef.current = rocketTextProp;

    rocketRef.current = viewer.entities.add({
      show:     false,
      position: rocketPosProp,
      point: {
        pixelSize:                10,
        color:                    MAROON,
        outlineColor:             Cesium.Color.WHITE,
        outlineWidth:             1,
        heightReference:          Cesium.HeightReference.NONE,
        disableDepthTestDistance: Number.POSITIVE_INFINITY,
      },
      label: {
        text:                     rocketTextProp,
        font:                     '12px "Courier New", monospace',
        fillColor:                Cesium.Color.WHITE,
        showBackground:           true,
        backgroundColor:          PANEL,
        backgroundPadding:        new Cesium.Cartesian2(6, 3),
        pixelOffset:              new Cesium.Cartesian2(0, -22),
        heightReference:          Cesium.HeightReference.NONE,
        disableDepthTestDistance: Number.POSITIVE_INFINITY,
      },
    });

    // -- Beam polylines (pre-allocated) ------------------------------------
    const beamLines = new Cesium.PolylineCollection();
    viewer.scene.primitives.add(beamLines);
    beamLinesRef.current = beamLines;

    const zero    = [Cesium.Cartesian3.ZERO, Cesium.Cartesian3.ZERO];
    const mkColor = (a: number) =>
      Cesium.Material.fromType('Color', { color: ORANGE.withAlpha(a) });
    const mkDash = () =>
      Cesium.Material.fromType('PolylineDash', { color: MAROON.withAlpha(0.8), dashLength: 16 });

    const pls: Cesium.Polyline[] = [];
    pls.push(beamLines.add({ show: false, positions: zero, width: 2, material: mkColor(0.9) })); // central
    for (let i = 0; i < N_CONE_EDGES; i++)
      pls.push(beamLines.add({ show: false, positions: zero, width: 1, material: mkColor(0.4) })); // edges
    for (let i = 0; i < N_CONE_EDGES; i++)
      pls.push(beamLines.add({ show: false, positions: zero, width: 1, material: mkColor(0.4) })); // ring
    pls.push(beamLines.add({ show: false, positions: zero, width: 2, material: mkDash() })); // target
    beamPolylinesRef.current = pls;

    // -- preRender listener: ALL high-frequency Cesium updates -------------
    // Runs synchronously before each GPU frame — eliminates the async React
    // useEffect -> Cesium render gap that caused polyline flicker.
    const removePreRender = viewer.scene.preRender.addEventListener(() => {
      const { history, latest: l, antenna, nodes: ns } = useTelemetryStore.getState();

      // -- Terrain offset: computed every frame from the rendered tile mesh --
      // globe.getHeight() returns the ellipsoidal height of whatever tile Cesium
      // has loaded and is about to draw — the same mesh that CLAMP_TO_GROUND
      // entities snap to. This keeps rocket, trail, and cone flush with the
      // terrain the user sees at every LOD. Returns undefined until the pad tile
      // loads; we hide objects until then (no one-frame snaps to wrong altitude).
      if (l && padAltRef.current === null) {
        padAltRef.current = l.alt_m;
        padLatRef.current = l.lat;
        padLonRef.current = l.lon;
      }

      let offset: number | null = null;
      if (padAltRef.current !== null && padLatRef.current !== null && padLonRef.current !== null) {
        const padH = viewer.scene.globe.getHeight(
          Cesium.Cartographic.fromDegrees(padLonRef.current, padLatRef.current),
        );
        if (padH !== undefined) {
          offset = padH - padAltRef.current;
        }
      }

      // Trajectory — rebuild when history OR offset changes
      if (history !== lastHistoryRef.current || offset !== lastOffsetRef.current) {
        lastHistoryRef.current = history;
        lastOffsetRef.current  = offset;
        if (trackPolyRef.current) {
          if (offset !== null) {
            const positions = history
              .filter((t) => t.lat !== 0 && t.lon !== 0)
              .map((t) => Cesium.Cartesian3.fromDegrees(t.lon, t.lat, t.alt_m + offset));
            trackPolyRef.current.positions = positions;
            trackPolyRef.current.show      = positions.length > 1;
          } else {
            trackPolyRef.current.show = false;
          }
        }
      }

      // Rocket entity — hidden until offset resolves (avoids one-frame snap)
      const rocket = rocketRef.current;
      const rpp    = rocketPosPropRef.current;
      const rtp    = rocketTextPropRef.current;
      if (l && rocket && rpp && rtp && offset !== null) {
        const displayAlt = l.alt_m + offset;
        const pos = Cesium.Cartesian3.fromDegrees(l.lon, l.lat, displayAlt, undefined, posScratch.current);
        rpp.setValue(pos, Cesium.ReferenceFrame.FIXED);
        rtp.setValue(`${l.alt_m.toFixed(0)} m`);
        rocket.show = true;
        if (!trackedRef.current) {
          viewer.trackedEntity = rocket;
          trackedRef.current   = true;
        }
      }

      // Beam cone
      const bpls = beamPolylinesRef.current;
      if (!bpls.length || beamLinesRef.current?.isDestroyed()) return;

      const hide = () => { for (const pl of bpls) pl.show = false; };
      const gsNode = Object.values(ns)[0];
      if (!antenna || !gsNode) { hide(); return; }

      // GS beam origin: globe.getHeight() gives the rendered tile height at the
      // GS position — exactly what CLAMP_TO_GROUND uses for the GS dot entity.
      // The beam base therefore sits flush with the GS dot at every camera LOD.
      const gsH = viewer.scene.globe.getHeight(
        Cesium.Cartographic.fromDegrees(gsNode.lon, gsNode.lat),
      );
      if (gsH === undefined) { hide(); return; }
      const gsEcef = Cesium.Cartesian3.fromDegrees(gsNode.lon, gsNode.lat, gsH);
      const { tip, ring } = coneRingPositions(
        gsEcef, antenna.actual_az, antenna.actual_el, BEAM_HALF_ANGLE_DEG, BEAM_RANGE_M,
      );

      bpls[0].positions = [gsEcef, tip];
      bpls[0].show      = true;

      for (let i = 0; i < N_CONE_EDGES; i++) {
        bpls[1 + i].positions = [gsEcef, ring[i]];
        bpls[1 + i].show      = true;
      }

      for (let i = 0; i < N_CONE_EDGES; i++) {
        bpls[1 + N_CONE_EDGES + i].positions = [ring[i], ring[(i + 1) % N_CONE_EDGES]];
        bpls[1 + N_CONE_EDGES + i].show      = true;
      }

      const tgt = bpls[bpls.length - 1];
      if (antenna.target_az != null && antenna.target_el != null) {
        const { tip: tgtTip } = coneRingPositions(
          gsEcef, antenna.target_az, antenna.target_el, 0, BEAM_RANGE_M,
        );
        tgt.positions = [gsEcef, tgtTip];
        tgt.show      = true;
      } else {
        tgt.show = false;
      }
    });

    viewerRef.current = viewer;

    // preRender listener must be removed before viewer.destroy()
    return () => {
      removePreRender();
      viewer.destroy();
    };
  }, []);

  // -- Node entities (slow path — nodes rarely change) ----------------------
  useEffect(() => {
    const viewer = viewerRef.current;
    if (!viewer) return;

    const map = nodeRefs.current;

    for (const [id, entity] of map) {
      if (!nodes[id]) {
        viewer.entities.remove(entity);
        map.delete(id);
      }
    }

    for (const node of Object.values(nodes)) {
      const pos = Cesium.Cartesian3.fromDegrees(node.lon, node.lat, 0);
      if (map.has(node.id)) {
        const e = map.get(node.id)!;
        e.position    = new Cesium.ConstantPositionProperty(pos);
        e.label!.text = new Cesium.ConstantProperty(node.name ?? node.id);
      } else {
        map.set(node.id, viewer.entities.add({
          position: new Cesium.ConstantPositionProperty(pos),
          point: {
            pixelSize:                8,
            color:                    ORANGE,
            outlineColor:             Cesium.Color.WHITE,
            outlineWidth:             1,
            heightReference:          Cesium.HeightReference.CLAMP_TO_GROUND,
            disableDepthTestDistance: Number.POSITIVE_INFINITY,
          },
          label: {
            text:                     new Cesium.ConstantProperty(node.name ?? node.id),
            font:                     '11px "Courier New", monospace',
            fillColor:                ORANGE,
            pixelOffset:              new Cesium.Cartesian2(0, -18),
            heightReference:          Cesium.HeightReference.CLAMP_TO_GROUND,
            disableDepthTestDistance: Number.POSITIVE_INFINITY,
          },
        }));
      }
    }
  }, [nodes]);

  // Reset per-flight refs when flight is cleared so the next launch re-learns them
  useEffect(() => {
    if (!latest) {
      padAltRef.current     = null;
      padLatRef.current     = null;
      padLonRef.current     = null;
      lastOffsetRef.current = null;
    }
  }, [latest]);

  return (
    <div className={styles.wrapper}>
      <div ref={containerRef} style={{ width: '100%', height: '100%' }} />
      {latest && (
        <div className={styles.coords}>
          {latest.lat.toFixed(6)},&nbsp;{latest.lon.toFixed(6)}
          &nbsp;&nbsp;↑&nbsp;{latest.alt_m.toFixed(0)}&nbsp;m
        </div>
      )}
    </div>
  );
}
