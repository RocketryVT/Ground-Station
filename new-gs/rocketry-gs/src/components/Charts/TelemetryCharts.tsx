import {
  LineChart, Line, XAxis, YAxis, ResponsiveContainer, Tooltip,
} from 'recharts';
import { useMemo } from 'react';
import { useTelemetryStore } from '../../store/telemetryStore';
import styles from './TelemetryCharts.module.css';

// Downsample to keep chart renders fast; take every Nth point from history.
const CHART_POINTS = 300;

function downsample<T>(arr: T[], n: number): T[] {
  if (arr.length <= n) return arr;
  const step = arr.length / n;
  return Array.from({ length: n }, (_, i) => arr[Math.floor(i * step)]);
}

const tooltipStyle = {
  contentStyle: { background: '#ffffff', border: '1px solid #D7D2CB', fontSize: 11 },
  labelStyle:   { display: 'none' },
};

function AltitudeChart() {
  const history = useTelemetryStore((s) => s.history);
  const data = useMemo(
    () =>
      downsample(history, CHART_POINTS).map((t, i) => ({
        i,
        alt: parseFloat(t.alt_m.toFixed(1)),
      })),
    [history],
  );

  return (
    <div className={styles.chart}>
      <span className={styles.label}>ALT (m)</span>
      <ResponsiveContainer width="100%" height="100%">
        <LineChart data={data} margin={{ top: 4, right: 8, bottom: 0, left: 0 }}>
          <XAxis dataKey="i" hide />
          <YAxis width={44} tick={{ fill: '#75787B', fontSize: 10 }} tickLine={false} axisLine={false} />
          <Tooltip {...tooltipStyle} formatter={(v: number) => [`${v} m`, 'Alt']} />
          <Line
            type="monotone"
            dataKey="alt"
            stroke="#861F41"
            dot={false}
            strokeWidth={1.5}
            isAnimationActive={false}
          />
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}

function VelocityChart() {
  const history = useTelemetryStore((s) => s.history);
  const data = useMemo(
    () =>
      downsample(history, CHART_POINTS).map((t, i) => ({
        i,
        vel: parseFloat(
          Math.sqrt(t.vel_n ** 2 + t.vel_e ** 2 + t.vel_d ** 2).toFixed(1),
        ),
        vz: parseFloat((-t.vel_d).toFixed(1)), // positive = climbing
      })),
    [history],
  );

  return (
    <div className={styles.chart}>
      <span className={styles.label}>VEL (m/s)</span>
      <ResponsiveContainer width="100%" height="100%">
        <LineChart data={data} margin={{ top: 4, right: 8, bottom: 0, left: 0 }}>
          <XAxis dataKey="i" hide />
          <YAxis width={44} tick={{ fill: '#75787B', fontSize: 10 }} tickLine={false} axisLine={false} />
          <Tooltip {...tooltipStyle} formatter={(v: number, name: string) => [`${v} m/s`, name === 'vel' ? 'Speed' : 'Vert']} />
          <Line type="monotone" dataKey="vel" stroke="#CA4F00" dot={false} strokeWidth={1.5} isAnimationActive={false} />
          <Line type="monotone" dataKey="vz"  stroke="#75787B" dot={false} strokeWidth={1}   isAnimationActive={false} strokeDasharray="3 3" />
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}

export function TelemetryCharts() {
  return (
    <div className={styles.wrapper}>
      <AltitudeChart />
      <VelocityChart />
    </div>
  );
}
