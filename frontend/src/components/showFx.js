// Flicker for the 3D preview - the same formula as flicker_is_lit() in
// backend/show_fx.h, applied per simulated point by its playback time
// (LaserScene.vue), exactly like the daemon does per point for the laser.
// The daemon sends preview frames unflickered plus its show clock (`t`) and
// the flicker settings (`fl`), so preview and laser blink in the same rhythm.

export const FLICKER_ON_MS_MAX = 1000

// rateHz <= 0: off (always lit). onMs 0 = auto (lit for half of every
// period). onMs >= period: always lit.
export function flickerIsLit(t, rateHz, onMs) {
  if (!(rateHz > 0) || !Number.isFinite(rateHz) || !Number.isFinite(t)) return true
  const period = 1 / rateHz
  const on = onMs > 0 ? Math.min(onMs, FLICKER_ON_MS_MAX) / 1000 : period * 0.5
  if (on >= period) return true
  let phase = t % period
  if (phase < 0) phase += period
  return phase < on
}

// Tracks the offset between the daemon's show clock and this browser's
// clock (seconds). Messages only ever arrive *late*, so the largest
// observed offset is the best estimate; it decays slowly so a changed
// network delay or a restarted daemon is picked up again.
export function createClockSync() {
  let offset = null
  return {
    update(serverT, clientT) {
      const sample = serverT - clientT
      if (!Number.isFinite(sample)) return
      if (offset === null || sample > offset || Math.abs(sample - offset) > 1) offset = sample
      else offset -= 0.0005 // ~15 ms/s at 30 messages/s
    },
    toServer(clientT) { return offset === null ? clientT : clientT + offset },
    get offset() { return offset },
  }
}
