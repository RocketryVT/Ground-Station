// Persistence + apply helpers for magnetometer calibration.
//
// The calibration blob is stored next to the app binary (Rust save_mag_cal /
// load_mag_cal) and re-published to the Pico on every MQTT connect, so the Pico
// — which has no flash persistence — always gets the last calibration on boot.

import { invoke } from '@tauri-apps/api/core';
import { TOPICS } from '../../config';
import type { MQTTHandle } from '../../hooks/useMQTT';
import type { Mat3, Vec3 } from './magfit';

export type MagSensor = 'yaw' | 'bar';

export interface SensorCal {
  hard_iron: Vec3;
  soft_iron: Mat3;
  field_radius: number;
  residual: number;
  samples: number;
  updated_ms: number;
}

export type MagCalStore = Partial<Record<MagSensor, SensorCal>>;

export async function loadMagCal(): Promise<MagCalStore> {
  try {
    const text = await invoke<string | null>('load_mag_cal');
    if (!text) return {};
    return JSON.parse(text) as MagCalStore;
  } catch {
    return {};
  }
}

export async function saveMagCal(store: MagCalStore): Promise<void> {
  await invoke('save_mag_cal', { payload: JSON.stringify(store, null, 2) });
}

// Publish one sensor's calibration to the Pico.
export function publishSensorCal(mqtt: MQTTHandle, sensor: MagSensor, cal: SensorCal) {
  mqtt.publish(
    TOPICS.MAG_CAL_CMD,
    JSON.stringify({ yaw: sensor === 'yaw', hard_iron: cal.hard_iron, soft_iron: cal.soft_iron }),
  );
}

// Push every stored sensor calibration to the Pico.
export function applyStoredCal(mqtt: MQTTHandle, store: MagCalStore) {
  (Object.keys(store) as MagSensor[]).forEach((sensor) => {
    const cal = store[sensor];
    if (cal) publishSensorCal(mqtt, sensor, cal);
  });
}
