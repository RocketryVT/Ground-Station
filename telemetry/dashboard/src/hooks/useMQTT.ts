import { useEffect, useRef } from 'react';
import mqtt, { type MqttClient } from 'mqtt';
import { useTelemetryStore } from '../store/telemetryStore';
import { MQTT_BROKER_URL, TOPICS } from '../config';
import type {
  RocketTelemetry, AntennaState, MobileNode, GroundImuState,
  RawImuSample, RawMagSample,
} from '../types/telemetry';

export interface MQTTHandle {
  publish: (topic: string, payload: string) => void;
}

export function useMQTT(enabled = true): MQTTHandle {
  const clientRef = useRef<MqttClient | null>(null);

  useEffect(() => {
    if (!enabled) return;
    const {
      addTelemetry,
      setAntenna,
      setGroundImu,
      addRawImu,
      addRawMag,
      updateNode,
      setConnected,
      addLogLine,
      addRawMessage,
    } = useTelemetryStore.getState();

    const client = mqtt.connect(MQTT_BROKER_URL, {
      reconnectPeriod: 2000,
      connectTimeout:  5000,
      keepalive:       30,
    });
    clientRef.current = client;

    client.on('connect', () => {
      setConnected(true);
      // Subscribe to all topics — raw inspector needs everything
      client.subscribe('#');
    });

    client.on('disconnect', () => setConnected(false));
    client.on('offline',    () => setConnected(false));
    client.on('error',      () => setConnected(false));

    client.on('message', (topic: string, payload: Buffer) => {
      const raw = payload.toString();
      addRawMessage(topic, raw);

      try {
        const data = JSON.parse(raw);
        if (topic === TOPICS.ROCKET_TELEMETRY) {
          addTelemetry(data as RocketTelemetry);
        } else if (topic === TOPICS.ANTENNA_STATE) {
          setAntenna(data as AntennaState);
        } else if (topic === TOPICS.GROUND_IMU) {
          setGroundImu({ ...(data as Omit<GroundImuState, 'timestamp'>), timestamp: Date.now() });
        } else if (topic === TOPICS.RAW_IMU) {
          addRawImu(data as RawImuSample);
        } else if (topic === TOPICS.RAW_MAG) {
          addRawMag(data as RawMagSample);
        } else if (topic.startsWith('nodes/')) {
          const id = topic.split('/')[1];
          updateNode({ ...(data as Omit<MobileNode, 'id'>), id });
        } else if (topic === TOPICS.GS_LOG) {
          addLogLine(typeof data === 'string' ? data : raw);
        }
      } catch {
        // Non-JSON topics (e.g. gs/log plain text)
        if (topic === TOPICS.GS_LOG) addLogLine(raw);
      }
    });

    return () => {
      client.removeAllListeners();
      client.end(true);
      if (clientRef.current === client) clientRef.current = null;
      setConnected(false);
    };
  }, [enabled]);

  return {
    publish: (topic, payload) => clientRef.current?.publish(topic, payload),
  };
}
