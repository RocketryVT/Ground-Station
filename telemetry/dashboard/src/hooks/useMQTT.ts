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

function firstNumber(data: Record<string, unknown>, keys: string[]): number | undefined {
  for (const key of keys) {
    const value = data[key];
    if (typeof value === 'number' && Number.isFinite(value)) return value;
  }
  return undefined;
}

function activeDragFromPayload(data: Record<string, unknown>): Partial<RocketTelemetry> | null {
  const deploymentPercent = firstNumber(data, [
    'flap_deployment_percent',
    'deployment_percent',
    'deployment_percentage',
    'desired_deployment',
  ]);
  const angle = firstNumber(data, [
    'flap_angle_deg',
    'flap_deployment_angle_deg',
    'active_drag_flap_angle_deg',
    'airbrake_angle_deg',
  ]) ?? (deploymentPercent != null ? deploymentPercent * 0.6 : undefined);
  const predictedApogee = firstNumber(data, ['predicted_apogee_m', 'apogee_prediction']);
  const targetApogee = firstNumber(data, ['target_apogee_m', 'desired_apogee_m']);

  if (angle == null && deploymentPercent == null && predictedApogee == null && targetApogee == null) return null;

  return {
    flap_angle_deg: angle,
    flap_deployment_percent: deploymentPercent,
    predicted_apogee_m: predictedApogee,
    target_apogee_m: targetApogee,
  };
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
      setActiveDrag,
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
          const activeDrag = activeDragFromPayload(data as Record<string, unknown>);
          if (activeDrag) setActiveDrag(activeDrag);
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
        } else if (topic === TOPICS.ROCKET_LORA0 || topic === TOPICS.ROCKET_INTER_PICO) {
          const activeDrag = activeDragFromPayload(data as Record<string, unknown>);
          if (activeDrag) setActiveDrag(activeDrag);
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
