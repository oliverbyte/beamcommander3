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
let cone, coneUniforms, scanLine, scanLineGeo, scanDot, flare, flareMat, dust
let scanGeo, scanPositions, scanColors, scanTimestamps
let scanWriteIndex = 0
let animationId = 0

function makeGlowTexture(size = 128) {
  const cvs = document.createElement('canvas')
  cvs.width = cvs.height = size
  const ctx = cvs.getContext('2d')
  const g = ctx.createRadialGradient(size / 2, size / 2, 0, size / 2, size / 2, size / 2)
  g.addColorStop(0.0, 'rgba(255,255,255,1)')
  g.addColorStop(0.25, 'rgba(255,255,255,0.5)')
  g.addColorStop(0.6, 'rgba(255,255,255,0.12)')
  g.addColorStop(1.0, 'rgba(255,255,255,0)')
  ctx.fillStyle = g
  ctx.fillRect(0, 0, size, size)
  return new THREE.CanvasTexture(cvs)
}

function setupScene() {
  renderer = new THREE.WebGLRenderer({ canvas: canvasEl.value, antialias: true })
  renderer.setSize(window.innerWidth, window.innerHeight)
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))

  scene = new THREE.Scene()
  scene.background = new THREE.Color(0x020208)
  scene.fog = new THREE.FogExp2(0x020208, 0.008)

  // The laser fixture sits at (0, 4.2, -8) and shoots down its local +Z
  // axis, so the projected pattern plane sits at world z = -8 + BEAM_LEN.
  // Put the camera further along that same axis (same x/y as the fixture)
  // so it looks straight down the beam at the plane face-on - this is what
  // guarantees the shape is always fully visible and undistorted by
  // default, matching the real DAC output point-for-point. OrbitControls
  // still lets the viewer rotate for a more dramatic angle afterwards.
  const planeZ = -8 + BEAM_LEN
  camera = new THREE.PerspectiveCamera(60, window.innerWidth / window.innerHeight, 0.1, 500)
  camera.position.set(0, 4.2, planeZ + 20)

  controls = new OrbitControls(camera, renderer.domElement)
  controls.target.set(0, 4.2, planeZ)
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

  const glowTex = makeGlowTexture()

  // Ambient haze cone (soft, angle-independent glow along the beam path)
  coneUniforms = {
    uColor: { value: new THREE.Color(0x00ff55) },
    uIntensity: { value: 1.0 },
  }
  const coneGeo = new THREE.CylinderGeometry(0.015, 1, 1, 160, 1, true)
  coneGeo.translate(0, -0.5, 0)
  coneGeo.rotateX(-Math.PI / 2)
  const coneMat = new THREE.ShaderMaterial({
    uniforms: coneUniforms,
    transparent: true,
    depthWrite: false,
    side: THREE.DoubleSide,
    blending: THREE.AdditiveBlending,
    vertexShader: `
      varying vec3 vPos;
      void main() {
        vPos = position;
        gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
      }
    `,
    fragmentShader: `
      uniform vec3 uColor;
      uniform float uIntensity;
      varying vec3 vPos;
      void main() {
        float along = clamp(vPos.z, 0.0, 1.0);
        float fade = pow(1.0 - along, 1.25) + 0.05;
        float b = 0.16 * fade * uIntensity;
        gl_FragColor = vec4(uColor * b, 1.0);
      }
    `,
  })
  cone = new THREE.Mesh(coneGeo, coneMat)
  laser.add(cone)

  // Instantaneous beam line: source -> most recently scanned point
  scanLineGeo = new THREE.BufferGeometry()
  scanLineGeo.setAttribute('position', new THREE.BufferAttribute(new Float32Array([0, 0, 0, 0, 0, 1]), 3))
  const scanLineMat = new THREE.LineBasicMaterial({
    color: 0xffffff, transparent: true, opacity: 0.9,
    blending: THREE.AdditiveBlending, depthWrite: false,
  })
  scanLine = new THREE.Line(scanLineGeo, scanLineMat)
  scanLine.userData.mat = scanLineMat
  laser.add(scanLine)

  // Real scanner output point cloud, fed entirely from the backend WebSocket
  scanPositions = new Float32Array(MAX_SCAN_POINTS * 3)
  scanColors = new Float32Array(MAX_SCAN_POINTS * 3)
  scanTimestamps = new Float64Array(MAX_SCAN_POINTS).fill(-Infinity)
  scanGeo = new THREE.BufferGeometry()
  scanGeo.setAttribute('position', new THREE.BufferAttribute(scanPositions, 3).setUsage(THREE.DynamicDrawUsage))
  scanGeo.setAttribute('color', new THREE.BufferAttribute(scanColors, 3).setUsage(THREE.DynamicDrawUsage))
  const scanPointsMat = new THREE.PointsMaterial({
    size: 0.16, map: glowTex, vertexColors: true, transparent: true,
    blending: THREE.AdditiveBlending, depthWrite: false, sizeAttenuation: true,
  })
  const scanPoints = new THREE.Points(scanGeo, scanPointsMat)
  laser.add(scanPoints)

  flareMat = new THREE.SpriteMaterial({
    map: glowTex, color: 0x00ff55, transparent: true, opacity: 0.8,
    blending: THREE.AdditiveBlending, depthWrite: false,
  })
  flare = new THREE.Sprite(flareMat)
  flare.scale.setScalar(1.5)
  laser.add(flare)

  const dotMat = new THREE.SpriteMaterial({
    map: glowTex, color: 0xffffff, transparent: true, opacity: 0.9,
    blending: THREE.AdditiveBlending, depthWrite: false,
  })
  scanDot = new THREE.Sprite(dotMat)
  scanDot.scale.setScalar(0.8)
  laser.add(scanDot)

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

// Advances the virtual scan head and writes newly "visited" points into the
// ring buffer with real per-point timestamps spaced 1/pointRateHz apart -
// exactly reproducing how a physical scanner would have hit them in time.
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

    const idx3 = scanWriteIndex * 3
    scanPositions[idx3] = x * WORLD_SCALE
    scanPositions[idx3 + 1] = y * WORLD_SCALE
    scanPositions[idx3 + 2] = lastBeamLen
    // Daemon sends 0..1 floats; simulation sends 0..255 integers.
    const scale = (r > 1 || g > 1 || b > 1) ? 1 / 255 : 1
    baseColors[idx3] = r * scale
    baseColors[idx3 + 1] = g * scale
    baseColors[idx3 + 2] = b * scale
    scanTimestamps[scanWriteIndex] = t
    scanWriteIndex = (scanWriteIndex + 1) % MAX_SCAN_POINTS
    lastPoint = [x, y]

    emitIndex += 1
  }
  lastEmitTime += n * period
  scanGeo.attributes.position.needsUpdate = true
}

const baseColors = new Float32Array(MAX_SCAN_POINTS * 3)
let lastBeamLen = BEAM_LEN
let lastPoint = [0, 0]
const tmpV1 = new THREE.Vector3()
const tmpV2 = new THREE.Vector3()

function animate() {
  animationId = requestAnimationFrame(animate)
  const now = performance.now() / 1000

  emitScanPoints(now)

  const r = laserState.radius * WORLD_SCALE
  const color = new THREE.Color(laserState.r, laserState.g, laserState.b)

  laser.getWorldPosition(tmpV1)
  const len = BEAM_LEN
  lastBeamLen = len

  cone.scale.set(r, r, len)
  coneUniforms.uIntensity.value = laserState.intensity
  coneUniforms.uColor.value.copy(color)
  flareMat.color.copy(color)
  scanLine.userData.mat.color.copy(color).lerp(new THREE.Color(0xffffff), 0.6)

  // Fade every point by its age — this approximates the eye's flicker-fusion
  // persistence, a fixed physiological time window (~human CFF, roughly
  // 15-25ms), independent of the scanner's own parameters. Whether the
  // circle *looks* solid or visibly flickers/strobes is then an emergent
  // result: if the beam revisits a point faster than this window (i.e. the
  // revolution rate exceeds the eye's flicker-fusion threshold), successive
  // hits overlap and it reads as a continuous beam/tunnel/circle. If the
  // scan is slower than that (low scan_rate_kpps relative to
  // points_per_circle), the dot genuinely fades out before the beam comes
  // back around — same as a real, too-slow scanner would look to a viewer.
  const persistenceTau = Math.max(0.001, props.persistenceMs / 1000)
  const colorArr = scanGeo.attributes.color.array
  for (let i = 0; i < MAX_SCAN_POINTS; i++) {
    const age = now - scanTimestamps[i]
    const brightness = age < 0 ? 0 : Math.exp(-age / persistenceTau)
    const idx3 = i * 3
    colorArr[idx3] = baseColors[idx3] * brightness
    colorArr[idx3 + 1] = baseColors[idx3 + 1] * brightness
    colorArr[idx3 + 2] = baseColors[idx3 + 2] * brightness
  }
  scanGeo.attributes.color.needsUpdate = true

  // The instantaneous beam line/hotspot always follows the latest point,
  // reprojected onto the current (viewer-relative) far plane.
  const [ex, ey] = lastPoint
  const pos = scanLineGeo.attributes.position
  pos.setXYZ(1, ex * WORLD_SCALE, ey * WORLD_SCALE, len)
  pos.needsUpdate = true
  scanDot.position.set(ex * WORLD_SCALE, ey * WORLD_SCALE, len)

  // "Looking into the laser" flare: brightens when the camera is inside the cone
  tmpV2.copy(camera.position).sub(tmpV1).normalize()
  const axis = new THREE.Vector3(0, 0, 1).applyQuaternion(laser.quaternion)
  const viewAngle = Math.acos(THREE.MathUtils.clamp(tmpV2.dot(axis), -1, 1))
  const halfAngle = Math.atan2(r, len)
  const inside = THREE.MathUtils.smoothstep((halfAngle + 0.25 - viewAngle) / 0.3, 0, 1)
  const flicker = 0.9 + 0.1 * Math.sin(now * 37.0) * Math.sin(now * 23.0)
  flare.scale.setScalar((1.2 + inside * 7.0) * flicker * laserState.intensity)
  flareMat.opacity = 0.5 + inside * 0.5

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
  scanTimestamps.fill(-Infinity)
  scanWriteIndex = 0
  currentFrame = []
  emitIndex = 0
  lastEmitTime = performance.now() / 1000
  if (scanGeo) {
    scanGeo.attributes.position.needsUpdate = true
    scanGeo.attributes.color.needsUpdate = true
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
