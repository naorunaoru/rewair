const LOOPBACK_HOSTS = new Set(['localhost', '127.0.0.1', '[::1]', '::1']);

/* A hosted HTTPS build is a Bluetooth client, not an HTTP device discovery
 * mechanism. HTTP is selected only when the user names a device explicitly
 * or when the UI itself is being served by a non-loopback HTTP device. */
export function initialHttpTarget(locationLike) {
  const qs = new URLSearchParams(locationLike.search || '');
  const device = qs.get('device');
  if (device) return { enabled: true, base: `http://${device}`, explicit: true };

  const sameOriginDevice = locationLike.protocol === 'http:' &&
    !LOOPBACK_HOSTS.has(locationLike.hostname);
  return { enabled: sameOriginDevice, base: '', explicit: false };
}
