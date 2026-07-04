// Talks directly to the C++ laser_daemon (port 8000).
// API:  GET/POST /api/state   POST /laser/shape/<s>
//       POST /laser/connect/<ip>  POST /laser/disconnect
//       WS   /ws/points
import { reactive } from 'vue'

export const laserState = reactive({
  shape:       'circle',
  radius:      0.7,
  points:      300,
  rate_kpps:   30,
  intensity:   1.0,
  r:           0.0,
  g:           1.0,
  b:           0.314,
  armed:       false,
  ip:          '',
  wsConnected: false,
  error:       null,
})

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

// ── Public API ─────────────────────────────────────────────────────────────────

export async function fetchState() {
  applyState(await api('/state'))
}

export async function updateState(partial) {
  applyState(await api('/state', { method: 'POST', body: JSON.stringify(partial) }))
}

export async function setShape(shape) {
  await laser(`/shape/${shape}`, { method: 'POST' })
  laserState.shape = shape
}

export async function setBrightness(v) {
  applyState(await laser(`/brightness/${v}`, { method: 'POST' }))
}

export async function connectLaser(ip) {
  applyState(await laser(`/connect/${encodeURIComponent(ip)}`, { method: 'POST' }))
}

export async function disconnectLaser() {
  applyState(await laser('/disconnect', { method: 'POST' }))
}

// ── Status polling ─────────────────────────────────────────────────────────────

let statusPoll = null
export function startStatusPolling(intervalMs = 2000) {
  if (statusPoll) return
  statusPoll = setInterval(() => fetchState().catch(() => {}), intervalMs)
}
export function stopStatusPolling() {
  clearInterval(statusPoll)
  statusPoll = null
}
