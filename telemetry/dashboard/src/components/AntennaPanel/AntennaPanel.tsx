import { useTelemetryStore } from '../../store/telemetryStore';
import styles from './AntennaPanel.module.css';

// ── Compass rose — needle points to azimuth ───────────────────────────────────
function Compass({ az }: { az: number }) {
  const cardinals = ['N', 'E', 'S', 'W'];
  const r = 38; // outer radius (svg units)

  return (
    <svg viewBox="-50 -50 100 100" className={styles.compass}>
      {/* Outer ring */}
      <circle cx={0} cy={0} r={46} stroke="#D7D2CB" strokeWidth={1.5} fill="none" />
      <circle cx={0} cy={0} r={42} stroke="#E5E1E6" strokeWidth={0.5} fill="none" />

      {/* Tick marks every 30° */}
      {Array.from({ length: 12 }, (_, i) => {
        const rad = (i * 30 * Math.PI) / 180;
        const inner = i % 3 === 0 ? r - 6 : r - 3;
        return (
          <line
            key={i}
            x1={Math.sin(rad) * inner}
            y1={-Math.cos(rad) * inner}
            x2={Math.sin(rad) * r}
            y2={-Math.cos(rad) * r}
            stroke={i % 3 === 0 ? '#75787B' : '#D7D2CB'}
            strokeWidth={i % 3 === 0 ? 1 : 0.5}
          />
        );
      })}

      {/* Cardinal labels */}
      {cardinals.map((c, i) => {
        const rad = (i * 90 * Math.PI) / 180;
        return (
          <text
            key={c}
            x={Math.sin(rad) * 26}
            y={-Math.cos(rad) * 26 + 3.5}
            textAnchor="middle"
            fontSize={8}
            fill={c === 'N' ? '#861F41' : '#75787B'}
            fontFamily="'Courier New', monospace"
          >
            {c}
          </text>
        );
      })}

      {/* Azimuth needle */}
      <g transform={`rotate(${az})`}>
        <polygon points="0,-34 3,6 0,2 -3,6" fill="#861F41" opacity={0.9} />
        <polygon points="0,34 3,6 0,2 -3,6"  fill="#D7D2CB" opacity={0.7} />
      </g>

      {/* Centre dot */}
      <circle cx={0} cy={0} r={2.5} fill="#861F41" opacity={0.8} />

      {/* Azimuth readout */}
      <text x={0} y={48} textAnchor="middle" fontSize={7} fill="#75787B" fontFamily="'Courier New', monospace">
        {az.toFixed(1)}°
      </text>
    </svg>
  );
}

// ── Elevation arc — filled arc shows zenith angle ────────────────────────────
function ElevationArc({ el }: { el: number }) {
  // Arc from 0° (horizon) to el° on a 180° semicircle.
  const clamp = Math.max(0, Math.min(90, el));
  const toRad = (d: number) => (d * Math.PI) / 180;

  // Start at horizon left (-180°) draw to el on the right side
  const startX = -40;
  const startY = 0;
  const endRad  = toRad(90 - clamp); // map el: 0→90°right, 90→top
  const endX = Math.cos(endRad) * 40;
  const endY = -Math.sin(endRad) * 40;
  const largeArc = clamp > 90 ? 1 : 0;

  return (
    <svg viewBox="-50 -50 100 55" className={styles.elevation}>
      {/* Background semicircle */}
      <path d="M -40 0 A 40 40 0 0 1 40 0" stroke="#D7D2CB" strokeWidth={4} fill="none" />
      {/* Filled arc */}
      {clamp > 0 && (
        <path
          d={`M ${startX} ${startY} A 40 40 0 ${largeArc} 1 ${endX.toFixed(2)} ${endY.toFixed(2)}`}
          stroke="#861F41"
          strokeWidth={4}
          fill="none"
          opacity={0.8}
        />
      )}
      {/* Horizon labels */}
      <text x={-44} y={8} textAnchor="middle" fontSize={7} fill="#75787B" fontFamily="'Courier New', monospace">0°</text>
      <text x={0}   y={-42} textAnchor="middle" fontSize={7} fill="#75787B" fontFamily="'Courier New', monospace">90°</text>
      <text x={44}  y={8} textAnchor="middle" fontSize={7} fill="#75787B" fontFamily="'Courier New', monospace">0°</text>
      {/* Value */}
      <text x={0} y={10} textAnchor="middle" fontSize={9} fill="#1a1a1a" fontFamily="'Courier New', monospace">
        {el.toFixed(1)}°
      </text>
    </svg>
  );
}

export function AntennaPanel() {
  const antenna = useTelemetryStore((s) => s.antenna);
  const latest  = useTelemetryStore((s) => s.latest);

  const az = antenna?.actual_az ?? 0;
  const el = antenna?.actual_el  ?? 0;

  return (
    <div className={styles.wrapper}>
      <div className={styles.panelLabel}>ANTENNA TRACKER</div>

      <div className={styles.gauges}>
        <div className={styles.gaugeBlock}>
          <span className={styles.gaugeTitle}>AZIMUTH</span>
          <Compass az={az} />
        </div>
        <div className={styles.gaugeBlock}>
          <span className={styles.gaugeTitle}>ELEVATION</span>
          <ElevationArc el={el} />
          <div className={styles.targetRow}>
            <span className={styles.dim}>TGT</span>
            <span>{antenna?.target_az?.toFixed(1) ?? '--'}°</span>
            <span>{antenna?.target_el?.toFixed(1) ?? '--'}°</span>
          </div>
        </div>
      </div>

      {latest && (
        <div className={styles.stats}>
          <div className={styles.stat}><span>SNR</span><span>{latest.snr?.toFixed(1) ?? '--'} dB</span></div>
          <div className={styles.stat}><span>RSSI</span><span>{latest.rssi} dBm</span></div>
          <div className={styles.stat}><span>ALT</span><span>{latest.alt_m.toFixed(0)} m</span></div>
        </div>
      )}
    </div>
  );
}
