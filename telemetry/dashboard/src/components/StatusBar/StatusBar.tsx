import { useEffect, useState, useCallback } from 'react';
import { useTelemetryStore } from '../../store/telemetryStore';
import styles from './StatusBar.module.css';

function formatElapsed(startMs: number): string {
  const s = Math.floor((Date.now() - startMs) / 1000);
  const m = Math.floor(s / 60);
  const ss = s % 60;
  return `T+${String(m).padStart(2, '0')}:${String(ss).padStart(2, '0')}`;
}

interface Props {
  demo: boolean;
  onToggleDemo: () => void;
}

export function StatusBar({ demo, onToggleDemo }: Props) {
  const { latest, antenna, connected, flightStart, clearFlight } = useTelemetryStore();
  const [elapsed, setElapsed] = useState('T+00:00');
  const [fullscreen, setFullscreen] = useState(false);

  useEffect(() => {
    const onFsChange = () => setFullscreen(!!document.fullscreenElement);
    document.addEventListener('fullscreenchange', onFsChange);
    return () => document.removeEventListener('fullscreenchange', onFsChange);
  }, []);

  const toggleFullscreen = useCallback(() => {
    if (!document.fullscreenElement) {
      document.documentElement.requestFullscreen();
    } else {
      document.exitFullscreen();
    }
  }, []);

  useEffect(() => {
    if (!flightStart) return;
    const id = setInterval(() => setElapsed(formatElapsed(flightStart)), 1000);
    return () => clearInterval(id);
  }, [flightStart]);

  const speed = latest
    ? Math.sqrt(latest.vel_n ** 2 + latest.vel_e ** 2 + latest.vel_d ** 2).toFixed(1)
    : '--';

  return (
    <div className={styles.bar}>
      <span className={styles.title}>GROUND STATION</span>

      <div className={`${styles.dot} ${connected ? styles.green : styles.red}`} />
      <span className={styles.label}>{connected ? 'CONNECTED' : 'NO LINK'}</span>

      <span className={styles.sep} />
      <span className={styles.timer}>{flightStart ? elapsed : 'STANDBY'}</span>

      <span className={styles.sep} />
      <span className={styles.metric}>ALT</span>
      <span className={styles.value}>{latest ? `${latest.alt_m.toFixed(0)} m` : '--'}</span>

      <span className={styles.sep} />
      <span className={styles.metric}>SPD</span>
      <span className={styles.value}>{speed !== '--' ? `${speed} m/s` : '--'}</span>

      <span className={styles.sep} />
      <span className={styles.metric}>RSSI</span>
      <span className={styles.value}>{latest ? `${latest.rssi} dBm` : '--'}</span>

      <span className={styles.sep} />
      <span className={styles.metric}>AZ</span>
      <span className={styles.value}>{antenna ? `${antenna.actual_az.toFixed(1)}°` : '--'}</span>

      <span className={styles.metric}>EL</span>
      <span className={styles.value}>{antenna ? `${antenna.actual_el.toFixed(1)}°` : '--'}</span>

      <span className={styles.spacer} />
      <button
        className={`${styles.clearBtn} ${demo ? styles.demoActive : ''}`}
        onClick={onToggleDemo}
      >
        {demo ? 'DEMO ON' : 'DEMO'}
      </button>
      <button className={styles.clearBtn} onClick={clearFlight}>CLEAR</button>
      <button className={styles.clearBtn} onClick={toggleFullscreen} title="Toggle fullscreen">
        {fullscreen ? '[ ]' : '[+]'}
      </button>
    </div>
  );
}
