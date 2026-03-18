import { create } from 'zustand';
import type { RocketTelemetry, AntennaState, MobileNode } from '../types/telemetry';
import { MAX_HISTORY } from '../config';

interface TelemetryState {
  latest:     RocketTelemetry | null;
  history:    RocketTelemetry[];
  antenna:    AntennaState | null;
  nodes:      Record<string, MobileNode>;
  connected:  boolean;
  flightStart: number | null;

  addTelemetry: (t: RocketTelemetry) => void;
  setAntenna:   (a: AntennaState) => void;
  updateNode:   (n: MobileNode) => void;
  setConnected: (v: boolean) => void;
  clearFlight:  () => void;
  loadHistory:  (rows: RocketTelemetry[]) => void;
}

export const useTelemetryStore = create<TelemetryState>((set) => ({
  latest:      null,
  history:     [],
  antenna:     null,
  nodes:       {},
  connected:   false,
  flightStart: null,

  addTelemetry: (t) =>
    set((s) => ({
      latest:      t,
      history:     [...s.history.slice(-(MAX_HISTORY - 1)), t],
      flightStart: s.flightStart ?? t.timestamp,
    })),

  setAntenna: (a) => set({ antenna: a }),

  updateNode: (n) =>
    set((s) => ({ nodes: { ...s.nodes, [n.id]: n } })),

  setConnected: (v) => set({ connected: v }),

  clearFlight: () =>
    set({ history: [], latest: null, flightStart: null }),

  loadHistory: (rows) =>
    set({ history: rows, latest: rows.at(-1) ?? null }),
}));
