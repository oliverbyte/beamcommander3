<template>
  <div id="ui">
    <h1>BeamCommander3</h1>

    <!-- Status -->
    <div class="status-row">
      <span class="dot" :class="{ on: laserState.wsConnected }"></span>
      Preview {{ laserState.wsConnected ? 'live' : 'connecting…' }}
    </div>
    <div class="status-row">
      <span class="dot" :class="{ on: laserState.armed }"></span>
      Laser {{ laserState.armed ? `→ ${laserState.ip}` : 'disarmed' }}
    </div>

    <hr />

    <!-- Shape selector -->
    <h2>Shape</h2>
    <div class="shape-grid">
      <button
        v-for="s in SHAPES" :key="s"
        :class="{ active: laserState.shape === s }"
        @click="pickShape(s)"
      >{{ s }}</button>
    </div>

    <hr />

    <!-- Laser params -->
    <label>Radius <span>{{ laserState.radius.toFixed(2) }}</span></label>
    <input type="range" min="0.05" max="1" step="0.01"
           :value="laserState.radius"
           @input="push({ radius: +$event.target.value })" />

    <label>Scan rate (kpps) <span>{{ laserState.rate_kpps }}</span></label>
    <input type="range" min="1" max="60" step="1"
           :value="laserState.rate_kpps"
           @input="push({ rate_kpps: +$event.target.value })" />

    <label>Points <span>{{ laserState.points }}</span></label>
    <input type="range" min="8" max="2000" step="1"
           :value="laserState.points"
           @input="push({ points: +$event.target.value })" />

    <label>Brightness <span>{{ laserState.intensity.toFixed(2) }}</span></label>
    <input type="range" min="0" max="1" step="0.01"
           :value="laserState.intensity"
           @input="push({ intensity: +$event.target.value })" />

    <label>Eye persistence (ms, preview) <span>{{ persistenceMs }}</span></label>
    <input type="range" min="5" max="100" step="1"
           v-model.number="persistenceMs"
           @input="$emit('update:persistence', persistenceMs)" />

    <div class="row">
      <input type="color" :value="hexColor" @input="onColor" />
      <label>Beam color</label>
    </div>

    <hr />

    <!-- Connect -->
    <h2>Controller</h2>
    <div class="row">
      <input type="text" v-model="ipInput" placeholder="IP address" />
    </div>
    <div class="row">
      <button v-if="!laserState.armed" @click="arm" :disabled="!ipInput">Connect</button>
      <button v-else @click="disarm" class="btn-stop">Disconnect</button>
    </div>
    <p class="error" v-if="laserState.error">{{ laserState.error }}</p>
  </div>
</template>

<script setup>
import { computed, ref } from 'vue'
import { laserState, updateState, setShape, connectLaser, disconnectLaser }
  from '../composables/useLaserSocket.js'

const emit = defineEmits(['update:persistence'])

const SHAPES = ['circle', 'line', 'triangle', 'square', 'wave', 'staticwave']
const persistenceMs = ref(25)
const ipInput = ref('10.10.10.4')

const hexColor = computed(() => {
  const r = Math.round(laserState.r * 255)
  const g = Math.round(laserState.g * 255)
  const b = Math.round(laserState.b * 255)
  return '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('')
})

function onColor(e) {
  const hex = e.target.value
  push({
    r: parseInt(hex.slice(1, 3), 16) / 255,
    g: parseInt(hex.slice(3, 5), 16) / 255,
    b: parseInt(hex.slice(5, 7), 16) / 255,
  })
}

let debounce = null
function push(partial) {
  Object.assign(laserState, partial) // optimistic update
  clearTimeout(debounce)
  debounce = setTimeout(() => updateState(partial).catch(console.error), 80)
}

async function pickShape(s) {
  laserState.shape = s
  await setShape(s).catch(console.error)
}

async function arm() {
  if (!ipInput.value) return
  await connectLaser(ipInput.value).catch(e => { laserState.error = String(e) })
}

async function disarm() {
  await disconnectLaser().catch(console.error)
}
</script>

<style scoped>
#ui {
  position: fixed;
  top: 12px; left: 12px;
  width: 270px;
  background: rgba(8,10,18,0.85);
  border: 1px solid rgba(120,130,200,0.25);
  border-radius: 10px;
  padding: 14px 16px;
  backdrop-filter: blur(6px);
  user-select: none;
  z-index: 10;
  color: #cfd3e6;
  font-family: -apple-system,"Segoe UI",Roboto,sans-serif;
  max-height: calc(100vh - 24px);
  overflow-y: auto;
}
h1 { font-size:13px; letter-spacing:2px; text-transform:uppercase;
     margin:0 0 10px; color:#8fe3ff; }
h2 { font-size:11px; letter-spacing:1.5px; text-transform:uppercase;
     margin:10px 0 6px; color:#9aa0bd; }
label { display:block; font-size:11px; margin:10px 0 2px; color:#9aa0bd; }
input[type="range"] { width:100%; accent-color:#48e07a; }
.row { display:flex; align-items:center; gap:8px; margin-top:8px; }
input[type="color"] { width:42px; height:24px; border:none; background:none;
                      padding:0; cursor:pointer; }
input[type="text"] {
  flex:1; min-width:0; background:rgba(255,255,255,0.06);
  border:1px solid rgba(255,255,255,0.15); color:#cfd3e6;
  border-radius:4px; padding:4px 6px; font-size:12px;
}
button {
  background:rgba(255,255,255,0.08); border:1px solid rgba(255,255,255,0.18);
  color:#cfd3e6; border-radius:4px; padding:4px 8px;
  font-size:11px; cursor:pointer;
}
button:hover { background:rgba(255,255,255,0.14); }
button:disabled { opacity:0.5; cursor:default; }
button.active { background:rgba(72,224,122,0.2); border-color:#48e07a; color:#48e07a; }
.btn-stop { border-color:rgba(255,100,100,0.5); color:#ff8080; }
hr { border:none; border-top:1px solid rgba(255,255,255,0.12); margin:12px 0; }
.status-row { display:flex; align-items:center; gap:6px; font-size:11px; margin-bottom:4px; }
.dot { width:7px; height:7px; border-radius:50%; background:#703030; flex-shrink:0; }
.dot.on { background:#48e07a; }
.error { font-size:11px; color:#ff8080; word-break:break-word; }
.shape-grid {
  display:grid; grid-template-columns:repeat(3,1fr); gap:4px; margin-top:4px;
}
.shape-grid button { padding:5px 2px; font-size:10px; text-align:center; }
</style>
