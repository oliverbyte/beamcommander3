// Talks directly to the C++ laser_daemon (port 8000).
// API:  GET/POST /api/state   POST /laser/shape/<s>
//       POST /laser/connect/<ip>  POST /laser/disconnect
//       GET /api/cues  POST /api/cue/<n>/save|recall|clear
//       WS   /ws/points
import { reactive } from 'vue'

export const laserState = reactive({
  // Shape
  shape:           'circle',
  radius:          0.7,
  points:          300,
  shape_scale:     0.0,
  // Color
  r: 0.0, g: 1.0, b: 0.314,
  intensity:       1.0,
  // Position & rotation
  pos_x:           0.0,
  pos_y:           0.0,
  rotation_speed:  0.0,
  // Movement
  move_mode:       'none',
  move_speed:      0.3,
  move_size:       0.5,
  // Wave
  wave_frequency:  1.0,
  wave_amplitude:  0.45,
  wave_speed:      0.0,
  // Rainbow
  rainbow_amount:  0.0,
  rainbow_speed:   0.0,
  // FX
  blackout:        false,
  dot_amount:      1.0,
  flicker_hz:      0.0,
  // Scan
  rate_kpps:       30,
  // Controller
  armed:           false,
  ip:              '',
  // UI-only
  wsConnected:     false,
  error:           null,
})

// Cue slots — { [cueNumber]: <full saved state object> } for populated
// slots only; a slot simply being absent from this object means it's empty.
export const cues = reactive({})

// ── WebSocket preview ──────────────────────────────────────────────────────────

let socket = null
let pointListener = null

export function onPoints(listener) { pointListener = listener }

export function connectSocket() {
  if (socket) return
  const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
  socket = new WebSocket(`${proto}://${window.location.host}/ws/points`)
  socket.addEventListener('open',    () => { laserState.wsConnected = true })
  socket.addEventListener('close',   () => {
    laserState.wsConnected = false
    socket = null
    setTimeout(connectSocket, 1000)
  })
  socket.addEventListener('error',   () => socket && socket.close())
  socket.addEventListener('message', (ev) => {
    try {
      const msg = JSON.parse(ev.data)
      if (pointListener) pointListener(msg)
    } catch (_) {}
  })
}

// ── HTTP helpers ───────────────────────────────────────────────────────────────

async function api(path, options = {}) {
  const res = await fetch(`/api${path}`, {
    headers: { 'Content-Type': 'application/json' },
    ...options,
  })
  if (!res.ok) throw new Error(`API ${path} → ${res.status}`)
  return res.json()
}

async function laser(path, options = {}) {
  const res = await fetch(`/laser${path}`, {
    headers: { 'Content-Type': 'application/json' },
    ...options,
  })
  if (!res.ok) throw new Error(`/laser${path} → ${res.status}`)
  return res.json()
}

function applyState(s) {
  if (!s) return
  Object.assign(laserState, s)
}

// Guards against the background poll clobbering an optimistic local edit
// that's still in flight (debounce + network round trip). Any local mutation
// calls markLocalChange(); polled responses are ignored for a short window
// afterwards so the just-changed value doesn't visibly "bounce back".
let lastLocalChangeAt = 0
export function markLocalChange() { lastLocalChangeAt = Date.now() }

function applyPolledState(s) {
  if (!s) return
  if (Date.now() - lastLocalChangeAt < 700) return
  Object.assign(laserState, s)
}

// ── Public API ─────────────────────────────────────────────────────────────────

export async function fetchState() {
  applyState(await api('/state'))
}

export async function updateState(partial) {
  markLocalChange()
  applyState(await api('/state', { method: 'POST', body: JSON.stringify(partial) }))
}

export async function resetState() {
  markLocalChange()
  applyState(await api('/reset', { method: 'POST' }))
}

export async function setShape(shape) {
  markLocalChange()
  await laser(`/shape/${shape}`, { method: 'POST' })
  laserState.shape = shape
}

export async function setBrightness(v) {
  markLocalChange()
  applyState(await laser(`/brightness/${v}`, { method: 'POST' }))
}

export async function connectLaser(ip) {
  markLocalChange()
  applyState(await laser(`/connect/${encodeURIComponent(ip)}`, { method: 'POST' }))
}

export async function disconnectLaser() {
  markLocalChange()
  applyState(await laser('/disconnect', { method: 'POST' }))
}

// Momentary flash: forces color to white *and* full brightness only while
// held, restores the exact prior color and brightness on release - mirrors
// the MIDI "flash" button (including a footswitch) so it behaves
// identically from any input. Not routed through /api/state since it's a
// transient action, not a persisted field.
export async function flashPress() {
  markLocalChange()
  applyState(await (await fetch('/flash/1', { method: 'POST' })).json())
}
export async function flashRelease() {
  markLocalChange()
  applyState(await (await fetch('/flash/0', { method: 'POST' })).json())
}

// ── Cue save/recall ─────────────────────────────────────────────────────────────

function applyCues(data) {
  for (const k of Object.keys(cues)) delete cues[k]
  Object.assign(cues, data || {})
}

export async function fetchCues() {
  applyCues(await api('/cues'))
}

export async function saveCue(n) {
  await api(`/cue/${n}/save`, { method: 'POST' })
  await fetchCues()
}

export async function recallCue(n) {
  markLocalChange()
  applyState(await api(`/cue/${n}/recall`, { method: 'POST' }))
}

export async function clearCue(n) {
  await api(`/cue/${n}/clear`, { method: 'POST' })
  await fetchCues()
}

// ── Status polling ─────────────────────────────────────────────────────────────
// Keeps this client's view of laserState in sync even when another client
// (a different browser tab, curl, etc.) changes settings on the backend.
// Uses the guarded merge so it never clobbers a still-in-flight local edit.
let statusPoll = null
export function startStatusPolling(intervalMs = 1000) {
  if (statusPoll) return
  statusPoll = setInterval(() => {
    api('/state').then(applyPolledState).catch(() => {})
    api('/cues').then(applyCues).catch(() => {})
  }, intervalMs)
}
export function stopStatusPolling() {
  clearInterval(statusPoll)
  statusPoll = null
}
