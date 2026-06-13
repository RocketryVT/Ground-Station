import { useCallback, useEffect, useRef, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import styles from './GpsTab.module.css';

interface SerialPortInfo {
  name:          string;
  kind:          string;
  product?:      string | null;
  manufacturer?: string | null;
  usb:           boolean;
}

interface GpsStatus {
  connected:    boolean;
  port?:        string | null;
  fix:          string;
  satellites:   number;
  lat?:         number | null;
  lon?:         number | null;
  alt_m?:       number | null;
  last_fix_ms?: number | null;
  error?:       string | null;
}

const BAUDS = [4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
const EMPTY: GpsStatus = { connected: false, fix: 'no-fix', satellites: 0 };

function portLabel(p: SerialPortInfo): string {
  const desc = p.product ?? p.manufacturer;
  return desc ? `${p.name} — ${desc}` : p.name;
}

export function GpsTab() {
  const [ports, setPorts]   = useState<SerialPortInfo[]>([]);
  const [selected, setSelected] = useState<string>('');
  const [baud, setBaud]     = useState(38400);
  const [status, setStatus] = useState<GpsStatus>(EMPTY);
  const [busy, setBusy]     = useState(false);
  const [error, setError]   = useState<string | null>(null);
  const selectedRef = useRef(selected);
  selectedRef.current = selected;

  const refreshPorts = useCallback(async () => {
    try {
      const list = await invoke<SerialPortInfo[]>('gps_list_ports');
      setPorts(list);
      // Default to the first USB port if nothing valid is selected yet.
      if (!list.some((p) => p.name === selectedRef.current)) {
        setSelected((list.find((p) => p.usb) ?? list[0])?.name ?? '');
      }
    } catch (e) {
      setError(String(e));
    }
  }, []);

  useEffect(() => {
    refreshPorts();
    const unlisten = listen<GpsStatus>('gps://status', (event) => setStatus(event.payload));
    return () => { unlisten.then((fn) => fn()); };
  }, [refreshPorts]);

  const connect = useCallback(async () => {
    if (!selected) return;
    setBusy(true);
    setError(null);
    try {
      await invoke('gps_connect', { port: selected, baud });
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }, [selected, baud]);

  const disconnect = useCallback(async () => {
    setBusy(true);
    try {
      await invoke('gps_disconnect');
      setStatus(EMPTY);
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }, []);

  const fix3d = status.fix === '3d';
  const fix2d = status.fix === '2d';
  const dotClass = status.connected
    ? (fix3d || fix2d ? styles.dotOk : styles.dotWarn)
    : styles.dotOff;

  return (
    <div className={styles.root}>
      <div className={styles.intro}>
        Select a USB u-blox receiver to provide the ground-station position to the
        antenna tracker. Each fix is republished to <code>gs/location</code>.
      </div>

      <div className={styles.controls}>
        <label className={styles.field}>
          <span>Serial port</span>
          <div className={styles.row}>
            <select
              value={selected}
              onChange={(e) => setSelected(e.target.value)}
              disabled={status.connected}
            >
              {ports.length === 0 && <option value="">No ports found</option>}
              {ports.map((p) => (
                <option key={p.name} value={p.name}>
                  {portLabel(p)}{p.usb ? '  (USB)' : ''}
                </option>
              ))}
            </select>
            <button className={styles.ghost} onClick={refreshPorts} disabled={status.connected}>
              Refresh
            </button>
          </div>
        </label>

        <label className={styles.field}>
          <span>Baud</span>
          <select
            value={baud}
            onChange={(e) => setBaud(Number(e.target.value))}
            disabled={status.connected}
          >
            {BAUDS.map((b) => <option key={b} value={b}>{b}</option>)}
          </select>
        </label>

        {status.connected ? (
          <button className={styles.danger} onClick={disconnect} disabled={busy}>
            Disconnect
          </button>
        ) : (
          <button className={styles.primary} onClick={connect} disabled={busy || !selected}>
            Connect
          </button>
        )}
      </div>

      <div className={styles.statusCard}>
        <div className={styles.statusHead}>
          <span className={dotClass} />
          <strong>{status.connected ? (status.port ?? 'GPS') : 'Disconnected'}</strong>
          <span className={styles.fixBadge}>{status.fix.toUpperCase()}</span>
        </div>
        <div className={styles.grid}>
          <div><span>Satellites</span><b>{status.satellites}</b></div>
          <div><span>Latitude</span><b>{status.lat != null ? status.lat.toFixed(7) : '--'}</b></div>
          <div><span>Longitude</span><b>{status.lon != null ? status.lon.toFixed(7) : '--'}</b></div>
          <div><span>Altitude</span><b>{status.alt_m != null ? `${status.alt_m.toFixed(1)} m` : '--'}</b></div>
        </div>
      </div>

      {(error ?? status.error) && (
        <div className={styles.error}>{error ?? status.error}</div>
      )}
    </div>
  );
}
