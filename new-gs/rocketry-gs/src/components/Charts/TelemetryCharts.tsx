import {
  Area, AreaChart, CartesianGrid, Line, LineChart, ResponsiveContainer, Tooltip, XAxis, YAxis,
} from 'recharts';
import { useMemo } from 'react';
import { useTelemetryStore } from '../../store/telemetryStore';
import { formatFeet, metersToFeet } from '../../utils/units';
import styles from './TelemetryCharts.module.css';

// Downsample to keep chart renders fast; take every Nth point from history.
const CHART_POINTS = 300;

function downsample<T>(arr: T[], n: number): T[] {
  if (arr.length <= n) return arr;
  const step = arr.length / n;
  return Array.from({ length: n }, (_, i) => arr[Math.floor(i * step)]);
}

function formatSeconds(value: number): string {
  if (!Number.isFinite(value)) return '--';
  if (value < 60) return `${Math.round(value)}s`;
  const minutes = Math.floor(value / 60);
  const seconds = Math.round(value % 60);
  return `${minutes}:${String(seconds).padStart(2, '0')}`;
}

function ChartTooltip({
  active,
  payload,
  label,
}: {
  active?: boolean;
  payload?: Array<{ name?: string; value?: number; color?: string; unit?: string }>;
  label?: number;
}) {
  if (!active || !payload?.length) return null;

  return (
    <div className={styles.tooltip}>
      <div className={styles.tooltipTime}>{formatSeconds(label ?? 0)}</div>
      {payload.map((entry) => (
        <div key={entry.name} className={styles.tooltipRow}>
          <span className={styles.tooltipKey}>
            <span className={styles.tooltipSwatch} style={{ background: entry.color ?? '#fff' }} />
            {entry.name}
          </span>
          <strong className={styles.tooltipValue}>
            {entry.value?.toFixed(1) ?? '--'}{entry.unit ? ` ${entry.unit}` : ''}
          </strong>
        </div>
      ))}
    </div>
  );
}

function sectionHeader(title: string, value: string, detail: string) {
  return (
    <div className={styles.sectionHeader}>
      <div>
        <div className={styles.sectionEyebrow}>Flight Trends</div>
        <h3 className={styles.sectionTitle}>{title}</h3>
      </div>
      <div className={styles.sectionStat}>
        <strong>{value}</strong>
        <span>{detail}</span>
      </div>
    </div>
  );
}

function AltitudeChart() {
  const history = useTelemetryStore((s) => s.history);
  const latest = history.at(-1) ?? null;
  const data = useMemo(
    () => {
      const sampled = downsample(history, CHART_POINTS);
      const start = sampled[0]?.timestamp ?? 0;
      return sampled.map((t) => ({
        t: (t.timestamp - start) / 1000,
        alt_ft: parseFloat(metersToFeet(t.alt_m).toFixed(1)),
      }));
    },
    [history],
  );

  return (
    <section className={styles.section}>
      {sectionHeader(
        'Altitude Envelope',
        formatFeet(latest?.alt_m),
        latest ? 'Current altitude' : 'Awaiting telemetry',
      )}
      <div className={styles.chartFrame}>
        <ResponsiveContainer width="100%" height="100%">
          <AreaChart data={data} margin={{ top: 8, right: 12, bottom: 4, left: -8 }}>
            <defs>
              <linearGradient id="alt-fill" x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stopColor="#ffb46b" stopOpacity={0.45} />
                <stop offset="100%" stopColor="#ffb46b" stopOpacity={0.03} />
              </linearGradient>
            </defs>
            <CartesianGrid vertical={false} stroke="rgba(217,227,238,0.09)" strokeDasharray="4 8" />
            <XAxis
              dataKey="t"
              minTickGap={28}
              tickFormatter={formatSeconds}
              tick={{ fill: 'rgba(222,231,240,0.48)', fontSize: 10 }}
              tickLine={false}
              axisLine={false}
            />
            <YAxis
              width={54}
              tickFormatter={(value: number) => `${Math.round(value)}`}
              tick={{ fill: 'rgba(222,231,240,0.48)', fontSize: 10 }}
              tickLine={false}
              axisLine={false}
            />
            <Tooltip content={<ChartTooltip />} />
            <Area
              type="monotone"
              dataKey="alt_ft"
              name="Altitude"
              unit="ft"
              stroke="#ffb46b"
              fill="url(#alt-fill)"
              strokeWidth={2.4}
              dot={false}
              activeDot={{ r: 4, fill: '#fff1d9', stroke: '#ffb46b', strokeWidth: 2 }}
              isAnimationActive={false}
            />
          </AreaChart>
        </ResponsiveContainer>
      </div>
    </section>
  );
}

function VelocityChart() {
  const history = useTelemetryStore((s) => s.history);
  const latest = history.at(-1) ?? null;
  const data = useMemo(
    () => {
      const sampled = downsample(history, CHART_POINTS);
      const start = sampled[0]?.timestamp ?? 0;
      return sampled.map((t) => ({
        t: (t.timestamp - start) / 1000,
        speed: parseFloat(
          Math.sqrt(t.vel_n ** 2 + t.vel_e ** 2 + t.vel_d ** 2).toFixed(1),
        ),
        vertical: parseFloat((-t.vel_d).toFixed(1)), // positive = climbing
      }));
    },
    [history],
  );

  const latestSpeed = latest
    ? Math.sqrt(latest.vel_n ** 2 + latest.vel_e ** 2 + latest.vel_d ** 2)
    : null;

  return (
    <section className={styles.section}>
      {sectionHeader(
        'Velocity Profile',
        latestSpeed != null ? `${latestSpeed.toFixed(1)} m/s` : '--',
        latest ? `${(-latest.vel_d).toFixed(1)} m/s vertical` : 'Awaiting telemetry',
      )}
      <div className={styles.legendRow}>
        <span className={styles.legendItem}><span className={`${styles.legendSwatch} ${styles.legendSpeed}`} />Speed</span>
        <span className={styles.legendItem}><span className={`${styles.legendSwatch} ${styles.legendVertical}`} />Vertical</span>
      </div>
      <div className={styles.chartFrame}>
        <ResponsiveContainer width="100%" height="100%">
          <LineChart data={data} margin={{ top: 8, right: 12, bottom: 4, left: -8 }}>
            <CartesianGrid vertical={false} stroke="rgba(217,227,238,0.09)" strokeDasharray="4 8" />
            <XAxis
              dataKey="t"
              minTickGap={28}
              tickFormatter={formatSeconds}
              tick={{ fill: 'rgba(222,231,240,0.48)', fontSize: 10 }}
              tickLine={false}
              axisLine={false}
            />
            <YAxis
              width={54}
              tick={{ fill: 'rgba(222,231,240,0.48)', fontSize: 10 }}
              tickLine={false}
              axisLine={false}
            />
            <Tooltip content={<ChartTooltip />} />
            <Line
              type="monotone"
              dataKey="speed"
              name="Speed"
              stroke="#f39a4a"
              dot={false}
              strokeWidth={2.2}
              activeDot={{ r: 4, fill: '#fff1d9', stroke: '#f39a4a', strokeWidth: 2 }}
              isAnimationActive={false}
            />
            <Line
              type="monotone"
              dataKey="vertical"
              name="Vertical"
              stroke="#83b9ff"
              dot={false}
              strokeWidth={1.6}
              strokeDasharray="6 6"
              activeDot={{ r: 4, fill: '#e7f1ff', stroke: '#83b9ff', strokeWidth: 2 }}
              isAnimationActive={false}
            />
          </LineChart>
        </ResponsiveContainer>
      </div>
    </section>
  );
}

export function TelemetryCharts() {
  return (
    <div className={styles.wrapper}>
      <div className={styles.headerRow}>
        <div>
          <div className={styles.eyebrow}>Telemetry</div>
          <h2 className={styles.title}>Flight Graphs</h2>
        </div>
      </div>
      <AltitudeChart />
      <VelocityChart />
    </div>
  );
}
