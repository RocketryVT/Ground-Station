import { useCallback, useEffect, useRef, useState } from 'react';
import { API_BASE_URL } from '../../config';
import styles from './SimTab.module.css';

// ── Types ─────────────────────────────────────────────────────────────────────

interface FlightEvent { t: number; name: string; }

interface UploadResult {
  valid:    boolean;
  missing:  string[];
  found:    string[];
  hint?:    string;
  preview?: {
    row_count:    number;
    duration_s:   number;
    launch_lat:   number;
    launch_lon:   number;
    apogee_agl_m: number;
    apogee_asl_m: number;
    events:       FlightEvent[];
  };
}

interface SimStatus {
  status:  'idle' | 'running' | 'done' | 'error';
  t:       number;
  total_t: number;
  state:   string;
  n_sent:  number;
  error:   string | null;
  has_csv: boolean;
}

type Phase = 'drop' | 'uploading' | 'invalid' | 'ready' | 'running' | 'done' | 'error';

const REQUIRED_COLS = [
  'Time (s)',
  'Altitude above sea level (m)',
  'Total velocity (m/s)',
  'Latitude (° N)',
  'Longitude (° E)',
  'Roll rate (°/s)',
  'Pitch rate (°/s)',
  'Yaw rate (°/s)',
];

// Key flight events to highlight in the preview
const NOTABLE_EVENTS = ['LIFTOFF', 'BURNOUT', 'APOGEE', 'GROUND_HIT'];

// ── Helpers ───────────────────────────────────────────────────────────────────

function fmtDuration(s: number): string {
  const m = Math.floor(s / 60);
  const sec = (s % 60).toFixed(1);
  return m > 0 ? `${m}m ${sec}s` : `${sec}s`;
}

function fmtCoord(v: number, axis: 'lat' | 'lon'): string {
  const abs  = Math.abs(v).toFixed(5);
  const dir  = axis === 'lat' ? (v >= 0 ? 'N' : 'S') : (v >= 0 ? 'E' : 'W');
  return `${abs}° ${dir}`;
}

// ── Sub-components ────────────────────────────────────────────────────────────

function DropZone({ onFile, dragging, onDragOver, onDragLeave, onDrop }: {
  onFile:      (f: File) => void;
  dragging:    boolean;
  onDragOver:  (e: React.DragEvent) => void;
  onDragLeave: (e: React.DragEvent) => void;
  onDrop:      (e: React.DragEvent) => void;
}) {
  const inputRef = useRef<HTMLInputElement>(null);
  return (
    <div
      className={`${styles.dropZone} ${dragging ? styles.dropZoneActive : ''}`}
      onDragOver={onDragOver}
      onDragLeave={onDragLeave}
      onDrop={onDrop}
      onClick={() => inputRef.current?.click()}
    >
      <div className={styles.dropIcon}>⬇</div>
      <div className={styles.dropTitle}>DROP OPENROCKET CSV HERE</div>
      <div className={styles.dropSub}>or click to browse</div>
      <input
        ref={inputRef}
        type="file"
        accept=".csv,text/csv"
        style={{ display: 'none' }}
        onChange={e => { const f = e.target.files?.[0]; if (f) onFile(f); }}
      />
    </div>
  );
}

function ValidationResult({ result, onReset }: { result: UploadResult; onReset: () => void }) {
  return (
    <div className={styles.validationBox}>
      <div className={styles.validationHeader}>
        <span className={result.valid ? styles.validBadge : styles.invalidBadge}>
          {result.valid ? '✓ CSV VALID' : '✗ MISSING COLUMNS'}
        </span>
        <button className={styles.resetBtn} onClick={onReset}>CHANGE FILE</button>
      </div>

      <div className={styles.colList}>
        {REQUIRED_COLS.map(col => {
          const found = (result.found ?? []).some(f => f.toLowerCase().includes(col.split(' (')[0].toLowerCase()));
          return (
            <div key={col} className={`${styles.colRow} ${found ? styles.colOk : styles.colMissing}`}>
              <span className={styles.colIcon}>{found ? '✓' : '✗'}</span>
              <span className={styles.colName}>{col}</span>
            </div>
          );
        })}
      </div>

      {!result.valid && result.hint && (
        <div className={styles.hint}>{result.hint}</div>
      )}
    </div>
  );
}

function FlightPreview({ preview }: { preview: NonNullable<UploadResult['preview']> }) {
  const notable = preview.events.filter(e => NOTABLE_EVENTS.includes(e.name));
  return (
    <div className={styles.preview}>
      <div className={styles.previewTitle}>FLIGHT SUMMARY</div>
      <div className={styles.previewGrid}>
        <span className={styles.previewLabel}>Rows</span>
        <span className={styles.previewValue}>{preview.row_count.toLocaleString()}</span>
        <span className={styles.previewLabel}>Duration</span>
        <span className={styles.previewValue}>{fmtDuration(preview.duration_s)}</span>
        <span className={styles.previewLabel}>Apogee (AGL)</span>
        <span className={styles.previewValue}>{preview.apogee_agl_m.toFixed(0)} m</span>
        <span className={styles.previewLabel}>Apogee (ASL)</span>
        <span className={styles.previewValue}>{preview.apogee_asl_m.toFixed(0)} m</span>
        <span className={styles.previewLabel}>Launch lat</span>
        <span className={styles.previewValue}>{fmtCoord(preview.launch_lat, 'lat')}</span>
        <span className={styles.previewLabel}>Launch lon</span>
        <span className={styles.previewValue}>{fmtCoord(preview.launch_lon, 'lon')}</span>
      </div>
      {notable.length > 0 && (
        <>
          <div className={styles.eventsTitle}>EVENTS</div>
          <div className={styles.eventsList}>
            {notable.map((ev, i) => (
              <div key={i} className={styles.eventRow}>
                <span className={styles.eventName}>{ev.name}</span>
                <span className={styles.eventT}>t = {ev.t.toFixed(2)} s</span>
              </div>
            ))}
          </div>
        </>
      )}
    </div>
  );
}

function ProgressBar({ t, total_t }: { t: number; total_t: number }) {
  const pct = total_t > 0 ? Math.min(100, (t / total_t) * 100) : 0;
  return (
    <div className={styles.progressTrack}>
      <div className={styles.progressFill} style={{ width: `${pct}%` }} />
    </div>
  );
}

// ── Main SimTab ───────────────────────────────────────────────────────────────

export function SimTab() {
  const [phase,        setPhase]        = useState<Phase>('drop');
  const [dragging,     setDragging]     = useState(false);
  const [uploadResult, setUploadResult] = useState<UploadResult | null>(null);
  const [simStatus,    setSimStatus]    = useState<SimStatus | null>(null);
  const [picoIp,       setPicoIp]       = useState('');
  const [port,         setPort]         = useState('5005');
  const [speed,        setSpeed]        = useState('1.0');
  const [loopMode,     setLoopMode]     = useState(false);
  const [apiError,     setApiError]     = useState<string | null>(null);

  // Poll /sim/status while running
  useEffect(() => {
    if (phase !== 'running') return;
    const id = setInterval(async () => {
      try {
        const res  = await fetch(`${API_BASE_URL}/sim/status`);
        const json = await res.json() as SimStatus;
        setSimStatus(json);
        if (json.status === 'done')  setPhase('done');
        if (json.status === 'idle')  setPhase('ready');
        if (json.status === 'error') setPhase('error');
      } catch {
        // network blip — keep polling
      }
    }, 400);
    return () => clearInterval(id);
  }, [phase]);

  const handleFile = useCallback(async (file: File) => {
    if (!file.name.endsWith('.csv') && file.type !== 'text/csv') {
      setApiError('File must be a .csv export from OpenRocket.');
      return;
    }
    setPhase('uploading');
    setApiError(null);
    try {
      const fd = new FormData();
      fd.append('file', file);
      const res  = await fetch(`${API_BASE_URL}/sim/upload`, { method: 'POST', body: fd });
      const json = await res.json() as UploadResult;
      setUploadResult(json);
      setPhase(json.valid ? 'ready' : 'invalid');
    } catch (e) {
      setApiError(e instanceof Error ? e.message : 'Upload failed');
      setPhase('drop');
    }
  }, []);

  function onDragOver(e: React.DragEvent) {
    e.preventDefault();
    setDragging(true);
  }

  function onDragLeave(e: React.DragEvent) {
    e.preventDefault();
    setDragging(false);
  }

  function onDrop(e: React.DragEvent) {
    e.preventDefault();
    setDragging(false);
    const f = e.dataTransfer.files[0];
    if (f) handleFile(f);
  }

  async function handleStart() {
    setApiError(null);
    const speedVal = parseFloat(speed);
    const portVal  = parseInt(port, 10);
    if (!picoIp.trim()) { setApiError('Enter the Pico IP address.'); return; }
    if (isNaN(speedVal) || speedVal <= 0) { setApiError('Speed must be > 0.'); return; }

    try {
      const res = await fetch(`${API_BASE_URL}/sim/start`, {
        method:  'POST',
        headers: { 'Content-Type': 'application/json' },
        body:    JSON.stringify({ pico_ip: picoIp.trim(), port: portVal, speed: speedVal, loop: loopMode }),
      });
      const json = await res.json();
      if (!json.ok) { setApiError(json.detail ?? 'Start failed'); return; }
      setSimStatus(null);
      setPhase('running');
    } catch (e) {
      setApiError(e instanceof Error ? e.message : 'Start failed');
    }
  }

  async function handleStop() {
    await fetch(`${API_BASE_URL}/sim/stop`, { method: 'POST' });
    setPhase('ready');
  }

  function handleReset() {
    setPhase('drop');
    setUploadResult(null);
    setSimStatus(null);
    setApiError(null);
  }

  // ── Render ──────────────────────────────────────────────────────────────────

  const isRunning = phase === 'running';

  return (
    <div className={styles.root}>

      {/* ---- Drop / uploading zone ---- */}
      {(phase === 'drop' || phase === 'uploading') && (
        <div className={styles.section}>
          {phase === 'uploading' ? (
            <div className={styles.uploading}>PARSING CSV…</div>
          ) : (
            <DropZone
              onFile={handleFile}
              dragging={dragging}
              onDragOver={onDragOver}
              onDragLeave={onDragLeave}
              onDrop={onDrop}
            />
          )}
          <div className={styles.colHint}>
            <div className={styles.colHintTitle}>REQUIRED OPENROCKET COLUMNS</div>
            {REQUIRED_COLS.map(c => <div key={c} className={styles.colHintRow}>{c}</div>)}
          </div>
        </div>
      )}

      {/* ---- Validation result (valid or invalid) ---- */}
      {(phase === 'invalid' || phase === 'ready' || isRunning || phase === 'done' || phase === 'error') && uploadResult && (
        <ValidationResult result={uploadResult} onReset={handleReset} />
      )}

      {/* ---- Flight preview ---- */}
      {(phase === 'ready' || isRunning || phase === 'done') && uploadResult?.preview && (
        <FlightPreview preview={uploadResult.preview} />
      )}

      {/* ---- Settings ---- */}
      {(phase === 'ready' || phase === 'done') && (
        <div className={styles.settings}>
          <div className={styles.settingsTitle}>REPLAY SETTINGS</div>

          <label className={styles.settingLabel}>Primary Pico IP</label>
          <input
            className={styles.settingInput}
            type="text"
            placeholder="192.168.x.x"
            value={picoIp}
            onChange={e => setPicoIp(e.target.value)}
          />

          <label className={styles.settingLabel}>UDP Port</label>
          <input
            className={styles.settingInput}
            type="number"
            value={port}
            onChange={e => setPort(e.target.value)}
          />

          <label className={styles.settingLabel}>Speed multiplier</label>
          <div className={styles.speedRow}>
            {['0.5', '1.0', '2.0', '5.0', '10.0'].map(s => (
              <button
                key={s}
                className={`${styles.speedBtn} ${speed === s ? styles.speedBtnActive : ''}`}
                onClick={() => setSpeed(s)}
              >
                {s}×
              </button>
            ))}
            <input
              className={styles.speedCustom}
              type="number"
              step="0.1"
              min="0.1"
              value={speed}
              onChange={e => setSpeed(e.target.value)}
              title="Custom speed"
            />
          </div>

          <label className={styles.loopLabel}>
            <input
              type="checkbox"
              checked={loopMode}
              onChange={e => setLoopMode(e.target.checked)}
            />
            &nbsp;Loop replay
          </label>

          <button className={styles.startBtn} onClick={handleStart}>
            ▶ START REPLAY
          </button>
        </div>
      )}

      {/* ---- Running progress ---- */}
      {isRunning && simStatus && (
        <div className={styles.progress}>
          <div className={styles.progressHeader}>
            <span className={styles.progressState}>{simStatus.state}</span>
            <span className={styles.progressTime}>
              {simStatus.t.toFixed(1)}s / {simStatus.total_t.toFixed(1)}s
            </span>
            <span className={styles.progressPkts}>{simStatus.n_sent.toLocaleString()} pkts</span>
          </div>
          <ProgressBar t={simStatus.t} total_t={simStatus.total_t} />
          <button className={styles.stopBtn} onClick={handleStop}>■ STOP</button>
        </div>
      )}

      {isRunning && !simStatus && (
        <div className={styles.progress}>
          <div className={styles.progressHeader}>
            <span className={styles.progressState}>STARTING…</span>
          </div>
          <ProgressBar t={0} total_t={1} />
          <button className={styles.stopBtn} onClick={handleStop}>■ STOP</button>
        </div>
      )}

      {/* ---- Done banner ---- */}
      {phase === 'done' && (
        <div className={styles.doneBanner}>
          REPLAY COMPLETE — {simStatus?.n_sent.toLocaleString() ?? '?'} packets sent
        </div>
      )}

      {/* ---- Error ---- */}
      {apiError && (
        <div className={styles.errorBox}>{apiError}</div>
      )}
      {phase === 'error' && simStatus?.error && (
        <div className={styles.errorBox}>REPLAY ERROR: {simStatus.error}</div>
      )}
    </div>
  );
}
