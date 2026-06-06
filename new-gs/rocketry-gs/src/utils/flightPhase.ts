// Flight phase: a small display layer over the firmware flight-state strings
// (codec.rs flight_state_name). Used by the top phase banner and the descent
// parachute visualization.

export type Phase =
  | 'PAD' | 'BOOST' | 'COAST' | 'APOGEE' | 'DROGUE' | 'MAIN' | 'LANDED'
  | 'FAULT' | 'UNKNOWN';

// Firmware state string → display phase.
const STATE_TO_PHASE: Record<string, Phase> = {
  GROUND_IDLE:    'PAD',
  ARMED:          'PAD',
  POWERED_ASCENT: 'BOOST',
  COAST_ASCENT:   'COAST',
  APOGEE:         'APOGEE',
  DESCENT_DROGUE: 'DROGUE',
  DESCENT_MAIN:   'MAIN',
  LANDED:         'LANDED',
  FAULT:          'FAULT',
};

export function phaseFromState(state?: string | null): Phase {
  if (state && STATE_TO_PHASE[state]) return STATE_TO_PHASE[state];
  return 'UNKNOWN';
}

export const PHASE_LABEL: Record<Phase, string> = {
  PAD: 'PAD', BOOST: 'BOOST', COAST: 'COAST', APOGEE: 'APOGEE',
  DROGUE: 'DROGUE', MAIN: 'MAIN', LANDED: 'LANDED', FAULT: 'FAULT', UNKNOWN: '—',
};

export const PHASE_COLOR: Record<Phase, string> = {
  PAD:     '#9aa3ad',
  BOOST:   '#CA4F00',
  COAST:   '#e0a040',
  APOGEE:  '#f0c14b',
  DROGUE:  '#4da8ff',
  MAIN:    '#4dff9a',
  LANDED:  '#75787B',
  FAULT:   '#ff4d5e',
  UNKNOWN: '#75787B',
};

// Descent visualization: the body is separated for drogue onward; the canopy
// grows from drogue (small) to main (large).
export function isSeparated(phase: Phase): boolean {
  return phase === 'DROGUE' || phase === 'MAIN' || phase === 'LANDED';
}
export function isMainChute(phase: Phase): boolean {
  return phase === 'MAIN' || phase === 'LANDED';
}
