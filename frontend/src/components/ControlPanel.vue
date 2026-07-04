<template>
  <div id="ui" :class="{ collapsed }">
    <button class="collapse-btn" @click="collapsed = !collapsed" :title="collapsed ? 'Show panel' : 'Hide panel'">
      {{ collapsed ? '☰' : '✕' }}
    </button>
    <div class="panel-body" v-show="!collapsed">
    <h1>BeamCommander<span class="ver">3</span></h1>
    <div class="status-row">
      <span class="dot" :class="{ on: laserState.wsConnected }"></span>
      Preview {{ laserState.wsConnected ? 'live' : 'connecting…' }}
    </div>
    <div class="status-row">
      <span class="dot" :class="{ on: laserState.armed }"></span>
      Laser {{ laserState.armed ? `→ ${laserState.ip}` : 'disarmed' }}
    </div>
    <button class="blackout-btn" :class="{ active: laserState.blackout }" @click="push({ blackout: !laserState.blackout })">
      {{ laserState.blackout ? '◼ BLACKOUT' : '◻ Blackout' }}
    </button>
    <button class="reset-btn" @click="reset">↺ Reset to defaults</button>

    <hr />
    <h2>Shape</h2>
    <div class="btn-grid">
      <button v-for="s in SHAPES" :key="s" :class="{ active: laserState.shape === s }" @click="push({ shape: s })">{{ s }}</button>
    </div>
    <div v-for="[lbl,key,mn,mx,st] in [['Radius','radius',0.05,1,0.01],['Scale','shape_scale',-1,1,0.01],['Points','points',8,1000,1]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>Color</h2>
    <div class="row">
      <input type="color" :value="hexColor" @input="onColor" />
      <label>Beam color</label>
    </div>
    <div v-for="[lbl,key,mn,mx,st] in [['Brightness','intensity',0,1,0.01],['Rainbow','rainbow_amount',0,1,0.01],['Rainbow speed','rainbow_speed',0,4,0.1]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>Transform</h2>
    <div v-for="[lbl,key,mn,mx,st] in [['Pos X','pos_x',-1,1,0.01],['Pos Y','pos_y',-1,1,0.01],['Rotation (rot/s)','rotation_speed',-4,4,0.1]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>Movement</h2>
    <div class="btn-grid">
      <button v-for="m in MOVES" :key="m" :class="{ active: laserState.move_mode === m }" @click="push({ move_mode: m })">{{ m }}</button>
    </div>
    <div v-for="[lbl,key,mn,mx,st] in [['Speed','move_speed',0,4,0.05],['Size','move_size',0,1,0.01]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>Wave</h2>
    <div v-for="[lbl,key,mn,mx,st] in [['Frequency','wave_frequency',0.1,8,0.1],['Amplitude','wave_amplitude',0,1,0.01],['Speed','wave_speed',-4,4,0.1]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>FX</h2>
    <div v-for="[lbl,key,mn,mx,st] in [['Dot amount','dot_amount',0.01,1,0.01],['Flicker Hz','flicker_hz',0,30,0.5]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>Scan / Preview</h2>
    <label>Rate (kpps) <span class="val">{{ fmt(laserState.rate_kpps) }}</span></label>
    <input type="range" min="1" max="60" step="1" :value="laserState.rate_kpps" @input="push({ rate_kpps: +$event.target.value })" />
    <label>Eye persistence (ms) <span class="val">{{ persistenceMs }}</span></label>
    <input type="range" min="0" max="100" step="1" :value="persistenceMs" @input="onPersist(+$event.target.value)" />

    <hr />
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
  </div>
</template>

<script setup>
import { computed, ref } from 'vue'
import { laserState, updateState, resetState, connectLaser, disconnectLaser, markLocalChange } from '../composables/useLaserSocket.js'

const emit = defineEmits(['update:persistence'])

const SHAPES = ['circle','line','triangle','square','wave','staticwave']
const MOVES  = ['none','circle','pan','tilt','eight','random']
const persistenceMs = ref(25)
const ipInput = ref('10.10.10.4')
const collapsed = ref(false)

const hexColor = computed(() => {
  const to = v => Math.round(v*255).toString(16).padStart(2,'0')
  return '#' + to(laserState.r) + to(laserState.g) + to(laserState.b)
})
function onColor(e) {
  const h = e.target.value
  push({ r: parseInt(h.slice(1,3),16)/255, g: parseInt(h.slice(3,5),16)/255, b: parseInt(h.slice(5,7),16)/255 })
}
function fmt(v) { return typeof v === 'boolean' ? String(v) : Number(v).toFixed(2) }
function onPersist(v) { persistenceMs.value = v; emit('update:persistence', v) }

let debounce = null
function push(partial) {
  markLocalChange()
  Object.assign(laserState, partial)
  clearTimeout(debounce)
  debounce = setTimeout(() => updateState(partial).catch(console.error), 80)
}

async function arm() {
  if (!ipInput.value) return
  await connectLaser(ipInput.value).catch(e => { laserState.error = String(e) })
}
async function disarm() { await disconnectLaser().catch(console.error) }
async function reset() { await resetState().catch(console.error) }
</script>

<style scoped>
#ui {
  position:fixed; top:12px; left:12px; width:275px;
  background:rgba(8,10,18,0.88); border:1px solid rgba(120,130,200,0.25);
  border-radius:10px; padding:14px 16px; backdrop-filter:blur(6px);
  user-select:none; z-index:10; color:#cfd3e6;
  font-family:-apple-system,"Segoe UI",Roboto,sans-serif;
  max-height:calc(100vh - 24px); overflow-y:auto;
}
#ui.collapsed {
  width:auto; max-height:none; overflow:visible; padding:8px;
}
.collapse-btn {
  position:absolute; top:10px; right:10px; z-index:11;
  width:22px; height:22px; padding:0; line-height:1;
  font-size:12px; display:flex; align-items:center; justify-content:center;
}
#ui.collapsed .collapse-btn { position:static; }
.panel-body { padding-right:26px; }
h1 { font-size:13px; letter-spacing:2px; text-transform:uppercase; margin:0 0 8px; color:#8fe3ff; }
h1 .ver { color:#48e07a; }
h2 { font-size:10px; letter-spacing:1.5px; text-transform:uppercase; margin:8px 0 4px; color:#9aa0bd; }
label { display:block; font-size:11px; margin:6px 0 1px; color:#9aa0bd; }
.val { color:#cfd3e6; margin-left:4px; }
input[type="range"] { width:100%; accent-color:#48e07a; display:block; }
.row { display:flex; align-items:center; gap:8px; margin-top:6px; }
input[type="color"] { width:38px; height:22px; border:none; background:none; padding:0; cursor:pointer; }
input[type="text"] { flex:1; min-width:0; background:rgba(255,255,255,0.06); border:1px solid rgba(255,255,255,0.15); color:#cfd3e6; border-radius:4px; padding:4px 6px; font-size:12px; }
button { background:rgba(255,255,255,0.07); border:1px solid rgba(255,255,255,0.18); color:#cfd3e6; border-radius:4px; padding:4px 8px; font-size:11px; cursor:pointer; }
button:hover { background:rgba(255,255,255,0.14); }
button:disabled { opacity:0.5; cursor:default; }
button.active { background:rgba(72,224,122,0.18); border-color:#48e07a; color:#48e07a; }
.btn-stop { border-color:rgba(255,100,100,0.5); color:#ff8080; }
.blackout-btn { width:100%; margin-top:6px; padding:6px; font-size:12px; letter-spacing:1px; }
.blackout-btn.active { background:rgba(255,50,50,0.28); border-color:#ff4040; color:#ff8080; }
.reset-btn { width:100%; margin-top:6px; padding:5px; font-size:11px; }
hr { border:none; border-top:1px solid rgba(255,255,255,0.1); margin:8px 0; }
.status-row { display:flex; align-items:center; gap:6px; font-size:11px; margin-bottom:3px; }
.dot { width:7px; height:7px; border-radius:50%; background:#703030; flex-shrink:0; }
.dot.on { background:#48e07a; }
.error { font-size:11px; color:#ff8080; word-break:break-word; margin-top:4px; }
.btn-grid { display:grid; grid-template-columns:repeat(3,1fr); gap:3px; margin-bottom:4px; }
.btn-grid button { padding:4px 2px; font-size:10px; text-align:center; }
</style>
