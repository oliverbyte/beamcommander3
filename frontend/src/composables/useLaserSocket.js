// Talks directly to the C++ laser_daemon (port 8000).
// API:  GET/POST /api/state   POST /laser/shape/<s>
//       POST /laser/connect/<ip>  POST /laser/disconnect
//       GET /api/cues  POST /api/cue/<n>/save|recall|clear
//       WS   /ws/points
import { reactive, watch } from 'vue'

export const laserState = reactive({
  // Shape
  shape:           'circle',
  // ILDA file look (shape 'ilda'): stored file, original name, animation fps
  ilda_file:       '',
  ilda_name:       '',
  ilda_fps:        30,
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
  // Every laser's own look (laser id -> look fields, "0" = the virtual
  // preview laser while no laser is configured) - see GET /api/state. The
  // look fields above always show the look of the first laser in this
  // browser's selection (projectSelection()), so every control displays and
  // edits "the selected lasers" without knowing about selections itself.
  looks: {},
  // ILDA test pattern on every armed laser ('off' or an id from
  // GET /api/test-patterns) and its size (0.05..1) - runtime only.
  test_pattern:    'off',
  test_pattern_size: 1.0,
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
  projectSelection()
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
  projectSelection()
}

// ── Laser selection (per browser) ──────────────────────────────────────────────
// Which lasers the look controls, cues and Magic Touch act on - kept in this
// browser only (localStorage), never on the daemon, so operators on
// different devices don't change each other's selection. Empty = all
// lasers. Every look change and cue action sends it as `lasers`; the daemon
// then changes only those lasers' looks (see request_lasers() in
// laser_daemon.cpp).
const SELECTION_KEY = 'bc3.laserSelection'
function loadSelection() {
  try {
    const ids = JSON.parse(localStorage.getItem(SELECTION_KEY))
    return Array.isArray(ids) ? ids.filter(Number.isInteger) : []
  } catch (_) { return [] }
}
export const laserSelection = reactive({ ids: loadSelection() })

// Selected lasers that still exist, in Lasers-list order ([] = all).
export function selectedLaserIds() {
  const chosen = new Set(laserSelection.ids)
  return lasers.filter((l) => chosen.has(l.id)).map((l) => l.id)
}
export function setLaserSelection(ids) {
  laserSelection.ids = [...ids]
  try { localStorage.setItem(SELECTION_KEY, JSON.stringify(laserSelection.ids)) } catch (_) {}
  projectSelection()
}
export function toggleLaserSelected(id) {
  const ids = selectedLaserIds()
  setLaserSelection(ids.includes(id) ? ids.filter((x) => x !== id) : [...ids, id])
}
// The looks the controls show and change: the selection, else every laser
// (or the virtual preview look without lasers).
export function targetLookIds() {
  const selected = selectedLaserIds()
  if (selected.length) return selected
  return lasers.length ? lasers.map((l) => l.id) : [0]
}
// `lasers` for a request: the selection, or null = all lasers.
function selectionTargets() {
  const selected = selectedLaserIds()
  return selected.length ? selected : null
}
function withLasers(path) {
  const targets = selectionTargets()
  return targets ? `${path}${path.includes('?') ? '&' : '?'}lasers=${targets.join(',')}` : path
}

// Fields of a laser's look (GET /api/state "looks").
export const LOOK_FIELDS = [
  'shape', 'ilda_file', 'ilda_name', 'ilda_fps', 'radius', 'points', 'r', 'g', 'b', 'shape_scale',
  'pos_x', 'pos_y', 'rotation_speed', 'mirror_x', 'move_mode', 'move_speed', 'move_size', 'move_phase_degrees',
  'wave_frequency', 'wave_amplitude', 'wave_speed', 'rainbow_amount', 'rainbow_speed', 'rainbow_phase_degrees',
  'dot_amount', 'flicker_hz', 'flicker_on_ms', 'flicker_phase_degrees', 'layers',
]
const LOOK_FIELD_SET = new Set(LOOK_FIELDS)
// Shows the first targeted laser's look in the top-level fields.
function projectSelection() {
  const look = laserState.looks?.[targetLookIds()[0]]
  if (!look) return
  for (const key of LOOK_FIELDS) if (key in look) laserState[key] = look[key]
}
// Local echo of a look change on every targeted laser, so "mixed" marks and
// the projection agree with the change before the daemon answers.
function patchLocalLooks(partial) {
  const lookPart = Object.fromEntries(Object.entries(partial).filter(([k]) => LOOK_FIELD_SET.has(k)))
  if (!Object.keys(lookPart).length || !laserState.looks) return
  for (const id of targetLookIds()) if (laserState.looks[id]) Object.assign(laserState.looks[id], lookPart)
}
function sameValue(a, b) {
  if (typeof a === 'number' && typeof b === 'number') return Math.abs(a - b) <= 0.00011
  if (Array.isArray(a) && Array.isArray(b)) return a.length === b.length && a.every((v, i) => v === b[i])
  return a === b
}
// True when the targeted lasers' looks differ in `key` (the control then
// shows the first laser's value and is marked "mixed").
export function isLookMixed(key) {
  const values = targetLookIds().map((id) => laserState.looks?.[id]?.[key]).filter((v) => v !== undefined)
  return values.some((v) => !sameValue(v, values[0]))
}
// Every value `key` has across the targeted lasers (e.g. all their shapes).
export function lookValues(key) {
  return new Set(targetLookIds().map((id) => laserState.looks?.[id]?.[key]).filter((v) => v !== undefined))
}
watch(() => [laserSelection.ids.join(','), lasers.map((l) => l.id).join(',')], projectSelection)

// ── Public API ─────────────────────────────────────────────────────────────────

export async function fetchState() {
  applyState(await api('/state'))
}

// Global settings change for everyone; look fields only on the selected
// lasers (the daemon sorts the keys).
export async function updateState(partial) {
  markLocalChange()
  patchLocalLooks(partial)
  const seq = nextStateSeq()
  const targets = selectionTargets()
  const body = targets ? { ...partial, lasers: targets } : partial
  applyStateIfCurrent(seq, await api('/state', { method: 'POST', body: JSON.stringify(body) }))
}

// Resets the selected lasers' looks (all lasers: also master brightness).
export async function resetState() {
  markLocalChange()
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await api(withLasers('/reset'), { method: 'POST' }))
}

export async function setShape(shape) {
  markLocalChange()
  patchLocalLooks({ shape })
  await laser(withLasers(`/shape/${shape}`), { method: 'POST' })
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

// ── Test patterns ────────────────────────────────────────────────────────────
// The official ILDA test patterns the daemon can output ({id, label, kpps},
// see backend/test_patterns.h). While one is active, every armed laser shows
// it instead of the show, at exactly `kpps` (the ILDA standard requires it).
export const testPatterns = reactive([])

export async function fetchTestPatterns() {
  testPatterns.splice(0, testPatterns.length, ...(await api('/test-patterns')))
}

// partial: { pattern: 'off' | <id> } and/or { size: 0.05..1 }. Rejected by
// the daemon if the pattern needs more than the max scan rate.
export async function setTestPattern(partial) {
  markLocalChange()
  const seq = nextStateSeq()
  const res = await fetch('/api/test-pattern', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(partial),
  })
  const data = await res.json().catch(() => ({}))
  if (!res.ok) throw new Error(data.error || `/api/test-pattern → ${res.status}`)
  applyStateIfCurrent(seq, data)
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
  applyStateIfCurrent(seq, await (await fetch(withLasers('/rotation/reset'), { method: 'POST' })).json())
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
  await api(withLasers(`/cue/${n}/save`), { method: 'POST' })
  await fetchCues()
}

export async function recallCue(n) {
  markLocalChange()
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await api(withLasers(`/cue/${n}/recall`), { method: 'POST' }))
}

// Fields a cue stores (the look, incl. dots and flicker). The Beam page's
// projector settings - scan rate, max rate, output latency, dot dwell/blanking -
// are global, not part of cues.
const CUE_STATE_FIELDS = [
  'shape', 'ilda_file', 'radius', 'points', 'r', 'g', 'b', 'shape_scale',
  'pos_x', 'pos_y', 'rotation_speed', 'mirror_x', 'move_mode', 'move_speed',
  'move_size', 'move_phase_degrees', 'wave_frequency', 'wave_amplitude', 'wave_speed',
  'rainbow_amount', 'rainbow_speed', 'rainbow_phase_degrees',
  'dot_amount', 'flicker_hz', 'flicker_on_ms', 'flicker_phase_degrees',
]

// True when recalling cue n onto the selected lasers would change nothing:
// every selected laser already shows the cue's look for it (its own saved
// look, or the cue's main look - see CueData in laser_daemon.cpp).
export function isCueCurrent(n) {
  const cue = cues[n]
  if (!cue) return false
  return targetLookIds().every((id) => {
    const current = laserState.looks?.[id] || laserState
    const saved = cue.lasers?.[id] || cue
    return CUE_STATE_FIELDS.every((field) => sameValue(saved[field], current[field]))
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
    const path = `/cue/${n}/momentary/${on ? 1 : 0}`
    applyStateIfCurrent(seq, await api(on ? withLasers(path) : path, { method: 'POST' }))
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
  applyStateIfCurrent(seq, await api(withLasers(`/cue/${n}/layer/${mode}`), { method: 'POST' }))
}
export async function clearLayers() {
  markLocalChange()
  const seq = nextStateSeq()
  applyStateIfCurrent(seq, await api(withLasers('/layers/clear'), { method: 'POST' }))
}

// Drag & drop of an ILDA file onto a cue slot: the daemon checks and stores
// the file, turns slot n into an ILDA look playing it (replacing the slot)
// and recalls it right away - see POST /api/cue/<n>/ilda.
export async function uploadIldaCue(n, file) {
  markLocalChange()
  const seq = nextStateSeq()
  const res = await fetch(withLasers(`/api/cue/${n}/ilda?name=${encodeURIComponent(file.name)}`), {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: file,
  })
  const data = await res.json().catch(() => ({}))
  if (!res.ok) throw new Error(data.error || `upload failed (${res.status})`)
  applyStateIfCurrent(seq, data)
  await fetchCues()
}

// Thumbnail data of a stored ILDA file ({frames, points:[[x,y,r,g,b],...]},
// first frame) - cached per file; stored files never change (content-hash names).
const ildaPreviewCache = new Map()
export function fetchIldaPreview(file) {
  if (!ildaPreviewCache.has(file)) {
    ildaPreviewCache.set(file, api(`/ilda/${file}/preview`).catch((e) => {
      ildaPreviewCache.delete(file)
      throw e
    }))
  }
  return ildaPreviewCache.get(file)
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
