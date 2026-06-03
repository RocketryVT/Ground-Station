import { useEffect, useState, useCallback } from 'react';
import { useTelemetryStore } from '../../store/telemetryStore';
import type { AppTab } from '../../App';
import styles from './StatusBar.module.css';

function formatElapsed(startMs: number): string {
  const s = Math.floor((Date.now() - startMs) / 1000);
  const m = Math.floor(s / 60);
  const ss = s % 60;
  return `T+${String(m).padStart(2, '0')}:${String(ss).padStart(2, '0')}`;
}

interface Props {
  demo:         boolean;
  tab:          AppTab;
  onToggleDemo: () => void;
  onSetTab:     (t: AppTab) => void;
}

export function StatusBar({ demo, tab, onToggleDemo, onSetTab }: Props) {
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
      <div className={styles.brand}>
        <span className={styles.title}>Ground Station</span>
        <span className={styles.subtitle}>{flightStart ? elapsed : 'Standby'}</span>
      </div>

      <div className={`${styles.linkPill} ${connected ? styles.linkPillConnected : styles.linkPillOffline}`}>
        <span className={styles.dot} />
        {connected ? 'Connected' : 'No link'}
      </div>

      <div className={styles.metrics}>
        <div className={styles.metricBlock}>
          <span className={styles.metric}>Altitude</span>
          <strong className={styles.value}>{latest ? `${latest.alt_m.toFixed(0)} m` : '--'}</strong>
        </div>
        <div className={styles.metricBlock}>
          <span className={styles.metric}>Speed</span>
          <strong className={styles.value}>{speed !== '--' ? `${speed} m/s` : '--'}</strong>
        </div>
        <div className={styles.metricBlock}>
          <span className={styles.metric}>RSSI</span>
          <strong className={styles.value}>{latest ? `${latest.rssi} dBm` : '--'}</strong>
        </div>
        <div className={styles.metricBlock}>
          <span className={styles.metric}>Antenna</span>
          <strong className={styles.value}>
            {antenna ? `${antenna.actual_az.toFixed(1)}° / ${antenna.actual_el.toFixed(1)}°` : '--'}
          </strong>
        </div>
      </div>

      <span className={styles.spacer} />

      <div className={styles.navGroup}>
        <button
          className={`${styles.tabBtn} ${tab === 'flight' ? styles.tabActive : ''}`}
          onClick={() => onSetTab('flight')}
        >
          Mission
        </button>
        <button
          className={`${styles.tabBtn} ${tab === 'debug' ? styles.tabActive : ''}`}
          onClick={() => onSetTab('debug')}
        >
          Systems
        </button>
      </div>

      <button
        className={`${styles.clearBtn} ${demo ? styles.demoActive : ''}`}
        onClick={onToggleDemo}
      >
        {demo ? 'Demo on' : 'Demo'}
      </button>
      <button className={styles.clearBtn} onClick={clearFlight}>Clear</button>
      <button className={styles.clearBtn} onClick={toggleFullscreen} title="Toggle fullscreen">
        {fullscreen ? 'Exit' : 'Full'}
      </button>
    </div>
  );
}
