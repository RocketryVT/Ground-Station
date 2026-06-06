import { useEffect, useRef } from 'react';
import type { MQTTHandle } from '../../hooks/useMQTT';
import { useTelemetryStore } from '../../store/telemetryStore';
import { applyStoredCal, loadMagCal } from './magCalPersist';

// On every MQTT connect, load the saved magnetometer calibration from disk and
// re-publish it to the Pico (which does not persist it across reboots). A short
// delay lets the Pico finish subscribing before we publish.
//
// `mqtt` is reached through a ref so the effect can depend only on the connection
// flag — the handle object identity changes every render, which would otherwise
// cancel the pending publish timer before it fires.
export function useMagCalAutoApply(mqtt: MQTTHandle) {
  const connected = useTelemetryStore((s) => s.connected);
  const mqttRef = useRef(mqtt);
  mqttRef.current = mqtt;

  useEffect(() => {
    if (!connected) return;
    const timer = setTimeout(() => {
      loadMagCal().then((store) => applyStoredCal(mqttRef.current, store));
    }, 1500);
    return () => clearTimeout(timer);
  }, [connected]);
}
