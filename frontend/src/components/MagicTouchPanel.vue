<template>
  <section class="magic-touch" :class="{ standalone, 'preview-mode': activePage === 'preview' }" aria-label="Magic Touch live controls" @contextmenu="suppressContextMenu">
    <nav class="magic-sidebar" aria-label="Magic Touch pages">
      <div class="sidebar-brand">
        <span class="brand-mark">BC3</span>
      </div>
      <div class="page-nav">
        <button
          v-for="item in PAGES" :key="item.id"
          class="page-nav-button"
          :class="{ active: activePage === item.id }"
          :aria-current="activePage === item.id ? 'page' : undefined"
          @click="selectPage(item.id)"
        >
          <span class="nav-icon" v-html="PAGE_ICONS[item.id]"></span>
          <span class="nav-label">{{ item.label }}</span>
        </button>
      </div>
    </nav>

    <main class="magic-main">
      <header v-if="activePage !== 'preview'" class="magic-header">
        <div>
          <p class="eyebrow">LIVE CONTROL</p>
          <h1>{{ activePageMeta.title }}</h1>
          <p v-if="activePageMeta.description" class="page-description">{{ activePageMeta.description }}</p>
        </div>
        <div class="header-actions">
          <!-- MagicTouchBuilder teleports its page tabs in here (saves a row at the bottom). -->
          <div v-if="activePage === 'builder'" id="mtb-page-slot" class="page-slot"></div>
          <button
            v-if="activePage === 'builder'"
            class="edit-toggle"
            :class="{ active: builderEditing }"
            :aria-pressed="builderEditing"
            @click="builderEditing = !builderEditing"
          >{{ builderEditing ? 'Edit mode' : 'Run mode' }}</button>
          <div class="laser-status" :class="{ online: connectedLaserCount > 0 }">
            <span class="status-light" :class="{ connected: connectedLaserCount > 0 }"></span>
            <span>{{ connectedLaserCount }} / {{ lasers.length }} lasers</span>
          </div>
        </div>
      </header>

      <div class="magic-content" :class="`page-${activePage}`">
        <div v-if="activePage === 'cues'" class="magic-cues">
          <CuePanel popout />
        </div>

        <div v-else-if="activePage === 'color'" class="page-body color-page">
          <section class="control-section palette-section">
            <div class="section-heading">
              <div><h2>Color palette</h2><p>Choose a look for the current beam.</p></div>
              <div class="current-color" :style="{ '--swatch': hexColor }" :aria-label="`Current color ${hexColor}`"></div>
            </div>
            <div class="color-grid">
              <button
                v-for="preset in COLOR_PRESETS" :key="preset.name"
                class="color-preset"
                :class="{ selected: isPresetActive(preset) }"
                :style="{ '--swatch': preset.color }"
                :aria-label="preset.name"
                :title="preset.name"
                @click="selectColor(preset.color)"
              ><span>{{ preset.name }}</span></button>
            </div>
            <label class="color-picker-row">
              <span class="color-picker-swatch" :style="{ '--swatch': hexColor }"></span>
              <span>Custom color</span>
              <input type="color" :value="hexColor" aria-label="Choose custom beam color" @input="onColorInput" />
            </label>
          </section>
          <section class="control-section">
            <div class="section-heading"><div><h2>Rainbow</h2><p>Blend animated color across the shape.</p></div></div>
            <div v-for="control in COLOR_CONTROLS" :key="control.key" class="range-control">
              <label :for="`magic-${control.key}`">{{ control.label }} <output>{{ formatValue(laserState[control.key], control) }}</output></label>
              <input :id="`magic-${control.key}`" type="range" :min="control.min" :max="control.max" :step="control.step" :value="laserState[control.key]" @input="setNumeric(control.key, $event.target.value)" />
            </div>
          </section>
        </div>

        <div v-else-if="activePage === 'shapes'" class="page-body">
          <section class="control-section">
            <div class="section-heading"><div><h2>Shape</h2><p>Pick the geometry and tune its outline.</p></div></div>
            <div class="choice-grid shape-grid">
              <button v-for="shape in SHAPES" :key="shape" class="choice-button" :class="{ selected: laserState.shape === shape }" @click="setValues({ shape })">
                <span class="choice-icon" aria-hidden="true" v-html="SHAPE_ICONS[shape]"></span>
                <span>{{ shape }}</span>
              </button>
            </div>
            <div class="range-grid">
              <div v-for="control in SHAPE_CONTROLS" :key="control.key" class="range-control">
                <label :for="`magic-${control.key}`">{{ control.label }} <output>{{ formatValue(laserState[control.key], control) }}</output></label>
                <input :id="`magic-${control.key}`" type="range" :min="control.min" :max="control.max" :step="control.step" :value="laserState[control.key]" @input="setNumeric(control.key, $event.target.value)" />
              </div>
            </div>
          </section>
          <section class="control-section">
            <div class="section-heading"><div><h2>Wave</h2><p>Set the wave profile and phase movement.</p></div></div>
            <div class="range-grid">
              <div v-for="control in WAVE_CONTROLS" :key="control.key" class="range-control">
                <label :for="`magic-${control.key}`">{{ control.label }} <output>{{ formatValue(laserState[control.key], control) }}</output></label>
                <input :id="`magic-${control.key}`" type="range" :min="control.min" :max="control.max" :step="control.step" :value="laserState[control.key]" @input="setNumeric(control.key, $event.target.value)" />
              </div>
            </div>
          </section>
        </div>

        <div v-else-if="activePage === 'move'" class="page-body">
          <section class="control-section">
            <div class="section-heading"><div><h2>Movement</h2><p>Choose how the beam travels across the stage.</p></div></div>
            <div class="choice-grid move-grid">
              <button v-for="mode in MOVES" :key="mode" class="choice-button" :class="{ selected: laserState.move_mode === mode }" @click="setValues({ move_mode: mode })">
                <span class="choice-icon" aria-hidden="true" v-html="MOVE_ICONS[mode]"></span>
                <span>{{ mode }}</span>
              </button>
            </div>
            <div class="range-grid">
              <div v-for="control in MOVE_CONTROLS" :key="control.key" class="range-control">
                <label :for="`magic-${control.key}`">{{ control.label }} <output>{{ formatValue(laserState[control.key], control) }}</output></label>
                <input :id="`magic-${control.key}`" type="range" :min="control.min" :max="control.max" :step="control.step" :value="laserState[control.key]" @input="setNumeric(control.key, $event.target.value)" />
              </div>
            </div>
          </section>
          <section class="control-section">
            <div class="section-heading"><div><h2>Position &amp; rotation</h2><p>Offset and spin the complete shape.</p></div></div>
            <div class="range-grid">
              <div v-for="control in TRANSFORM_CONTROLS" :key="control.key" class="range-control">
                <label :for="`magic-${control.key}`">{{ control.label }} <output>{{ formatValue(laserState[control.key], control) }}</output></label>
                <input :id="`magic-${control.key}`" type="range" :min="control.min" :max="control.max" :step="control.step" :value="laserState[control.key]" @input="setNumeric(control.key, $event.target.value)" />
              </div>
            </div>
          </section>
        </div>

        <div v-else-if="activePage === 'beam'" class="page-body">
          <section class="control-section beam-rate-section">
            <div class="section-heading"><div><h2>Scan rate</h2><p>Lower rates create a slower, more deliberate scan.</p></div><strong class="rate-readout">{{ formatValue(laserState.rate_kpps, { decimals: 1 }) }} <small>kpps</small></strong></div>
            <div class="range-control">
              <label for="magic-rate">Rate <output>{{ formatValue(laserState.rate_kpps, { decimals: 1 }) }} kpps</output></label>
              <input id="magic-rate" type="range" min="0" max="1" step="0.001" :value="ratePosition" @input="setRatePosition($event.target.value)" />
            </div>
            <div class="range-control">
              <label for="magic-max-rate">Maximum rate <output>{{ formatValue(laserState.max_rate_kpps, { decimals: 0 }) }} kpps</output></label>
              <input id="magic-max-rate" type="range" min="1" max="100" step="1" :value="laserState.max_rate_kpps" @input="setNumeric('max_rate_kpps', $event.target.value)" />
            </div>
            <div class="range-control" title="How far ahead frames are queued for the lasers. Lower = changes and cues show up sooner; too low can cause short blank gaps on a slow or busy network.">
              <label for="magic-latency">Output latency <output>{{ formatValue(laserState.output_latency_ms, { decimals: 0 }) }} ms</output></label>
              <input id="magic-latency" type="range" min="10" max="200" step="5" :value="laserState.output_latency_ms" @input="setNumeric('output_latency_ms', $event.target.value)" />
            </div>
          </section>
          <section class="control-section">
            <div class="section-heading"><div><h2>Beam detail</h2><p>Control dot density and flicker rate, on time and phase.</p></div></div>
            <div class="range-grid">
              <div v-for="control in BEAM_CONTROLS" :key="control.key" class="range-control">
                <label :for="`magic-${control.key}`">{{ control.label }} <output>{{ formatValue(laserState[control.key], control) }}</output></label>
                <input :id="`magic-${control.key}`" type="range" :min="control.min" :max="control.max" :step="control.step" :value="laserState[control.key]" @input="setNumeric(control.key, $event.target.value)" />
              </div>
              <div class="range-control">
                <label for="magic-persistence">Preview persistence <output>{{ persistenceMs }} ms</output></label>
                <input id="magic-persistence" type="range" min="0" max="100" step="1" :value="persistenceMs" @input="setPersistence($event.target.value)" />
              </div>
            </div>
          </section>
        </div>

        <MagicTouchBuilder v-else-if="activePage === 'builder'" :editing="builderEditing" />

        <div v-else-if="activePage === 'zoning'" class="magic-existing-panel">
          <ZoningPanel popout />
        </div>

        <div v-else-if="activePage === 'lasers'" class="magic-existing-panel">
          <LasersPanel popout />
        </div>

        <div v-else-if="activePage === 'preview'" class="preview-page">
          <section class="preview-heading">
            <h2>Laser preview</h2>
            <button
              class="preview-toggle"
              :class="{ enabled: previewEnabled }"
              :aria-pressed="previewEnabled"
              @click="emit('update:previewEnabled', !previewEnabled)"
            >
              <span class="status-light" :class="{ connected: previewEnabled }"></span>
              {{ previewEnabled ? 'Preview on' : 'Preview off' }}
            </button>
          </section>
        </div>

        <div v-else class="page-body master-page">
          <section class="control-section master-brightness">
            <div class="section-heading"><div><h2>Master brightness</h2><p>Live output level for the whole show.</p></div><strong class="brightness-readout">{{ Math.round(laserState.intensity * 100) }}<small>%</small></strong></div>
            <div class="range-control">
              <label for="magic-brightness">Brightness <output>{{ Math.round(laserState.intensity * 100) }}%</output></label>
              <input id="magic-brightness" class="brightness-range" type="range" min="0" max="1" step="0.01" :value="laserState.intensity" @input="setNumeric('intensity', $event.target.value)" />
            </div>
          </section>
          <section class="master-actions">
            <button
              class="flash-button"
              @pointerdown="onFlashDown"
              @pointerup="onFlashUp"
              @pointerleave="onFlashUp"
              @pointercancel="onFlashUp"
            >
              <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M13 2 4 14h6l-1 8 11-13h-7l1-7z" /></svg>
              <span>Hold to flash</span>
            </button>
            <div class="range-control release-control">
              <label for="magic-flash-release">Flash release <output>{{ formatValue(laserState.flash_release_ms, { decimals: 0 }) }} ms</output></label>
              <input id="magic-flash-release" type="range" min="0" max="2000" step="10" :value="laserState.flash_release_ms" @input="setNumeric('flash_release_ms', $event.target.value)" />
            </div>
          </section>
          <section class="control-section safety-section">
            <div class="section-heading"><div><h2>Output safety</h2><p>Blackout must be off and the output gate open for hardware output.</p></div></div>
            <div class="safety-actions">
              <button class="safety-button blackout-button" :class="{ engaged: laserState.blackout }" @click="setValues({ blackout: !laserState.blackout })">
                <span class="safety-indicator"></span>{{ laserState.blackout ? 'Blackout on' : 'Blackout off' }}
              </button>
              <button class="safety-button" :class="{ engaged: !laserState.brightness_gate_open }" @click="toggleGate">
                <span class="safety-indicator"></span>{{ laserState.brightness_gate_open ? 'Output gate open' : 'Output gate closed' }}
              </button>
              <button class="reset-button" @click="resetState().catch(console.error)">Reset settings</button>
            </div>
          </section>
          <p v-if="laserState.error" class="error-message">{{ laserState.error }}</p>
        </div>
      </div>
    </main>
  </section>
</template>

<script setup>
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import CuePanel from './CuePanel.vue'
import ZoningPanel from './ZoningPanel.vue'
import LasersPanel from './LasersPanel.vue'
import MagicTouchBuilder from './MagicTouchBuilder.vue'
import { MOVE_ICONS, SHAPE_ICONS } from './magicIcons.js'
import { flashPress, flashRelease, laserState, lasers, markLocalChange, resetState, setBrightnessGate, updateState } from '../composables/useLaserSocket.js'

const props = defineProps({
  standalone: { type: Boolean, default: false },
  persistenceMs: { type: Number, default: 20 },
  previewEnabled: { type: Boolean, default: true },
})
const emit = defineEmits(['update:persistence', 'update:previewEnabled', 'page-change'])

const PAGES = [
  { id: 'master', label: 'Master', title: 'Master', description: 'Control brightness, flash, and output safety.' },
  { id: 'preview', label: 'Preview', title: 'Preview', description: 'Inspect the live 3D stage and place laser markers.' },
  { id: 'builder', label: 'Magic Touch', title: 'Magic Touch' }, // no subtitle: header room goes to the page tabs
  { id: 'cues', label: 'Cues', title: 'Cues', description: 'Recall, save, and arrange live looks.' },
  { id: 'color', label: 'Color', title: 'Color', description: 'Set a solid color or add animated rainbow.' },
  { id: 'shapes', label: 'Shapes', title: 'Shapes', description: 'Choose a shape and tune its geometry.' },
  { id: 'move', label: 'Move', title: 'Move', description: 'Animate the shape and adjust its position.' },
  { id: 'beam', label: 'Beam', title: 'Beam', description: 'Adjust scan rate, dot density, and preview.' },
  { id: 'zoning', label: 'Zoning', title: 'Zoning', description: 'Calibrate the output area for each laser.' },
  { id: 'lasers', label: 'Lasers', title: 'Lasers', description: 'Configure and arm connected laser projectors.' },
]
const PAGE_ICONS = {
  cues: '<svg viewBox="0 0 24 24" fill="none"><rect x="3" y="3" width="18" height="18" rx="2"/><path d="M8 3v18M16 3v18M3 8h5M16 8h5M3 16h5M16 16h5"/></svg>',
  color: '<svg viewBox="0 0 24 24" fill="none"><path d="M12 3a9 9 0 100 18h1.2a2 2 0 001.4-3.4 1.8 1.8 0 011.3-3.1H18a3 3 0 003-3c0-4.7-4-8.5-9-8.5Z"/><circle cx="7.5" cy="11" r="1"/><circle cx="10" cy="7.5" r="1"/><circle cx="15" cy="8" r="1"/></svg>',
  shapes: '<svg viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="8"/><path d="m12 4 8 16H4L12 4Z"/></svg>',
  move: '<svg viewBox="0 0 24 24" fill="none"><path d="M12 3v18M3 12h18M12 3l-3 3m3-3 3 3M21 12l-3-3m3 3-3 3M12 21l3-3m-3 3-3-3M3 12l3 3m-3-3 3-3"/></svg>',
  beam: '<svg viewBox="0 0 24 24" fill="none"><path d="M3 12h5m8 0h5M8 7l8 10M8 17l8-10"/><circle cx="12" cy="12" r="2"/></svg>',
  builder: '<svg viewBox="0 0 24 24" fill="none"><rect x="3" y="3" width="7" height="7" rx="1"/><path d="M14 3v7m3-7v7m3-7v7M4 15h16M4 19h10"/></svg>',
  master: '<svg viewBox="0 0 24 24" fill="none"><path d="M12 3v18M5 7h14M7 17h10"/><circle cx="8" cy="7" r="2"/><circle cx="16" cy="17" r="2"/></svg>',
  zoning: '<svg viewBox="0 0 24 24" fill="none"><polygon points="2 6 9 3 15 6 22 3 22 18 15 21 9 18 2 21"/><path d="M9 3v15M15 6v15"/></svg>',
  lasers: '<svg viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="2"/><path d="M16.2 7.8a6 6 0 010 8.4M7.8 7.8a6 6 0 000 8.4M19 4.9a10 10 0 010 14.2M5 4.9a10 10 0 000 14.2"/></svg>',
  preview: '<svg viewBox="0 0 24 24" fill="none"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>',
}
const COLOR_PRESETS = [
  { name: 'Pure red', color: '#ff0000' }, { name: 'Pure green', color: '#00ff00' }, { name: 'Pure blue', color: '#0000ff' },
  { name: 'Warm white', color: '#fff1d0' }, { name: 'White', color: '#ffffff' }, { name: 'Rose red', color: '#ff304f' },
  { name: 'Amber', color: '#ff9f1c' }, { name: 'Yellow', color: '#ffe64d' }, { name: 'Lime', color: '#b7f531' },
  { name: 'Mint green', color: '#30e878' }, { name: 'Cyan', color: '#37e5e5' }, { name: 'Sky', color: '#42a5ff' },
  { name: 'Electric blue', color: '#4666ff' }, { name: 'Violet', color: '#9e63ff' }, { name: 'Magenta', color: '#f34bd7' },
]
const SHAPES = ['circle', 'line', 'triangle', 'square', 'wave', 'staticwave']
const MOVES = ['none', 'circle', 'pan', 'tilt', 'tiltup', 'tiltdown', 'eight', 'random']
const COLOR_CONTROLS = [
  { label: 'Rainbow amount', key: 'rainbow_amount', min: 0, max: 1, step: 0.01, decimals: 2 },
  { label: 'Rainbow speed', key: 'rainbow_speed', min: 0, max: 4, step: 0.1, decimals: 1 },
  { label: 'Rainbow phase per laser', key: 'rainbow_phase_degrees', min: 0, max: 360, step: 1, decimals: 0, suffix: '°' },
]
const SHAPE_CONTROLS = [
  { label: 'Radius', key: 'radius', min: 0.05, max: 1, step: 0.01, decimals: 2 },
  { label: 'Shape scale', key: 'shape_scale', min: -1, max: 1, step: 0.01, decimals: 2 },
  { label: 'Points', key: 'points', min: 8, max: 1000, step: 1, decimals: 0 },
]
const WAVE_CONTROLS = [
  { label: 'Frequency', key: 'wave_frequency', min: 0.1, max: 8, step: 0.1, decimals: 1 },
  { label: 'Amplitude', key: 'wave_amplitude', min: 0, max: 1, step: 0.01, decimals: 2 },
  { label: 'Speed', key: 'wave_speed', min: -4, max: 4, step: 0.1, decimals: 1 },
]
const MOVE_CONTROLS = [
  { label: 'Speed', key: 'move_speed', min: 0, max: 4, step: 0.05, decimals: 2 },
  { label: 'Size', key: 'move_size', min: 0, max: 6, step: 0.01, decimals: 2 },
  { label: 'Phase step per laser', key: 'move_phase_degrees', min: 0, max: 360, step: 1, decimals: 0, suffix: '°' },
]
const TRANSFORM_CONTROLS = [
  { label: 'Position X', key: 'pos_x', min: -1, max: 1, step: 0.01, decimals: 2 },
  { label: 'Position Y', key: 'pos_y', min: -1, max: 1, step: 0.01, decimals: 2 },
  { label: 'Rotation', key: 'rotation_speed', min: -4, max: 4, step: 0.1, decimals: 1, suffix: ' rot/s' },
]
const BEAM_CONTROLS = [
  { label: 'Dot density', key: 'dot_amount', min: 0.01, max: 1, step: 0.01, decimals: 2 },
  { label: 'Flicker rate', key: 'flicker_hz', min: 0, max: 30, step: 0.5, decimals: 1, suffix: ' Hz' },
  // How long each flicker flash is lit; 0 = auto (half of every period).
  { label: 'Flicker on time', key: 'flicker_on_ms', min: 0, max: 1000, step: 5, decimals: 0, suffix: ' ms', zeroLabel: 'Auto (50 %)' },
  // Delay per laser in list order: 0° = all flash together, e.g. 90° = chase.
  { label: 'Flicker phase per laser', key: 'flicker_phase_degrees', min: 0, max: 360, step: 1, decimals: 0, suffix: '°' },
]

const activePage = ref('cues')
const builderEditing = ref(false)

// Long-press on a pad must not open the browser's context menu (copy/paste, share, ...).
// Text fields keep it; the builder's own edit-mode menu calls preventDefault itself.
function suppressContextMenu(event) {
  if (!event.target.closest('input, textarea, select, [contenteditable="true"]')) event.preventDefault()
}
const persistenceMs = ref(props.persistenceMs)
const activePageMeta = computed(() => PAGES.find((item) => item.id === activePage.value))
const connectedLaserCount = computed(() => lasers.filter((laser) => laser.connected).length)
const hexColor = computed(() => {
  const toHex = (value) => Math.round(Math.max(0, Math.min(1, value)) * 255).toString(16).padStart(2, '0')
  return `#${toHex(laserState.r)}${toHex(laserState.g)}${toHex(laserState.b)}`
})
const ratePosition = computed(() => {
  const maximum = laserState.max_rate_kpps || 1
  return Math.pow(Math.min(1, Math.max(0, laserState.rate_kpps / maximum)), 1 / 3)
})

watch(() => props.persistenceMs, (value) => { persistenceMs.value = value })

let pendingState = {}
let updateTimer = null
let flashHeld = false

function queueState(partial) {
  markLocalChange()
  Object.assign(laserState, partial)
  Object.assign(pendingState, partial)
  clearTimeout(updateTimer)
  updateTimer = setTimeout(() => {
    const update = pendingState
    pendingState = {}
    updateState(update).catch(console.error)
  }, 70)
}

function selectPage(page) {
  activePage.value = page
  emit('page-change', page)
}
function setValues(partial) { queueState(partial) }
function setNumeric(key, value) { queueState({ [key]: Number(value) }) }
function setRatePosition(value) {
  const maximum = laserState.max_rate_kpps || 1
  queueState({ rate_kpps: Math.pow(Number(value), 3) * maximum })
}
function setPersistence(value) {
  persistenceMs.value = Number(value)
  emit('update:persistence', persistenceMs.value)
}
function formatValue(value, control = {}) {
  const decimals = control.decimals ?? 2
  const numeric = Number(value)
  if (control.zeroLabel && numeric === 0) return control.zeroLabel
  return `${numeric.toFixed(decimals)}${control.suffix || ''}`
}
function selectColor(color) {
  const red = parseInt(color.slice(1, 3), 16) / 255
  const green = parseInt(color.slice(3, 5), 16) / 255
  const blue = parseInt(color.slice(5, 7), 16) / 255
  queueState({ r: red, g: green, b: blue, rainbow_amount: 0 })
}
function onColorInput(event) { selectColor(event.target.value) }
function isPresetActive(preset) { return hexColor.value.toLowerCase() === preset.color.toLowerCase() && laserState.rainbow_amount === 0 }
function toggleGate() { setBrightnessGate(!laserState.brightness_gate_open).catch(console.error) }
function onFlashDown(event) {
  event.preventDefault()
  if (flashHeld) return
  flashHeld = true
  flashPress().catch(console.error)
}
function onFlashUp() {
  if (!flashHeld) return
  flashHeld = false
  flashRelease().catch(console.error)
}

onBeforeUnmount(() => {
  clearTimeout(updateTimer)
  if (Object.keys(pendingState).length) updateState(pendingState).catch(console.error)
  if (flashHeld) flashRelease().catch(console.error)
})
</script>

<style scoped>
.magic-touch {
  --mt-bg: #0c1116;
  --mt-panel: #11191f;
  --mt-line: rgba(159, 190, 187, 0.16);
  --mt-text: #e7f0ed;
  --mt-muted: #91a6a2;
  --mt-accent: #65e6a5;
  box-sizing: border-box;
  position: relative; z-index: 2;
  width: 100%; height: 100%; min-height: 0;
  display: grid; grid-template-columns: 172px minmax(0, 1fr);
  overflow: hidden; color: var(--mt-text); background: transparent;
  font-family: "Avenir Next", Avenir, "Segoe UI", sans-serif;
}
.magic-sidebar {
  display: flex; flex-direction: column; min-height: 0;
  padding: 18px 12px 12px; background: #10191c; border-right: 1px solid var(--mt-line);
}
.sidebar-brand { display: flex; align-items: center; padding: 2px 6px 20px; }
.brand-mark { color: var(--mt-accent); font-size: 14px; font-weight: 800; }
.page-nav { display: grid; gap: 6px; }
.page-nav-button {
  display: flex; align-items: center; gap: 12px; width: 100%; min-height: 54px; padding: 8px 10px;
  color: #a7b9b5; background: transparent; border: 1px solid transparent; border-radius: 8px;
  font: inherit; font-size: 14px; text-align: left; cursor: pointer; touch-action: manipulation;
}
.page-nav-button:hover { background: rgba(255,255,255,.045); color: var(--mt-text); }
.page-nav-button.active { color: #eafff3; background: rgba(101,230,165,.11); border-color: rgba(101,230,165,.25); }
.nav-icon { display: grid; place-items: center; width: 24px; height: 24px; flex: 0 0 24px; }
.nav-icon :deep(svg) { width: 21px; height: 21px; stroke: currentColor; stroke-width: 1.7; stroke-linecap: round; stroke-linejoin: round; }
.sidebar-status { display: flex; align-items: center; gap: 8px; margin-top: auto; padding: 12px 6px 2px; color: var(--mt-muted); font-size: 11px; }
.status-light { width: 8px; height: 8px; flex: 0 0 8px; border-radius: 50%; background: #687571; }
.status-light.connected { background: var(--mt-accent); box-shadow: 0 0 10px rgba(101,230,165,.5); }
.magic-main { display: flex; flex-direction: column; min-width: 0; min-height: 0; background: rgba(12,17,22,.86); backdrop-filter: blur(7px); }
.preview-mode .magic-main { background: transparent; backdrop-filter: none; pointer-events: none; }
.magic-header { display: flex; align-items: center; justify-content: space-between; gap: 16px; padding: 18px 24px 16px; border-bottom: 1px solid var(--mt-line); }
.eyebrow { margin: 0 0 4px; color: var(--mt-accent); font-size: 10px; font-weight: 700; letter-spacing: 1.4px; }
h1 { margin: 0; font-size: 23px; line-height: 1.2; }
.page-description { margin: 5px 0 0; color: var(--mt-muted); font-size: 13px; }
.magic-header > div:first-child { flex: 0 0 auto; min-width: 0; }
.magic-header h1 { white-space: nowrap; }
.header-actions { display: flex; align-items: center; justify-content: flex-end; gap: 10px; flex: 1 1 auto; min-width: 0; }
.page-slot { flex: 0 1 auto; min-width: 0; }
.edit-toggle { min-height: 36px; padding: 6px 14px; color: var(--mt-text); background: rgba(255,255,255,.04); border: 1px solid var(--mt-line); border-radius: 8px; font: inherit; font-size: 12px; cursor: pointer; touch-action: manipulation; white-space: nowrap; }
.edit-toggle.active { color: #eafff3; background: rgba(101,230,165,.13); border-color: var(--mt-accent); }
.laser-status { display: flex; align-items: center; gap: 8px; padding: 9px 12px; color: var(--mt-muted); border: 1px solid var(--mt-line); border-radius: 8px; font-size: 12px; white-space: nowrap; }
.laser-status.online { color: var(--mt-text); }
.magic-content { flex: 1 1 auto; min-width: 0; min-height: 0; overflow: auto; overscroll-behavior: contain; }
.magic-content.page-builder { display: flex; flex-direction: column; overflow: hidden; }
.magic-content.page-builder > * { flex: 1 1 auto; min-height: 0; }
.page-body { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); align-content: start; gap: 14px; padding: 18px 22px 24px; }
.control-section { min-width: 0; padding: 18px; background: var(--mt-panel); border: 1px solid var(--mt-line); border-radius: 8px; }
.section-heading { display: flex; align-items: center; justify-content: space-between; gap: 14px; margin-bottom: 16px; }
.section-heading h2 { margin: 0; font-size: 16px; }
.section-heading p { margin: 5px 0 0; color: var(--mt-muted); font-size: 12px; line-height: 1.4; }
.range-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 18px 24px; }
.range-control { min-width: 0; }
.range-control label { display: flex; align-items: baseline; justify-content: space-between; gap: 8px; margin-bottom: 10px; color: #c7d4d0; font-size: 13px; }
.range-control output { color: var(--mt-accent); font-variant-numeric: tabular-nums; white-space: nowrap; }
.range-control input[type="range"] { display: block; width: 100%; height: 48px; margin: 0; accent-color: var(--mt-accent); touch-action: pan-y; cursor: pointer; }
.range-control input[type="range"]::-webkit-slider-runnable-track { height: 8px; border-radius: 999px; }
.range-control input[type="range"]::-webkit-slider-thumb {
  width: 28px; height: 28px; margin-top: -10px; appearance: none;
  border: 2px solid #eafff3; border-radius: 50%; background: var(--mt-accent);
  box-shadow: 0 0 0 3px rgba(101,230,165,.2);
}
.range-control input[type="range"]::-moz-range-track { height: 8px; border-radius: 999px; }
.range-control input[type="range"]::-moz-range-thumb {
  width: 24px; height: 24px; border: 2px solid #eafff3; border-radius: 50%;
  background: var(--mt-accent); box-shadow: 0 0 0 3px rgba(101,230,165,.2);
}
.choice-grid { display: grid; gap: 9px; margin-bottom: 22px; }
.shape-grid { grid-template-columns: repeat(3, minmax(0, 1fr)); }
.move-grid { grid-template-columns: repeat(4, minmax(0, 1fr)); }
.choice-button { display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 5px; min-width: 0; min-height: 74px; padding: 8px; color: #c5d2ce; background: rgba(255,255,255,.035); border: 1px solid var(--mt-line); border-radius: 7px; font: inherit; font-size: 13px; text-transform: capitalize; cursor: pointer; touch-action: manipulation; }
.choice-icon { display: grid; place-items: center; width: 24px; height: 24px; flex: 0 0 24px; }
.choice-icon :deep(svg) { width: 22px; height: 22px; stroke: currentColor; stroke-width: 1.7; stroke-linecap: round; stroke-linejoin: round; }
.choice-button:hover { border-color: rgba(101,230,165,.45); }
.choice-button.selected { color: #eafff3; background: rgba(101,230,165,.13); border-color: var(--mt-accent); }
.color-page { grid-template-columns: minmax(0, 1.3fr) minmax(240px, .7fr); }
.color-page .palette-section { grid-row: span 2; }
.current-color, .color-picker-swatch { display: block; width: 38px; height: 38px; flex: 0 0 38px; background: var(--swatch); border: 1px solid rgba(255,255,255,.55); border-radius: 7px; box-shadow: 0 0 16px color-mix(in srgb, var(--swatch) 40%, transparent); }
.color-grid { display: grid; grid-template-columns: repeat(4, minmax(0,1fr)); gap: 10px; }
.color-preset { position: relative; display: flex; align-items: end; justify-content: start; min-height: 90px; padding: 9px; overflow: hidden; color: #fff; background: linear-gradient(180deg, transparent 30%, rgba(0,0,0,.68)), var(--swatch); border: 2px solid transparent; border-radius: 7px; text-align: left; cursor: pointer; touch-action: manipulation; }
.color-preset span { position: relative; font-size: 12px; font-weight: 700; text-shadow: 0 1px 5px rgba(0,0,0,.9); }
.color-preset.selected { border-color: #fff; box-shadow: 0 0 0 2px var(--mt-accent), 0 0 18px color-mix(in srgb, var(--swatch) 42%, transparent); }
.color-picker-row { display: flex; align-items: center; gap: 12px; min-height: 58px; margin-top: 12px; padding-top: 12px; color: #c7d4d0; border-top: 1px solid var(--mt-line); font-size: 13px; }
.color-picker-row input { width: 52px; height: 42px; margin-left: auto; padding: 3px; background: var(--mt-panel); border: 1px solid var(--mt-line); border-radius: 6px; cursor: pointer; }
.rate-readout, .brightness-readout { color: var(--mt-accent); font-size: 23px; font-variant-numeric: tabular-nums; }
.rate-readout small, .brightness-readout small { margin-left: 4px; color: var(--mt-muted); font-size: 12px; font-weight: 500; }
.beam-rate-section .range-control + .range-control { margin-top: 18px; }
.master-page { grid-template-columns: minmax(0, 1fr); max-width: 980px; margin: 0 auto; }
.master-brightness { padding: 22px 24px; }
.brightness-readout { font-size: 34px; }
.brightness-range { height: 44px !important; }
.master-actions { display: grid; grid-template-columns: minmax(200px, .8fr) minmax(0, 1.2fr); align-items: stretch; gap: 14px; }
.flash-button { display: flex; align-items: center; justify-content: center; gap: 12px; min-height: 104px; color: #fff3cc; background: linear-gradient(135deg, #7d461c, #3e291b); border: 1px solid rgba(255,188,89,.55); border-radius: 8px; font: inherit; font-size: 16px; font-weight: 700; text-transform: uppercase; cursor: pointer; touch-action: none; user-select: none; }
.flash-button:active { background: #e59036; color: #17120c; }
.flash-button svg { width: 25px; height: 25px; fill: currentColor; }
.release-control { display: flex; flex-direction: column; justify-content: center; padding: 18px; background: var(--mt-panel); border: 1px solid var(--mt-line); border-radius: 8px; }
.safety-section { border-color: rgba(255,124,111,.25); }
.safety-actions { display: grid; grid-template-columns: repeat(2, minmax(0,1fr)); gap: 10px; }
.safety-button, .reset-button { display: flex; align-items: center; justify-content: center; gap: 10px; min-height: 56px; padding: 8px; color: var(--mt-text); background: rgba(255,255,255,.04); border: 1px solid var(--mt-line); border-radius: 7px; font: inherit; font-size: 13px; cursor: pointer; touch-action: manipulation; }
.safety-button.engaged { color: #ffaaa1; background: rgba(255,92,79,.12); border-color: rgba(255,124,111,.55); }
.safety-indicator { width: 9px; height: 9px; border-radius: 50%; background: var(--mt-accent); }
.safety-button.engaged .safety-indicator { background: #ff786b; box-shadow: 0 0 10px rgba(255,120,107,.4); }
.reset-button { color: var(--mt-muted); }
.error-message { margin: 0; padding: 12px 14px; color: #ffaaa1; background: rgba(255,92,79,.1); border: 1px solid rgba(255,124,111,.35); border-radius: 7px; font-size: 12px; overflow-wrap: anywhere; }
.magic-existing-panel { flex: 1 1 auto; min-width: 0; min-height: 0; overflow: auto; padding: 18px 22px 24px; }
.magic-existing-panel :deep(#zoning-panel.popout), .magic-existing-panel :deep(#lasers-panel.popout) { height: auto; min-height: 100%; max-height: none; padding: 0; overflow: visible; }
.magic-existing-panel :deep(#zoning-panel .stage) { max-width: 100%; }
.magic-existing-panel :deep(#lasers-panel .hint) { max-width: 860px; font-size: 12px; }
.magic-existing-panel :deep(#lasers-panel .laser-list) { max-width: 980px; }
.magic-existing-panel :deep(#lasers-panel .laser-row) { min-height: 48px; gap: 8px; padding: 8px; }
.magic-existing-panel :deep(#lasers-panel input.name), .magic-existing-panel :deep(#lasers-panel input.ip) { padding: 8px; font-size: 13px; }
.magic-existing-panel :deep(#lasers-panel button) { min-height: 40px; padding: 8px 10px; }
.preview-mode .magic-content { overflow: hidden; pointer-events: none; }
.preview-page { display: flex; flex-direction: column; height: 100%; min-height: 0; padding: 8px 16px; }
.preview-heading { display: flex; align-items: center; justify-content: space-between; gap: 12px; pointer-events: auto; }
.preview-heading h2 { margin: 0; font-size: 16px; }
.preview-toggle { display: inline-flex; align-items: center; justify-content: center; gap: 8px; min-height: 36px; padding: 6px 10px; color: var(--mt-text); background: rgba(12,17,22,.88); border: 1px solid var(--mt-line); border-radius: 7px; font: inherit; font-size: 12px; cursor: pointer; touch-action: manipulation; }
.preview-toggle.enabled { border-color: rgba(101,230,165,.5); }
.preview-controls { display: grid; grid-template-columns: minmax(0,1fr) minmax(220px,.8fr); align-items: center; gap: 20px; max-width: 760px; padding: 16px; background: rgba(17,25,31,.94); border: 1px solid var(--mt-line); border-radius: 8px; }
.preview-control-copy h3 { margin: 0; font-size: 14px; }
.preview-control-copy p { margin: 5px 0 0; color: var(--mt-muted); font-size: 12px; }
.preview-hint { margin: auto 0 0; color: var(--mt-muted); font-size: 12px; text-shadow: 0 1px 4px #000; }
.preview-offline { display: flex; align-items: flex-start; gap: 10px; max-width: 560px; margin-top: 8px; padding: 12px 14px; color: #c7d4d0; background: rgba(17,25,31,.88); border: 1px solid rgba(255,188,89,.3); border-radius: 7px; font-size: 12px; }
.preview-offline .status-light { margin-top: 3px; background: #e6ad58; }
.preview-offline p { margin: 4px 0 0; color: var(--mt-muted); line-height: 1.4; }
.magic-cues { width: 100%; height: 100%; min-height: 0; overflow: auto; }
.magic-cues :deep(#cue-panel.popout) { height: auto; min-height: 100%; padding: 16px; overflow: visible; }
.magic-cues :deep(#cue-panel.popout .panel-body) { display: block; height: auto; }
.magic-cues :deep(#cue-panel.popout .cue-grid) { grid-template-columns: repeat(auto-fill, minmax(52px, 1fr)); grid-template-rows: none; grid-auto-rows: minmax(52px, auto); gap: 8px; }
.magic-cues :deep(#cue-panel.popout .cue-btn) { aspect-ratio: 1; }
.magic-cues :deep(#cue-panel.popout .hint) { margin-top: 10px; }
.standalone { min-height: 100vh; }

@media (max-width: 760px) {
  .magic-touch { grid-template-columns: 82px minmax(0,1fr); }
  .magic-sidebar { padding: 12px 7px 10px; }
  .sidebar-brand { justify-content: center; padding: 2px 0 14px; }
  .page-nav-button { flex-direction: column; justify-content: center; gap: 4px; min-height: 62px; padding: 6px 2px; font-size: 10px; }
  .nav-icon { width: 25px; height: 25px; }
  .sidebar-status { justify-content: center; padding-inline: 0; font-size: 0; }
  .magic-header { align-items: flex-start; padding: 14px 16px 12px; }
  h1 { font-size: 20px; }
  .page-description { max-width: 40vw; font-size: 11px; }
  .laser-status { gap: 6px; padding: 8px; font-size: 10px; }
  .page-body { grid-template-columns: minmax(0,1fr); gap: 10px; padding: 12px; }
  .color-page { grid-template-columns: minmax(0,1fr); }
  .color-page .palette-section { grid-row: auto; }
  .color-grid { grid-template-columns: repeat(3,minmax(0,1fr)); }
  .color-preset { min-height: 72px; }
  .range-grid { grid-template-columns: minmax(0,1fr); gap: 12px; }
  .control-section { padding: 14px; }
  .choice-button { min-height: 54px; }
  .move-grid { grid-template-columns: repeat(2,minmax(0,1fr)); }
  .master-actions { grid-template-columns: minmax(0,1fr); }
  .flash-button { min-height: 76px; }
  .magic-existing-panel { padding: 12px; }
  .magic-existing-panel :deep(#zoning-panel .stage) { transform: scale(.88); transform-origin: top center; margin-bottom: -24px; }
  .magic-existing-panel :deep(#lasers-panel .laser-row) { flex-wrap: wrap; }
  .preview-page { padding: 8px 12px; }
}
</style>