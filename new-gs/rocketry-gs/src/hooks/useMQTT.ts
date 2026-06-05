import { useEffect, useRef } from 'react';
import mqtt, { type MqttClient } from 'mqtt';
import { useTelemetryStore } from '../store/telemetryStore';
import { MQTT_BROKER_URL, TOPICS } from '../config';
import { decodeTopicPayload, encodeCommandPayload } from '../proto/groundStationCodec';
import type {
  RocketTelemetry, AntennaState, MobileNode, GroundImuState,
  RawImuSample, RawMagSample, RawYawImuSample,
} from '../types/telemetry';

export interface MQTTHandle {
  publish: (topic: string, payload: string | Uint8Array) => void;
}

const MQTT_ERROR_LOG_INTERVAL_MS = 5000;

interface MQTTRefState {
  client: MqttClient | null;
  lastErrorLog: { text: string; ts: number } | null;
}

function mqttErrorText(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
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
  const mqttRef = useRef<MQTTRefState>({ client: null, lastErrorLog: null });

  useEffect(() => {
    if (!enabled) return;
    const mqttState = mqttRef.current;
    const {
      addTelemetry,
      setAntenna,
      setGroundImu,
      addRawImu,
      addRawMag,
      addRawYawImu,
      updateNode,
      setConnected,
      addLogLine,
      addRawMessage,
      setActiveDrag,
    } = useTelemetryStore.getState();

    const logMqttError = (error: unknown) => {
      const text = mqttErrorText(error);
      const now = Date.now();
      const last = mqttState.lastErrorLog;
      if (!last || last.text !== text || now - last.ts > MQTT_ERROR_LOG_INTERVAL_MS) {
        addLogLine(`[mqtt] ${text} (${MQTT_BROKER_URL})`);
        mqttState.lastErrorLog = { text, ts: now };
      }
    };

    const client = mqtt.connect(MQTT_BROKER_URL, {
      manualConnect:   true,
      reconnectPeriod: 2000,
      connectTimeout:  5000,
      keepalive:       30,
    });
    client.on('error', () => {});
    mqttState.client = client;

    const handleConnect = () => {
      setConnected(true);
      // Subscribe to all topics — raw inspector needs everything
      client.subscribe('#', (error) => {
        if (error) logMqttError(error);
      });
    };

    const handleDisconnected = () => setConnected(false);

    const handleError = (error: Error) => {
      setConnected(false);
      logMqttError(error);
    };

    client.on('connect', handleConnect);
    client.on('disconnect', handleDisconnected);
    client.on('offline', handleDisconnected);
    client.on('close', handleDisconnected);
    client.on('error', handleError);

    const handleMessage = (topic: string, payload: Buffer) => {
      let raw = '';
      let decoded: Record<string, unknown> | string | null = null;

      try {
        decoded = decodeTopicPayload(topic, payload);
      } catch {
        decoded = null;
      }

      if (decoded != null) {
        raw = typeof decoded === 'string' ? decoded : JSON.stringify(decoded);
      } else {
        raw = payload.toString();
      }
      addRawMessage(topic, raw);

      try {
        const data = typeof decoded === 'object' && decoded !== null
          ? decoded
          : JSON.parse(raw);
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
        } else if (topic === TOPICS.RAW_YAW_IMU) {
          addRawYawImu(data as RawYawImuSample);
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
    };

    client.on('message', handleMessage);

    try {
      client.connect();
    } catch (error) {
      handleError(error instanceof Error ? error : new Error(String(error)));
      client.end(true);
    }

    return () => {
      client.off('connect', handleConnect);
      client.off('disconnect', handleDisconnected);
      client.off('offline', handleDisconnected);
      client.off('close', handleDisconnected);
      client.off('message', handleMessage);
      client.off('error', handleError);
      client.on('error', () => {});
      client.end(true);
      if (mqttState.client === client) mqttState.client = null;
      setConnected(false);
    };
  }, [enabled]);

  return {
    publish: (topic, payload) => {
      if (!mqttRef.current.client?.connected) {
        const { addLogLine } = useTelemetryStore.getState();
        addLogLine(`[mqtt] publish skipped while disconnected: ${topic}`);
        return;
      }
      const encoded = encodeCommandPayload(topic, payload);
      mqttRef.current.client?.publish(topic, encoded as Buffer | string);
    },
  };
}
