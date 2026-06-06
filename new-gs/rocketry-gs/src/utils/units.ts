const FEET_PER_METER = 3.280839895;

export function metersToFeet(value: number): number {
  return value * FEET_PER_METER;
}

export function formatFeet(value: number | null | undefined, digits = 0): string {
  if (value == null || !Number.isFinite(value)) return '--';
  return `${metersToFeet(value).toFixed(digits)} ft`;
}