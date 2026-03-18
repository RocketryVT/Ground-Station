import { useEffect } from 'react';
import mqtt from 'mqtt';
import { useTelemetryStore } from '../store/telemetryStore';
import { MQTT_BROKER_URL, TOPICS } from '../config';
import type { RocketTelemetry, AntennaState, MobileNode } from '../types/telemetry';

export function useMQTT(enabled = true) {
  const { addTelemetry, setAntenna, updateNode, setConnected } = useTelemetryStore();

  useEffect(() => {
    if (!enabled) return;
    const client = mqtt.connect(MQTT_BROKER_URL, {
      reconnectPeriod: 2000,
      connectTimeout:  5000,
      keepalive:       30,
    });

    client.on('connect', () => {
      setConnected(true);
      client.subscribe([
        TOPICS.ROCKET_TELEMETRY,
        TOPICS.ANTENNA_STATE,
        TOPICS.NODES_WILDCARD,
      ]);
    });

    client.on('disconnect', () => setConnected(false));
    client.on('offline',    () => setConnected(false));
    client.on('error',      () => setConnected(false));

    client.on('message', (topic: string, payload: Buffer) => {
      try {
        const data = JSON.parse(payload.toString());
        if (topic === TOPICS.ROCKET_TELEMETRY) {
          addTelemetry(data as RocketTelemetry);
        } else if (topic === TOPICS.ANTENNA_STATE) {
          setAntenna(data as AntennaState);
        } else if (topic.startsWith('nodes/')) {
          const id = topic.split('/')[1];
          updateNode({ ...(data as Omit<MobileNode, 'id'>), id });
        }
      } catch (e) {
        console.error('MQTT parse error:', topic, e);
      }
    });

    return () => { client.end(); };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps
}
