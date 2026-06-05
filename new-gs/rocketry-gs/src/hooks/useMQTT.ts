import { useEffect, useRef } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { listen, type UnlistenFn } from '@tauri-apps/api/event';
import { useTelemetryStore, type LogLine, type RawMessage } from '../store/telemetryStore';
import type {
  AntennaState,
  GroundImuState,
  MobileNode,
  RawImuSample,
  RawMagSample,
  RawYawImuSample,
  RocketTelemetry,
} from '../types/telemetry';

export interface MQTTHandle {
  publish: (topic: string, payload: string | Uint8Array) => void;
}

interface TelemetrySnapshot {
  latest: RocketTelemetry | null;
  history: RocketTelemetry[];
  antenna: AntennaState | null;
  groundImu: GroundImuState | null;
  nodes: Record<string, MobileNode>;
  connected: boolean;
  flightStart: number | null;
  logLines: LogLine[];
  rawMessages: RawMessage[];
  rawImu: RawImuSample[];
  rawMag: RawMagSample[];
  rawYawImu: RawYawImuSample[];
  ahrsHistory: GroundImuState[];
}

type TelemetryEvent =
  | { kind: 'connected'; connected: boolean }
  | { kind: 'log_line'; line: LogLine }
  | { kind: 'raw_message'; message: RawMessage }
  | { kind: 'telemetry'; telemetry: RocketTelemetry }
  | { kind: 'antenna'; antenna: AntennaState }
  | { kind: 'ground_imu'; imu: GroundImuState }
  | { kind: 'node'; node: MobileNode }
  | { kind: 'raw_imu'; sample: RawImuSample }
  | { kind: 'raw_mag'; sample: RawMagSample }
  | { kind: 'raw_yaw_imu'; sample: RawYawImuSample }
  | { kind: 'active_drag'; data: Partial<RocketTelemetry> }
  | { kind: 'snapshot'; snapshot: TelemetrySnapshot };

const EVENT_NAME = 'telemetry://event';

function applySnapshot(snapshot: TelemetrySnapshot) {
  useTelemetryStore.setState({
    latest: snapshot.latest,
    history: snapshot.history ?? [],
    antenna: snapshot.antenna,
    groundImu: snapshot.groundImu,
    nodes: snapshot.nodes ?? {},
    connected: snapshot.connected,
    flightStart: snapshot.flightStart,
    logLines: snapshot.logLines ?? [],
    rawMessages: snapshot.rawMessages ?? [],
    rawImu: snapshot.rawImu ?? [],
    rawMag: snapshot.rawMag ?? [],
    rawYawImu: snapshot.rawYawImu ?? [],
    ahrsHistory: snapshot.ahrsHistory ?? [],
  });
}

function applyEvent(event: TelemetryEvent) {
  const store = useTelemetryStore.getState();

  switch (event.kind) {
    case 'connected':
      store.setConnected(event.connected);
      break;
    case 'log_line':
      useTelemetryStore.setState((state) => ({
        logLines: [...state.logLines.slice(-499), event.line],
      }));
      break;
    case 'raw_message':
      useTelemetryStore.setState((state) => ({
        rawMessages: [...state.rawMessages.slice(-499), event.message],
      }));
      break;
    case 'telemetry':
      store.addTelemetry(event.telemetry);
      break;
    case 'antenna':
      store.setAntenna(event.antenna);
      break;
    case 'ground_imu':
      store.setGroundImu(event.imu);
      break;
    case 'node':
      store.updateNode(event.node);
      break;
    case 'raw_imu':
      store.addRawImu(event.sample);
      break;
    case 'raw_mag':
      store.addRawMag(event.sample);
      break;
    case 'raw_yaw_imu':
      store.addRawYawImu(event.sample);
      break;
    case 'active_drag':
      store.setActiveDrag(event.data);
      break;
    case 'snapshot':
      applySnapshot(event.snapshot);
      break;
  }
}

function payloadToString(payload: string | Uint8Array): string {
  if (typeof payload === 'string') return payload;
  return Array.from(payload, (byte) => String.fromCharCode(byte)).join('');
}

export function useMQTT(enabled = true): MQTTHandle {
  const enabledRef = useRef(enabled);
  enabledRef.current = enabled;

  useEffect(() => {
    let unlisten: UnlistenFn | null = null;
    let cancelled = false;

    invoke<TelemetrySnapshot>('get_telemetry_snapshot')
      .then((snapshot) => {
        if (!cancelled && enabledRef.current) applySnapshot(snapshot);
      })
      .catch((error) => {
        useTelemetryStore.getState().addLogLine(`[tauri] snapshot failed: ${String(error)}`);
      });

    listen<TelemetryEvent>(EVENT_NAME, ({ payload }) => {
      if (enabledRef.current) applyEvent(payload);
    })
      .then((fn) => {
        if (cancelled) fn();
        else unlisten = fn;
      })
      .catch((error) => {
        useTelemetryStore.getState().addLogLine(`[tauri] event bridge failed: ${String(error)}`);
      });

    return () => {
      cancelled = true;
      unlisten?.();
    };
  }, []);

  return {
    publish: (topic, payload) => {
      invoke('publish_mqtt', { topic, payload: payloadToString(payload) }).catch((error) => {
        useTelemetryStore.getState().addLogLine(`[mqtt] publish failed: ${String(error)}`);
      });
    },
  };
}
