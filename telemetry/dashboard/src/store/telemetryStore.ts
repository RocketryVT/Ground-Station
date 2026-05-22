import { create } from 'zustand';
import type {
  RocketTelemetry, AntennaState, MobileNode, GroundImuState,
  RawImuSample, RawMagSample,
} from '../types/telemetry';
import { MAX_HISTORY } from '../config';

const MAX_LOG = 500;
const MAX_RAW = 500;
const MAX_SENSOR_RAW = 800;
const MAX_AHRS = 800;

let _seq = 0;

export interface LogLine    { id: number; ts: number; text: string; }
export interface RawMessage { id: number; ts: number; topic: string; payload: string; }

interface TelemetryState {
  latest:      RocketTelemetry | null;
  history:     RocketTelemetry[];
  antenna:     AntennaState | null;
  groundImu:   GroundImuState | null;
  nodes:       Record<string, MobileNode>;
  connected:   boolean;
  flightStart: number | null;
  logLines:    LogLine[];
  rawMessages: RawMessage[];
  rawImu:      RawImuSample[];
  rawMag:      RawMagSample[];
  ahrsHistory: GroundImuState[];

  addTelemetry: (t: RocketTelemetry) => void;
  setAntenna:   (a: AntennaState) => void;
  setGroundImu: (i: GroundImuState) => void;
  updateNode:   (n: MobileNode) => void;
  setConnected: (v: boolean) => void;
  clearFlight:  () => void;
  loadHistory:  (rows: RocketTelemetry[]) => void;
  addLogLine:   (text: string) => void;
  addRawMessage:(topic: string, payload: string) => void;
  addRawImu:    (s: RawImuSample) => void;
  addRawMag:    (s: RawMagSample) => void;
  clearRawSensors: () => void;
  clearAhrsHistory: () => void;
  clearDebug:   () => void;
}

export const useTelemetryStore = create<TelemetryState>((set) => ({
  latest:      null,
  history:     [],
  antenna:     null,
  groundImu:   null,
  nodes:       {},
  connected:   false,
  flightStart: null,
  logLines:    [],
  rawMessages: [],
  rawImu:      [],
  rawMag:      [],
  ahrsHistory: [],

  addTelemetry: (t) =>
    set((s) => ({
      latest:      t,
      history:     [...s.history.slice(-(MAX_HISTORY - 1)), t],
      flightStart: s.flightStart ?? t.timestamp,
    })),

  setAntenna: (a) => set({ antenna: a }),

  setGroundImu: (i) =>
    set((s) => ({
      groundImu: i,
      ahrsHistory: [...s.ahrsHistory.slice(-(MAX_AHRS - 1)), i],
    })),

  updateNode: (n) =>
    set((s) => ({ nodes: { ...s.nodes, [n.id]: n } })),

  setConnected: (v) => set({ connected: v }),

  clearFlight: () =>
    set({ history: [], latest: null, flightStart: null }),

  loadHistory: (rows) =>
    set({ history: rows, latest: rows.at(-1) ?? null }),

  addLogLine: (text) =>
    set((s) => ({
      logLines: [...s.logLines.slice(-(MAX_LOG - 1)), { id: _seq++, ts: Date.now(), text }],
    })),

  addRawMessage: (topic, payload) =>
    set((s) => ({
      rawMessages: [...s.rawMessages.slice(-(MAX_RAW - 1)), { id: _seq++, ts: Date.now(), topic, payload }],
    })),

  addRawImu: (sample) =>
    set((s) => ({ rawImu: [...s.rawImu.slice(-(MAX_SENSOR_RAW - 1)), sample] })),

  addRawMag: (sample) =>
    set((s) => ({ rawMag: [...s.rawMag.slice(-(MAX_SENSOR_RAW - 1)), sample] })),

  clearRawSensors: () => set({ rawImu: [], rawMag: [] }),

  clearAhrsHistory: () => set({ ahrsHistory: [] }),

  clearDebug: () => set({ logLines: [], rawMessages: [] }),
}));
