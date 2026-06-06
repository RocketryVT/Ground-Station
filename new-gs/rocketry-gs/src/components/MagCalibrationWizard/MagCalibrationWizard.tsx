import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { TOPICS } from '../../config';
import type { MQTTHandle } from '../../hooks/useMQTT';
import type { RawMagSample, RawYawImuSample } from '../../types/telemetry';
import { coverage, fitEllipsoid, type FitResult, type MagSample } from './magfit';
import {
  loadMagCal, saveMagCal, publishSensorCal,
  type MagCalStore, type MagSensor, type SensorCal,
} from './magCalPersist';
import styles from './MagCalibrationWizard.module.css';

interface Props {
  mqtt:       MQTTHandle;
  connected:  boolean;
  rawMag:     RawMagSample[];
  rawYawImu:  RawYawImuSample[];
}

const MAX_SAMPLES = 4000;
const SENSOR_LABEL: Record<MagSensor, string> = {
  yaw: 'Yaw platform (LSM6DSOX + LIS3MDL)',
  bar: 'Zenith bar (ISM330DLC + LIS3MDL)',
};

function fmt(n: number, d = 2) { return Number.isFinite(n) ? n.toFixed(d) : '--'; }

export function MagCalibrationWizard({ mqtt, connected, rawMag, rawYawImu }: Props) {
  const [sensor, setSensor] = useState<MagSensor>('yaw');
  const [collecting, setCollecting] = useState(false);
  const [samples, setSamples] = useState<MagSample[]>([]);
  const [store, setStore] = useState<MagCalStore>({});
  const [result, setResult] = useState<FitResult | null>(null);
  const [msg, setMsg] = useState<string | null>(null);
  const lastTs = useRef(0);

  // Load persisted calibration once.
  useEffect(() => { loadMagCal().then(setStore); }, []);

  // Pull new raw samples into the buffer while collecting.
  const source = sensor === 'yaw' ? rawYawImu : rawMag;
  useEffect(() => {
    if (!collecting) return;
    const fresh: MagSample[] = [];
    for (const s of source) {
      if (s.timestamp <= lastTs.current) continue;
      lastTs.current = s.timestamp;
      if (sensor === 'yaw') {
        const y = s as RawYawImuSample;
        if (y.mag_valid === false) continue;
        fresh.push({ x: y.mx_ut, y: y.my_ut, z: y.mz_ut });
      } else {
        const b = s as RawMagSample;
        fresh.push({ x: b.mx, y: b.my, z: b.mz });
      }
    }
    if (fresh.length) {
      setSamples((prev) => {
        const next = prev.concat(fresh);
        return next.length > MAX_SAMPLES ? next.slice(next.length - MAX_SAMPLES) : next;
      });
    }
  }, [source, collecting, sensor]);

  const setRawStream = useCallback((on: boolean) => {
    // Enable/disable the raw streams the wizard depends on.
    mqtt.publish(TOPICS.RAW_SENSORS_CMD, JSON.stringify({ mag: on, yaw_imu: on }));
  }, [mqtt]);

  const start = useCallback(() => {
    setSamples([]);
    setResult(null);
    setMsg(null);
    lastTs.current = 0;
    setRawStream(true);
    setCollecting(true);
  }, [setRawStream]);

  const stop = useCallback(() => {
    setCollecting(false);
    setRawStream(false);
  }, [setRawStream]);

  // Stop streaming on unmount if we were collecting.
  useEffect(() => () => { if (collecting) setRawStream(false); }, [collecting, setRawStream]);

  const cov = useMemo(() => coverage(samples), [samples]);
  const canCompute = samples.length >= 50;

  const computeAndApply = useCallback(async () => {
    const fit = fitEllipsoid(samples);
    setResult(fit);
    if (!fit.ok) { setMsg(fit.reason ?? 'fit failed'); return; }

    const cal: SensorCal = {
      hard_iron: fit.hardIron,
      soft_iron: fit.softIron,
      field_radius: fit.fieldRadius,
      residual: fit.residual,
      samples: fit.samples,
      updated_ms: Date.now(),
    };
    const next: MagCalStore = { ...store, [sensor]: cal };
    setStore(next);
    try {
      await saveMagCal(next);
      publishSensorCal(mqtt, sensor, cal);
      setMsg(`Applied & saved ${SENSOR_LABEL[sensor]} — residual ${(fit.residual * 100).toFixed(1)}%`);
    } catch (e) {
      setMsg(`Applied, but save failed: ${String(e)}`);
    }
  }, [samples, store, sensor, mqtt]);

  const reapply = useCallback((s: MagSensor) => {
    const cal = store[s];
    if (cal) { publishSensorCal(mqtt, s, cal); setMsg(`Re-sent stored ${SENSOR_LABEL[s]} cal to Pico`); }
  }, [store, mqtt]);

  return (
    <div className={styles.wrap}>
      <div className={styles.header}>
        <h3>Magnetometer Hard/Soft-Iron Calibration</h3>
        <span className={connected ? styles.ok : styles.bad}>
          {connected ? 'MQTT ●' : 'MQTT ○'}
        </span>
      </div>

      <p className={styles.hint}>
        Rotate the selected sensor slowly through <em>all</em> orientations (figure-8s,
        tumble around every axis) while collecting. The app fits an ellipsoid, pushes the
        hard/soft-iron result to the Pico, and saves it next to the app binary so it is
        re-applied automatically on every connect.
      </p>

      {/* Sensor picker */}
      <div className={styles.sensorRow}>
        {(['yaw', 'bar'] as MagSensor[]).map((s) => (
          <button
            key={s}
            className={s === sensor ? styles.sensorActive : styles.sensorBtn}
            disabled={collecting}
            onClick={() => { setSensor(s); setSamples([]); setResult(null); lastTs.current = 0; }}
          >
            {s === 'yaw' ? 'YAW' : 'BAR'}
          </button>
        ))}
        <span className={styles.sensorName}>{SENSOR_LABEL[sensor]}</span>
      </div>

      {/* Collection status */}
      <div className={styles.statGrid}>
        <span>Samples</span><strong>{samples.length}</strong>
        <span>Coverage</span>
        <strong>
          <div className={styles.bar}><div className={styles.barFill} style={{ width: `${Math.round(cov * 100)}%` }} /></div>
          {Math.round(cov * 100)}%
        </strong>
        <span>State</span>
        <strong className={collecting ? styles.ok : undefined}>{collecting ? 'COLLECTING' : 'idle'}</strong>
      </div>

      <div className={styles.btnRow}>
        {!collecting
          ? <button className={styles.primary} disabled={!connected} onClick={start}>START COLLECTING</button>
          : <button className={styles.warn} onClick={stop}>STOP</button>}
        <button className={styles.primary} disabled={!canCompute} onClick={computeAndApply}>
          COMPUTE & APPLY
        </button>
      </div>
      {!canCompute && <div className={styles.note}>Collect at least 50 samples (more coverage = better fit).</div>}

      {msg && <div className={styles.msg}>{msg}</div>}

      {/* Fit result */}
      {result?.ok && (
        <div className={styles.resultCard}>
          <div className={styles.resultTitle}>LAST FIT — {SENSOR_LABEL[sensor]}</div>
          <div className={styles.statGrid}>
            <span>Hard iron</span><strong>[{result.hardIron.map((v) => fmt(v)).join(', ')}] µT</strong>
            <span>Field radius</span><strong>{fmt(result.fieldRadius)} µT</strong>
            <span>Sphericity</span><strong>{fmt(result.residual * 100, 1)}% RMS</strong>
          </div>
          <details>
            <summary>Soft-iron matrix</summary>
            <pre className={styles.matrix}>{[0, 3, 6].map((r) =>
              result.softIron.slice(r, r + 3).map((v) => fmt(v, 4).padStart(9)).join(' ')).join('\n')}</pre>
          </details>
        </div>
      )}

      {/* Persisted calibration */}
      <div className={styles.savedCard}>
        <div className={styles.resultTitle}>STORED (auto-applied on connect)</div>
        {(['yaw', 'bar'] as MagSensor[]).map((s) => {
          const cal = store[s];
          return (
            <div key={s} className={styles.savedRow}>
              <span className={styles.savedName}>{s.toUpperCase()}</span>
              {cal
                ? <>
                    <span>hard [{cal.hard_iron.map((v) => fmt(v, 1)).join(', ')}], {fmt(cal.residual * 100, 1)}% RMS</span>
                    <button className={styles.smallBtn} disabled={!connected} onClick={() => reapply(s)}>Re-send</button>
                  </>
                : <span className={styles.muted}>none</span>}
            </div>
          );
        })}
      </div>
    </div>
  );
}
