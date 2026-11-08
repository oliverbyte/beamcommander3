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
  flash_release_ms: 0.0,
  // Position & rotation
  pos_x:           0.0,
  pos_y:           0.0,
  rotation_speed:  0.0,
  // Movement
  move_mode:       'none',
  move_speed:      0.3,
  move_size:       0.5,
  move_phase_degrees: 0,
  // Wave
  wave_frequency:  1.0,
  wave_amplitude:  0.45,
  wave_speed:      0.0,
  // Rainbow
  rainbow_amount:  0.0,
  rainbow_speed:   0.0,
  rainbow_phase_degrees: 0,
  // FX
  blackout:        true,
  dot_amount:      1.0,
  flicker_hz:      0.0,
  flicker_on_ms:   0,   // lit time per flicker period, 0 = auto (half the period)
  flicker_phase_degrees: 0, // flicker delay per laser in list order (360 = one period)
  // Scan
  rate_kpps:       30,
  max_rate_kpps:   30,
  output_latency_ms: 40, // how far ahead frames are queued for the DACs (global, not in cues)
  dot_dwell_us:    400, // scanner tuning: lit time per dot (global, not in cues)
  dot_blank_us:    300, // scanner tuning: dark time before/after each dot (global, not in cues)
  layers: [],      // cue numbers currently playing as layers on top of the look (runtime only)
  // Footswitch-style master output gate (see /brightness/gate/<0|1> in
  // laser_daemon.cpp): defaults OPEN (true) - only `blackout` above is the
  // active safety default. Hardware output stays forced dark only if this
  // is explicitly closed (rigs with a physical footswitch) or blackout is on.
  brightness_gate_open: true,
  // UI-only
  wsConnected:     false,
  error:           null,
})

// Cue slots — { [cueNumber]: <full saved state object> } for populated
// slots only; a slot simply being absent from this object means it's empty.
export const cues = reactive({})

// Configured lasers (physical DACs) — see GET/POST/DELETE /api/lasers in
// laser_daemon.cpp. Each entry: {id, name, ip, armed, zone_x, zone_y,
// zone_scale_x, zone_scale_y, masks, connected}. `armed` is the master enable -
// false means configured but idle (never connected to), true means
// laser_thread() connects and streams Zone 1's output to it. Any number of
// lasers can be armed at once, all receiving the same show output in
// parallel, each transformed by its own zone_x/y/zone_scale_x/y pan-zoom
// calibration (so two projectors in the same rig can each be aimed at a
// different physical sub-region while showing the exact same content).
export const lasers = reactive([])

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

// Guards against out-of-order network responses: if two state-mutating
// requests are in flight at once (e.g. two quick clicks, each sent 80ms
// apart but with overlapping round trips), nothing guarantees the FIRST
// request's response arrives first too. Applying whichever response
// resolves last - regardless of which request was actually issued last -
// could silently revert a newer, already-applied value back to a stale
// one. Each state-mutating call below grabs a ticket via nextStateSeq()
// before awaiting its request, then only calls applyState() if no newer
// request has been issued meanwhile (seq still matches); a now-stale
// response is discarded instead of overwriting the correct current state.
let stateSeq = 0
function nextStateSeq() { return ++stateSeq }
function applyStateIfCurrent(seq, s) {
  if (seq === stateSeq) applyState(s)
}

// Guards against the background poll clobbering an optimistic local edit
// that's still in flight (debounce + network round trip). Any local mutation
// calls markLocalChange(); polled responses are ignored for a short window
// afterwards so the just-changed value doesn't visibly "bounce back".
let lastLocalChangeAt = 0
export function markLocalChange() {
  lastLocalChangeAt = Date.now()
  nextStateSeq()
}

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
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await api('/state', { method: 'POST', body: JSON.stringify(partial) }))
}

export async function resetState() {
  markLocalChange()
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await api('/reset', { method: 'POST' }))
}

export async function setShape(shape) {
  markLocalChange()
  await laser(`/shape/${shape}`, { method: 'POST' })
  laserState.shape = shape
}

export async function setBrightness(v) {
  markLocalChange()
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await laser(`/brightness/${v}`, { method: 'POST' }))
}

// ── Lasers (DACs) ────────────────────────────────────────────────────────────
// Manage the configured laser list, each one's armed state and its own
// zone calibration - see GET/POST/DELETE /api/lasers in laser_daemon.cpp.
function applyLasers(data) {
  lasers.splice(0, lasers.length, ...(data || []))
}

export async function fetchLasers() {
  applyLasers(await api('/lasers'))
}

// Guards against the background poll clobbering an optimistic local edit
// (arm toggle, zoning drag) that's still in flight - same reasoning as
// markLocalChange() above for laserState.
let lastLaserLocalChangeAt = 0
export function markLaserLocalChange() { lastLaserLocalChangeAt = Date.now() }

function applyPolledLasers(data) {
  if (Date.now() - lastLaserLocalChangeAt < 700) return
  applyLasers(data)
}

// Mutates a laser's fields locally (optimistic, no network round trip) so
// e.g. ZoningPanel's drag can update the box position every pointermove
// without spamming the API - callers debounce the actual updateLaser() call
// separately.
export function patchLaserLocal(id, partial) {
  markLaserLocalChange()
  const l = lasers.find((x) => x.id === id)
  if (l) Object.assign(l, partial)
}

export async function addLaser(name, ip) {
  await api('/lasers', { method: 'POST', body: JSON.stringify({ name, ip }) })
  await fetchLasers()
}

export async function updateLaser(id, partial) {
  markLaserLocalChange()
  await api(`/lasers/${id}`, { method: 'POST', body: JSON.stringify(partial) })
  await fetchLasers()
}

// Replaces a laser's complete safety mask list - see POST
// /api/lasers/<id>/masks and LaserMask in backend/output_pipeline.h. Each
// mask: {x0, y0, x1, y1} in that laser's output space (-1..1, y up),
// reduce (0..100 % of light removed inside), outline (draw the border in red
// on the real laser; runtime-only, never persisted by the backend).
// `keepalive` lets the request finish even while the page/popup is being
// closed (used to switch red setup outlines off on close).
export async function updateLaserMasks(id, masks, { keepalive = false } = {}) {
  markLaserLocalChange()
  await api(`/lasers/${id}/masks`, { method: 'POST', body: JSON.stringify({ masks }), keepalive })
  if (!keepalive) await fetchLasers()
}

export async function deleteLaser(id) {
  await api(`/lasers/${id}`, { method: 'DELETE' })
  await fetchLasers()
}

export async function reorderLasers(ids) {
  markLaserLocalChange()
  const byId = new Map(lasers.map((entry) => [entry.id, entry]))
  const ordered = ids.map((id) => byId.get(Number(id))).filter(Boolean)
  if (ordered.length !== lasers.length) throw new Error('Laser order must include every configured laser')
  applyLasers(ordered)
  try {
    await api('/lasers/order', { method: 'POST', body: JSON.stringify({ ids: ordered.map((entry) => entry.id) }) })
    await fetchLasers()
  } catch (error) {
    await fetchLasers().catch(() => {})
    throw error
  }
}

// Momentary flash: forces color to white *and* full brightness only while
// held, restores the exact prior color and brightness on release - mirrors
// the MIDI "flash" button (including a footswitch) so it behaves
// identically from any input. Not routed through /api/state since it's a
// transient action, not a persisted field.
export async function flashPress() {
  markLocalChange()
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await (await fetch('/flash/1', { method: 'POST' })).json())
}
export async function flashRelease() {
  markLocalChange()
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await (await fetch('/flash/0', { method: 'POST' })).json())
}

// Opens/closes the master brightness gate (see laserState.brightness_gate_open
// above) - the UI's equivalent of holding/releasing a footswitch wired to
// CC64. Unlike flash, this is a persistent toggle, not momentary.
export async function setBrightnessGate(open) {
  markLocalChange()
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await (await fetch(`/brightness/gate/${open ? 1 : 0}`, { method: 'POST' })).json())
}

// Snaps the rotation angle back to 0 and stops spinning (rotation_speed 0) -
// same as the MIDI "rotation_reset" action, see POST /rotation/reset.
export async function rotationReset() {
  markLocalChange()
  laserState.rotation_speed = 0
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await (await fetch('/rotation/reset', { method: 'POST' })).json())
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
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await api(`/cue/${n}/recall`, { method: 'POST' }))
}

// Fields a cue stores (the look). Beam settings - scan rate, dot density,
// dot timing, flicker, output latency - are global, not part of cues.
const CUE_STATE_FIELDS = [
  'shape', 'radius', 'points', 'r', 'g', 'b', 'shape_scale',
  'pos_x', 'pos_y', 'rotation_speed', 'mirror_x', 'move_mode', 'move_speed',
  'move_size', 'move_phase_degrees', 'wave_frequency', 'wave_amplitude', 'wave_speed',
  'rainbow_amount', 'rainbow_speed', 'rainbow_phase_degrees',
]

// True when cue slot n is populated and every saved field equals the live state.
export function isCueCurrent(n) {
  const cue = cues[n]
  if (!cue) return false
  return CUE_STATE_FIELDS.every((field) => {
    const saved = cue[field]
    const current = laserState[field]
    if (typeof saved === 'number' && typeof current === 'number') {
      return Math.abs(saved - current) <= 0.00011
    }
    return saved === current
  })
}

// Momentary cue: active only while held. Handled by the daemon (POST
// /api/cue/<n>/momentary/<1|0>, same as the MIDI "cue_momentary" action):
// the first press remembers the current look *and rotation angle*, the
// final release puts both back (global settings like master brightness,
// blackout and release time aren't touched). Several momentary cues can be
// held at once (multitouch): the most recently pressed one shows; releasing
// it falls back to the previous still-held one, and only releasing the last
// one restores the original look. Requests are chained so the daemon always
// sees presses and releases in the order they happened.
const heldMomentaryCues = [] // cue numbers in press order
let momentaryChain = Promise.resolve()
function momentaryRequest(n, on) {
  const run = async () => {
    markLocalChange()
    const seq = nextStateSeq()
    applyStateIfCurrent(seq, await api(`/cue/${n}/momentary/${on ? 1 : 0}`, { method: 'POST' }))
  }
  const next = momentaryChain.then(run)
  momentaryChain = next.catch(() => {})
  return next
}
export function momentaryCuePress(n) {
  heldMomentaryCues.push(n)
  return momentaryRequest(n, true)
}
export function momentaryCueRelease(n) {
  const i = heldMomentaryCues.lastIndexOf(n)
  if (i < 0) return Promise.resolve()
  heldMomentaryCues.splice(i, 1)
  if (heldMomentaryCues.length) return momentaryRequest(heldMomentaryCues[heldMomentaryCues.length - 1], true)
  return momentaryRequest(n, false)
}

// Cue layers: play cue n *on top of* the current look instead of replacing
// it (Layer playback) - mode 'on' | 'off' | 'toggle'. See POST
// /api/cue/<n>/layer/<mode> and ShowLayer in laser_daemon.cpp.
export async function setCueLayer(n, mode) {
  markLocalChange()
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await api(`/cue/${n}/layer/${mode}`, { method: 'POST' }))
}
export async function clearLayers() {
  markLocalChange()
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await api('/layers/clear', { method: 'POST' }))
}

export async function clearCue(n) {
  await api(`/cue/${n}/clear`, { method: 'POST' })
  await fetchCues()
}

export async function moveCue(from, to) {
  await api(`/cue/${from}/move/${to}`, { method: 'POST' })
  await fetchCues()
}

// ── Magic Touch layout (shared by all clients, see /api/magic-touch) ──────────
// Last layout seen from / sent to the backend, kept at module level so the
// builder can show it instantly when it is mounted again.
export const magicTouch = { rev: null, layout: null }
// Bumped by every save, so a fetch that was already in flight during a save can't
// bring back the layout from before it.
let magicSaveSeq = 0

// Returns true when the backend has a layout revision this client hasn't seen yet.
export async function fetchMagicTouch() {
  const seq = magicSaveSeq
  const data = await api(`/magic-touch${magicTouch.rev === null ? '' : `?rev=${magicTouch.rev}`}`)
  if (seq !== magicSaveSeq || !('layout' in data)) return false
  magicTouch.rev = data.rev
  magicTouch.layout = data.layout
  return true
}

// `body` is the already serialized layout object.
// The cache is updated before the request so a quick remount already shows the new layout.
export async function saveMagicTouch(body) {
  magicSaveSeq++
  magicTouch.layout = JSON.parse(body)
  const data = await api('/magic-touch', { method: 'POST', body })
  magicTouch.rev = data.rev
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
    api('/lasers').then(applyPolledLasers).catch(() => {})
  }, intervalMs)
}
export function stopStatusPolling() {
  clearInterval(statusPoll)
  statusPoll = null
}
