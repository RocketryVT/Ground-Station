import { useEffect, useMemo, useRef, useState, useCallback } from 'react';
import {
  LineChart, Line, XAxis, YAxis, ResponsiveContainer, Tooltip,
} from 'recharts';
import { useTelemetryStore } from '../../store/telemetryStore';
import { TOPICS, STARLINK_PROXY_URL } from '../../config';
import type { MQTTHandle } from '../../hooks/useMQTT';
import type { AntennaState, GroundImuState, RawImuSample, RawMagSample } from '../../types/telemetry';
import { SimTab } from './SimTab';
import styles from './DebugPanel.module.css';

interface Props {
  mqtt: MQTTHandle;
}

// -- Starlink data types -------------------------------------------------------
interface StarlinkData {
  // location
  lat:                      number | null;
  lon:                      number | null;
  alt:                      number | null;
  horizontal_speed_mps:     number | null;
  vertical_speed_mps:       number | null;
  // status
  pop_ping_latency_ms:      number | null;
  pop_ping_drop_rate:       number | null;
  downlink_throughput_bps:  number | null;
  uplink_throughput_bps:    number | null;
  boresight_azimuth_deg:    number | null;
  boresight_elevation_deg:  number | null;
  gps_valid:                boolean;
  gps_sats:                 number;
  hardware_version:         string | null;
  software_version:         string | null;
  uptime_s:                 number | null;
  // meta
  error:   string | null;
  last_ok: number | null;
}

interface AhrsStatus {
  timestamp: number;
  running: boolean;
  have_imu: boolean;
  have_mag: boolean;
  updates: number;
}

// -- Starlink tab component ---------------------------------------------------
function StarlinkTab() {
  const [data, setData]       = useState<StarlinkData | null>(null);
  const [loading, setLoading] = useState(false);
  const [fetchErr, setFetchErr] = useState<string | null>(null);

  const poll = useCallback(async () => {
    setLoading(true);
    try {
      const res = await fetch(STARLINK_PROXY_URL);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const json = await res.json();
      setData(json);
      setFetchErr(null);
    } catch (e: unknown) {
      setFetchErr(e instanceof Error ? e.message : String(e));
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    poll();
    const id = setInterval(poll, 3000);
    return () => clearInterval(id);
  }, [poll]);

  function fmt(v: number | null | undefined, decimals = 1, unit = ''): string {
    if (v == null) return '--';
    return `${v.toFixed(decimals)}${unit}`;
  }
  function fmtMbps(bps: number | null): string {
    if (bps == null) return '--';
    return `${(bps / 1e6).toFixed(2)} Mbps`;
  }
  function fmtUptime(s: number | null): string {
    if (s == null) return '--';
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = s % 60;
    return `${h}h ${m}m ${sec}s`;
  }

  const proxyErr = fetchErr ?? data?.error;
  const stale    = data?.last_ok != null && (Date.now() / 1000 - data.last_ok) > 10;

  return (
    <div className={styles.starlinkRoot}>
      <div className={styles.starlinkHeader}>
        <span className={styles.starlinkTitle}>STARLINK DISH  192.168.100.1</span>
        {loading && <span className={styles.starlinkPoll}>polling…</span>}
        {stale   && <span className={styles.starlinkStale}>STALE</span>}
        {proxyErr && <span className={styles.starlinkErr}>{proxyErr}</span>}
      </div>

      <div className={styles.starlinkGrid}>

        {/* -- Connectivity -------------------------------------------------- */}
        <div className={styles.starlinkCard}>
          <div className={styles.cardTitle}>CONNECTIVITY</div>
          <Row label="Latency"   value={fmt(data?.pop_ping_latency_ms, 1, ' ms')} />
          <Row label="Drop rate" value={fmt(data?.pop_ping_drop_rate != null ? (data.pop_ping_drop_rate * 100) : null, 2, ' %')} />
          <Row label="Downlink"  value={fmtMbps(data?.downlink_throughput_bps ?? null)} highlight />
          <Row label="Uplink"    value={fmtMbps(data?.uplink_throughput_bps ?? null)} highlight />
        </div>

        {/* -- Location ----------------------------------------------------- */}
        <div className={styles.starlinkCard}>
          <div className={styles.cardTitle}>LOCATION</div>
          <Row label="Latitude"   value={fmt(data?.lat,  6, '°')} />
          <Row label="Longitude"  value={fmt(data?.lon,  6, '°')} />
          <Row label="Altitude"   value={fmt(data?.alt,  1, ' m')} />
          <Row label="Horiz spd" value={fmt(data?.horizontal_speed_mps, 2, ' m/s')} />
          <Row label="Vert spd"  value={fmt(data?.vertical_speed_mps,  2, ' m/s')} />
        </div>

        {/* -- Pointing ----------------------------------------------------- */}
        <div className={styles.starlinkCard}>
          <div className={styles.cardTitle}>DISH POINTING</div>
          <Row label="Azimuth"   value={fmt(data?.boresight_azimuth_deg,   1, '°')} />
          <Row label="Elevation" value={fmt(data?.boresight_elevation_deg, 1, '°')} />
          <Row label="GPS valid" value={data ? (data.gps_valid ? 'YES' : 'NO') : '--'} />
          <Row label="GPS sats"  value={data?.gps_sats != null ? String(data.gps_sats) : '--'} />
        </div>

        {/* -- Device ------------------------------------------------------- */}
        <div className={styles.starlinkCard}>
          <div className={styles.cardTitle}>DEVICE</div>
          <Row label="HW version" value={data?.hardware_version ?? '--'} />
          <Row label="SW version" value={data?.software_version ?? '--'} />
          <Row label="Uptime"     value={fmtUptime(data?.uptime_s ?? null)} />
          <Row label="Last poll"  value={data?.last_ok ? new Date(data.last_ok * 1000).toISOString().slice(11, 19) : '--'} />
        </div>

      </div>
    </div>
  );
}

function Row({ label, value, highlight }: { label: string; value: string; highlight?: boolean }) {
  return (
    <div className={styles.starlinkRow}>
      <span className={styles.starlinkLabel}>{label}</span>
      <span className={`${styles.starlinkValue} ${highlight ? styles.starlinkHighlight : ''}`}>{value}</span>
    </div>
  );
}

// -- Confirmation dialog -------------------------------------------------------
interface ConfirmDialogProps {
  az:     string;
  zen:    string;
  speed:  string;
  onConfirm: () => void;
  onCancel:  () => void;
}

function ConfirmDialog({ az, zen, speed, onConfirm, onCancel }: ConfirmDialogProps) {
  return (
    <div className={styles.overlay}>
      <div className={styles.dialog}>
        <div className={styles.dialogTitle}>⚠ CONFIRM ANTENNA COMMAND</div>
        <div className={styles.dialogBody}>
          <div className={styles.dialogRow}>
            <span className={styles.dialogLabel}>Azimuth</span>
            <span className={styles.dialogValue}>{az}°</span>
          </div>
          <div className={styles.dialogRow}>
            <span className={styles.dialogLabel}>Zenith (elevation)</span>
            <span className={styles.dialogValue}>{zen}°</span>
          </div>
          <div className={styles.dialogRow}>
            <span className={styles.dialogLabel}>Speed</span>
            <span className={styles.dialogValue}>{speed} deg/s</span>
          </div>
          <div className={styles.dialogWarning}>
            This will move the physical antenna. Make sure the area is clear.
          </div>
        </div>
        <div className={styles.dialogButtons}>
          <button className={styles.cancelBtn} onClick={onCancel}>CANCEL</button>
          <button className={styles.confirmBtn} onClick={onConfirm}>SEND COMMAND</button>
        </div>
      </div>
    </div>
  );
}

function fmtDeg(v: number | null | undefined): string {
  return v == null || !Number.isFinite(v) ? '--' : `${v.toFixed(1)}°`;
}

function norm360(v: number | null | undefined): number | null {
  if (v == null || !Number.isFinite(v)) return null;
  return ((v % 360) + 360) % 360;
}

function fmtSeen(ts: number | null): string {
  if (ts == null) return 'never';
  const age = Math.max(0, Date.now() - ts);
  if (age < 2000) return 'now';
  return `${(age / 1000).toFixed(1)}s ago`;
}

function latestJson<T>(messages: { ts: number; topic: string; payload: string }[], topic: string): (T & { timestamp: number }) | null {
  for (let i = messages.length - 1; i >= 0; i--) {
    const msg = messages[i];
    if (msg.topic !== topic) continue;
    try {
      const data = JSON.parse(msg.payload) as T;
      return { ...data, timestamp: msg.ts };
    } catch {
      return null;
    }
  }
  return null;
}

function jsonHistory<T>(messages: { ts: number; topic: string; payload: string }[], topic: string): Array<T & { timestamp: number }> {
  const parsed: Array<T & { timestamp: number }> = [];
  for (const msg of messages) {
    if (msg.topic !== topic) continue;
    try {
      const data = JSON.parse(msg.payload) as T;
      parsed.push({ ...data, timestamp: msg.ts });
    } catch {
      // The AHRS tab shows seen-vs-parsed counts for malformed payloads.
    }
  }
  return parsed;
}

function OrientationTab({
  antenna,
  imu,
  connected,
  imuSeen,
  antennaSeen,
}: {
  antenna: AntennaState | null;
  imu: GroundImuState | null;
  connected: boolean;
  imuSeen: number | null;
  antennaSeen: number | null;
}) {
  const baseAz = antenna?.actual_az ?? 0;
  const zenith = antenna?.actual_el ?? 0;
  const imuYaw = imu?.yaw360 ?? norm360(imu?.yaw) ?? 0;
  const yawDelta = imu && antenna
    ? ((((imuYaw - antenna.actual_az + 540) % 360) - 180))
    : null;

  return (
    <div className={styles.orientationRoot}>
      <div className={styles.orientationGrid}>
        <div className={styles.orientationCard}>
          <div className={styles.cardTitle}>TOP VIEW</div>
          <div className={styles.compass}>
            <span className={styles.north}>N</span>
            <span className={styles.east}>E</span>
            <span className={styles.south}>S</span>
            <span className={styles.west}>W</span>
            <div
              className={styles.baseNeedle}
              style={{ transform: `translate(-50%, -100%) rotate(${baseAz}deg)` }}
            />
            <div
              className={styles.imuNeedle}
              style={{ transform: `translate(-50%, -100%) rotate(${imuYaw}deg)` }}
            />
            <div className={styles.compassHub} />
          </div>
          <div className={styles.legendRow}><span className={styles.swatchBase} />Base azimuth {fmtDeg(antenna?.actual_az)}</div>
          <div className={styles.legendRow}><span className={styles.swatchImu} />IMU yaw {fmtDeg(imu?.yaw360 ?? norm360(imu?.yaw))}</div>
        </div>

        <div className={styles.orientationCard}>
          <div className={styles.cardTitle}>SIDE VIEW</div>
          <div className={styles.sideView}>
            <div className={styles.sideBase} />
            <div
              className={styles.zenithArm}
              style={{ transform: `rotate(${-zenith}deg)` }}
            />
            <div className={styles.sideHub} />
          </div>
          <div className={styles.legendRow}>Zenith / elevation {fmtDeg(antenna?.actual_el)}</div>
          <div className={styles.legendRow}>Target {fmtDeg(antenna?.target_el)}</div>
        </div>

        <div className={styles.orientationCard}>
          <div className={styles.cardTitle}>SENSOR ON ZENITH BAR</div>
          <div className={styles.kvGrid}>
            <span>MQTT</span><strong>{connected ? 'CONNECTED' : 'OFFLINE'}</strong>
            <span>IMU seen</span><strong>{fmtSeen(imuSeen)}</strong>
            <span>State seen</span><strong>{fmtSeen(antennaSeen)}</strong>
            <span>Roll</span><strong>{fmtDeg(imu?.roll)}</strong>
            <span>Pitch</span><strong>{fmtDeg(imu?.pitch)}</strong>
            <span>Yaw 0-360</span><strong>{fmtDeg(imu?.yaw360 ?? norm360(imu?.yaw))}</strong>
            <span>Yaw signed</span><strong>{fmtDeg(imu?.yaw)}</strong>
            <span>Yaw - base</span><strong>{fmtDeg(yawDelta)}</strong>
            <span>Mag valid</span><strong>{imu ? (imu.have_mag ? 'YES' : 'NO') : '--'}</strong>
            <span>AHRS</span><strong>{imu ? (imu.valid ? 'VALID' : 'STARTUP') : '--'}</strong>
            <span>Mag rec</span><strong>{imu ? (imu.mag_rec ? 'ON' : 'OFF') : '--'}</strong>
            <span>Acc rec</span><strong>{imu ? (imu.acc_rec ? 'ON' : 'OFF') : '--'}</strong>
          </div>
        </div>

        <div className={styles.orientationCard}>
          <div className={styles.cardTitle}>RAW VECTORS</div>
          <div className={styles.vectorBlock}>
            <span>Accel</span>
            <code>{imu?.a ? imu.a.map(v => v.toFixed(3)).join(', ') : '--'}</code>
            <span>Mag</span>
            <code>{imu?.m ? imu.m.map(v => v.toFixed(3)).join(', ') : '--'}</code>
            <span>Quaternion</span>
            <code>{imu?.q ? imu.q.map(v => v.toFixed(4)).join(', ') : '--'}</code>
          </div>
        </div>
      </div>
      <div className={styles.orientationNote}>
        The base azimuth is the true tracker yaw reference. The IMU is mounted after the azimuth joint on the zenith bar, so its quaternion includes base rotation, zenith tilt, and the fixed sensor mount offset.
      </div>
    </div>
  );
}

const rawTooltipStyle = {
  contentStyle: { background: '#1f1f1f', border: '1px solid #444', color: '#e0e0e0', fontSize: 11 },
  labelStyle:   { display: 'none' },
};

function RawChart({
  title,
  data,
  lines,
  unit,
}: {
  title: string;
  data: Array<Record<string, number>>;
  lines: { key: string; label: string; color: string }[];
  unit: string;
}) {
  return (
    <div className={styles.rawChart}>
      <div className={styles.rawChartTitle}>{title}</div>
      <ResponsiveContainer width="100%" height="100%">
        <LineChart data={data} margin={{ top: 8, right: 8, bottom: 0, left: 0 }}>
          <XAxis dataKey="i" hide />
          <YAxis width={50} tick={{ fill: 'rgba(255,255,255,0.45)', fontSize: 10 }} tickLine={false} axisLine={false} />
          <Tooltip
            {...rawTooltipStyle}
            formatter={(v: number, name: string) => [`${Number(v).toFixed(4)} ${unit}`, name]}
          />
          {lines.map((line) => (
            <Line
              key={line.key}
              type="monotone"
              dataKey={line.key}
              name={line.label}
              stroke={line.color}
              dot={false}
              strokeWidth={1.4}
              isAnimationActive={false}
            />
          ))}
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}

function RawSensorsTab({
  mqtt,
  rawImu,
  rawMag,
  clearRawSensors,
}: {
  mqtt: MQTTHandle;
  rawImu: RawImuSample[];
  rawMag: RawMagSample[];
  clearRawSensors: () => void;
}) {
  const [imuEnabled, setImuEnabled] = useState(false);
  const [magEnabled, setMagEnabled] = useState(false);

  function publish(nextImu = imuEnabled, nextMag = magEnabled) {
    mqtt.publish(TOPICS.RAW_SENSORS_CMD, JSON.stringify({ imu: nextImu, mag: nextMag }));
  }

  function toggleImu() {
    const next = !imuEnabled;
    setImuEnabled(next);
    publish(next, magEnabled);
  }

  function toggleMag() {
    const next = !magEnabled;
    setMagEnabled(next);
    publish(imuEnabled, next);
  }

  function stopBoth() {
    setImuEnabled(false);
    setMagEnabled(false);
    publish(false, false);
  }

  const imuData = useMemo(
    () => rawImu.map((s, i) => ({
      i,
      ax: s.ax, ay: s.ay, az: s.az,
      gx: s.gx, gy: s.gy, gz: s.gz,
    })),
    [rawImu],
  );
  const magData = useMemo(
    () => rawMag.map((s, i) => ({ i, mx: s.mx, my: s.my, mz: s.mz })),
    [rawMag],
  );

  return (
    <div className={styles.rawSensorsRoot}>
      <div className={styles.rawSensorToolbar}>
        <button className={`${styles.axisBtn} ${imuEnabled ? styles.axisBtnActive : ''}`} onClick={toggleImu}>
          IMU RAW {imuEnabled ? 'ON' : 'OFF'}
        </button>
        <button className={`${styles.axisBtn} ${magEnabled ? styles.axisBtnActive : ''}`} onClick={toggleMag}>
          MAG RAW {magEnabled ? 'ON' : 'OFF'}
        </button>
        <button className={styles.stopBtn} onClick={stopBoth}>STOP STREAMS</button>
        <button className={styles.clearBtn} onClick={clearRawSensors}>CLEAR DATA</button>
        <span className={styles.rawSensorCount}>
          IMU {rawImu.length} samples / MAG {rawMag.length} samples
        </span>
      </div>

      <div className={styles.rawChartGrid}>
        <RawChart
          title="ACCELEROMETER"
          data={imuData}
          unit="g"
          lines={[
            { key: 'ax', label: 'ax', color: '#00ccff' },
            { key: 'ay', label: 'ay', color: '#CA4F00' },
            { key: 'az', label: 'az', color: '#b0ffb0' },
          ]}
        />
        <RawChart
          title="GYROSCOPE"
          data={imuData}
          unit="dps"
          lines={[
            { key: 'gx', label: 'gx', color: '#00ccff' },
            { key: 'gy', label: 'gy', color: '#CA4F00' },
            { key: 'gz', label: 'gz', color: '#b0ffb0' },
          ]}
        />
        <RawChart
          title="MAGNETOMETER"
          data={magData}
          unit="gauss"
          lines={[
            { key: 'mx', label: 'mx', color: '#00ccff' },
            { key: 'my', label: 'my', color: '#CA4F00' },
            { key: 'mz', label: 'mz', color: '#b0ffb0' },
          ]}
        />
      </div>

      <div className={styles.orientationNote}>
        Toggles publish to <code>{TOPICS.RAW_SENSORS_CMD}</code>. The Pico streams raw samples on <code>{TOPICS.RAW_IMU}</code> and <code>{TOPICS.RAW_MAG}</code> while enabled.
      </div>
    </div>
  );
}

function AhrsTab({
  ahrsHistory,
  latest,
  status,
  seenCount,
  parsedCount,
  clearAhrsHistory,
}: {
  ahrsHistory: GroundImuState[];
  latest: GroundImuState | null;
  status: AhrsStatus | null;
  seenCount: number;
  parsedCount: number;
  clearAhrsHistory: () => void;
}) {
  const rpyData = useMemo(
    () => ahrsHistory.map((s, i) => ({
      i,
      roll: s.roll,
      pitch: s.pitch,
    })),
    [ahrsHistory],
  );

  const yawData = useMemo(
    () => ahrsHistory.map((s, i) => ({
      i,
      yaw: s.yaw360 ?? norm360(s.yaw) ?? 0,
    })),
    [ahrsHistory],
  );

  const quatData = useMemo(
    () => ahrsHistory
      .filter((s) => s.q != null)
      .map((s, i) => ({
        i,
        qw: s.q?.[0] ?? 0,
        qx: s.q?.[1] ?? 0,
        qy: s.q?.[2] ?? 0,
        qz: s.q?.[3] ?? 0,
      })),
    [ahrsHistory],
  );

  return (
    <div className={styles.rawSensorsRoot}>
      <div className={styles.rawSensorToolbar}>
        <span className={styles.rawSensorCount}>
          AHRS {ahrsHistory.length} samples / seen {seenCount} / parsed {parsedCount} / latest {latest ? (latest.valid ? 'VALID' : 'STARTUP') : '--'}
        </span>
        <button className={styles.clearBtn} onClick={clearAhrsHistory}>CLEAR DATA</button>
      </div>

      <div className={styles.rawChartGrid}>
        <RawChart
          title="ROLL / PITCH"
          data={rpyData}
          unit="deg"
          lines={[
            { key: 'roll',  label: 'roll',  color: '#00ccff' },
            { key: 'pitch', label: 'pitch', color: '#CA4F00' },
          ]}
        />
        <RawChart
          title="YAW 0-360"
          data={yawData}
          unit="deg"
          lines={[
            { key: 'yaw', label: 'yaw', color: '#b0ffb0' },
          ]}
        />
        <RawChart
          title="QUATERNION"
          data={quatData}
          unit=""
          lines={[
            { key: 'qw', label: 'qw', color: '#e0e0e0' },
            { key: 'qx', label: 'qx', color: '#00ccff' },
            { key: 'qy', label: 'qy', color: '#CA4F00' },
            { key: 'qz', label: 'qz', color: '#b0ffb0' },
          ]}
        />
      </div>

      <div className={styles.orientationGrid}>
        <div className={styles.orientationCard}>
          <div className={styles.cardTitle}>LATEST AHRS</div>
          <div className={styles.kvGrid}>
            <span>Task</span><strong>{status ? (status.running ? 'RUNNING' : 'STOPPED') : '--'}</strong>
            <span>Task seen</span><strong>{fmtSeen(status?.timestamp ?? null)}</strong>
            <span>Updates</span><strong>{status?.updates != null ? String(status.updates) : '--'}</strong>
            <span>IMU input</span><strong>{status ? (status.have_imu ? 'YES' : 'NO') : '--'}</strong>
            <span>Mag input</span><strong>{status ? (status.have_mag ? 'YES' : 'NO') : '--'}</strong>
            <span>Roll</span><strong>{fmtDeg(latest?.roll)}</strong>
            <span>Pitch</span><strong>{fmtDeg(latest?.pitch)}</strong>
            <span>Yaw 0-360</span><strong>{fmtDeg(latest?.yaw360 ?? norm360(latest?.yaw))}</strong>
            <span>Yaw signed</span><strong>{fmtDeg(latest?.yaw)}</strong>
            <span>Valid</span><strong>{latest ? (latest.valid ? 'YES' : 'NO') : '--'}</strong>
            <span>Mag rec</span><strong>{latest ? (latest.mag_rec ? 'ON' : 'OFF') : '--'}</strong>
            <span>Acc rec</span><strong>{latest ? (latest.acc_rec ? 'ON' : 'OFF') : '--'}</strong>
          </div>
        </div>
        <div className={styles.orientationCard}>
          <div className={styles.cardTitle}>LATEST QUATERNION</div>
          <div className={styles.vectorBlock}>
            <span>q[w,x,y,z]</span>
            <code>{latest?.q ? latest.q.map(v => v.toFixed(5)).join(', ') : '--'}</code>
          </div>
        </div>
      </div>
    </div>
  );
}

// -- Main panel ----------------------------------------------------------------
export function DebugPanel({ mqtt }: Props) {
  const {
    logLines, rawMessages, clearDebug, antenna, groundImu, connected,
    rawImu, rawMag, clearRawSensors, ahrsHistory, clearAhrsHistory,
  } = useTelemetryStore();

  // Tab within the debug panel
  const [subTab, setSubTab] = useState<'log' | 'topics' | 'orientation' | 'ahrs' | 'raw' | 'starlink' | 'sim'>('log');

  // Topic filter for the inspector
  const [topicFilter, setTopicFilter] = useState('');

  // Antenna command state
  const [az,    setAz]    = useState('0');
  const [zen,   setZen]   = useState('0');
  const [speed, setSpeed] = useState('30');
  const [showConfirm, setShowConfirm] = useState(false);
  const [jogStep,  setJogStep]  = useState('2');
  const [jogSpeed, setJogSpeed] = useState('10');

  // Debug oscillation state
  const [oscAxis,    setOscAxis]    = useState<'az' | 'zen'>('zen');
  const [oscDeg,     setOscDeg]     = useState('5');
  const [oscSpeed,   setOscSpeed]   = useState('30');
  const [oscActive,  setOscActive]  = useState(false);

  // Auto-scroll the log
  const logEndRef    = useRef<HTMLDivElement>(null);
  const topicEndRef  = useRef<HTMLDivElement>(null);
  const [autoScroll, setAutoScroll] = useState(true);

  useEffect(() => {
    if (autoScroll) {
      logEndRef.current?.scrollIntoView({ behavior: 'instant' });
      topicEndRef.current?.scrollIntoView({ behavior: 'instant' });
    }
  }, [logLines, rawMessages, autoScroll]);

  function handleOscStart() {
    const deg   = parseFloat(oscDeg);
    const spd   = parseFloat(oscSpeed);
    if (isNaN(deg) || isNaN(spd)) return;
    const topic = oscAxis === 'az' ? TOPICS.DEBUG_AZ_OSC : TOPICS.DEBUG_ZEN_OSC;
    mqtt.publish(topic, JSON.stringify({ deg, speed_dps: spd, stop: false }));
    setOscActive(true);
  }

  function handleOscStop() {
    const topic = oscAxis === 'az' ? TOPICS.DEBUG_AZ_OSC : TOPICS.DEBUG_ZEN_OSC;
    mqtt.publish(topic, JSON.stringify({ stop: true }));
    setOscActive(false);
  }

  function handleSendCmd() {
    const azVal    = parseFloat(az);
    const zenVal   = parseFloat(zen);
    const speedVal = parseFloat(speed);
    if (isNaN(azVal) || isNaN(zenVal) || isNaN(speedVal)) return;

    mqtt.publish(TOPICS.STEPPER_AZ_CMD,  JSON.stringify({ target_angle_deg: azVal,  speed_dps: speedVal, stop: false }));
    mqtt.publish(TOPICS.STEPPER_ZEN_CMD, JSON.stringify({ target_angle_deg: zenVal, speed_dps: speedVal, stop: false }));
    setShowConfirm(false);
  }

  function publishCalibration(action: string) {
    mqtt.publish(TOPICS.CALIBRATION_CMD, JSON.stringify({ action }));
  }

  function handleJog(axis: 'az' | 'zen', sign: 1 | -1) {
    const step = parseFloat(jogStep);
    const spd  = parseFloat(jogSpeed);
    if (isNaN(step) || isNaN(spd)) return;
    mqtt.publish(TOPICS.STEPPER_JOG_CMD, JSON.stringify({
      axis,
      delta_deg: sign * step,
      speed_dps: spd,
    }));
  }

  function handleStopMotion() {
    mqtt.publish(TOPICS.STEPPER_AZ_CMD,  JSON.stringify({ stop: true }));
    mqtt.publish(TOPICS.STEPPER_ZEN_CMD, JSON.stringify({ stop: true }));
    publishCalibration('disable_tracking');
  }

  const filteredMessages = topicFilter.trim()
    ? rawMessages.filter(m => m.topic.includes(topicFilter.trim()))
    : rawMessages;

  const fallbackImu = useMemo(
    () => latestJson<GroundImuState>(rawMessages, TOPICS.GROUND_IMU),
    [rawMessages],
  );
  const fallbackAntenna = useMemo(
    () => latestJson<AntennaState>(rawMessages, TOPICS.ANTENNA_STATE),
    [rawMessages],
  );
  const ahrsStatus = useMemo(
    () => latestJson<AhrsStatus>(rawMessages, TOPICS.AHRS_STATUS),
    [rawMessages],
  );
  const fallbackAhrsHistory = useMemo(
    () => jsonHistory<GroundImuState>(rawMessages, TOPICS.GROUND_IMU),
    [rawMessages],
  );
  const ahrsSeenCount = useMemo(
    () => rawMessages.filter(m => m.topic === TOPICS.GROUND_IMU).length,
    [rawMessages],
  );
  const orientationImu = groundImu ?? fallbackImu;
  const orientationAntenna = antenna ?? fallbackAntenna;
  const imuSeen = groundImu?.timestamp ?? fallbackImu?.timestamp ?? null;
  const antennaSeen = fallbackAntenna?.timestamp ?? null;
  const displayAhrsHistory = ahrsHistory.length > 0 ? ahrsHistory : fallbackAhrsHistory;

  function formatTs(ts: number): string {
    return new Date(ts).toISOString().slice(11, 23); // HH:MM:SS.mmm
  }

  return (
    <div className={styles.root}>
      {showConfirm && (
        <ConfirmDialog
          az={az} zen={zen} speed={speed}
          onConfirm={handleSendCmd}
          onCancel={() => setShowConfirm(false)}
        />
      )}

      {/* ---- Left: log / topic inspector ---- */}
      <div className={styles.left}>
        <div className={styles.toolbar}>
          <button
            className={`${styles.subTab} ${subTab === 'log' ? styles.subTabActive : ''}`}
            onClick={() => setSubTab('log')}
          >
            CONSOLE ({logLines.length})
          </button>
          <button
            className={`${styles.subTab} ${subTab === 'topics' ? styles.subTabActive : ''}`}
            onClick={() => setSubTab('topics')}
          >
            TOPICS ({rawMessages.length})
          </button>
          <button
            className={`${styles.subTab} ${subTab === 'starlink' ? styles.subTabActive : ''}`}
            onClick={() => setSubTab('starlink')}
          >
            STARLINK
          </button>
          <button
            className={`${styles.subTab} ${subTab === 'orientation' ? styles.subTabActive : ''}`}
            onClick={() => setSubTab('orientation')}
          >
            ORIENTATION
          </button>
          <button
            className={`${styles.subTab} ${subTab === 'ahrs' ? styles.subTabActive : ''}`}
            onClick={() => setSubTab('ahrs')}
          >
            AHRS
          </button>
          <button
            className={`${styles.subTab} ${subTab === 'raw' ? styles.subTabActive : ''}`}
            onClick={() => setSubTab('raw')}
          >
            RAW SENSORS
          </button>
          <button
            className={`${styles.subTab} ${subTab === 'sim' ? styles.subTabActive : ''}`}
            onClick={() => setSubTab('sim')}
          >
            SIMULATION
          </button>

          {subTab === 'topics' && (
            <input
              className={styles.filterInput}
              placeholder="filter topic…"
              value={topicFilter}
              onChange={e => setTopicFilter(e.target.value)}
            />
          )}

          <span className={styles.spacer} />
          <label className={styles.scrollToggle}>
            <input type="checkbox" checked={autoScroll} onChange={e => setAutoScroll(e.target.checked)} />
            &nbsp;auto-scroll
          </label>
          <button className={styles.clearBtn} onClick={clearDebug}>CLEAR</button>
        </div>

        {subTab === 'log' && (
          <div className={styles.console}>
            {logLines.map(line => (
              <div key={line.id} className={styles.logLine}>
                <span className={styles.logTs}>{formatTs(line.ts)}</span>
                <span className={styles.logText}>{line.text.replace(/\n$/, '')}</span>
              </div>
            ))}
            <div ref={logEndRef} />
          </div>
        )}

        {subTab === 'topics' && (
          <div className={styles.console}>
            {filteredMessages.map(msg => (
              <div key={msg.id} className={styles.rawLine}>
                <span className={styles.logTs}>{formatTs(msg.ts)}</span>
                <span className={styles.rawTopic}>{msg.topic}</span>
                <span className={styles.rawPayload}>{msg.payload}</span>
              </div>
            ))}
            <div ref={topicEndRef} />
          </div>
        )}

        {subTab === 'starlink' && <StarlinkTab />}
        {subTab === 'orientation' && (
          <OrientationTab
            antenna={orientationAntenna}
            imu={orientationImu}
            connected={connected}
            imuSeen={imuSeen}
            antennaSeen={antennaSeen}
          />
        )}
        {subTab === 'ahrs' && (
          <AhrsTab
            ahrsHistory={displayAhrsHistory}
            latest={orientationImu}
            status={ahrsStatus}
            seenCount={ahrsSeenCount}
            parsedCount={fallbackAhrsHistory.length}
            clearAhrsHistory={clearAhrsHistory}
          />
        )}
        {subTab === 'raw' && (
          <RawSensorsTab
            mqtt={mqtt}
            rawImu={rawImu}
            rawMag={rawMag}
            clearRawSensors={clearRawSensors}
          />
        )}
        {subTab === 'sim'      && <SimTab />}
      </div>

      {/* ---- Right: antenna command panel ---- */}
      <div className={styles.right}>
        <div className={styles.ctrlGrid}>

          {/* STATUS */}
          <div className={styles.ctrlCard}>
            <div className={styles.ctrlCardTitle}>STATUS</div>
            <div className={styles.statusGrid}>
              <span>MODE</span>
              <strong>{antenna?.mode ?? '--'}</strong>
              <span>AZ ZERO</span>
              <strong>{antenna?.az_calibrated ? 'SET' : 'OPEN'}</strong>
              <span>ZEN ZERO</span>
              <strong>{antenna?.zen_calibrated ? 'SET' : 'OPEN'}</strong>
              <span>TRACKING</span>
              <strong>{antenna?.tracking_enabled ? 'ON' : 'OFF'}</strong>
              <span>AZ MECH</span>
              <strong>{antenna?.actual_az_mech != null ? `${antenna.actual_az_mech.toFixed(1)}°` : '--'}</strong>
            </div>
          </div>

          {/* JOG */}
          <div className={styles.ctrlCard}>
            <div className={styles.ctrlCardTitle}>JOG</div>
            <div className={styles.ctrlInlineRow}>
              <span className={styles.cmdLabel}>STEP</span>
              <input
                className={styles.cmdInputSm}
                type="number" step="0.1" min="0.1"
                value={jogStep}
                onChange={e => setJogStep(e.target.value)}
              />
              <span className={styles.cmdLabel}>SPD</span>
              <input
                className={styles.cmdInputSm}
                type="number" step="1" min="1" max="30"
                value={jogSpeed}
                onChange={e => setJogSpeed(e.target.value)}
              />
            </div>
            <div className={styles.jogGrid}>
              <button className={styles.axisBtn} onClick={() => handleJog('az', -1)}>AZ −</button>
              <button className={styles.axisBtn} onClick={() => handleJog('az',  1)}>AZ +</button>
              <button className={styles.axisBtn} onClick={() => handleJog('zen', -1)}>ZEN −</button>
              <button className={styles.axisBtn} onClick={() => handleJog('zen',  1)}>ZEN +</button>
            </div>
          </div>

          {/* CALIBRATION */}
          <div className={styles.ctrlCard}>
            <div className={styles.ctrlCardTitle}>CALIBRATION</div>
            <button className={styles.sendBtn} onClick={() => publishCalibration('set_az_zero')}>SET AZ ZERO</button>
            <button className={styles.sendBtn} onClick={() => publishCalibration('set_zen_zero')}>SET ZEN ZERO</button>
            <button className={styles.sendBtn} onClick={() => publishCalibration('enable_tracking')}>ENABLE TRACKING</button>
            <button className={styles.stopBtn} onClick={handleStopMotion}>STOP / DISABLE</button>
            <button className={styles.stopBtn} onClick={() => publishCalibration('clear_calibration')}>CLEAR CAL</button>
            <div className={styles.cmdNote}>
              Jog to cable-center, set az zero, jog to horizontal reference, set zen zero, then enable tracking.
            </div>
          </div>

          {/* DIRECT COMMAND */}
          <div className={styles.ctrlCard}>
            <div className={styles.ctrlCardTitle}>DIRECT CMD</div>
            <label className={styles.cmdLabel}>Azimuth (deg)</label>
            <input
              className={styles.cmdInput}
              type="number" step="0.1"
              value={az}
              onChange={e => setAz(e.target.value)}
            />
            <label className={styles.cmdLabel}>Zenith (deg)</label>
            <input
              className={styles.cmdInput}
              type="number" step="0.1" min="0" max="90"
              value={zen}
              onChange={e => setZen(e.target.value)}
            />
            <label className={styles.cmdLabel}>Speed (deg/s)</label>
            <input
              className={styles.cmdInput}
              type="number" step="1" min="1" max="90"
              value={speed}
              onChange={e => setSpeed(e.target.value)}
            />
            <button className={styles.sendBtn} onClick={() => setShowConfirm(true)}>SEND →</button>
            <div className={styles.cmdNote}>
              Publishes to<br />
              <code>{TOPICS.STEPPER_AZ_CMD}</code><br />
              <code>{TOPICS.STEPPER_ZEN_CMD}</code>
            </div>
          </div>

          {/* DEBUG OSCILLATION — full width */}
          <div className={`${styles.ctrlCard} ${styles.ctrlCardFull}`}>
            <div className={styles.ctrlCardTitle}>DEBUG OSCILLATION</div>
            <div className={styles.cmdNote}>
              Bounces between 0° and the target. Watch <code>debug/stepper</code> for DIR changes.
            </div>
            <div className={styles.oscRow}>
              <div className={styles.axisToggle}>
                <button
                  className={`${styles.axisBtn} ${oscAxis === 'az' ? styles.axisBtnActive : ''}`}
                  onClick={() => setOscAxis('az')}
                >AZ</button>
                <button
                  className={`${styles.axisBtn} ${oscAxis === 'zen' ? styles.axisBtnActive : ''}`}
                  onClick={() => setOscAxis('zen')}
                >ZEN</button>
              </div>
              <span className={styles.cmdLabel}>TARGET</span>
              <input
                className={styles.cmdInputSm}
                type="number" step="1"
                value={oscDeg}
                onChange={e => setOscDeg(e.target.value)}
              />
              <span className={styles.cmdLabel}>°&nbsp;&nbsp;SPD</span>
              <input
                className={styles.cmdInputSm}
                type="number" step="1" min="1" max="90"
                value={oscSpeed}
                onChange={e => setOscSpeed(e.target.value)}
              />
              <span className={styles.cmdLabel}>dps</span>
              <div className={styles.oscBtns}>
                <button
                  className={`${styles.sendBtn} ${oscActive ? styles.sendBtnActive : ''}`}
                  onClick={handleOscStart}
                  disabled={oscActive}
                >START</button>
                <button
                  className={styles.stopBtn}
                  onClick={handleOscStop}
                  disabled={!oscActive}
                >STOP</button>
              </div>
            </div>
            {oscActive && (
              <div className={styles.oscStatus}>
                OSCILLATING {oscAxis.toUpperCase()} 0° ↔ {oscDeg}° @ {oscSpeed} dps
              </div>
            )}
          </div>

        </div>
      </div>
    </div>
  );
}
