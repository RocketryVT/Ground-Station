import { useEffect, useMemo, useRef, useState, useCallback } from 'react';
import {
  LineChart, Line, XAxis, YAxis, ResponsiveContainer, Tooltip,
} from 'recharts';
import { useTelemetryStore } from '../../store/telemetryStore';
import { TOPICS, STARLINK_PROXY_URL } from '../../config';
import type { MQTTHandle } from '../../hooks/useMQTT';
import { decodeStarlinkProxyStatus } from '../../proto/groundStationCodec';
import type {
  AhrsStatus,
  AntennaState,
  GroundImuState,
  RawImuSample,
  RawMagSample,
  RawYawImuSample,
} from '../../types/telemetry';
import { SimTab } from './SimTab';
import { GpsTab } from './GpsTab';
import { CalibrationWizard } from '../CalibrationWizard/CalibrationWizard';
import { MagCalibrationWizard } from '../MagCalibrationWizard/MagCalibrationWizard';
import { AhrsFrameScene } from '../AhrsFrameScene/AhrsFrameScene';
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
  error:          string | null;
  location_error: string | null;
  last_ok:        number | null;
}

type DebugTab = 'log' | 'orientation' | 'ahrs' | 'raw' | 'starlink' | 'gps' | 'sim' | 'magcal' | 'calibration';

const DEBUG_TABS: Array<{ id: DebugTab; label: string; section: string }> = [
  { id: 'log',         label: 'Console',     section: 'Streams' },
  { id: 'orientation', label: 'Orientation', section: 'Avionics' },
  { id: 'ahrs',        label: 'AHRS',        section: 'Avionics' },
  { id: 'raw',         label: 'Raw Sensors', section: 'Avionics' },
  { id: 'starlink',    label: 'Starlink',    section: 'Network' },
  { id: 'gps',         label: 'GPS Source',  section: 'Network' },
  { id: 'sim',         label: 'Simulation',  section: 'Tools' },
  { id: 'calibration', label: 'Simple Calibration',    section: 'Setup' },
  { id: 'magcal',      label: 'Full Axis Calibration', section: 'Setup' },
];

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
      const contentType = res.headers.get('content-type') ?? '';
      const data = contentType.includes('json')
        ? await res.json()
        : decodeStarlinkProxyStatus(new Uint8Array(await res.arrayBuffer()));
      setData(data as unknown as StarlinkData);
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
  function fmtLocation(v: number | null | undefined, decimals = 1, unit = ''): string {
    if (data?.location_error && v == null) return 'NO RESPONSE';
    return fmt(v, decimals, unit);
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
          <Row label="Latitude"   value={fmtLocation(data?.lat,  6, '°')} />
          <Row label="Longitude"  value={fmtLocation(data?.lon,  6, '°')} />
          <Row label="Altitude"   value={fmtLocation(data?.alt,  1, ' m')} />
          <Row label="Horiz spd" value={fmtLocation(data?.horizontal_speed_mps, 2, ' m/s')} />
          <Row label="Vert spd"  value={fmtLocation(data?.vertical_speed_mps,  2, ' m/s')} />
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
type DirectAxis = 'az' | 'el';
type DirectCommandKind = 'stepper' | 'absolute_ahrs' | 'direct_el_ahrs';
type PendingDirectCommand = {
  axis: DirectAxis;
  kind: DirectCommandKind;
};
type TrackerMode = 'stop' | 'manual' | 'auto' | 'scan';
type PendingTrackerCommand = {
  id: 'arm' | 'disarm' | 'stop_all' | TrackerMode;
  label: string;
  startedAt: number;
};

interface ConfirmDialogProps {
  axis:   DirectAxis;
  kind:   DirectCommandKind;
  value:  string;
  speed:  string;
  onConfirm: () => void;
  onCancel:  () => void;
}

function ConfirmDialog({ axis, kind, value, speed, onConfirm, onCancel }: ConfirmDialogProps) {
  const label = axis === 'az' ? 'Azimuth' : 'Elevation';
  const frame = kind === 'stepper' ? 'Stepper target' : 'AHRS reported angle';

  return (
    <div className={styles.overlay}>
      <div className={styles.dialog}>
        <div className={styles.dialogTitle}>CONFIRM ANTENNA COMMAND</div>
        <div className={styles.dialogBody}>
          <div className={styles.dialogRow}>
            <span className={styles.dialogLabel}>{label}</span>
            <span className={styles.dialogValue}>{value}°</span>
          </div>
          <div className={styles.dialogRow}>
            <span className={styles.dialogLabel}>Frame</span>
            <span className={styles.dialogValue}>{frame}</span>
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

const SENSOR_WINDOW_OPTIONS = [5, 10, 30, 60] as const;

function filterSensorWindow<T extends { timestamp: number }>(samples: T[], seconds: number): T[] {
  const latest = samples.at(-1)?.timestamp;
  if (latest == null) return samples;
  const cutoff = latest - seconds * 1000;
  return samples.filter((sample) => sample.timestamp >= cutoff);
}

function OrientationTab({
  antenna,
  imu,
  connected,
  imuSeen,
  antennaSeen,
  mqtt,
}: {
  antenna: AntennaState | null;
  imu: GroundImuState | null;
  connected: boolean;
  imuSeen: number | null;
  antennaSeen: number | null;
  mqtt: MQTTHandle;
}) {
  const baseAz = antenna?.actual_az ?? 0;
  // Use yaw-frame-relative bar pitch for elevation. Raw bar Euler pitch is
  // sensor-frame dependent and is negative for the current mounting.
  const imuElevation = imu?.bar_rel_pitch;
  const elevation = imuElevation ?? antenna?.actual_el ?? 0;

  // Declination state
  const [declInput,     setDeclInput]     = useState('-8.53');
  const [noaaLat,       setNoaaLat]       = useState('');
  const [noaaLon,       setNoaaLon]       = useState('');
  const [noaaFetching,  setNoaaFetching]  = useState(false);
  const [noaaMsg,       setNoaaMsg]       = useState<string | null>(null);

  function sendDeclination(deg: number) {
    mqtt.publish(TOPICS.DECLINATION_CMD, JSON.stringify({ declination_deg: deg }));
  }

  async function fetchNoaa() {
    const lat = parseFloat(noaaLat);
    const lon = parseFloat(noaaLon);
    if (!Number.isFinite(lat) || !Number.isFinite(lon)) {
      setNoaaMsg('Enter valid lat / lon first');
      return;
    }
    setNoaaFetching(true);
    setNoaaMsg(null);
    try {
      const url = `https://www.ngdc.noaa.gov/geomag-web/calculators/calculateDeclination` +
        `?lat1=${lat}&lon1=${lon}&key=zNEw7&resultFormat=json`;
      const res = await fetch(url);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const json = await res.json();
      const deg: number = json?.result?.[0]?.declination;
      if (!Number.isFinite(deg)) throw new Error('No declination in response');
      const rounded = Math.round(deg * 100) / 100;
      setDeclInput(String(rounded));
      sendDeclination(rounded);
      setNoaaMsg(`Fetched ${rounded.toFixed(2)}° — sent to Pico`);
    } catch (e: unknown) {
      setNoaaMsg(`Fetch failed: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setNoaaFetching(false);
    }
  }
  // Use yaw-platform heading (tilt-compensated, stable across bar elevation).
  // Fall back to bar-frame yaw360 only when yaw_frame data is absent.
  const imuYaw = imu?.yaw_frame_yaw360 ?? imu?.yaw360 ?? norm360(imu?.yaw) ?? 0;
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
          <div className={styles.legendRow}><span className={styles.swatchImu} />Yaw platform heading {fmtDeg(imu?.yaw_frame_yaw360 ?? imu?.yaw360 ?? norm360(imu?.yaw))}</div>
        </div>

        <div className={styles.orientationCard}>
          <div className={styles.cardTitle}>SIDE VIEW</div>
          <div className={styles.sideView}>
            <div className={styles.sideBase} />
            <div
              className={styles.elevationArm}
              style={{ transform: `rotate(${-elevation}deg)` }}
            />
            <div className={styles.sideHub} />
          </div>
          <div className={styles.legendRow}>IMU elevation {fmtDeg(imuElevation)}</div>
          <div className={styles.legendRow}>Raw pitch {fmtDeg(imu?.pitch)}</div>
          <div className={styles.legendRow}>Stepper el {fmtDeg(antenna?.actual_el)}</div>
          <div className={styles.legendRow}>Target {fmtDeg(antenna?.target_el)}</div>
        </div>

        <div className={styles.orientationCard}>
          <div className={styles.cardTitle}>SENSOR STATUS</div>
          <div className={styles.kvGrid}>
            <span>MQTT</span><strong>{connected ? 'CONNECTED' : 'OFFLINE'}</strong>
            <span>IMU seen</span><strong>{fmtSeen(imuSeen)}</strong>
            <span>State seen</span><strong>{fmtSeen(antennaSeen)}</strong>
          </div>
          <div className={styles.rawGroupHeader} style={{marginTop:8}}>YAW PLATFORM — LSM6DSOX+LIS3MDL</div>
          <div className={styles.kvGrid}>
            <span>Heading</span><strong>{fmtDeg(imu?.yaw_frame_yaw360)}</strong>
            <span>Heading signed</span><strong>{fmtDeg(imu?.yaw_frame_yaw)}</strong>
            <span>Hdg − base</span><strong>{fmtDeg(yawDelta)}</strong>
            <span>Yaw AHRS</span><strong>{imu ? (imu.yaw_startup ? 'STARTUP' : 'VALID') : '--'}</strong>
          </div>
          <div className={styles.rawGroupHeader} style={{marginTop:8}}>ZENITH BAR — ISM330DLC+LIS3MDL</div>
          <div className={styles.kvGrid}>
            <span>Roll</span><strong>{fmtDeg(imu?.roll)}</strong>
            <span>Raw pitch</span><strong>{fmtDeg(imu?.pitch)}</strong>
            <span>Bar yaw</span><strong>{fmtDeg(imu?.yaw360 ?? norm360(imu?.yaw))}</strong>
            <span>Mag valid</span><strong>{imu ? (imu.have_mag ? 'YES' : 'NO') : '--'}</strong>
            <span>AHRS</span><strong>{imu ? (imu.valid ? 'VALID' : 'STARTUP') : '--'}</strong>
            <span>Mag rec</span><strong>{imu ? (imu.mag_rec ? 'ON' : 'OFF') : '--'}</strong>
            <span>Acc rec</span><strong>{imu ? (imu.acc_rec ? 'ON' : 'OFF') : '--'}</strong>
          </div>
          <div className={styles.rawGroupHeader} style={{marginTop:8}}>RELATIVE (YAW→BAR)</div>
          <div className={styles.kvGrid}>
            <span>Rel roll</span><strong>{fmtDeg(imu?.bar_rel_roll)}</strong>
            <span>Elevation</span><strong>{fmtDeg(imu?.bar_rel_pitch)}</strong>
            <span>Rel yaw</span><strong>{fmtDeg(imu?.bar_rel_yaw)}</strong>
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

        <div className={styles.orientationCard}>
          <div className={styles.cardTitle}>MAGNETIC DECLINATION</div>
          <div className={styles.kvGrid}>
            <span>Pico default</span><strong>−8.53° (Blacksburg VA)</strong>
          </div>
          <div className={styles.rawGroupHeader} style={{marginTop:8}}>MANUAL OVERRIDE</div>
          <div className={styles.declinationRow}>
            <input
              className={styles.declinationInput}
              type="number"
              step="0.01"
              value={declInput}
              onChange={e => setDeclInput(e.target.value)}
              placeholder="deg (neg=west)"
            />
            <span className={styles.declinationUnit}>°</span>
            <button
              className={styles.declinationSendBtn}
              onClick={() => {
                const v = parseFloat(declInput);
                if (Number.isFinite(v)) { sendDeclination(v); setNoaaMsg(`Sent ${v.toFixed(2)}°`); }
              }}
            >SEND</button>
          </div>
          <div className={styles.rawGroupHeader} style={{marginTop:8}}>NOAA AUTO-FETCH</div>
          <div className={styles.declinationRow}>
            <input
              className={styles.declinationInput}
              type="number" step="0.0001" placeholder="lat"
              value={noaaLat} onChange={e => setNoaaLat(e.target.value)}
            />
            <input
              className={styles.declinationInput}
              type="number" step="0.0001" placeholder="lon"
              value={noaaLon} onChange={e => setNoaaLon(e.target.value)}
            />
          </div>
          <button
            className={styles.declinationFetchBtn}
            onClick={fetchNoaa}
            disabled={noaaFetching}
          >
            {noaaFetching ? 'FETCHING…' : 'FETCH FROM NOAA & SEND'}
          </button>
          {noaaMsg && <div className={styles.declinationMsg}>{noaaMsg}</div>}
          <div className={styles.declinationNote}>
            Future: auto-populate lat/lon from ground station GPS.
          </div>
        </div>

      </div>
      <div className={styles.orientationNote}>
        Side view uses IMU pitch (physical bar tilt). True heading = magnetic + declination.
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

function SensorGroupHeader({ label }: { label: string }) {
  return <div className={styles.rawGroupHeader}>{label}</div>;
}

// ---------------------------------------------------------------------------
// CSV recording types
// ---------------------------------------------------------------------------
type RecAz      = RawYawImuSample & { rec_ms: number };
type RecZenImu  = RawImuSample    & { rec_ms: number };
type RecZenMag  = RawMagSample    & { rec_ms: number };

function downloadCsv(filename: string, csv: string) {
  const blob = new Blob([csv], { type: 'text/csv' });
  const url  = URL.createObjectURL(blob);
  const a    = document.createElement('a');
  a.href     = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

function buildCsv(az: RecAz[], zenImu: RecZenImu[], zenMag: RecZenMag[]): string {
  const header = 'rec_ms,sensor,fw_ts_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,mx,my,mz,mag_unit,temp_c,mag_valid';
  const rows: string[] = [header];

  for (const s of az) {
    rows.push([
      s.rec_ms, 'az', s.timestamp,
      s.ax, s.ay, s.az,
      s.gx, s.gy, s.gz,
      s.mx_ut, s.my_ut, s.mz_ut, 'uT',
      s.temp ?? '', s.mag_valid ?? '',
    ].join(','));
  }
  for (const s of zenImu) {
    rows.push([
      s.rec_ms, 'zen_imu', s.timestamp,
      s.ax, s.ay, s.az,
      s.gx, s.gy, s.gz,
      '', '', '', '',
      s.temp ?? '', '',
    ].join(','));
  }
  for (const s of zenMag) {
    rows.push([
      s.rec_ms, 'zen_mag', s.timestamp,
      '', '', '', '', '', '',
      s.mx, s.my, s.mz, 'gauss',
      '', '',
    ].join(','));
  }

  // Sort all data rows (skip header) by rec_ms
  const sorted = rows.slice(1).sort((a, b) => {
    const ta = parseFloat(a.split(',')[0]);
    const tb = parseFloat(b.split(',')[0]);
    return ta - tb;
  });
  return [header, ...sorted].join('\n');
}

function RawSensorsTab({
  mqtt,
  rawImu,
  rawMag,
  rawYawImu,
  clearRawSensors,
}: {
  mqtt: MQTTHandle;
  rawImu: RawImuSample[];
  rawMag: RawMagSample[];
  rawYawImu: RawYawImuSample[];
  clearRawSensors: () => void;
}) {
  const [zenithImuEnabled, setZenithImuEnabled] = useState(false);
  const [zenithMagEnabled, setZenithMagEnabled] = useState(false);
  const [azEnabled,        setAzEnabled]        = useState(false);
  const [windowSeconds,    setWindowSeconds]    = useState<number>(10);

  // Recording state
  const [isRecording,   setIsRecording]   = useState(false);
  const [elapsedSec,    setElapsedSec]    = useState(0);
  const [hasRecording,  setHasRecording]  = useState(false);
  const recStartRef   = useRef<number>(0);
  const recAzRef      = useRef<RecAz[]>([]);
  const recZenImuRef  = useRef<RecZenImu[]>([]);
  const recZenMagRef  = useRef<RecZenMag[]>([]);
  const seenAzTs      = useRef(new Set<number>());
  const seenZenImuTs  = useRef(new Set<number>());
  const seenZenMagTs  = useRef(new Set<number>());

  // Elapsed timer
  useEffect(() => {
    if (!isRecording) return;
    const id = setInterval(() => setElapsedSec(Math.floor((Date.now() - recStartRef.current) / 1000)), 250);
    return () => clearInterval(id);
  }, [isRecording]);

  // Accumulate new samples while recording
  useEffect(() => {
    if (!isRecording) return;
    const now = Date.now();
    const rec_ms = now - recStartRef.current;
    for (const s of rawYawImu) {
      if (!seenAzTs.current.has(s.timestamp)) {
        seenAzTs.current.add(s.timestamp);
        recAzRef.current.push({ ...s, rec_ms });
      }
    }
  }, [rawYawImu, isRecording]);

  useEffect(() => {
    if (!isRecording) return;
    const now = Date.now();
    const rec_ms = now - recStartRef.current;
    for (const s of rawImu) {
      if (!seenZenImuTs.current.has(s.timestamp)) {
        seenZenImuTs.current.add(s.timestamp);
        recZenImuRef.current.push({ ...s, rec_ms });
      }
    }
  }, [rawImu, isRecording]);

  useEffect(() => {
    if (!isRecording) return;
    const now = Date.now();
    const rec_ms = now - recStartRef.current;
    for (const s of rawMag) {
      if (!seenZenMagTs.current.has(s.timestamp)) {
        seenZenMagTs.current.add(s.timestamp);
        recZenMagRef.current.push({ ...s, rec_ms });
      }
    }
  }, [rawMag, isRecording]);

  function startRecording() {
    recAzRef.current     = [];
    recZenImuRef.current = [];
    recZenMagRef.current = [];
    seenAzTs.current     = new Set();
    seenZenImuTs.current = new Set();
    seenZenMagTs.current = new Set();
    recStartRef.current  = Date.now();
    setElapsedSec(0);
    setHasRecording(false);
    setIsRecording(true);
  }

  function stopRecording() {
    setIsRecording(false);
    setHasRecording(
      recAzRef.current.length > 0 ||
      recZenImuRef.current.length > 0 ||
      recZenMagRef.current.length > 0,
    );
  }

  function exportCsv() {
    const ts   = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
    const csv  = buildCsv(recAzRef.current, recZenImuRef.current, recZenMagRef.current);
    downloadCsv(`raw_sensors_${ts}.csv`, csv);
  }

  function publish(nextZenImu = zenithImuEnabled, nextZenMag = zenithMagEnabled, nextAz = azEnabled) {
    mqtt.publish(TOPICS.RAW_SENSORS_CMD, JSON.stringify({
      imu: nextZenImu, mag: nextZenMag, yaw_imu: nextAz,
    }));
  }

  function toggleZenithImu() { const n = !zenithImuEnabled; setZenithImuEnabled(n); publish(n, zenithMagEnabled, azEnabled); }
  function toggleZenithMag() { const n = !zenithMagEnabled; setZenithMagEnabled(n); publish(zenithImuEnabled, n, azEnabled); }
  function toggleAz()        { const n = !azEnabled;        setAzEnabled(n);        publish(zenithImuEnabled, zenithMagEnabled, n); }

  function stopAll() {
    setZenithImuEnabled(false);
    setZenithMagEnabled(false);
    setAzEnabled(false);
    publish(false, false, false);
  }

  const recAzCount     = isRecording ? recAzRef.current.length     : (hasRecording ? recAzRef.current.length     : 0);
  const recZenImuCount = isRecording ? recZenImuRef.current.length  : (hasRecording ? recZenImuRef.current.length  : 0);
  const recZenMagCount = isRecording ? recZenMagRef.current.length  : (hasRecording ? recZenMagRef.current.length  : 0);

  const chartRawImu = useMemo(
    () => filterSensorWindow(rawImu, windowSeconds),
    [rawImu, windowSeconds],
  );
  const chartRawMag = useMemo(
    () => filterSensorWindow(rawMag, windowSeconds),
    [rawMag, windowSeconds],
  );
  const chartRawYawImu = useMemo(
    () => filterSensorWindow(rawYawImu, windowSeconds),
    [rawYawImu, windowSeconds],
  );

  const zenithImuData = useMemo(
    () => chartRawImu.map((s, i) => ({ i, ax: s.ax, ay: s.ay, az: s.az, gx: s.gx, gy: s.gy, gz: s.gz })),
    [chartRawImu],
  );
  const zenithMagData = useMemo(
    () => chartRawMag.map((s, i) => ({ i, mx: s.mx, my: s.my, mz: s.mz })),
    [chartRawMag],
  );
  const azData = useMemo(
    () => chartRawYawImu.map((s, i) => ({
      i,
      ax: s.ax, ay: s.ay, az: s.az,
      gx: s.gx, gy: s.gy, gz: s.gz,
      mx: s.mx_ut, my: s.my_ut, mz: s.mz_ut,
    })),
    [chartRawYawImu],
  );

  const accelLines = [
    { key: 'ax', label: 'ax', color: '#00ccff' },
    { key: 'ay', label: 'ay', color: '#CA4F00' },
    { key: 'az', label: 'az', color: '#b0ffb0' },
  ];
  const gyroLines = [
    { key: 'gx', label: 'gx', color: '#00ccff' },
    { key: 'gy', label: 'gy', color: '#CA4F00' },
    { key: 'gz', label: 'gz', color: '#b0ffb0' },
  ];
  const magLines = [
    { key: 'mx', label: 'mx', color: '#00ccff' },
    { key: 'my', label: 'my', color: '#CA4F00' },
    { key: 'mz', label: 'mz', color: '#b0ffb0' },
  ];

  return (
    <div className={styles.rawSensorsRoot}>
      {/* ---- Stream toggles ---- */}
      <div className={styles.rawSensorToolbar}>
        <button
          className={`${styles.rawToggleBtn} ${azEnabled ? styles.rawToggleBtnActive : ''}`}
          onClick={toggleAz}
          aria-pressed={azEnabled}
        >
          {azEnabled ? 'AZ IMU ON' : 'START AZ IMU'}
        </button>
        <button
          className={`${styles.rawToggleBtn} ${zenithImuEnabled ? styles.rawToggleBtnActive : ''}`}
          onClick={toggleZenithImu}
          aria-pressed={zenithImuEnabled}
        >
          {zenithImuEnabled ? 'ZEN IMU ON' : 'START ZEN IMU'}
        </button>
        <button
          className={`${styles.rawToggleBtn} ${zenithMagEnabled ? styles.rawToggleBtnActive : ''}`}
          onClick={toggleZenithMag}
          aria-pressed={zenithMagEnabled}
        >
          {zenithMagEnabled ? 'ZEN MAG ON' : 'START ZEN MAG'}
        </button>
        <button className={styles.rawStopBtn} onClick={stopAll}>STOP ALL</button>
        <button className={styles.rawClearDataBtn} onClick={clearRawSensors}>CLEAR DATA</button>
        <select
          className={styles.windowSelect}
          value={windowSeconds}
          onChange={(event) => setWindowSeconds(Number(event.target.value))}
          title="Chart buffer"
        >
          {SENSOR_WINDOW_OPTIONS.map((seconds) => (
            <option key={seconds} value={seconds}>{seconds}s</option>
          ))}
        </select>
        <span className={styles.rawSensorCount}>
          AZ {chartRawYawImu.length}/{rawYawImu.length} / ZEN IMU {chartRawImu.length}/{rawImu.length} / ZEN MAG {chartRawMag.length}/{rawMag.length}
        </span>
      </div>

      {/* ---- CSV recording ---- */}
      <div className={styles.recToolbar}>
        {!isRecording ? (
          <button className={styles.recStartBtn} onClick={startRecording}>
            ⏺ START RECORDING
          </button>
        ) : (
          <button className={styles.recStopBtn} onClick={stopRecording}>
            ⏹ STOP  {elapsedSec}s
          </button>
        )}
        {hasRecording && !isRecording && (
          <button className={styles.recExportBtn} onClick={exportCsv}>
            ↓ EXPORT CSV
          </button>
        )}
        {(isRecording || hasRecording) && (
          <span className={styles.recCount}>
            AZ {recAzCount} / ZEN IMU {recZenImuCount} / ZEN MAG {recZenMagCount} rows
          </span>
        )}
      </div>

      <SensorGroupHeader label="AZIMUTH BAR  —  LSM6DSOX (IMU) + LIS3MDL (MAG)" />
      <div className={styles.rawChartGrid}>
        <RawChart title="AZ ACCELEROMETER" data={azData} unit="g"    lines={accelLines} />
        <RawChart title="AZ GYROSCOPE"     data={azData} unit="dps"  lines={gyroLines} />
        <RawChart title="AZ MAGNETOMETER"  data={azData} unit="µT"   lines={magLines} />
      </div>

      <SensorGroupHeader label="ZENITH  —  ISM330DLC (IMU) + LIS3MDL (MAG)" />
      <div className={styles.rawChartGrid}>
        <RawChart title="ZEN ACCELEROMETER" data={zenithImuData} unit="g"     lines={accelLines} />
        <RawChart title="ZEN GYROSCOPE"     data={zenithImuData} unit="dps"   lines={gyroLines} />
        <RawChart title="ZEN MAGNETOMETER"  data={zenithMagData} unit="gauss" lines={magLines} />
      </div>

      <div className={styles.orientationNote}>
        CSV columns: <code>rec_ms, sensor, fw_ts_ms, ax_g…gz_dps, mx…mz, mag_unit, temp_c, mag_valid</code>
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
  const [windowSeconds, setWindowSeconds] = useState<number>(10);
  const chartHistory = useMemo(
    () => filterSensorWindow(ahrsHistory, windowSeconds),
    [ahrsHistory, windowSeconds],
  );

  const rpyData = useMemo(
    () => chartHistory.map((s, i) => ({
      i,
      roll: s.roll,
      pitch: s.pitch,
    })),
    [chartHistory],
  );

  const yawData = useMemo(
    () => chartHistory.map((s, i) => ({
      i,
      yaw: s.yaw360 ?? norm360(s.yaw) ?? 0,
    })),
    [chartHistory],
  );

  const quatData = useMemo(
    () => chartHistory
      .filter((s) => s.q != null)
      .map((s, i) => ({
        i,
        qw: s.q?.[0] ?? 0,
        qx: s.q?.[1] ?? 0,
        qy: s.q?.[2] ?? 0,
        qz: s.q?.[3] ?? 0,
      })),
    [chartHistory],
  );

  return (
    <div className={styles.rawSensorsRoot}>
      <div className={styles.rawSensorToolbar}>
        <span className={styles.rawSensorCount}>
          AHRS {chartHistory.length}/{ahrsHistory.length} samples / seen {seenCount} / parsed {parsedCount} / latest {latest ? (latest.valid ? 'VALID' : 'STARTUP') : '--'}
        </span>
        <select
          className={styles.windowSelect}
          value={windowSeconds}
          onChange={(event) => setWindowSeconds(Number(event.target.value))}
          title="Chart buffer"
        >
          {SENSOR_WINDOW_OPTIONS.map((seconds) => (
            <option key={seconds} value={seconds}>{seconds}s</option>
          ))}
        </select>
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

      <AhrsFrameScene imu={latest} />

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
            <span>Heading 0-360</span><strong>{fmtDeg(latest?.yaw_frame_yaw360 ?? latest?.yaw360 ?? norm360(latest?.yaw))}</strong>
            <span>Heading signed</span><strong>{fmtDeg(latest?.yaw_frame_yaw ?? latest?.yaw)}</strong>
            <span>Bar yaw 0-360</span><strong>{fmtDeg(latest?.yaw360 ?? norm360(latest?.yaw))}</strong>
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
            <span>bar q[w,x,y,z]</span>
            <code>{latest?.bar_q ? latest.bar_q.map(v => v.toFixed(5)).join(', ') : '--'}</code>
            <span>yaw q[w,x,y,z]</span>
            <code>{latest?.yaw_q ? latest.yaw_q.map(v => v.toFixed(5)).join(', ') : '--'}</code>
            <span>rel q[w,x,y,z]</span>
            <code>{latest?.bar_rel_q ? latest.bar_rel_q.map(v => v.toFixed(5)).join(', ') : '--'}</code>
          </div>
        </div>
      </div>
    </div>
  );
}

// -- Main panel ----------------------------------------------------------------
export function DebugPanel({ mqtt }: Props) {
  const logLines = useTelemetryStore((s) => s.logLines);
  const clearDebug = useTelemetryStore((s) => s.clearDebug);
  const antenna = useTelemetryStore((s) => s.antenna);
  const groundImu = useTelemetryStore((s) => s.groundImu);
  const ahrsStatus = useTelemetryStore((s) => s.ahrsStatus);
  const connected = useTelemetryStore((s) => s.connected);
  const rawImu = useTelemetryStore((s) => s.rawImu);
  const rawMag = useTelemetryStore((s) => s.rawMag);
  const rawYawImu = useTelemetryStore((s) => s.rawYawImu);
  const clearRawSensors = useTelemetryStore((s) => s.clearRawSensors);
  const ahrsHistory = useTelemetryStore((s) => s.ahrsHistory);
  const clearAhrsHistory = useTelemetryStore((s) => s.clearAhrsHistory);
  const calibrationEvents = useTelemetryStore((s) => s.calibrationEvents);

  const [subTab, setSubTab] = useState<DebugTab>('log');

  // Antenna command state
  const [az,    setAz]    = useState('0');
  const [el,    setEl]    = useState('0');
  const [absAz, setAbsAz] = useState('0');
  const [absEl, setAbsEl] = useState('0');
  const [absAzEdited, setAbsAzEdited] = useState(false);
  const [absElEdited, setAbsElEdited] = useState(false);
  const [speed, setSpeed] = useState('10');
  const [pendingDirectCommand, setPendingDirectCommand] = useState<PendingDirectCommand | null>(null);
  const [jogStep,  setJogStep]  = useState('2');
  const [jogSpeed, setJogSpeed] = useState('10');
  const [pendingTracker, setPendingTracker] = useState<PendingTrackerCommand | null>(null);

  // Tracker config/calibration state
  const [cfgYawTrim, setCfgYawTrim] = useState('0');
  const [cfgElTrim, setCfgElTrim] = useState('0');
  const [cfgAzMin, setCfgAzMin] = useState('0');
  const [cfgAzMax, setCfgAzMax] = useState('360');
  const [cfgElMin, setCfgElMin] = useState('-10');
  const [cfgElMax, setCfgElMax] = useState('90');
  const [cfgDefaultSpeed, setCfgDefaultSpeed] = useState('30');
  const [cfgMaxSpeed, setCfgMaxSpeed] = useState('90');
  const [cfgScanAz, setCfgScanAz] = useState('20');
  const [cfgScanEl, setCfgScanEl] = useState('5');
  const [cfgGsTimeout, setCfgGsTimeout] = useState('30000');
  const [cfgTargetTimeout, setCfgTargetTimeout] = useState('5000');
  const [cfgDistanceMin, setCfgDistanceMin] = useState('3');
  const [cfgScanOnLoss, setCfgScanOnLoss] = useState(true);
  const [cfgUseAhrsEl, setCfgUseAhrsEl] = useState(true);
  const [cfgUseAhrsAz, setCfgUseAhrsAz] = useState(false);
  const [cfgAhrsAge, setCfgAhrsAge] = useState('250');
  const [cfgAhrsGain, setCfgAhrsGain] = useState('0.35');
  const [cfgAhrsMaxCorrection, setCfgAhrsMaxCorrection] = useState('8');
  // Debug oscillation state
  const [oscAxis,    setOscAxis]    = useState<'az' | 'el'>('el');
  const [oscDeg,     setOscDeg]     = useState('5');
  const [oscSpeed,   setOscSpeed]   = useState('30');
  const [oscActive,  setOscActive]  = useState(false);

  // Auto-scroll the log
  const logEndRef    = useRef<HTMLDivElement>(null);
  const [autoScroll, setAutoScroll] = useState(true);

  useEffect(() => {
    if (autoScroll) {
      logEndRef.current?.scrollIntoView({ behavior: 'instant' });
    }
  }, [logLines, autoScroll]);

  function handleOscStart() {
    const deg   = parseFloat(oscDeg);
    const spd   = parseFloat(oscSpeed);
    if (isNaN(deg) || isNaN(spd)) return;
    const topic = oscAxis === 'az' ? TOPICS.DEBUG_AZ_OSC : TOPICS.DEBUG_EL_OSC;
    mqtt.publish(topic, JSON.stringify({ deg, speed_dps: spd, stop: false }));
    setOscActive(true);
  }

  function handleOscStop() {
    const topic = oscAxis === 'az' ? TOPICS.DEBUG_AZ_OSC : TOPICS.DEBUG_EL_OSC;
    mqtt.publish(topic, JSON.stringify({ stop: true }));
    setOscActive(false);
  }

  function handleSendCmd(axis: DirectAxis, kind: DirectCommandKind) {
    const speedVal = parseFloat(speed);
    const targetVal = parseFloat(kind === 'absolute_ahrs'
      ? (axis === 'az' ? absAz : absEl)
      : (axis === 'az' ? az : el));
    if (isNaN(targetVal) || isNaN(speedVal)) return;

    const topic = axis === 'az' ? TOPICS.STEPPER_AZ_CMD : TOPICS.STEPPER_EL_CMD;
    mqtt.publish(topic, JSON.stringify({
      target_angle_deg: targetVal,
      speed_dps: speedVal,
      stop: false,
      absolute_ahrs: kind !== 'stepper',
    }));
    setPendingDirectCommand(null);
  }

  function numberOrUndefined(value: string): number | undefined {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : undefined;
  }

  function publishTrackerConfig() {
    mqtt.publish(TOPICS.TRACKER_CONFIG_CMD, JSON.stringify({
      yaw_trim_deg: numberOrUndefined(cfgYawTrim),
      el_trim_deg: numberOrUndefined(cfgElTrim),
      az_min_deg: numberOrUndefined(cfgAzMin),
      az_max_deg: numberOrUndefined(cfgAzMax),
      el_min_deg: numberOrUndefined(cfgElMin),
      el_max_deg: numberOrUndefined(cfgElMax),
      default_speed_dps: numberOrUndefined(cfgDefaultSpeed),
      max_speed_dps: numberOrUndefined(cfgMaxSpeed),
      scan_speed_az_dps: numberOrUndefined(cfgScanAz),
      scan_speed_el_dps: numberOrUndefined(cfgScanEl),
      gs_timeout_ms: numberOrUndefined(cfgGsTimeout),
      target_timeout_ms: numberOrUndefined(cfgTargetTimeout),
      distance_min_m: numberOrUndefined(cfgDistanceMin),
      scan_on_loss: cfgScanOnLoss,
      use_ahrs_el: cfgUseAhrsEl,
      use_ahrs_az: cfgUseAhrsAz,
      ahrs_max_age_ms: numberOrUndefined(cfgAhrsAge),
      ahrs_feedback_gain: numberOrUndefined(cfgAhrsGain),
      ahrs_max_correction_deg: numberOrUndefined(cfgAhrsMaxCorrection),
    }));
  }

  function handleJog(axis: 'az' | 'el', sign: 1 | -1) {
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
    setPendingTracker({ id: 'stop_all', label: 'STOP ALL', startedAt: Date.now() });
    mqtt.publish(TOPICS.STEPPER_AZ_CMD,  JSON.stringify({ stop: true }));
    mqtt.publish(TOPICS.STEPPER_EL_CMD, JSON.stringify({ stop: true }));
    mqtt.publish(TOPICS.TRACKER_MODE_CMD, JSON.stringify({ mode: 'stop' }));
    mqtt.publish(TOPICS.TRACKER_ARM_CMD, JSON.stringify({ armed: false }));
  }

  function handleTrackerMode(mode: TrackerMode) {
    setPendingTracker({ id: mode, label: mode.toUpperCase(), startedAt: Date.now() });
    mqtt.publish(TOPICS.TRACKER_MODE_CMD, JSON.stringify({ mode }));
  }

  function handleTrackerArm(armed: boolean) {
    setPendingTracker({ id: armed ? 'arm' : 'disarm', label: armed ? 'ARM' : 'DISARM', startedAt: Date.now() });
    mqtt.publish(TOPICS.TRACKER_ARM_CMD, JSON.stringify({ armed }));
  }

  useEffect(() => {
    if (!pendingTracker || !antenna) return;
    const mode = (antenna.mode ?? '').toLowerCase();
    const acked =
      (pendingTracker.id === 'arm' && antenna.armed === true) ||
      (pendingTracker.id === 'disarm' && antenna.armed === false) ||
      (pendingTracker.id === 'stop_all' && antenna.armed === false && mode === 'stop') ||
      (pendingTracker.id !== 'arm' &&
        pendingTracker.id !== 'disarm' &&
        pendingTracker.id !== 'stop_all' &&
        mode === pendingTracker.id);
    if (acked) setPendingTracker(null);
  }, [antenna, pendingTracker]);

  const orientationImu = groundImu;
  const orientationAntenna = antenna;
  const imuSeen = groundImu?.timestamp ?? null;
  const antennaSeen = antenna?.timestamp ?? null;
  const ahrsSeenCount = ahrsHistory.length;
  const displayAhrsHistory = ahrsHistory;
  const trackerMode = (antenna?.mode ?? '').toLowerCase();
  const directCommandReady = antenna?.armed === true && trackerMode === 'manual';
  const magneticAz = groundImu?.yaw_frame_yaw360 ?? null;
  const absoluteEl = groundImu?.bar_rel_pitch ?? null;
  const absoluteCommandReady = directCommandReady && magneticAz != null && absoluteEl != null;
  const activeTab = DEBUG_TABS.find((item) => item.id === subTab) ?? DEBUG_TABS[0];
  const tabCounts: Partial<Record<DebugTab, number>> = {
    log: logLines.length,
    raw: rawImu.length + rawMag.length + rawYawImu.length,
    ahrs: displayAhrsHistory.length,
  };

  function formatTs(ts: number): string {
    return new Date(ts).toISOString().slice(11, 23); // HH:MM:SS.mmm
  }

  useEffect(() => {
    if (!absAzEdited && magneticAz != null) {
      setAbsAz(magneticAz.toFixed(1));
    }
  }, [absAzEdited, magneticAz]);

  useEffect(() => {
    if (!absElEdited && absoluteEl != null) {
      setAbsEl(absoluteEl.toFixed(1));
    }
  }, [absElEdited, absoluteEl]);

  return (
    <div className={styles.root}>
      {pendingDirectCommand && (
        <ConfirmDialog
          axis={pendingDirectCommand.axis}
          kind={pendingDirectCommand.kind}
          value={pendingDirectCommand.kind === 'absolute_ahrs'
            ? (pendingDirectCommand.axis === 'az' ? absAz : absEl)
            : (pendingDirectCommand.axis === 'az' ? az : el)}
          speed={speed}
          onConfirm={() => handleSendCmd(pendingDirectCommand.axis, pendingDirectCommand.kind)}
          onCancel={() => setPendingDirectCommand(null)}
        />
      )}

      <aside className={styles.nav}>
        <div className={styles.navHeader}>
          <span>Systems</span>
          <strong>Diagnostics</strong>
        </div>
        {['Setup', 'Streams', 'Avionics', 'Network', 'Tools'].map((section) => (
          <div key={section} className={styles.navSection}>
            <div className={styles.navSectionLabel}>{section}</div>
            {DEBUG_TABS.filter((item) => item.section === section).map((item) => (
              <button
                key={item.id}
                className={`${styles.navItem} ${subTab === item.id ? styles.navItemActive : ''}`}
                onClick={() => setSubTab(item.id)}
              >
                <span>{item.label}</span>
                {tabCounts[item.id] != null && <strong>{tabCounts[item.id]}</strong>}
              </button>
            ))}
          </div>
        ))}
      </aside>

      <main className={styles.left}>
        <div className={styles.workspaceHeader}>
          <div>
            <span className={styles.workspaceEyebrow}>{activeTab.section}</span>
            <h2>{activeTab.label}</h2>
          </div>
          <div className={styles.workspaceTools}>
            {subTab === 'log' && (
              <label className={styles.scrollToggle}>
                <input type="checkbox" checked={autoScroll} onChange={e => setAutoScroll(e.target.checked)} />
                auto-scroll
              </label>
            )}
            <button className={styles.clearBtn} onClick={clearDebug}>Clear</button>
          </div>
        </div>

        <div className={styles.workspaceBody}>
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

          {subTab === 'starlink' && <StarlinkTab />}
          {subTab === 'gps' && <GpsTab />}
          {subTab === 'orientation' && (
            <OrientationTab
              antenna={orientationAntenna}
              imu={orientationImu}
              connected={connected}
              imuSeen={imuSeen}
              antennaSeen={antennaSeen}
              mqtt={mqtt}
            />
          )}
          {subTab === 'ahrs' && (
            <AhrsTab
              ahrsHistory={displayAhrsHistory}
              latest={orientationImu}
              status={ahrsStatus}
              seenCount={ahrsSeenCount}
              parsedCount={displayAhrsHistory.length}
              clearAhrsHistory={clearAhrsHistory}
            />
          )}
          {subTab === 'raw' && (
            <RawSensorsTab
              mqtt={mqtt}
              rawImu={rawImu}
              rawMag={rawMag}
              rawYawImu={rawYawImu}
              clearRawSensors={clearRawSensors}
            />
          )}
          {subTab === 'sim' && <SimTab />}
          {subTab === 'magcal' && (
            <div className={styles.calibrationScroll}>
              <MagCalibrationWizard
                mqtt={mqtt}
                connected={connected}
                rawMag={rawMag}
                rawYawImu={rawYawImu}
              />
            </div>
          )}
          {subTab === 'calibration' && (
            <div className={styles.calibrationScroll}>
              <CalibrationWizard
                mqtt={mqtt}
                antenna={antenna}
                imu={orientationImu}
                connected={connected}
                calibrationEvents={calibrationEvents}
              />
            </div>
          )}
        </div>
      </main>

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
              <span>EL ZERO</span>
              <strong>{antenna?.zen_calibrated ? 'SET' : 'OPEN'}</strong>
              <span>TRACKING</span>
              <strong>{antenna?.tracking_enabled ? 'ON' : 'OFF'}</strong>
              <span>ARMED</span>
              <strong>{antenna?.armed ? 'YES' : 'NO'}</strong>
              <span>FIXES</span>
              <strong>{antenna?.gs_fresh ? 'GS' : '--'} / {antenna?.target_fresh ? 'TGT' : '--'}</strong>
              <span>ERR</span>
              <strong>
                {antenna?.pointing_error_az != null ? antenna.pointing_error_az.toFixed(1) : '--'} /
                {antenna?.pointing_error_el != null ? antenna.pointing_error_el.toFixed(1) : '--'}°
              </strong>
              <span>AHRS</span>
              <strong>{antenna?.ahrs_az_used ? 'AZ' : '--'} / {antenna?.ahrs_el_used ? 'EL' : '--'}</strong>
              <span>AZ MECH</span>
              <strong>{antenna?.actual_az_mech != null ? `${antenna.actual_az_mech.toFixed(1)}°` : '--'}</strong>
              <span>CAL</span>
              <strong>{antenna?.calibration_status ?? '--'}</strong>
              <span>REF</span>
              <strong>
                {antenna?.az_reference_deg != null ? antenna.az_reference_deg.toFixed(1) : '--'} /
                {antenna?.el_reference_deg != null ? antenna.el_reference_deg.toFixed(1) : '--'}°
              </strong>
            </div>
          </div>

          {/* TRACKER */}
          <div className={styles.ctrlCard}>
            <div className={styles.ctrlCardTitle}>TRACKER</div>
            <div className={styles.jogGrid}>
              <button
                className={`${styles.sendBtn} ${antenna?.armed ? styles.trackerBtnActive : ''} ${pendingTracker?.id === 'arm' ? styles.trackerBtnPending : ''}`}
                onClick={() => handleTrackerArm(true)}
              >ARM</button>
              <button
                className={`${styles.stopBtn} ${antenna?.armed === false ? styles.trackerBtnActive : ''} ${pendingTracker?.id === 'disarm' ? styles.trackerBtnPending : ''}`}
                onClick={() => handleTrackerArm(false)}
              >DISARM</button>
              <button
                className={`${styles.axisBtn} ${trackerMode === 'auto' ? styles.trackerBtnActive : ''} ${pendingTracker?.id === 'auto' ? styles.trackerBtnPending : ''}`}
                onClick={() => handleTrackerMode('auto')}
              >AUTO</button>
              <button
                className={`${styles.axisBtn} ${trackerMode === 'scan' ? styles.trackerBtnActive : ''} ${pendingTracker?.id === 'scan' ? styles.trackerBtnPending : ''}`}
                onClick={() => handleTrackerMode('scan')}
              >SCAN</button>
              <button
                className={`${styles.axisBtn} ${trackerMode === 'manual' ? styles.trackerBtnActive : ''} ${pendingTracker?.id === 'manual' ? styles.trackerBtnPending : ''}`}
                onClick={() => handleTrackerMode('manual')}
              >MANUAL</button>
              <button
                className={`${styles.stopBtn} ${trackerMode === 'stop' ? styles.trackerBtnActive : ''} ${pendingTracker?.id === 'stop' ? styles.trackerBtnPending : ''}`}
                onClick={() => handleTrackerMode('stop')}
              >STOP</button>
              <button
                className={`${styles.stopBtn} ${pendingTracker?.id === 'stop_all' ? styles.trackerBtnPending : ''}`}
                onClick={handleStopMotion}
              >STOP ALL</button>
            </div>
            <div className={styles.trackerAck}>
              {pendingTracker ? (
                <>
                  <span>Waiting for Pico: {pendingTracker.label}</span>
                  <div className={styles.trackerProgress}><span /></div>
                </>
              ) : (
                <span>State: {antenna?.armed ? 'armed' : 'disarmed'} / {trackerMode || '--'}</span>
              )}
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
              <button className={styles.axisBtn} onClick={() => handleJog('el', -1)}>EL −</button>
              <button className={styles.axisBtn} onClick={() => handleJog('el',  1)}>EL +</button>
            </div>
          </div>

          {/* TRACKER CONFIG */}
          <div className={`${styles.ctrlCard} ${styles.ctrlCardFull}`}>
            <div className={styles.ctrlCardTitle}>TRACKER CONFIG</div>
            <div className={styles.ctrlInlineRow}>
              <span className={styles.cmdLabel}>YAW TRIM</span>
              <input className={styles.cmdInputSm} type="number" step="0.1" value={cfgYawTrim} onChange={e => setCfgYawTrim(e.target.value)} />
              <span className={styles.cmdLabel}>EL TRIM</span>
              <input className={styles.cmdInputSm} type="number" step="0.1" value={cfgElTrim} onChange={e => setCfgElTrim(e.target.value)} />
            </div>
            <div className={styles.ctrlInlineRow}>
              <span className={styles.cmdLabel}>AZ MIN/MAX</span>
              <input className={styles.cmdInputSm} type="number" step="1" value={cfgAzMin} onChange={e => setCfgAzMin(e.target.value)} />
              <input className={styles.cmdInputSm} type="number" step="1" value={cfgAzMax} onChange={e => setCfgAzMax(e.target.value)} />
            </div>
            <div className={styles.ctrlInlineRow}>
              <span className={styles.cmdLabel}>EL MIN/MAX</span>
              <input className={styles.cmdInputSm} type="number" step="1" value={cfgElMin} onChange={e => setCfgElMin(e.target.value)} />
              <input className={styles.cmdInputSm} type="number" step="1" value={cfgElMax} onChange={e => setCfgElMax(e.target.value)} />
            </div>
            <div className={styles.ctrlInlineRow}>
              <span className={styles.cmdLabel}>SPEED</span>
              <input className={styles.cmdInputSm} type="number" step="1" min="1" value={cfgDefaultSpeed} onChange={e => setCfgDefaultSpeed(e.target.value)} />
              <input className={styles.cmdInputSm} type="number" step="1" min="1" value={cfgMaxSpeed} onChange={e => setCfgMaxSpeed(e.target.value)} />
            </div>
            <div className={styles.ctrlInlineRow}>
              <span className={styles.cmdLabel}>SCAN AZ/EL</span>
              <input className={styles.cmdInputSm} type="number" step="1" min="0.1" value={cfgScanAz} onChange={e => setCfgScanAz(e.target.value)} />
              <input className={styles.cmdInputSm} type="number" step="1" min="0.1" value={cfgScanEl} onChange={e => setCfgScanEl(e.target.value)} />
            </div>
            <div className={styles.ctrlInlineRow}>
              <span className={styles.cmdLabel}>TIMEOUTS</span>
              <input className={styles.cmdInputSm} type="number" step="250" min="1000" value={cfgGsTimeout} onChange={e => setCfgGsTimeout(e.target.value)} />
              <input className={styles.cmdInputSm} type="number" step="250" min="250" value={cfgTargetTimeout} onChange={e => setCfgTargetTimeout(e.target.value)} />
            </div>
            <div className={styles.ctrlInlineRow}>
              <span className={styles.cmdLabel}>DIST MIN</span>
              <input className={styles.cmdInputSm} type="number" step="1" min="0" value={cfgDistanceMin} onChange={e => setCfgDistanceMin(e.target.value)} />
              <label className={styles.scrollToggle}>
                <input type="checkbox" checked={cfgScanOnLoss} onChange={e => setCfgScanOnLoss(e.target.checked)} />
                scan loss
              </label>
            </div>
            <div className={styles.ctrlInlineRow}>
              <label className={styles.scrollToggle}>
                <input type="checkbox" checked={cfgUseAhrsEl} onChange={e => setCfgUseAhrsEl(e.target.checked)} />
                AHRS EL
              </label>
              <label className={styles.scrollToggle}>
                <input type="checkbox" checked={cfgUseAhrsAz} onChange={e => setCfgUseAhrsAz(e.target.checked)} />
                AHRS AZ
              </label>
            </div>
            <div className={styles.ctrlInlineRow}>
              <span className={styles.cmdLabel}>AHRS AGE/GAIN/CORR</span>
              <input className={styles.cmdInputSm} type="number" step="10" min="20" value={cfgAhrsAge} onChange={e => setCfgAhrsAge(e.target.value)} />
              <input className={styles.cmdInputSm} type="number" step="0.05" min="0" max="1" value={cfgAhrsGain} onChange={e => setCfgAhrsGain(e.target.value)} />
              <input className={styles.cmdInputSm} type="number" step="1" min="0" value={cfgAhrsMaxCorrection} onChange={e => setCfgAhrsMaxCorrection(e.target.value)} />
            </div>
            <button className={styles.sendBtn} onClick={publishTrackerConfig}>APPLY CONFIG</button>
            <div className={styles.cmdNote}>
              Runtime config only. The Pico sanitizes values and applies them through <code>{TOPICS.TRACKER_CONFIG_CMD}</code>.
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
            <label className={styles.cmdLabel}>Elevation (deg)</label>
            <input
              className={styles.cmdInput}
              type="number" step="0.1" min="-10" max="90"
              value={el}
              onChange={e => setEl(e.target.value)}
            />
            <label className={styles.cmdLabel}>Speed (deg/s)</label>
            <input
              className={styles.cmdInput}
              type="number" step="1" min="1" max="90"
              value={speed}
              onChange={e => setSpeed(e.target.value)}
            />
            <button
              className={styles.sendBtn}
              onClick={() => setPendingDirectCommand({ axis: 'az', kind: 'stepper' })}
              disabled={!directCommandReady}
            >SEND AZ</button>
            <button
              className={styles.sendBtn}
              onClick={() => setPendingDirectCommand({ axis: 'el', kind: 'direct_el_ahrs' })}
              disabled={!directCommandReady}
            >SEND EL</button>

            <div className={styles.directSection}>
              <div className={styles.directSectionTitle}>AHRS MANUAL</div>
              <div className={styles.directReadout}>
                <span>Now</span>
                <strong>{fmtDeg(magneticAz)} / {fmtDeg(absoluteEl)}</strong>
              </div>
              <label className={styles.cmdLabel}>Reported azimuth (deg)</label>
              <input
                className={styles.cmdInput}
                type="number" step="0.1" min="0" max="360"
                value={absAz}
                onChange={e => {
                  setAbsAzEdited(true);
                  setAbsAz(e.target.value);
                }}
              />
              <label className={styles.cmdLabel}>Reported elevation (deg)</label>
              <input
                className={styles.cmdInput}
                type="number" step="0.1" min="-10" max="90"
                value={absEl}
                onChange={e => {
                  setAbsElEdited(true);
                  setAbsEl(e.target.value);
                }}
              />
              <button
                className={styles.sendBtn}
                onClick={() => setPendingDirectCommand({ axis: 'az', kind: 'absolute_ahrs' })}
                disabled={!absoluteCommandReady}
              >SEND AHRS AZ</button>
              <button
                className={styles.sendBtn}
                onClick={() => setPendingDirectCommand({ axis: 'el', kind: 'absolute_ahrs' })}
                disabled={!absoluteCommandReady}
              >SEND AHRS EL</button>
            </div>
            <div className={styles.cmdNote}>
              {directCommandReady
                ? 'Azimuth uses the calibrated/mechanical axis. Elevation uses the live IMU elevation as the target.'
                : 'Arm the tracker and switch to manual before direct motion.'}
              <br />
              <code>{TOPICS.STEPPER_AZ_CMD}</code><br />
              <code>{TOPICS.STEPPER_EL_CMD}</code>
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
                  className={`${styles.axisBtn} ${oscAxis === 'el' ? styles.axisBtnActive : ''}`}
                  onClick={() => setOscAxis('el')}
                >EL</button>
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
