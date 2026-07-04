<template>
  <canvas ref="canvasEl" class="laser-canvas"></canvas>
</template>

<script setup>
import { onMounted, onBeforeUnmount, ref, watch } from 'vue'
import * as THREE from 'three'
import { OrbitControls } from 'three/addons/controls/OrbitControls.js'
import { laserState, onPoints } from '../composables/useLaserSocket.js'

const props = defineProps({
  persistenceMs: { type: Number, default: 25 },
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
const MAX_SCAN_POINTS = 20000

let renderer, scene, camera, controls, laser
let dust
let spokeGeo, spokePositions, spokeColors, spokeTimestamps
let spokeWriteIndex = 0
let animationId = 0

function setupScene() {
  renderer = new THREE.WebGLRenderer({ canvas: canvasEl.value, antialias: true })
  renderer.setSize(window.innerWidth, window.innerHeight)
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))

  scene = new THREE.Scene()
  scene.background = new THREE.Color(0x020208)
  scene.fog = new THREE.FogExp2(0x020208, 0.008)

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
  const planeZ = -8 + BEAM_LEN
  camera = new THREE.PerspectiveCamera(60, window.innerWidth / window.innerHeight, 0.1, 500)
  camera.position.set(0, 4.2, planeZ + 6)

  controls = new OrbitControls(camera, renderer.domElement)
  controls.target.set(0, 4.2, -8)
  controls.enableDamping = true
  controls.maxPolarAngle = Math.PI * 0.62
  controls.minDistance = 4
  controls.maxDistance = 90
  controls.update()

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
  truss.position.set(0, 4.6, -8)
  scene.add(truss)

  laser = new THREE.Group()
  laser.position.set(0, 4.2, -8)
  scene.add(laser)

  const housing = new THREE.Mesh(
    new THREE.BoxGeometry(0.55, 0.4, 0.7),
    new THREE.MeshBasicMaterial({ color: 0x1a1d28 })
  )
  housing.position.z = -0.36
  laser.add(housing)

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
  spokePositions = new Float32Array(MAX_SCAN_POINTS * 4 * 3)
  spokeColors = new Float32Array(MAX_SCAN_POINTS * 4 * 3)
  spokeTimestamps = new Float64Array(MAX_SCAN_POINTS).fill(-Infinity)
  spokeGeo = new THREE.BufferGeometry()
  spokeGeo.setAttribute('position', new THREE.BufferAttribute(spokePositions, 3).setUsage(THREE.DynamicDrawUsage))
  spokeGeo.setAttribute('color', new THREE.BufferAttribute(spokeColors, 3).setUsage(THREE.DynamicDrawUsage))
  const spokeMat = new THREE.LineBasicMaterial({
    vertexColors: true, transparent: true,
    blending: THREE.AdditiveBlending, depthWrite: false,
  })
  const spokeSegments = new THREE.LineSegments(spokeGeo, spokeMat)
  laser.add(spokeSegments)

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
}

// The backend sends one complete shape/frame (all points of one revolution)
// every ~33ms - a snapshot, not a continuous point stream. A real Ether
// Dream DAC loops that exact same point buffer continuously at rate_kpps
// points/sec until a new buffer arrives. To make the preview point-for-point
// faithful to that real behaviour (not just flash the whole frame in sync
// every 33ms), we replay the latest received frame locally at the true scan
// rate: a virtual scan head advances through `currentFrame` at
// `rate_kpps*1000` points/sec, looping forever, exactly like the DAC does.
let currentFrame = []      // latest [x,y,r,g,b] points from the backend
let emitIndex = 0          // fractional read position into currentFrame
let lastEmitTime = 0       // performance.now()/1000 of the last emitted point
const MAX_EMIT_PER_TICK = 4000  // safety cap (e.g. tab was backgrounded)

function handleIncomingPoints(msg) {
  currentFrame = msg.pts
}

// Advances the virtual scan head and writes each newly "visited" point as
// one spoke (source -> point) into the ring buffer with a real timestamp
// spaced 1/pointRateHz apart - exactly reproducing how a physical scanner
// would have traced them in time.
function emitScanPoints(now) {
  if (currentFrame.length === 0) return
  const pointRateHz = Math.max(1, laserState.rate_kpps * 1000)
  const period = 1 / pointRateHz

  let n = Math.floor((now - lastEmitTime) / period)
  if (n <= 0) return
  if (n > MAX_EMIT_PER_TICK) {
    // Fell far behind (backgrounded tab, GC pause, ...) - skip ahead instead
    // of trying to replay a huge backlog.
    lastEmitTime = now - MAX_EMIT_PER_TICK * period
    n = MAX_EMIT_PER_TICK
  }

  for (let k = 0; k < n; k++) {
    const frame = currentFrame
    const idx = Math.floor(emitIndex) % frame.length
    const [x, y, r, g, b] = frame[idx]
    const t = lastEmitTime + (k + 1) * period

    const wx = x * WORLD_SCALE
    const wy = y * WORLD_SCALE
    // Daemon sends 0..1 floats; simulation sends 0..255 integers.
    const scale = (r > 1 || g > 1 || b > 1) ? 1 / 255 : 1
    const cr = r * scale, cg = g * scale, cb = b * scale

    // Spoke: source (local origin) -> shape point -> far continuation.
    // Segment 1 (source->point) fades bright-to-dim; segment 2
    // (point->far) continues fading from dim all the way to black, so the
    // ray appears to travel on indefinitely with no visible endpoint,
    // instead of hard-stopping exactly at the shape.
    const vi = spokeWriteIndex * 12   // 4 vertices * 3 floats
    const fx = wx * SPOKE_FAR_EXTEND, fy = wy * SPOKE_FAR_EXTEND, fz = BEAM_LEN * SPOKE_FAR_EXTEND
    spokePositions[vi]      = 0
    spokePositions[vi + 1]  = 0
    spokePositions[vi + 2]  = 0
    spokePositions[vi + 3]  = wx
    spokePositions[vi + 4]  = wy
    spokePositions[vi + 5]  = BEAM_LEN
    spokePositions[vi + 6]  = wx
    spokePositions[vi + 7]  = wy
    spokePositions[vi + 8]  = BEAM_LEN
    spokePositions[vi + 9]  = fx
    spokePositions[vi + 10] = fy
    spokePositions[vi + 11] = fz
    spokeBaseColors[vi]     = cr * SPOKE_NEAR_BRIGHT; spokeBaseColors[vi + 1] = cg * SPOKE_NEAR_BRIGHT; spokeBaseColors[vi + 2] = cb * SPOKE_NEAR_BRIGHT
    spokeBaseColors[vi + 3] = cr * SPOKE_FAR_DIM;      spokeBaseColors[vi + 4] = cg * SPOKE_FAR_DIM;      spokeBaseColors[vi + 5] = cb * SPOKE_FAR_DIM
    spokeBaseColors[vi + 6] = cr * SPOKE_FAR_DIM;      spokeBaseColors[vi + 7] = cg * SPOKE_FAR_DIM;      spokeBaseColors[vi + 8] = cb * SPOKE_FAR_DIM
    spokeBaseColors[vi + 9] = 0;                       spokeBaseColors[vi + 10] = 0;                      spokeBaseColors[vi + 11] = 0
    spokeTimestamps[spokeWriteIndex] = t
    spokeWriteIndex = (spokeWriteIndex + 1) % MAX_SCAN_POINTS

    lastPoint = [x, y]
    emitIndex += 1
  }
  lastEmitTime += n * period
  spokeGeo.attributes.position.needsUpdate = true
}

// Brightness falloff along each spoke: bright near the source, dim at the
// shape point, then fading all the way to black on the continuation past
// it - approximates how a real beam's apparent brightness in haze falls
// off as it diverges over distance, with no hard visible end.
const SPOKE_NEAR_BRIGHT = 1.0
const SPOKE_FAR_DIM = 0.08
// How much further the beam continues past the shape point (as a
// multiple of the source->point distance) before fully fading to black.
const SPOKE_FAR_EXTEND = 6

const spokeBaseColors = new Float32Array(MAX_SCAN_POINTS * 4 * 3)
let lastPoint = [0, 0]

function animate() {
  animationId = requestAnimationFrame(animate)
  const now = performance.now() / 1000

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
  const colorArr = spokeGeo.attributes.color.array
  for (let i = 0; i < MAX_SCAN_POINTS; i++) {
    const age = now - spokeTimestamps[i]
    const brightness = age < 0 ? 0 : Math.exp(-age / persistenceTau)
    const vi = i * 12
    for (let c = 0; c < 12; c++) {
      colorArr[vi + c] = spokeBaseColors[vi + c] * brightness
    }
  }
  spokeGeo.attributes.color.needsUpdate = true

  dust.rotation.y = now * 0.01
  dust.position.y = Math.sin(now * 0.1) * 0.3

  controls.update()
  renderer.render(scene, camera)
}

function handleResize() {
  camera.aspect = window.innerWidth / window.innerHeight
  camera.updateProjectionMatrix()
  renderer.setSize(window.innerWidth, window.innerHeight)
}

function clearPointBuffer() {
  spokeTimestamps.fill(-Infinity)
  spokeWriteIndex = 0
  currentFrame = []
  emitIndex = 0
  lastEmitTime = performance.now() / 1000
  if (spokeGeo) {
    spokeGeo.attributes.position.needsUpdate = true
    spokeGeo.attributes.color.needsUpdate = true
  }
}

onMounted(() => {
  setupScene()
  lastEmitTime = performance.now() / 1000
  onPoints(handleIncomingPoints)
  window.addEventListener('resize', handleResize)
  animate()
})

// Clear stale points whenever laser armed state changes
watch(() => laserState.armed, () => {
  clearPointBuffer()
})

onBeforeUnmount(() => {
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
</style>
