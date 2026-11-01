<template>
  <canvas ref="canvasEl" class="laser-canvas" v-show="enabled"></canvas>
  <div v-if="enabled && lasers.length" class="laser-layout" aria-label="Configured lasers">
    <button
      v-for="entry in lasers"
      :key="entry.id"
      type="button"
      class="laser-marker"
      :class="{ 'is-armed': entry.armed, 'is-connected': entry.connected }"
      :style="positionStyle(entry)"
      :title="`Drag to arrange ${entry.name}`"
      @pointerdown="startLaserDrag($event, entry)"
    >
      <span class="laser-marker-status" aria-hidden="true"></span>
      <span class="laser-marker-name">{{ entry.name }}</span>
      <span class="laser-marker-grip" aria-hidden="true"><i></i><i></i><i></i></span>
    </button>
  </div>
</template>

<script setup>
import { onMounted, onBeforeUnmount, ref, watch } from 'vue'
import * as THREE from 'three'
import { OrbitControls } from 'three/addons/controls/OrbitControls.js'
import { fetchLasers, lasers, laserState, onPoints, patchLaserLocal, updateLaser } from '../composables/useLaserSocket.js'

const props = defineProps({
  persistenceMs: { type: Number, default: 20 },
  // Purely a local, per-browser-tab preview toggle (see App.vue) - never
  // sent to the backend, so it never affects the real hardware output or
  // any other connected browser client's preview.
  enabled: { type: Boolean, default: true },
})

const canvasEl = ref(null)

// World-space scale: the backend reports x/y as a 0..1 fraction of full
// scanner range; we map that to this many scene units for the visualisation.
const WORLD_SCALE = 8
// Fixed distance from the fixture to the projected "screen" plane. Using a
// fixed length (instead of deriving it from the live camera distance) keeps
// the pattern plane's world position stable, so the default camera can be
// placed to always look straight down the beam axis at it.
const BEAM_LEN = 22
const MAX_SCAN_POINTS = 3000
const PREVIEW_VIEW_HEIGHT = 18
const FRONT_PLANE_Z = -8
const PREVIEW_FOG_DENSITY = 0.008
const PREVIEW_FOG_REFERENCE_DISTANCE = 28
const FRONT_PLANE = new THREE.Plane(new THREE.Vector3(0, 0, 1), -FRONT_PLANE_Z)

let renderer, scene, camera, controls, laserTemplate
let dust
let animationId = 0
const laserSources = new Map()
const laserStreams = new Map()
const sourceRaycaster = new THREE.Raycaster()
const sourceScreenPoint = new THREE.Vector2()
const sourceWorldPosition = new THREE.Vector3()
const screenRight = new THREE.Vector3()
const screenUp = new THREE.Vector3()
const rayDirection = new THREE.Vector3()
const sourceRelative = new THREE.Vector3()
const continuationEndpoint = new THREE.Vector3()
let edgeViewWidth = 1
let edgeViewHeight = 1
const drag = { id: null, offsetX: 0, offsetY: 0, marginX: 0, marginY: 0, moved: false }

function previewPosition(entry) {
  const index = lasers.findIndex((laser) => laser.id === entry.id)
  return {
    x: Number.isFinite(entry.preview_x) ? entry.preview_x : (index + 1) / (lasers.length + 1),
    y: Number.isFinite(entry.preview_y) ? entry.preview_y : 0.5,
  }
}

function positionStyle(entry) {
  const position = previewPosition(entry)
  return { left: `${position.x * 100}%`, top: `${position.y * 100}%` }
}

function createLaserStream(id) {
  const spokePositions = new Float32Array(MAX_SCAN_POINTS * 12)
  const spokeColors = new Float32Array(MAX_SCAN_POINTS * 12)
  const spokeBaseColors = new Float32Array(MAX_SCAN_POINTS * 12)
  const spokeTimestamps = new Float64Array(MAX_SCAN_POINTS).fill(-Infinity)
  const spokeGeo = new THREE.BufferGeometry()
  spokeGeo.setDrawRange(0, 0)
  spokeGeo.setAttribute('position', new THREE.BufferAttribute(spokePositions, 3).setUsage(THREE.DynamicDrawUsage))
  spokeGeo.setAttribute('color', new THREE.BufferAttribute(spokeColors, 3).setUsage(THREE.DynamicDrawUsage))

  const hazePositions = new Float32Array(MAX_SCAN_POINTS * 9)
  const hazeBaseColors = new Float32Array(MAX_SCAN_POINTS * 9)
  const hazeColors = new Float32Array(MAX_SCAN_POINTS * 9)
  const hazeGeo = new THREE.BufferGeometry()
  hazeGeo.setDrawRange(0, 0)
  hazeGeo.setAttribute('position', new THREE.BufferAttribute(hazePositions, 3).setUsage(THREE.DynamicDrawUsage))
  hazeGeo.setAttribute('color', new THREE.BufferAttribute(hazeColors, 3).setUsage(THREE.DynamicDrawUsage))

  const hazeMaterial = new THREE.MeshBasicMaterial({
    vertexColors: true, transparent: true,
    blending: THREE.AdditiveBlending, depthWrite: false,
    side: THREE.DoubleSide, fog: false, opacity: 0.02,
  })
  const output = new THREE.Group()
  output.name = 'laser-output'
  const hazeMesh = new THREE.Mesh(hazeGeo, hazeMaterial)
  hazeMesh.name = 'laser-haze'
  output.add(hazeMesh)

  const spokeMaterial = new THREE.LineBasicMaterial({
    vertexColors: true, transparent: true,
    blending: THREE.AdditiveBlending, depthWrite: false, fog: false,
  })
  const spokes = new THREE.LineSegments(spokeGeo, spokeMaterial)
  spokes.name = 'laser-rays'
  output.add(spokes)

  return {
    id, output, currentFrame: [], emitIndex: 0, lastEmitTime: 0,
    lastPoint: [0, 0], lastColor: [0, 0, 0], spokeWriteIndex: 0, spokeCount: 0,
    spokeGeo, spokePositions, spokeColors, spokeBaseColors, spokeTimestamps, spokeMaterial,
    hazeGeo, hazePositions, hazeBaseColors, hazeColors, hazeMaterial,
    edgeX: 0.5, edgeY: 0.5, mirrorX: false,
  }
}

function disposeLaserStream(stream) {
  stream.spokeGeo.dispose()
  stream.spokeMaterial.dispose()
  stream.hazeGeo.dispose()
  stream.hazeMaterial.dispose()
}

function syncLaserSources() {
  if (!scene || !laserTemplate) return
  const entries = lasers.length ? lasers : [{ id: '__virtual__', preview_x: 0.5, preview_y: 0.5 }]
  const desiredIds = new Set(entries.map((entry) => entry.id))

  for (const [id, source] of laserSources) {
    if (desiredIds.has(id)) continue
    scene.remove(source)
    laserSources.delete(id)
    const stream = laserStreams.get(id)
    if (stream) disposeLaserStream(stream)
    laserStreams.delete(id)
  }
  for (const entry of entries) {
    if (laserSources.has(entry.id)) continue
    const source = laserTemplate.clone(true)
    source.name = `preview-laser-${entry.id}`
    const stream = createLaserStream(entry.id)
    source.add(stream.output)
    scene.add(source)
    laserSources.set(entry.id, source)
    laserStreams.set(entry.id, stream)
  }
}

function positionLaserSources() {
  if (!camera || !controls) return
  camera.updateMatrixWorld()

  for (const [id, source] of laserSources) {
    const entry = id === '__virtual__' ? { preview_x: 0.5, preview_y: 0.5 } : lasers.find((laser) => laser.id === id)
    if (!entry) continue
    const output = source.getObjectByName('laser-output')
    if (output) {
      output.visible = entry.armed !== false
      output.scale.x = entry.mirror_x === true ? -1 : 1
    }
    const position = previewPosition(entry)
    sourceScreenPoint.set(position.x * 2 - 1, 1 - position.y * 2)
    sourceRaycaster.setFromCamera(sourceScreenPoint, camera)
    if (sourceRaycaster.ray.intersectPlane(FRONT_PLANE, sourceWorldPosition)) {
      source.position.copy(sourceWorldPosition)
      source.rotation.set(0, 0, 0)
    }
  }
}

function startLaserDrag(event, entry) {
  if (event.button !== 0) return
  event.preventDefault()
  event.stopPropagation()

  const bounds = canvasEl.value.getBoundingClientRect()
  const position = previewPosition(entry)
  drag.id = entry.id
  drag.offsetX = position.x * bounds.width - (event.clientX - bounds.left)
  drag.offsetY = position.y * bounds.height - (event.clientY - bounds.top)
  drag.marginX = event.currentTarget.offsetWidth / (2 * bounds.width)
  drag.marginY = event.currentTarget.offsetHeight / (2 * bounds.height)
  drag.moved = false
  window.addEventListener('pointermove', moveLaser)
  window.addEventListener('pointerup', endLaserDrag)
  window.addEventListener('pointercancel', endLaserDrag)
}

function moveLaser(event) {
  if (drag.id === null) return
  const bounds = canvasEl.value.getBoundingClientRect()
  const preview_x = Math.min(1 - drag.marginX, Math.max(drag.marginX, (event.clientX - bounds.left + drag.offsetX) / bounds.width))
  const preview_y = Math.min(1 - drag.marginY, Math.max(drag.marginY, (event.clientY - bounds.top + drag.offsetY) / bounds.height))
  drag.moved = true
  patchLaserLocal(drag.id, { preview_x, preview_y })
}

function endLaserDrag() {
  if (drag.id === null) return
  const id = drag.id
  const entry = lasers.find((laser) => laser.id === id)
  drag.id = null
  window.removeEventListener('pointermove', moveLaser)
  window.removeEventListener('pointerup', endLaserDrag)
  window.removeEventListener('pointercancel', endLaserDrag)
  if (drag.moved && entry) {
    updateLaser(id, { preview_x: entry.preview_x, preview_y: entry.preview_y }).catch(() => fetchLasers())
  }
}

function setupScene() {
  renderer = new THREE.WebGLRenderer({ canvas: canvasEl.value, antialias: true })
  renderer.setSize(window.innerWidth, window.innerHeight)
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))

  scene = new THREE.Scene()
  scene.background = new THREE.Color(0x020208)

  // The laser fixture sits at (0, 4.2, -8) and shoots down its local +Z
  // axis, so the projected pattern plane sits at world z = -8 + BEAM_LEN.
  // The default camera sits out on the audience side of the beam (past
  // the pattern plane), looking back toward the fixture - i.e. the beam
  // is aimed straight at the viewer, like standing in the crowd at a real
  // show and looking directly into the projector ("audience blinding"),
  // rather than a bystander's 3/4 side angle or a view from behind the
  // fixture looking away from the audience. Because the beam is drawn
  // purely as additive line spokes (no silhouette geometry), there's no
  // degenerate-view artifact from staring straight down it either way.
  // OrbitControls lets the viewer rotate away from there if they want to.
  const halfHeight = PREVIEW_VIEW_HEIGHT / 2
  const halfWidth = halfHeight * window.innerWidth / window.innerHeight
  camera = new THREE.OrthographicCamera(-halfWidth, halfWidth, halfHeight, -halfHeight, 0.1, 500)
  camera.position.set(0, 4.2, FRONT_PLANE_Z + BEAM_LEN + 6)

  controls = new OrbitControls(camera, renderer.domElement)
  controls.target.set(0, 4.2, FRONT_PLANE_Z)
  controls.enableDamping = true
  controls.maxPolarAngle = Math.PI * 0.62
  controls.minDistance = 4
  controls.maxDistance = 90
  controls.update()
  const cameraDistance = camera.position.distanceTo(controls.target)
  scene.fog = new THREE.FogExp2(
    0x020208,
    PREVIEW_FOG_DENSITY * PREVIEW_FOG_REFERENCE_DISTANCE / cameraDistance
  )

  const floor = new THREE.Mesh(
    new THREE.PlaneGeometry(160, 160),
    new THREE.MeshBasicMaterial({ color: 0x04050a })
  )
  floor.rotation.x = -Math.PI / 2
  scene.add(floor)

  const grid = new THREE.GridHelper(160, 80, 0x11131f, 0x0a0c16)
  grid.position.y = 0.01
  scene.add(grid)

  const backWall = new THREE.Mesh(
    new THREE.PlaneGeometry(160, 40),
    new THREE.MeshBasicMaterial({ color: 0x03040a })
  )
  backWall.position.set(0, 20, -12)
  scene.add(backWall)

  const truss = new THREE.Mesh(
    new THREE.BoxGeometry(14, 0.25, 0.25),
    new THREE.MeshBasicMaterial({ color: 0x14161f })
  )
  truss.position.set(0, 4.6, FRONT_PLANE_Z)
  scene.add(truss)

  laserTemplate = new THREE.Group()

  const housing = new THREE.Mesh(
    new THREE.BoxGeometry(0.55, 0.4, 0.7),
    new THREE.MeshBasicMaterial({ color: 0x1a1d28 })
  )
  housing.position.z = -0.36
  laserTemplate.add(housing)

  // The visible beam is rendered purely as a fan of lines from the source
  // (local origin) out to each actually-scanned point - there is no
  // separate flat "projected shape" outline drawn anywhere. This matches
  // a real aerial beam show: there is no 2D image in the air, only
  // individual light rays passing through haze; the viewer's own
  // persistence of vision is what makes the pattern legible, exactly as
  // it does here via the age-based fade below. Each spoke's shape and
  // density is driven entirely by the real scanned points, so it always
  // matches whatever pattern is currently projected (a full circle fills
  // in solidly, a thin line/wave only lights up a narrow sliver, etc).
  // Each spoke is actually TWO collinear segments: source -> shape point
  // (bright to dim), then shape point -> a far continuation point well
  // beyond it (dim fading all the way to fully black). Real laser beams
  // don't stop at the pattern - they keep travelling on into the haze/sky
  // until they're no longer visible; fading the continuation to zero
  // rather than drawing a hard endpoint there reproduces that look
  // instead of every ray visibly terminating in mid-air.

  const DUST_COUNT = 600
  const dustPos = new Float32Array(DUST_COUNT * 3)
  for (let i = 0; i < DUST_COUNT; i++) {
    dustPos[i * 3 + 0] = (Math.random() - 0.5) * 40
    dustPos[i * 3 + 1] = Math.random() * 10
    dustPos[i * 3 + 2] = -10 + Math.random() * 40
  }
  const dustGeo = new THREE.BufferGeometry()
  dustGeo.setAttribute('position', new THREE.BufferAttribute(dustPos, 3))
  const dustMat = new THREE.PointsMaterial({
    color: 0x99aadd, size: 0.05, transparent: true, opacity: 0.25,
    blending: THREE.AdditiveBlending, depthWrite: false,
  })
  dust = new THREE.Points(dustGeo, dustMat)
  scene.add(dust)
  syncLaserSources()
}

// The backend sends one complete shape/frame (all points of one revolution)
// every ~33ms - a snapshot, not a continuous point stream. A real Ether
// Dream DAC loops that exact same point buffer continuously at rate_kpps
// points/sec until a new buffer arrives. To make the preview point-for-point
// faithful to that real behaviour (not just flash the whole frame in sync
// every 33ms), we replay the latest received frame locally at the true scan
// rate: a virtual scan head advances through `currentFrame` at
// `rate_kpps*1000` points/sec, looping forever, exactly like the DAC does.
const MAX_EMIT_PER_TICK = 4000  // safety cap (e.g. tab was backgrounded)

function handleIncomingPoints(msg) {
  for (const [id, stream] of laserStreams) {
    stream.currentFrame = msg.laser_pts?.[id] || msg.pts || []
  }
}

function prepareContinuation() {
  camera.updateMatrixWorld()
  screenRight.setFromMatrixColumn(camera.matrixWorld, 0).normalize()
  screenUp.setFromMatrixColumn(camera.matrixWorld, 1).normalize()

  edgeViewWidth = (camera.right - camera.left) / camera.zoom
  edgeViewHeight = (camera.top - camera.bottom) / camera.zoom
  for (const [id, stream] of laserStreams) {
    const entry = id === '__virtual__' ? null : lasers.find((laser) => laser.id === id)
    const source = laserSources.get(id)
    if (!source) continue
    sourceRelative.subVectors(source.position, camera.position)
    stream.edgeX = 0.5 + sourceRelative.dot(screenRight) / edgeViewWidth
    stream.edgeY = 0.5 - sourceRelative.dot(screenUp) / edgeViewHeight
    stream.mirrorX = entry?.mirror_x === true
  }
}

function continuationEnd(wx, wy, stream) {
  rayDirection.set(wx, wy, BEAM_LEN)
  const screenDx = rayDirection.dot(screenRight)
  const screenDy = rayDirection.dot(screenUp)
  const normalizedDx = (stream.mirrorX ? -screenDx : screenDx) / edgeViewWidth
  const normalizedDy = -screenDy / edgeViewHeight
  let factor = Infinity
  if (normalizedDx > 1e-8) factor = Math.min(factor, (1 - stream.edgeX) / normalizedDx)
  else if (normalizedDx < -1e-8) factor = Math.min(factor, -stream.edgeX / normalizedDx)
  if (normalizedDy > 1e-8) factor = Math.min(factor, (1 - stream.edgeY) / normalizedDy)
  else if (normalizedDy < -1e-8) factor = Math.min(factor, -stream.edgeY / normalizedDy)
  if (!Number.isFinite(factor) || factor < 1) factor = 1

  return continuationEndpoint.copy(rayDirection)
    .addScaledVector(screenRight, screenDx * (factor - 1))
    .addScaledVector(screenUp, screenDy * (factor - 1))
}

// Advances the virtual scan head and writes each newly "visited" point as
// one spoke (source -> point) into the ring buffer with a real timestamp
// spaced 1/pointRateHz apart - exactly reproducing how a physical scanner
// would have traced them in time.
function emitScanPoints(now) {
  for (const stream of laserStreams.values()) emitLaserScan(stream, now)
}

function emitLaserScan(stream, now) {
  if (stream.currentFrame.length === 0) return
  const pointRateHz = Math.max(1, laserState.rate_kpps * 1000)
  const period = 1 / pointRateHz

  let n = Math.floor((now - stream.lastEmitTime) / period)
  if (n <= 0) return
  if (n > MAX_EMIT_PER_TICK) {
    // Fell far behind (backgrounded tab, GC pause, ...) - skip ahead instead
    // of trying to replay a huge backlog.
    stream.lastEmitTime = now - MAX_EMIT_PER_TICK * period
    n = MAX_EMIT_PER_TICK
  }

  for (let k = 0; k < n; k++) {
    const frame = stream.currentFrame
    const idx = Math.floor(stream.emitIndex) % frame.length
    const [x, y, r, g, b] = frame[idx]
    const t = stream.lastEmitTime + (k + 1) * period

    const wx = x * WORLD_SCALE
    const wy = y * WORLD_SCALE
    // Daemon sends 0..1 floats; simulation sends 0..255 integers.
    const scale = (r > 1 || g > 1 || b > 1) ? 1 / 255 : 1
    const cr = r * scale, cg = g * scale, cb = b * scale
    const fanBrightness = Math.min(1, Math.max(0.01, 5 / frame.length) * laserState.intensity)

    // Spoke: source (local origin) -> shape point -> far continuation.
    // Segment 1 (source->point) fades bright-to-dim; segment 2
    // (point->far) continues fading from dim all the way to black, so the
    // ray appears to travel on indefinitely with no visible endpoint,
    // instead of hard-stopping exactly at the shape.
    const vi = stream.spokeWriteIndex * 12
    const endpoint = continuationEnd(wx, wy, stream)
    const fi = stream.spokeWriteIndex * 9
    stream.hazePositions[fi] = 0
    stream.hazePositions[fi + 1] = 0
    stream.hazePositions[fi + 2] = 0
    stream.hazePositions[fi + 3] = stream.lastPoint[0] * WORLD_SCALE
    stream.hazePositions[fi + 4] = stream.lastPoint[1] * WORLD_SCALE
    stream.hazePositions[fi + 5] = BEAM_LEN
    stream.hazePositions[fi + 6] = wx
    stream.hazePositions[fi + 7] = wy
    stream.hazePositions[fi + 8] = BEAM_LEN
    stream.hazeBaseColors[fi] = (stream.lastColor[0] + cr) * 0.5 * fanBrightness
    stream.hazeBaseColors[fi + 1] = (stream.lastColor[1] + cg) * 0.5 * fanBrightness
    stream.hazeBaseColors[fi + 2] = (stream.lastColor[2] + cb) * 0.5 * fanBrightness
    stream.hazeBaseColors[fi + 3] = stream.lastColor[0] * 0.1 * fanBrightness
    stream.hazeBaseColors[fi + 4] = stream.lastColor[1] * 0.1 * fanBrightness
    stream.hazeBaseColors[fi + 5] = stream.lastColor[2] * 0.1 * fanBrightness
    stream.hazeBaseColors[fi + 6] = cr * 0.1 * fanBrightness
    stream.hazeBaseColors[fi + 7] = cg * 0.1 * fanBrightness
    stream.hazeBaseColors[fi + 8] = cb * 0.1 * fanBrightness
    stream.spokePositions[vi] = 0
    stream.spokePositions[vi + 1] = 0
    stream.spokePositions[vi + 2] = 0
    stream.spokePositions[vi + 3] = wx
    stream.spokePositions[vi + 4] = wy
    stream.spokePositions[vi + 5] = BEAM_LEN
    stream.spokePositions[vi + 6] = wx
    stream.spokePositions[vi + 7] = wy
    stream.spokePositions[vi + 8] = BEAM_LEN
    stream.spokePositions[vi + 9] = endpoint.x
    stream.spokePositions[vi + 10] = endpoint.y
    stream.spokePositions[vi + 11] = endpoint.z
    stream.spokeBaseColors[vi] = cr * SPOKE_NEAR_BRIGHT
    stream.spokeBaseColors[vi + 1] = cg * SPOKE_NEAR_BRIGHT
    stream.spokeBaseColors[vi + 2] = cb * SPOKE_NEAR_BRIGHT
    stream.spokeBaseColors[vi + 3] = cr * SPOKE_FAR_DIM
    stream.spokeBaseColors[vi + 4] = cg * SPOKE_FAR_DIM
    stream.spokeBaseColors[vi + 5] = cb * SPOKE_FAR_DIM
    stream.spokeBaseColors[vi + 6] = cr * SPOKE_FAR_DIM
    stream.spokeBaseColors[vi + 7] = cg * SPOKE_FAR_DIM
    stream.spokeBaseColors[vi + 8] = cb * SPOKE_FAR_DIM
    stream.spokeBaseColors[vi + 9] = 0
    stream.spokeBaseColors[vi + 10] = 0
    stream.spokeBaseColors[vi + 11] = 0
    stream.spokeTimestamps[stream.spokeWriteIndex] = t
    stream.spokeWriteIndex = (stream.spokeWriteIndex + 1) % MAX_SCAN_POINTS
    stream.spokeCount = Math.min(stream.spokeCount + 1, MAX_SCAN_POINTS)

    stream.lastPoint = [x, y]
    stream.lastColor = [cr, cg, cb]
    stream.emitIndex += 1
  }
  stream.lastEmitTime += n * period
  stream.spokeGeo.setDrawRange(0, stream.spokeCount * 4)
  stream.hazeGeo.setDrawRange(0, stream.spokeCount * 3)
  stream.spokeGeo.attributes.position.needsUpdate = true
  stream.hazeGeo.attributes.position.needsUpdate = true
}

// Brightness falloff along each spoke: bright near the source, dim at the
// shape point, then fading all the way to black on the continuation past
// it - approximates how a real beam's apparent brightness in haze falls
// off as it diverges over distance, with no hard visible end.
const SPOKE_NEAR_BRIGHT = 1.0
const SPOKE_FAR_DIM = 0.08

// Paused entirely (no rAF scheduled, no per-frame work) while `enabled` is
// false, rather than just hiding the canvas via CSS - this is meant to
// actually stop consuming GPU/CPU in the background, not just visually
// hide it.
function animate() {
  if (!props.enabled) return
  animationId = requestAnimationFrame(animate)
  const now = performance.now() / 1000

  controls.update()
  positionLaserSources()
  prepareContinuation()
  emitScanPoints(now)

  const r = laserState.radius * WORLD_SCALE
  const color = new THREE.Color(laserState.r, laserState.g, laserState.b)

  // Fade every spoke by its age — this approximates the eye's
  // flicker-fusion persistence, a fixed physiological time window (~human
  // CFF, roughly 15-25ms), independent of the scanner's own parameters.
  // Whether the pattern *looks* solid or visibly flickers/strobes is then
  // an emergent result: if the beam revisits the same point faster than
  // this window (i.e. the revolution rate exceeds the eye's
  // flicker-fusion threshold), successive passes overlap and it reads as
  // continuous. If the scan is slower than that, spokes genuinely fade out
  // before the beam comes back around — same as a real, too-slow scanner
  // would look to a viewer.
  const persistenceTau = Math.max(0.001, props.persistenceMs / 1000)
  for (const stream of laserStreams.values()) {
    const colorArr = stream.spokeColors
    const hazeColorArr = stream.hazeColors
    for (let i = 0; i < stream.spokeCount; i++) {
      const age = now - stream.spokeTimestamps[i]
      const brightness = age < 0 ? 0 : Math.exp(-age / persistenceTau)
      const vi = i * 12
      for (let c = 0; c < 12; c++) {
        colorArr[vi + c] = stream.spokeBaseColors[vi + c] * brightness
      }
      const fi = i * 9
      for (let c = 0; c < 9; c++) {
        hazeColorArr[fi + c] = stream.hazeBaseColors[fi + c] * brightness
      }
    }
    stream.spokeGeo.attributes.color.needsUpdate = true
    stream.hazeGeo.attributes.color.needsUpdate = true
  }

  dust.rotation.y = now * 0.01
  dust.position.y = Math.sin(now * 0.1) * 0.3

  renderer.render(scene, camera)
}

function handleResize() {
  camera.aspect = window.innerWidth / window.innerHeight
  const halfHeight = PREVIEW_VIEW_HEIGHT / 2
  const halfWidth = halfHeight * camera.aspect
  camera.left = -halfWidth
  camera.right = halfWidth
  camera.top = halfHeight
  camera.bottom = -halfHeight
  camera.updateProjectionMatrix()
  renderer.setSize(window.innerWidth, window.innerHeight)
}

function clearPointBuffer() {
  const now = performance.now() / 1000
  for (const stream of laserStreams.values()) {
    stream.spokeTimestamps.fill(-Infinity)
    stream.spokeWriteIndex = 0
    stream.spokeCount = 0
    stream.currentFrame = []
    stream.emitIndex = 0
    stream.lastEmitTime = now
    stream.lastPoint = [0, 0]
    stream.lastColor = [0, 0, 0]
    stream.spokeGeo.setDrawRange(0, 0)
    stream.hazeGeo.setDrawRange(0, 0)
    stream.spokeGeo.attributes.position.needsUpdate = true
    stream.spokeGeo.attributes.color.needsUpdate = true
    stream.hazeGeo.attributes.position.needsUpdate = true
    stream.hazeGeo.attributes.color.needsUpdate = true
  }
}

onMounted(() => {
  setupScene()
  for (const stream of laserStreams.values()) stream.lastEmitTime = performance.now() / 1000
  onPoints(handleIncomingPoints)
  window.addEventListener('resize', handleResize)
  animate()
})

watch(() => lasers.map((entry) => entry.id).join(','), syncLaserSources)

// Clear stale points whenever laser armed state changes
watch(() => laserState.armed, () => {
  clearPointBuffer()
})

// Resume the paused render loop when the preview is switched back on -
// drop any stale backlog first so it doesn't burst-replay everything that
// arrived while paused.
watch(() => props.enabled, (enabled) => {
  if (enabled) {
    clearPointBuffer()
    cancelAnimationFrame(animationId)
    animate()
  } else {
    cancelAnimationFrame(animationId)
  }
})

onBeforeUnmount(() => {
  window.removeEventListener('pointermove', moveLaser)
  window.removeEventListener('pointerup', endLaserDrag)
  window.removeEventListener('pointercancel', endLaserDrag)
  cancelAnimationFrame(animationId)
  window.removeEventListener('resize', handleResize)
  renderer?.dispose()
})
</script>

<style scoped>
.laser-canvas {
  position: fixed;
  inset: 0;
  display: block;
}
.laser-layout {
  position: fixed;
  inset: 0;
  z-index: 10;
  pointer-events: none;
}
.laser-marker {
  position: absolute;
  display: flex;
  align-items: center;
  gap: 8px;
  width: 156px;
  min-width: 0;
  padding: 9px 10px;
  transform: translate(-50%, -50%);
  border: 1px solid rgba(255, 255, 255, 0.18);
  border-radius: 6px;
  background: rgba(5, 10, 20, 0.88);
  color: #d7ecf6;
  box-shadow: 0 4px 18px rgba(0, 0, 0, 0.45);
  font: 12px/1.2 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  text-align: left;
  pointer-events: auto;
  touch-action: none;
  user-select: none;
  cursor: grab;
}
.laser-marker:active { cursor: grabbing; }
.laser-marker-status {
  flex: 0 0 8px;
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #703030;
}
.laser-marker.is-armed {
  background: rgba(72, 224, 122, 0.18);
  border-color: #48e07a;
}
.laser-marker.is-armed .laser-marker-name { color: #48e07a; }
.laser-marker.is-connected .laser-marker-status {
  background: #48e07a;
  box-shadow: 0 0 9px rgba(72, 224, 122, 0.7);
}
.laser-marker-name {
  overflow: hidden;
  flex: 1;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.laser-marker-grip {
  display: flex;
  gap: 2px;
  flex: 0 0 auto;
  opacity: 0.55;
}
.laser-marker-grip i {
  display: block;
  width: 2px;
  height: 12px;
  border-radius: 2px;
  background: #8fe3ff;
}
@media (max-width: 420px) {
  .laser-marker { width: 132px; padding: 8px; }
}
</style>
