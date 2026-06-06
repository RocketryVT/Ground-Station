import { useEffect, useRef, useState } from 'react';
import { TOPICS } from '../../config';
import type { MQTTHandle } from '../../hooks/useMQTT';
import type { AntennaState, GroundImuState } from '../../types/telemetry';
import styles from './CalibrationWizard.module.css';

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

type Phase =
  | 'idle'        // Not started — shows current cal status and Begin button
  | 'checks'      // Pre-flight checks: MQTT, IMU, AHRS, motors
  | 'az_ref'      // Set azimuth reference via phone compass
  | 'el_ref'      // Confirm elevation home
  | 'enabling'    // Sending enable_tracking, waiting for Pico ack
  | 'done';       // All done

interface CalEvent {
  timestamp: number;
  action?: string;
  result?: string;
  az_calibrated?: boolean;
  el_calibrated?: boolean;
  az_reference_deg?: number;
  el_reference_deg?: number;
}

interface Props {
  mqtt:               MQTTHandle;
  antenna:            AntennaState | null;
  imu:                GroundImuState | null;
  connected:          boolean;
  calibrationEvents:  CalEvent[];
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const YAW_WINDOW   = 40;   // samples to average for stability check
const STABLE_SIGMA = 1.5;  // degrees — threshold for "sensor is stable"
const IMU_STALE_MS = 2000; // ms without IMU data → stale

// ---------------------------------------------------------------------------
// Stability helpers
// ---------------------------------------------------------------------------

function circularMean(angles: number[]): number {
  const sinSum = angles.reduce((s, a) => s + Math.sin((a * Math.PI) / 180), 0);
  const cosSum = angles.reduce((s, a) => s + Math.cos((a * Math.PI) / 180), 0);
  return ((Math.atan2(sinSum / angles.length, cosSum / angles.length) * 180) / Math.PI + 360) % 360;
}

function circularStddev(angles: number[], mean: number): number {
  if (angles.length < 3) return 999;
  const ss = angles.reduce((s, a) => {
    let d = a - mean;
    while (d > 180)  d -= 360;
    while (d < -180) d += 360;
    return s + d * d;
  }, 0);
  return Math.sqrt(ss / angles.length);
}

// ---------------------------------------------------------------------------
// Sub-components
// ---------------------------------------------------------------------------

function StepIndicator({ phase }: { phase: Phase }) {
  const steps: Array<{ id: Phase | 'checks'; label: string }> = [
    { id: 'checks',   label: 'Checks'   },
    { id: 'az_ref',   label: 'AZ Ref'   },
    { id: 'el_ref',   label: 'EL Home'  },
    { id: 'enabling', label: 'Activate' },
  ];

  const currentIdx = steps.findIndex(s => s.id === phase);

  return (
    <div className={styles.stepper}>
      {steps.map((step, i) => {
        const done    = currentIdx > i;
        const active  = currentIdx === i;
        return (
          <div key={step.id} className={styles.stepperItem}>
            <div className={`${styles.stepDot}
              ${done   ? styles.stepDotDone   : ''}
              ${active ? styles.stepDotActive : ''}
            `}>
              {done ? '✓' : i + 1}
            </div>
            <span className={`${styles.stepLabel}
              ${active ? styles.stepLabelActive : ''}
              ${done   ? styles.stepLabelDone   : ''}
            `}>{step.label}</span>
            {i < steps.length - 1 && (
              <div className={`${styles.stepLine} ${done ? styles.stepLineDone : ''}`} />
            )}
          </div>
        );
      })}
    </div>
  );
}

function Check({ ok, label, detail }: { ok: boolean | null; label: string; detail?: string }) {
  const icon = ok === null ? '○' : ok ? '✓' : '✗';
  return (
    <div className={`${styles.check} ${ok === true ? styles.checkOk : ok === false ? styles.checkFail : styles.checkPending}`}>
      <span className={styles.checkIcon}>{icon}</span>
      <span className={styles.checkLabel}>{label}</span>
      {detail && <span className={styles.checkDetail}>{detail}</span>}
    </div>
  );
}

function StabilityBar({ sigma }: { sigma: number }) {
  // sigma: 0 = perfectly stable, >5 = very noisy. Bar fills left-to-right as stable.
  const fill  = Math.max(0, Math.min(100, (1 - sigma / 6) * 100));
  const color = sigma < STABLE_SIGMA ? '#4caf50' : sigma < 3 ? '#ff9800' : '#f44336';
  return (
    <div className={styles.stabilityWrap}>
      <div className={styles.stabilityBar}>
        <div className={styles.stabilityFill} style={{ width: `${fill}%`, background: color }} />
      </div>
      <span className={styles.stabilitySigma} style={{ color }}>
        σ = {sigma < 99 ? sigma.toFixed(2) : '--'}°
        {sigma < STABLE_SIGMA ? ' — STABLE' : sigma < 3 ? ' — SETTLING' : ' — UNSTABLE'}
      </span>
    </div>
  );
}

function ResultBanner({ ok, msg, onDismiss }: { ok: boolean; msg: string; onDismiss: () => void }) {
  return (
    <div className={`${styles.banner} ${ok ? styles.bannerOk : styles.bannerFail}`}>
      <span>{ok ? '✓ ' : '✗ '}{msg}</span>
      <button className={styles.bannerDismiss} onClick={onDismiss}>✕</button>
    </div>
  );
}

// ---------------------------------------------------------------------------
// Main component
// ---------------------------------------------------------------------------

export function CalibrationWizard({ mqtt, antenna, imu, connected, calibrationEvents }: Props) {
  const [phase,        setPhase]       = useState<Phase>('idle');
  const [phoneBearing, setPhoneBearing]= useState('');
  const [elHome,       setElHome]      = useState('0');
  const [result,       setResult]      = useState<{ ok: boolean; msg: string } | null>(null);
  const [awaiting,     setAwaiting]    = useState<'az' | 'el' | 'enable' | null>(null);

  // Rolling yaw window for stability
  const yawWindowRef  = useRef<number[]>([]);
  const [sigma,       setSigma]        = useState(999);
  const [yawMean,     setYawMean]      = useState<number | null>(null);

  const lastEventRef  = useRef<number>(0);

  // Accumulate yaw readings
  useEffect(() => {
    const val = imu?.yaw_frame_yaw360;
    if (val == null || !Number.isFinite(val)) return;
    const w = [...yawWindowRef.current.slice(-(YAW_WINDOW - 1)), val];
    yawWindowRef.current = w;
    if (w.length >= 3) {
      const mean = circularMean(w);
      const s    = circularStddev(w, mean);
      setSigma(s);
      setYawMean(mean);
    }
  }, [imu?.yaw_frame_yaw360]);

  // Watch calibration events for Pico acks
  useEffect(() => {
    if (!awaiting || calibrationEvents.length === 0) return;
    const latest = calibrationEvents[calibrationEvents.length - 1];
    if (latest.timestamp === lastEventRef.current) return;
    lastEventRef.current = latest.timestamp;

    if (awaiting === 'az') {
      const accepted = latest.result === 'ok' || latest.az_calibrated === true;
      const rejected = latest.result === 'error' || latest.result === 'rejected';
      if (accepted) {
        setResult({ ok: true, msg: `AZ reference set — ${latest.az_reference_deg?.toFixed(1) ?? phoneBearing}°` });
        setAwaiting(null);
        setPhase('el_ref');
      } else if (rejected) {
        setResult({ ok: false, msg: `Pico rejected AZ reference: ${latest.result}` });
        setAwaiting(null);
      }
    } else if (awaiting === 'el') {
      const accepted = latest.result === 'ok' || latest.el_calibrated === true;
      const rejected = latest.result === 'error' || latest.result === 'rejected';
      if (accepted) {
        setResult({ ok: true, msg: `EL home set — ${latest.el_reference_deg?.toFixed(1) ?? elHome}°` });
        setAwaiting(null);
        setPhase('enabling');
      } else if (rejected) {
        setResult({ ok: false, msg: `Pico rejected EL home: ${latest.result}` });
        setAwaiting(null);
      }
    } else if (awaiting === 'enable') {
      const accepted = latest.result === 'ok' || (latest.az_calibrated && latest.el_calibrated);
      const rejected = latest.result === 'error' || latest.result === 'rejected';
      if (accepted) {
        setResult({ ok: true, msg: 'Calibration complete — tracking enabled' });
        setAwaiting(null);
        setPhase('done');
      } else if (rejected) {
        setResult({ ok: false, msg: `Enable tracking failed: ${latest.result}` });
        setAwaiting(null);
      }
    }
  }, [calibrationEvents, awaiting, phoneBearing, elHome]);

  // ---------------------------------------------------------------------------
  // Pre-flight check values
  // ---------------------------------------------------------------------------

  const imuFresh   = imu != null && (Date.now() - (imu.timestamp ?? 0)) < IMU_STALE_MS;
  const ahrsValid  = imu?.valid === true && imu?.yaw_startup !== true;
  const motorsIdle = antenna != null && !antenna.az_moving && !antenna.zen_moving;
  const notFaulted = antenna != null && !antenna.az_faulted && !antenna.zen_faulted;
  const notArmed   = antenna?.armed !== true;

  const checksPass = connected && imuFresh && ahrsValid && motorsIdle && notFaulted;

  // ---------------------------------------------------------------------------
  // Phone bearing → derived values
  // ---------------------------------------------------------------------------

  const phoneDeg    = parseFloat(phoneBearing);
  const phoneBearingValid = Number.isFinite(phoneDeg) && phoneDeg >= 0 && phoneDeg < 360;
  const offset      = phoneBearingValid && yawMean != null
    ? ((phoneDeg - yawMean + 540) % 360) - 180
    : null;

  const stable = sigma < STABLE_SIGMA;
  const axesCalibrated = antenna?.az_calibrated === true && antenna?.zen_calibrated === true;

  // ---------------------------------------------------------------------------
  // Actions
  // ---------------------------------------------------------------------------

  function beginCalibration() {
    yawWindowRef.current = [];
    setSigma(999);
    setYawMean(null);
    setResult(null);
    setAwaiting(null);
    setPhase('checks');
    mqtt.publish(TOPICS.CALIBRATION_CMD, JSON.stringify({ action: 'begin_guided', note: 'wizard' }));
  }

  function captureAzReference() {
    if (!phoneBearingValid || !stable) return;
    setResult(null);
    setAwaiting('az');
    mqtt.publish(TOPICS.CALIBRATION_CMD, JSON.stringify({
      action: 'set_az_reference',
      reference_deg: phoneDeg,
      note: 'phone compass',
    }));
  }

  function captureElHome() {
    const deg = parseFloat(elHome);
    if (!Number.isFinite(deg)) return;
    setResult(null);
    setAwaiting('el');
    mqtt.publish(TOPICS.CALIBRATION_CMD, JSON.stringify({
      action: 'set_el_reference',
      reference_deg: deg,
      note: 'wizard',
    }));
  }

  function enableTracking() {
    setResult(null);
    setAwaiting('enable');
    setPhase('enabling');
    mqtt.publish(TOPICS.CALIBRATION_CMD, JSON.stringify({ action: 'enable_tracking', note: 'wizard' }));
  }

  function resetWizard() {
    setPhase('idle');
    setResult(null);
    setAwaiting(null);
    mqtt.publish(TOPICS.CALIBRATION_CMD, JSON.stringify({ action: 'clear', note: 'wizard' }));
  }

  // ---------------------------------------------------------------------------
  // Render helpers
  // ---------------------------------------------------------------------------

  const currentYaw  = imu?.yaw_frame_yaw360;
  const currentPitch = imu?.bar_rel_pitch ?? imu?.pitch;

  function fmtDeg(v: number | null | undefined) {
    return v == null || !Number.isFinite(v) ? '--' : `${v.toFixed(1)}°`;
  }

  // Live sensor card — shown on az_ref and el_ref steps
  function SensorCard() {
    return (
      <div className={styles.sensorCard}>
        <div className={styles.sensorCardTitle}>LIVE SENSOR</div>
        <div className={styles.sensorRow}>
          <span>Yaw sensor</span>
          <strong className={stable ? styles.valStable : styles.valUnstable}>
            {fmtDeg(currentYaw)}
          </strong>
        </div>
        <StabilityBar sigma={sigma} />
        <div className={styles.sensorRow}>
          <span>Tilt pitch</span>
          <strong>{fmtDeg(currentPitch)}</strong>
        </div>
        <div className={styles.sensorRow}>
          <span>Actual AZ</span>
          <strong>{fmtDeg(antenna?.actual_az)}</strong>
        </div>
        <div className={styles.sensorRow}>
          <span>AHRS</span>
          <strong>{ahrsValid ? 'VALID' : (imu?.yaw_startup ? 'STARTUP' : 'NO DATA')}</strong>
        </div>
        {antenna?.az_reference_deg != null && (
          <div className={styles.sensorRow}>
            <span>Stored AZ ref</span>
            <strong className={styles.valStable}>{fmtDeg(antenna.az_reference_deg)}</strong>
          </div>
        )}
      </div>
    );
  }

  // ---------------------------------------------------------------------------
  // Render
  // ---------------------------------------------------------------------------

  return (
    <div className={styles.root}>

      {/* ── Persistent status bar ── */}
      <div className={styles.statusRow}>
        <span className={`${styles.statusPill} ${connected ? styles.pillGreen : styles.pillRed}`}>
          {connected ? 'MQTT ●' : 'MQTT ✗'}
        </span>
        <span className={`${styles.statusPill} ${ahrsValid ? styles.pillGreen : styles.pillAmber}`}>
          {ahrsValid ? 'AHRS VALID' : 'AHRS STARTUP'}
        </span>
        <span className={`${styles.statusPill} ${antenna?.az_calibrated ? styles.pillGreen : styles.pillAmber}`}>
          AZ {antenna?.az_calibrated ? 'SET' : 'OPEN'}
        </span>
        <span className={`${styles.statusPill} ${antenna?.zen_calibrated ? styles.pillGreen : styles.pillAmber}`}>
          EL {antenna?.zen_calibrated ? 'SET' : 'OPEN'}
        </span>
        <span className={`${styles.statusPill} ${antenna?.tracking_enabled ? styles.pillGreen : styles.pillAmber}`}>
          {antenna?.tracking_enabled ? 'TRACKING' : 'TRACKING OFF'}
        </span>
        {antenna?.calibration_status && (
          <span className={styles.statusPill}>{antenna.calibration_status}</span>
        )}
      </div>

      {/* ── Result banner ── */}
      {result && (
        <ResultBanner ok={result.ok} msg={result.msg} onDismiss={() => setResult(null)} />
      )}

      {/* ── Idle ── */}
      {phase === 'idle' && (
        <div className={styles.card}>
          <div className={styles.cardTitle}>ANTENNA CALIBRATION</div>
          <div className={styles.introText}>
            <p>
              Calibration sets the current stepper positions to known azimuth and elevation
              references. If the yaw AHRS is fresh, the Pico also applies the phone-vs-sensor
              heading correction used by AHRS azimuth feedback.
            </p>
            <p>
              <strong>You will need:</strong> a phone with a compass app (or a handheld compass),
              and an unobstructed view of the antenna boresight.
            </p>
          </div>

          {antenna?.az_calibrated && (
            <div className={styles.existingCal}>
              <div className={styles.existingCalTitle}>CURRENT CALIBRATION</div>
              <div className={styles.kvRow}><span>AZ reference</span><strong>{fmtDeg(antenna?.az_reference_deg)}</strong></div>
              <div className={styles.kvRow}><span>EL reference</span><strong>{fmtDeg(antenna?.el_reference_deg)}</strong></div>
              <div className={styles.kvRow}><span>Tracking</span><strong>{antenna?.tracking_enabled ? 'ENABLED' : 'DISABLED'}</strong></div>
            </div>
          )}

          <div className={styles.actionRow}>
            <button
              className={styles.primaryBtn}
              onClick={beginCalibration}
              disabled={!connected}
              title={!connected ? 'MQTT not connected' : ''}
            >
              BEGIN CALIBRATION
            </button>
            {!connected && <span className={styles.disableReason}>Requires MQTT connection</span>}
          </div>
        </div>
      )}

      {/* ── Checks ── */}
      {phase === 'checks' && (
        <div className={styles.phaseWrap}>
          <StepIndicator phase="checks" />
          <div className={styles.card}>
            <div className={styles.cardTitle}>STEP 1 — PRE-FLIGHT CHECKS</div>
            <div className={styles.checks}>
              <Check ok={connected}     label="MQTT connected" detail={connected ? undefined : 'Check broker / network'} />
              <Check ok={imuFresh}      label="IMU receiving data" detail={imuFresh ? undefined : `No data for >${IMU_STALE_MS / 1000}s`} />
              <Check ok={ahrsValid}     label="AHRS valid (not in startup)" detail={imu?.yaw_startup ? 'Wait ~5s for gyro settle' : undefined} />
              <Check ok={motorsIdle}    label="Motors not moving" detail={!motorsIdle ? 'Wait for motion to stop' : undefined} />
              <Check ok={notFaulted}    label="No motor faults" detail={!notFaulted ? 'Check fault indicators' : undefined} />
              <Check ok={notArmed}      label="Tracker disarmed" detail={!notArmed ? 'Disarm before calibrating' : undefined} />
            </div>
            <div className={styles.actionRow}>
              <button
                className={styles.primaryBtn}
                disabled={!checksPass}
                onClick={() => setPhase('az_ref')}
              >
                PROCEED TO AZ REFERENCE
              </button>
              {!checksPass && (
                <span className={styles.disableReason}>All checks must pass</span>
              )}
              <button className={styles.secondaryBtn} onClick={() => setPhase('idle')}>CANCEL</button>
            </div>
          </div>
        </div>
      )}

      {/* ── AZ Reference ── */}
      {phase === 'az_ref' && (
        <div className={styles.phaseWrap}>
          <StepIndicator phase="az_ref" />
          <div className={styles.twoCol}>

            <div className={styles.card}>
              <div className={styles.cardTitle}>STEP 2 — AZIMUTH REFERENCE</div>
              <ol className={styles.instructions}>
                <li>Point the antenna boresight at a distant landmark or known bearing.</li>
                <li>Stand directly behind the antenna, aligned with the boresight.</li>
                <li>Open your phone's <strong>Compass</strong> app. Hold the phone flat and level,
                    parallel to the antenna's pointing direction.</li>
                <li>Read the compass bearing (0–360°, true or magnetic — be consistent).</li>
                <li>Enter that bearing below and wait for the yaw sensor to stabilise.</li>
              </ol>

              <div className={styles.inputGroup}>
                <label className={styles.inputLabel}>
                  PHONE COMPASS BEARING
                </label>
                <div className={styles.inputRow}>
                  <input
                    className={styles.bearingInput}
                    type="number"
                    min="0"
                    max="359.9"
                    step="0.1"
                    placeholder="e.g. 165.0"
                    value={phoneBearing}
                    onChange={e => setPhoneBearing(e.target.value)}
                  />
                  <span className={styles.degLabel}>°</span>
                </div>
              </div>

              {phoneBearingValid && yawMean != null && (
                <div className={styles.offsetBox}>
                  <div className={styles.offsetRow}>
                    <span>Sensor reads</span>
                    <strong>{fmtDeg(yawMean)}</strong>
                  </div>
                  <div className={styles.offsetRow}>
                    <span>Phone says</span>
                    <strong>{phoneDeg.toFixed(1)}°</strong>
                  </div>
                  <div className={styles.offsetRow}>
                    <span>Applied yaw correction</span>
                    <strong className={styles.offsetVal}>
                      {offset != null ? `${offset >= 0 ? '+' : ''}${offset.toFixed(1)}°` : '--'}
                    </strong>
                  </div>
                </div>
              )}

              <div className={styles.actionRow}>
                <button
                  className={styles.primaryBtn}
                  disabled={!phoneBearingValid || !stable || awaiting === 'az'}
                  onClick={captureAzReference}
                  title={
                    !phoneBearingValid ? 'Enter a bearing from your phone compass' :
                    !stable ? `Wait for sensor to stabilise (σ = ${sigma.toFixed(2)}°, need < ${STABLE_SIGMA}°)` :
                    awaiting === 'az' ? 'Waiting for Pico confirmation…' : ''
                  }
                >
                  {awaiting === 'az' ? 'WAITING FOR PICO…' : 'CAPTURE REFERENCE'}
                </button>
                {!phoneBearingValid && <span className={styles.disableReason}>Enter phone compass bearing above</span>}
                {phoneBearingValid && !stable && (
                  <span className={styles.disableReason}>
                    Sensor not yet stable — σ = {sigma.toFixed(2)}° (need &lt; {STABLE_SIGMA}°)
                  </span>
                )}
                <button className={styles.secondaryBtn} onClick={() => setPhase('checks')}>← BACK</button>
              </div>
            </div>

            <SensorCard />
          </div>
        </div>
      )}

      {/* ── EL Home ── */}
      {phase === 'el_ref' && (
        <div className={styles.phaseWrap}>
          <StepIndicator phase="el_ref" />
          <div className={styles.twoCol}>

            <div className={styles.card}>
              <div className={styles.cardTitle}>STEP 3 — ELEVATION HOME</div>
              <ol className={styles.instructions}>
                <li>Move the elevation axis to your desired <strong>home position</strong>
                    (usually 0° = level/horizon, or your physical hard stop).</li>
                <li>Enter the true elevation angle for that home position below.</li>
                <li>Click <strong>SET EL HOME</strong>.</li>
              </ol>

              <div className={styles.inputGroup}>
                <label className={styles.inputLabel}>HOME ELEVATION ANGLE</label>
                <div className={styles.inputRow}>
                  <input
                    className={styles.bearingInput}
                    type="number"
                    min="-90"
                    max="90"
                    step="0.1"
                    value={elHome}
                    onChange={e => setElHome(e.target.value)}
                  />
                  <span className={styles.degLabel}>°</span>
                </div>
                <div className={styles.inputHint}>0° = level with horizon.  Positive = above horizon.</div>
              </div>

              <div className={styles.actionRow}>
                <button
                  className={styles.primaryBtn}
                  disabled={awaiting === 'el'}
                  onClick={captureElHome}
                >
                  {awaiting === 'el' ? 'WAITING FOR PICO…' : 'SET EL HOME'}
                </button>
                <button className={styles.secondaryBtn} onClick={() => setPhase('az_ref')}>← BACK</button>
              </div>
            </div>

            <SensorCard />
          </div>
        </div>
      )}

      {/* ── Done ── */}
      {phase === 'done' && (
        <div className={styles.phaseWrap}>
          <StepIndicator phase="done" />
          <div className={styles.card}>
            <div className={styles.cardTitle}>CALIBRATION COMPLETE</div>
            <div className={styles.doneBlock}>
              <div className={styles.doneIcon}>✓</div>
              <p>The tracker is calibrated and tracking is enabled.</p>
              <p>
                Switch to <strong>AUTO</strong> mode and arm the tracker to begin antenna
                pointing. Use <strong>SCAN</strong> mode if the rocket GPS is not yet
                available.
              </p>
            </div>
            <div className={styles.summary}>
              <div className={styles.summaryRow}>
                <span>AZ reference</span>
                <strong>{fmtDeg(antenna?.az_reference_deg)}</strong>
              </div>
              <div className={styles.summaryRow}>
                <span>EL reference</span>
                <strong>{fmtDeg(antenna?.el_reference_deg)}</strong>
              </div>
              <div className={styles.summaryRow}>
                <span>Tracking</span>
                <strong>{antenna?.tracking_enabled ? 'ENABLED' : 'DISABLED'}</strong>
              </div>
            </div>
            <div className={styles.actionRow}>
              <button className={styles.secondaryBtn} onClick={resetWizard}>
                RECALIBRATE
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Enabling ── */}
      {phase === 'enabling' && (
        <div className={styles.phaseWrap}>
          <StepIndicator phase="enabling" />
          <div className={styles.card}>
            <div className={styles.cardTitle}>STEP 4 — ACTIVATE TRACKING</div>
            <div className={styles.summary}>
              <div className={styles.summaryRow}>
                <span>AZ reference</span>
                <strong>{fmtDeg(antenna?.az_reference_deg)}</strong>
                <span className={`${styles.badge} ${antenna?.az_calibrated ? styles.badgeGreen : styles.badgeAmber}`}>
                  {antenna?.az_calibrated ? 'SET' : 'WAITING'}
                </span>
              </div>
              <div className={styles.summaryRow}>
                <span>EL reference</span>
                <strong>{fmtDeg(antenna?.el_reference_deg)}</strong>
                <span className={`${styles.badge} ${antenna?.zen_calibrated ? styles.badgeGreen : styles.badgeAmber}`}>
                  {antenna?.zen_calibrated ? 'SET' : 'WAITING'}
                </span>
              </div>
            </div>
            <div className={styles.actionRow}>
              <button
                className={styles.primaryBtn}
                disabled={!axesCalibrated || awaiting === 'enable'}
                onClick={enableTracking}
                title={!axesCalibrated ? 'Waiting for Pico to publish both calibrated axis states' : ''}
              >
                {awaiting === 'enable' ? 'ACTIVATING…' : 'ACTIVATE TRACKER'}
              </button>
              {!axesCalibrated && (
                <span className={styles.disableReason}>Waiting for both axis references</span>
              )}
            </div>
          </div>
        </div>
      )}

    </div>
  );
}
