import { useEffect, useRef, useState } from 'react';
import styles from './VideoFeed.module.css';

type Source = 'device' | 'url';

export function VideoFeed() {
  const videoRef = useRef<HTMLVideoElement>(null);
  const [source, setSource] = useState<Source>('device');
  const [hlsUrl, setHlsUrl] = useState('');
  const [devices, setDevices] = useState<MediaDeviceInfo[]>([]);
  const [deviceId, setDeviceId] = useState<string>('');
  const [error, setError] = useState('');

  // Enumerate video capture devices on mount.
  useEffect(() => {
    navigator.mediaDevices
      .enumerateDevices()
      .then((d) => {
        const vids = d.filter((x) => x.kind === 'videoinput');
        setDevices(vids);
        if (vids.length > 0) setDeviceId(vids[0].deviceId);
      })
      .catch(() => setError('Cannot enumerate devices'));
  }, []);

  // Start camera/capture device stream.
  useEffect(() => {
    if (source !== 'device' || !deviceId) return;
    let stream: MediaStream;
    navigator.mediaDevices
      .getUserMedia({ video: { deviceId: { exact: deviceId } } })
      .then((s) => {
        stream = s;
        if (videoRef.current) videoRef.current.srcObject = s;
        setError('');
      })
      .catch((e) => setError(String(e)));
    return () => {
      stream?.getTracks().forEach((t) => t.stop());
      if (videoRef.current) videoRef.current.srcObject = null;
    };
  }, [source, deviceId]);

  // HLS / direct URL stream.
  useEffect(() => {
    if (source !== 'url' || !hlsUrl) return;
    if (videoRef.current) {
      videoRef.current.srcObject = null;
      videoRef.current.src = hlsUrl;
    }
  }, [source, hlsUrl]);

  return (
    <div className={styles.wrapper}>
      <div className={styles.panelLabel}>VIDEO FEED</div>

      <video
        ref={videoRef}
        className={styles.video}
        autoPlay
        muted
        playsInline
      />

      {error && <div className={styles.error}>{error}</div>}

      {/* Controls — hidden during a livestream, shown in dev */}
      <div className={styles.controls}>
        <select
          className={styles.select}
          value={source}
          onChange={(e) => setSource(e.target.value as Source)}
        >
          <option value="device">Capture device</option>
          <option value="url">HLS / URL</option>
        </select>

        {source === 'device' && (
          <select
            className={styles.select}
            value={deviceId}
            onChange={(e) => setDeviceId(e.target.value)}
          >
            {devices.map((d) => (
              <option key={d.deviceId} value={d.deviceId}>
                {d.label || d.deviceId.slice(0, 20)}
              </option>
            ))}
          </select>
        )}

        {source === 'url' && (
          <input
            className={styles.input}
            placeholder="http://…/stream.m3u8"
            value={hlsUrl}
            onChange={(e) => setHlsUrl(e.target.value)}
          />
        )}
      </div>
    </div>
  );
}
