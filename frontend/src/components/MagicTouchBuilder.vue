<template>
  <section class="mtb" :class="{ editing, loading: !loaded }">
    <div ref="bodyEl" class="mtb-body">
      <div
        ref="canvasEl" class="mtb-canvas"
        @pointerdown.self="selectedIds = []"
        @pointerdown.capture="onPanDown" @pointermove.capture="onPanMove"
        @pointerup.capture="onPanUp" @pointercancel.capture="onPanUp"
      >
        <p v-if="!widgets.length" class="mtb-empty">
          {{ editing ? 'Add an element to get started.' : 'Nothing here yet. Switch to edit mode to add controls.' }}
        </p>

        <div
          v-for="w in widgets" :key="w.id"
          class="mtb-widget"
          :class="{ selected: editing && selectedIds.includes(w.id) }"
          :style="{ left: w.x + 'px', top: w.y + 'px', width: w.w + 'px', height: w.h + 'px', '--cs': (w.controlScale ?? 100) / 100 }"
          @pointerdown="onWidgetDown($event, w)"
          @contextmenu.prevent="onContextMenu($event, w)"
        >
          <template v-if="w.type === 'fader'">
            <div v-if="w.showLabel !== false" class="mtb-label" :style="textStyle(w, 'labelScale', 12)">{{ w.label }}</div>
            <div
              class="mtb-fader"
              @pointerdown="onFaderDown($event, w)"
              @pointermove="onFaderMove($event, w)"
              @pointerup="endGesture"
              @pointercancel="endGesture"
            >
              <div class="mtb-track">
                <div class="mtb-fill" :style="{ height: percent(w) + '%' }"></div>
                <div class="mtb-thumb" :style="{ bottom: `calc(${percent(w)}% - 10px * var(--cs, 1))` }"></div>
              </div>
            </div>
            <div v-if="w.showValue !== false" class="mtb-value" :style="textStyle(w, 'valueScale', 12)">{{ display(w) }}</div>
          </template>

          <template v-else-if="w.type === 'knob'">
            <div v-if="w.showLabel !== false" class="mtb-label" :style="textStyle(w, 'labelScale', 12)">{{ w.label }}</div>
            <div
              class="mtb-knob"
              @pointerdown="onKnobDown($event, w)"
              @pointermove="onKnobMove($event, w)"
              @pointerup="endGesture"
              @pointercancel="endGesture"
            >
              <svg viewBox="0 0 100 100" preserveAspectRatio="xMidYMid meet">
                <circle cx="50" cy="50" r="38" class="knob-body" />
                <path d="M 22.1 77.9 A 39.5 39.5 0 1 1 77.9 77.9" class="knob-track" pathLength="100" />
                <path d="M 22.1 77.9 A 39.5 39.5 0 1 1 77.9 77.9" class="knob-arc" pathLength="100" :stroke-dasharray="`${percent(w) * 0.75 + 0.01} 200`" />
                <line x1="50" y1="50" x2="50" y2="18" class="knob-pointer" :transform="`rotate(${-135 + percent(w) * 2.7} 50 50)`" />
              </svg>
            </div>
            <div v-if="w.showValue !== false" class="mtb-value" :style="textStyle(w, 'valueScale', 12)">{{ display(w) }}</div>
          </template>

          <template v-else-if="w.type === 'xy'">
            <div v-if="w.showLabel !== false" class="mtb-label" :style="textStyle(w, 'labelScale', 12)">{{ w.label }}</div>
            <div
              class="mtb-xy"
              @pointerdown="onXYDown($event, w)"
              @pointermove="onXYMove($event, w)"
              @pointerup="endGesture"
              @pointercancel="endGesture"
            >
              <div class="mtb-xy-line v" :style="{ left: ratio(w) * 100 + '%' }"></div>
              <div class="mtb-xy-line h" :style="{ top: (1 - axisRatio(w, defForY(w), w.invY)) * 100 + '%' }"></div>
              <div class="mtb-xy-dot" :style="{ left: ratio(w) * 100 + '%', top: (1 - axisRatio(w, defForY(w), w.invY)) * 100 + '%' }"></div>
            </div>
            <div v-if="w.showValue !== false" class="mtb-value" :style="textStyle(w, 'valueScale', 12)">{{ display(w) }} · {{ displayY(w) }}</div>
          </template>

          <template v-else>
            <button
              class="mtb-pad"
              :class="{ down: isLit(w), swatch: defFor(w).swatch, cue: defFor(w).cuePad, populated: defFor(w).cuePad && cues[w.param] }"
              :style="defFor(w).swatch ? { '--swatch': defFor(w).swatch } : null"
              :disabled="editing"
              @pointerdown.prevent="onButtonDown($event, w)"
              @pointerup="onButtonUp($event, w)"
              @pointercancel="onButtonUp($event, w)"
              @lostpointercapture="onButtonUp($event, w)"
            >
              <CuePreview v-if="defFor(w).cuePad && cues[w.param]" :cue="cues[w.param]" />
              <span v-if="defFor(w).icon" class="mtb-pad-icon" aria-hidden="true" v-html="defFor(w).icon"></span>
              <span v-if="w.showLabel !== false" class="mtb-pad-text" :class="{ 'mtb-pad-text-small': defFor(w).icon }" :style="textStyle(w, 'labelScale', defFor(w).icon ? 9 : 12)"><span v-fit="`${w.label}|${w.labelScale}`" class="mtb-fit">{{ w.label }}</span></span>
            </button>
          </template>

          <div v-if="editing" class="mtb-resize" @pointerdown.stop="onResizeDown($event, w)"></div>
        </div>
        <!-- Stretches the scroll area past the outermost element (layouts built on wider screens). -->
        <div class="mtb-spacer" :style="{ left: `${contentSize.w}px`, top: `${contentSize.h}px` }" aria-hidden="true"></div>
        <template v-for="(g, i) in guides" :key="i">
          <div v-if="g.x !== undefined" class="mtb-guide v" :style="{ left: g.x + 'px' }"></div>
          <div v-else class="mtb-guide h" :style="{ top: g.y + 'px' }"></div>
        </template>
      </div>

      <div v-if="menu" class="mtb-menu" :style="{ left: menu.x + 'px', top: menu.y + 'px' }" @pointerdown.stop @contextmenu.prevent>
        <button @click="duplicateSelected">Duplicate</button>
        <button class="danger" @click="removeSelected(); closeMenu()">Delete</button>
      </div>

      <div
        v-if="editing" ref="editorEl" class="mtb-editor" role="dialog" aria-label="Editor"
        :style="editorPos ? { left: editorPos.x + 'px', top: editorPos.y + 'px' } : { right: '16px', top: '16px' }"
      >
        <div
          class="mtb-editor-head"
          @pointerdown="onEditorDown"
          @pointermove="onEditorMove"
          @pointerup="onEditorUp"
          @pointercancel="onEditorUp"
        >
          <span>Editor</span>
          <button class="mtb-editor-collapse" :aria-expanded="!editorCollapsed" aria-label="Collapse editor" @pointerdown.stop @click="editorCollapsed = !editorCollapsed">{{ editorCollapsed ? '+' : '−' }}</button>
        </div>
        <div v-show="!editorCollapsed" class="mtb-editor-body">
          <section>
            <h2>Add element</h2>
            <div class="mtb-row">
              <button class="mtb-btn" @click="addWidget('fader')">Fader</button>
              <button class="mtb-btn" @click="addWidget('knob')">Knob</button>
              <button class="mtb-btn" @click="addWidget('button')">Button</button>
              <button class="mtb-btn" @click="addWidget('xy')">X/Y pad</button>
            </div>
          </section>
          <section>
            <template v-if="selected">
              <h2>{{ TYPE_NAMES[selected.type] }}</h2>
              <label>Label
                <input v-model="selected.label" class="mtb-field" type="text" maxlength="24" />
              </label>
              <label>{{ selected.type === 'xy' ? 'X function (horizontal)' : 'Function' }}
                <select class="mtb-field" :value="selected.fn" @change="onFunctionChange(selected, $event.target.value)">
                  <optgroup v-for="group in groupedFunctions(selected.type)" :key="group.name" :label="group.name">
                    <option v-for="item in group.items" :key="item.id" :value="item.id">{{ item.label }}</option>
                  </optgroup>
                </select>
              </label>
              <label v-if="selected.type !== 'button'" class="mtb-check">
                <input type="checkbox" :checked="!!selected.inv" @change="selected.inv = $event.target.checked || undefined" /> Invert{{ selected.type === 'xy' ? ' X' : '' }}
              </label>
              <template v-if="selected.type === 'xy'">
                <label>Y function (vertical)
                  <select class="mtb-field" :value="selected.fnY" @change="onFunctionYChange(selected, $event.target.value)">
                    <optgroup v-for="group in groupedFunctions('xy')" :key="group.name" :label="group.name">
                      <option v-for="item in group.items" :key="item.id" :value="item.id">{{ item.label }}</option>
                    </optgroup>
                  </select>
                </label>
                <label class="mtb-check">
                  <input type="checkbox" :checked="!!selected.invY" @change="selected.invY = $event.target.checked || undefined" /> Invert Y
                </label>
              </template>
              <label v-if="defFor(selected).param">{{ defFor(selected).param.label }}
                <input class="mtb-field" type="number" :min="defFor(selected).param.min" :max="defFor(selected).param.max" :value="selected.param" @change="onParamChange(selected, $event.target.value)" />
              </label>
              <label v-if="defFor(selected).layerOption" title="Solo replaces the current look. Layer plays this cue on top of the current look - tap again to take it off.">Playback
                <select class="mtb-field" :value="selected.layer ? 'layer' : 'solo'" @change="selected.layer = $event.target.value === 'layer' || undefined">
                  <option value="solo">Solo – replaces the current look</option>
                  <option value="layer">Layer – plays on top of the current look</option>
                </select>
              </label>
              <label
                v-if="defFor(selected).momentaryOption" class="mtb-check"
                :title="selected.layer ? 'The layer only plays while the button is held' : 'The cue is only active while the button is held; releasing restores the previous look'"
              >
                <input type="checkbox" :checked="!!selected.momentary" @change="selected.momentary = $event.target.checked || undefined" /> Flash (only while held)
              </label>
              <label class="mtb-check">
                <input type="checkbox" :checked="selected.showLabel !== false" @change="selected.showLabel = $event.target.checked" /> Show label
              </label>
              <label v-if="selected.type !== 'button'" class="mtb-check">
                <input type="checkbox" :checked="selected.showValue !== false" @change="selected.showValue = $event.target.checked" /> Show value
              </label>
              <label
                v-if="(selected.type === 'knob' || selected.type === 'xy') && (defFor(selected)?.tapReset || defForY(selected)?.tapReset)"
                class="mtb-check"
                title="Double tap resets the rotation: angle back to 0 and spinning stops. Untick to only stop spinning and keep the current angle."
              >
                <input type="checkbox" :checked="!selected.keepAngle" @change="selected.keepAngle = !$event.target.checked || undefined" /> Double tap: rotation reset (angle to 0)
              </label>
              <label v-if="selected.type !== 'button'" class="mtb-check" title="Value changes slowly at first and faster towards the end (around the centre for functions centred on 0)">
                <input type="checkbox" :checked="!!selected.log" @change="selected.log = $event.target.checked || undefined" /> Logarithmic curve
              </label>
              <div v-for="item in sizeControls(selected)" :key="item.key" class="mtb-size">
                <span>{{ item.label }}</span>
                <input type="range" min="30" max="250" step="5" :value="selected[item.key] ?? 100" @input="setSize(selected, item.key, $event.target.value)" />
                <button class="mtb-size-auto" :disabled="selected[item.key] === undefined" @click="delete selected[item.key]">{{ selected[item.key] === undefined ? 'Auto' : selected[item.key] + '% ✕' }}</button>
              </div>
              <button class="mtb-btn danger" @click="removeSelected">Delete element</button>
            </template>
            <template v-else-if="selectedIds.length > 1">
              <h2>{{ selectedIds.length }} elements</h2>
              <p class="mtb-muted">Drag any of them to move together.</p>
              <button class="mtb-btn danger" @click="removeSelected">Delete elements</button>
            </template>
            <p v-else class="mtb-muted">Select an element to edit it. Shift-click selects several.</p>
          </section>
          <section>
            <h2>Page</h2>
            <label>Name
              <input v-model="activePage.name" class="mtb-field" type="text" maxlength="20" />
            </label>
            <div class="mtb-row">
              <button class="mtb-btn" :disabled="pages[0].id === activePage.id" @click="movePage(-1)">◂ Left</button>
              <button class="mtb-btn" :disabled="pages[pages.length - 1].id === activePage.id" @click="movePage(1)">Right ▸</button>
            </div>
            <button class="mtb-btn danger" :disabled="pages.length < 2" @click="removePage">Delete page</button>
            <button class="mtb-btn" @click="resetLayout">Restore default layout</button>
          </section>
        </div>
      </div>
    </div>

    <!-- Page tabs live in the Magic Touch header, left of the Run/Edit toggle (frees the bottom row). -->
    <Teleport defer to="#mtb-page-slot">
      <nav class="mtb-pages" aria-label="Pages">
        <button
          v-for="page in pages" :key="page.id"
          class="mtb-page-tab"
          :class="{ active: page.id === activePage.id }"
          @click="selectPage(page.id)"
        >{{ page.name }}</button>
        <button v-if="editing" class="mtb-page-tab add" aria-label="Add page" @click="addPage">+</button>
      </nav>
    </Teleport>
  </section>
</template>

<script setup>
import { computed, onBeforeUnmount, reactive, ref, watch } from 'vue'
import {
  flashPress, flashRelease, cues, isCueCurrent, laserState, lasers, markLocalChange, recallCue, resetState, rotationReset,
  momentaryCuePress, momentaryCueRelease, setCueLayer, clearLayers,
  setBrightnessGate, updateLaser, updateState, magicTouch, fetchMagicTouch, saveMagicTouch,
} from '../composables/useLaserSocket.js'
import CuePreview from './CuePreview.vue'
import { MOVE_ICONS, SHAPE_ICONS } from './magicIcons.js'
import { buildDefaultPages, colorFnId, COLORS, MOVES, SHAPES } from './magicLayout.js'

// Pages live on the backend (GET/POST /api/magic-touch) so every client shares one layout.
// This old per-browser key is only read once, to migrate a layout into an empty backend.
const LEGACY_STORAGE_KEY = 'bc3.magicTouch.layout.v3'
const MIN_SIZE = { fader: [70, 140], knob: [80, 100], button: [70, 50], xy: [120, 120] }
const DEFAULT_SIZE = { fader: [110, 280], knob: [120, 150], button: [120, 80], xy: [240, 264] }
const DEFAULT_FN = { fader: 'master_brightness', knob: 'release_time', button: 'flash', xy: 'pos_x' }
const DEFAULT_FN_Y = 'pos_y'
const TYPE_NAMES = { fader: 'Fader', knob: 'Knob', button: 'Button', xy: 'X/Y pad' }
const CUE_COUNT = 128
const LASER_SLOTS = 8

// Value functions (fader/knob) write a backend field via POST /api/state.
const num = (group, label, key, min, max, decimals = 2, suffix = '') => ({
  group, label, key, min, max,
  format: (v) => `${key === 'intensity' ? Math.round(v * 100) : v.toFixed(decimals)}${key === 'intensity' ? '%' : suffix}`,
})
const VALUE_FNS = {
  master_brightness: num('Master', 'Master brightness', 'intensity', 0, 1),
  release_time: num('Master', 'Release time', 'flash_release_ms', 0, 2000, 0, ' ms'),
  color_r: num('Color', 'Red', 'r', 0, 1),
  color_g: num('Color', 'Green', 'g', 0, 1),
  color_b: num('Color', 'Blue', 'b', 0, 1),
  rainbow_amount: num('Color', 'Rainbow amount', 'rainbow_amount', 0, 1),
  rainbow_speed: num('Color', 'Rainbow speed', 'rainbow_speed', 0, 4, 1),
  rainbow_phase: num('Color', 'Rainbow phase per laser', 'rainbow_phase_degrees', 0, 360, 0, '°'),
  radius: num('Shapes', 'Radius', 'radius', 0.05, 1),
  shape_scale: num('Shapes', 'Shape scale', 'shape_scale', -1, 1),
  points: num('Shapes', 'Points', 'points', 8, 1000, 0),
  wave_frequency: num('Shapes', 'Wave frequency', 'wave_frequency', 0.1, 8, 1),
  wave_amplitude: num('Shapes', 'Wave amplitude', 'wave_amplitude', 0, 1),
  wave_speed: num('Shapes', 'Wave speed', 'wave_speed', -4, 4, 1),
  move_speed: num('Move', 'Move speed', 'move_speed', 0, 4),
  move_size: num('Move', 'Move size', 'move_size', 0, 6),
  move_phase: num('Move', 'Phase step per laser', 'move_phase_degrees', 0, 360, 0, '°'),
  pos_x: num('Move', 'Position X', 'pos_x', -1, 1),
  pos_y: num('Move', 'Position Y', 'pos_y', -1, 1),
  // Double tap = full rotation reset (angle back to 0 + stop), unless the
  // element opts out via w.keepAngle (then it only stops spinning).
  rotation: { ...num('Move', 'Rotation', 'rotation_speed', -4, 4, 1, ' rot/s'), tapReset: (w) => (w.keepAngle ? null : rotationReset()) },
  dot_amount: num('Shapes', 'Dot density', 'dot_amount', 0.01, 1),
  dot_dwell: num('Beam', 'Dot dwell', 'dot_dwell_us', 30, 3000, 0, ' µs'),
  dot_blank: num('Beam', 'Dot blanking', 'dot_blank_us', 0, 3000, 0, ' µs'),
  flicker: num('Shapes', 'Flicker rate', 'flicker_hz', 0, 30, 1, ' Hz'),
  // Lit time per flicker flash; 0 = auto (half of every period, the old behaviour).
  flicker_on: { ...num('Shapes', 'Flicker on time', 'flicker_on_ms', 0, 1000, 0, ' ms'), format: (v) => (Number(v) === 0 ? 'Auto' : `${Math.round(v)} ms`) },
  flicker_phase: num('Shapes', 'Flicker phase per laser', 'flicker_phase_degrees', 0, 360, 0, '°'),
  max_rate: num('Beam', 'Maximum rate', 'max_rate_kpps', 1, 100, 0, ' kpps'),
  output_latency: num('Beam', 'Output latency', 'output_latency_ms', 10, 200, 0, ' ms'),
  rate: {
    group: 'Beam', label: 'Scan rate', key: 'rate_kpps', min: 0, max: 1,
    format: (v) => `${v.toFixed(1)} kpps`,
    // Cubic taper relative to the maximum rate, same as the Beam page.
    toRatio: (v) => Math.cbrt(v / (laserState.max_rate_kpps || 1)),
    fromRatio: (r) => Math.pow(r, 3) * (laserState.max_rate_kpps || 1),
  },
}

// Button functions: 'momentary' = press on touch down, release on up; 'trigger' = action on touch down only.
const hexOf = () => {
  const h = (v) => Math.round(Math.max(0, Math.min(1, v)) * 255).toString(16).padStart(2, '0')
  return `#${h(laserState.r)}${h(laserState.g)}${h(laserState.b)}`
}
const BUTTON_FNS = {
  flash: { group: 'Master', label: 'White flash', mode: 'momentary', press: () => flashPress(), release: () => flashRelease() },
  // Footswitch behavior: gate open while held, fades out over the release time.
  master_brightness_flash: { group: 'Master', label: 'Master brightness flash', mode: 'momentary', press: () => setBrightnessGate(true), release: () => setBrightnessGate(false) },
  blackout_toggle: { group: 'Master', label: 'Blackout toggle', mode: 'trigger', press: () => updateState({ blackout: !laserState.blackout }), active: () => laserState.blackout },
  gate_toggle: { group: 'Master', label: 'Output gate toggle', mode: 'trigger', press: () => setBrightnessGate(!laserState.brightness_gate_open), active: () => laserState.brightness_gate_open },
  reset: { group: 'Master', label: 'Reset settings', mode: 'trigger', press: () => resetState() },
  rotation_reset: { group: 'Move', label: 'Rotation reset', mode: 'trigger', press: () => rotationReset() },
  // Cue playback, named like in lighting consoles / Pangolin Beyond:
  // - Solo (default): the cue replaces the current look.
  // - Layer (w.layer): the cue plays *on top of* the current look; tap toggles it.
  // - Flash (w.momentary): only while held - Solo flash puts the previous look
  //   back on release, Layer flash removes the layer on release.
  cue_recall: {
    group: 'Cues', label: 'Recall cue', mode: 'trigger', momentaryOption: true, layerOption: true, cuePad: true,
    param: { label: 'Cue number', min: 1, max: CUE_COUNT, default: 1 },
    press: (w) => {
      if (w.layer) return setCueLayer(w.param, w.momentary ? 'on' : 'toggle')
      return w.momentary ? momentaryCuePress(w.param) : recallCue(w.param)
    },
    release: (w) => (w.layer ? setCueLayer(w.param, 'off') : momentaryCueRelease(w.param)),
    active: (w) => (w.layer ? (laserState.layers || []).includes(Number(w.param)) : isCueCurrent(w.param)),
  },
  layers_clear: {
    group: 'Cues', label: 'Release all layers', mode: 'trigger',
    press: () => clearLayers(),
    active: () => (laserState.layers || []).length > 0,
  },
}
for (const [name, color] of COLORS) {
  const r = parseInt(color.slice(1, 3), 16) / 255
  const g = parseInt(color.slice(3, 5), 16) / 255
  const b = parseInt(color.slice(5, 7), 16) / 255
  BUTTON_FNS[colorFnId(name)] = {
    group: 'Color', label: `Color: ${name}`, swatch: color, mode: 'trigger',
    press: () => updateState({ r, g, b, rainbow_amount: 0 }),
    active: () => hexOf() === color && laserState.rainbow_amount === 0,
  }
}
for (const shape of SHAPES) {
  BUTTON_FNS[`shape_${shape}`] = { group: 'Shapes', label: `Shape: ${shape}`, icon: SHAPE_ICONS[shape], mode: 'trigger', press: () => updateState({ shape }), active: () => laserState.shape === shape }
}
for (const mode of MOVES) {
  BUTTON_FNS[`move_${mode}`] = { group: 'Move', label: `Move: ${mode}`, icon: MOVE_ICONS[mode], mode: 'trigger', press: () => updateState({ move_mode: mode }), active: () => laserState.move_mode === mode }
}
for (let n = 1; n <= LASER_SLOTS; n++) {
  BUTTON_FNS[`laser_arm_${n}`] = {
    group: 'Lasers', label: `Laser ${n} arm toggle`, mode: 'trigger',
    press: () => { const l = lasers[n - 1]; return l ? updateLaser(l.id, { armed: !l.armed }) : Promise.resolve() },
    active: () => !!lasers[n - 1]?.armed,
  }
}

// X/Y pads: `fn` is the horizontal function, `fnY` the vertical one (both value functions).
const FUNCTIONS = { fader: VALUE_FNS, knob: VALUE_FNS, button: BUTTON_FNS, xy: VALUE_FNS }
const defFor = (w) => FUNCTIONS[w.type]?.[w.fn]
const defForY = (w) => VALUE_FNS[w.fnY]
function groupedFunctions(type) {
  const groups = new Map()
  for (const [id, def] of Object.entries(FUNCTIONS[type])) {
    if (!groups.has(def.group)) groups.set(def.group, [])
    groups.get(def.group).push({ id, label: def.label })
  }
  return [...groups].map(([name, items]) => ({ name, items }))
}
const defaultLabel = (w) => {
  const def = defFor(w)
  if (w.type === 'xy') return `${def.label} / ${defForY(w)?.label ?? ''}`
  return def.param ? `${def.label} ${w.param}` : def.label
}

const newId = () => `w${Date.now().toString(36)}${Math.random().toString(36).slice(2, 5)}`

function materialize(spec) {
  const widget = { ...spec, id: newId() }
  widget.label ??= defaultLabel(widget)
  return widget
}
function cleanWidgets(list) {
  return (Array.isArray(list) ? list : []).filter((w) => w && defFor(w) && (w.type !== 'xy' || defForY(w)))
}
function defaultPages() {
  return buildDefaultPages().map((p) => ({ id: newId(), name: p.name, widgets: p.widgets.map(materialize) }))
}
// Returns null when `stored` holds no usable pages.
function parsePages(stored) {
  if (!Array.isArray(stored?.pages) || !stored.pages.length) return null
  return stored.pages.map((p) => ({ id: p.id || newId(), name: String(p.name || 'Page'), widgets: cleanWidgets(p.widgets) }))
}
function loadLegacyPages() {
  try {
    return parsePages(JSON.parse(localStorage.getItem(LEGACY_STORAGE_KEY) || 'null'))
  } catch { return null }
}

// The pages are shared by all clients; the open page, selection, edit mode and editor window
// stay per client. Starts from the last layout this tab saw (instant on remount); hidden until
// the backend answered once.
const pages = ref(parsePages(magicTouch.layout) || defaultPages())
const loaded = ref(!!magicTouch.layout)
const activePageId = ref(pages.value[0].id)
const activePage = computed(() => pages.value.find((p) => p.id === activePageId.value) || pages.value[0])
const widgets = computed({
  get: () => activePage.value.widgets,
  set: (list) => { activePage.value.widgets = list },
})
// Edit/run mode is toggled from the page header in MagicTouchPanel.
const props = defineProps({ editing: { type: Boolean, default: false } })
const editing = computed(() => props.editing)

// Floating editor window: free position (persisted per browser), translucent, collapsible.
const EDITOR_POS_KEY = 'bc3.magicTouch.editorPos.v1'
const editorEl = ref(null)
const editorCollapsed = ref(false)
const editorPos = ref((() => {
  try {
    const p = JSON.parse(localStorage.getItem(EDITOR_POS_KEY) || 'null')
    return Number.isFinite(p?.x) && Number.isFinite(p?.y) ? p : null
  } catch { return null }
})())
let editorDrag = null
function onEditorDown(event) {
  const el = editorEl.value
  editorDrag = { startX: event.clientX, startY: event.clientY, x: el.offsetLeft, y: el.offsetTop }
  event.currentTarget.setPointerCapture(event.pointerId)
}
function onEditorMove(event) {
  if (!editorDrag) return
  const el = editorEl.value
  const body = el.offsetParent
  const maxX = Math.max(0, body.clientWidth - el.offsetWidth)
  const maxY = Math.max(0, body.clientHeight - 40)
  editorPos.value = {
    x: Math.min(maxX, Math.max(0, editorDrag.x + event.clientX - editorDrag.startX)),
    y: Math.min(maxY, Math.max(0, editorDrag.y + event.clientY - editorDrag.startY)),
  }
}
function onEditorUp() {
  if (!editorDrag) return
  editorDrag = null
  localStorage.setItem(EDITOR_POS_KEY, JSON.stringify(editorPos.value))
}
// Bottom-right corner of the content plus a grid margin; drives the scroll area size.
const contentSize = computed(() => ({
  w: Math.max(0, ...widgets.value.map((w) => w.x + w.w)) + GRID,
  h: Math.max(0, ...widgets.value.map((w) => w.y + w.h)) + GRID,
}))
const selectedIds = ref([])
// Momentary buttons currently held: pointerId -> widget (several at once, multitouch).
const held = reactive(new Map())
const canvasEl = ref(null)
const selected = computed(() => (selectedIds.value.length === 1 ? widgets.value.find((w) => w.id === selectedIds.value[0]) || null : null))

watch(editing, (on) => {
  if (on) releaseHeldButton()
  else selectedIds.value = []
})

function selectPage(id) {
  releaseHeldButton()
  selectedIds.value = []
  activePageId.value = id
}
function addPage() {
  const page = { id: newId(), name: `Page ${pages.value.length + 1}`, widgets: [] }
  pages.value.push(page)
  selectPage(page.id)
}
function removePage() {
  if (pages.value.length < 2 || !window.confirm(`Delete page "${activePage.value.name}"?`)) return
  const index = pages.value.findIndex((p) => p.id === activePageId.value)
  pages.value.splice(index, 1)
  selectPage(pages.value[Math.max(0, index - 1)].id)
}
function movePage(delta) {
  const index = pages.value.findIndex((p) => p.id === activePageId.value)
  const target = index + delta
  if (target < 0 || target >= pages.value.length) return
  const [page] = pages.value.splice(index, 1)
  pages.value.splice(target, 0, page)
}
function resetLayout() {
  if (!window.confirm('Replace all pages with the default live layout? Your own layout will be lost.')) return
  releaseHeldButton()
  pages.value = defaultPages()
  selectPage(pages.value[0].id)
}

function addWidget(type) {
  const offset = (widgets.value.length % 6) * 24
  const fn = DEFAULT_FN[type]
  const [w, h] = DEFAULT_SIZE[type]
  const spec = { type, fn, x: 24 + offset, y: 24 + offset, w, h }
  if (type === 'xy') spec.fnY = DEFAULT_FN_Y
  const widget = materialize(spec)
  widgets.value.push(widget)
  selectedIds.value = [widget.id]
}
function removeSelected() {
  widgets.value = widgets.value.filter((w) => !selectedIds.value.includes(w.id))
  selectedIds.value = []
}
function onFunctionYChange(w, fn) {
  const previousLabel = defaultLabel(w)
  w.fnY = fn
  if (w.label === previousLabel) w.label = defaultLabel(w)
}
function onFunctionChange(w, fn) {
  const previousLabel = defaultLabel(w)
  w.fn = fn
  const param = defFor(w).param
  if (param) w.param = param.default
  else delete w.param
  if (w.label === previousLabel) w.label = defaultLabel(w)
}
// Optional per-element size overrides in percent; undefined keeps the automatic fit.
function textStyle(w, key, base) {
  return w[key] === undefined ? null : { fontSize: `${(base * w[key]) / 100}px` }
}
function sizeControls(w) {
  const list = [{ key: 'controlScale', label: w.type === 'button' ? 'Icon size' : 'Control size' }]
  if (w.showLabel !== false) list.push({ key: 'labelScale', label: 'Label size' })
  if (w.type !== 'button' && w.showValue !== false) list.push({ key: 'valueScale', label: 'Value size' })
  return list
}
function setSize(w, key, value) { w[key] = Number(value) }

// v-fit: makes a button label fit inside its button. The text first wraps at spaces onto
// more lines; only if a word or the line stack still doesn't fit is the font shrunk (via
// --fit, relative to its normal size). Icon buttons keep at least half the height for the icon.
// Re-measured when the button resizes or the bound value (label, size) changes.
function fitText(el) {
  const box = el.closest('.mtb-pad')
  if (!box) return
  const cs = getComputedStyle(box)
  let availW = box.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight) - 2
  let availH = box.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom)
  if (box.querySelector('.mtb-pad-icon')) availH *= 0.5
  availW = Math.max(1, availW)
  availH = Math.max(1, availH)
  el.style.maxWidth = `${availW}px`
  const fits = (f) => {
    el.style.setProperty('--fit', f)
    return el.scrollWidth <= availW + 0.5 && el.offsetHeight <= availH + 0.5
  }
  if (fits(1)) return
  let lo = 0.02
  let hi = 1
  for (let i = 0; i < 10; i++) {
    const mid = (lo + hi) / 2
    if (fits(mid)) lo = mid
    else hi = mid
  }
  el.style.setProperty('--fit', lo.toFixed(3))
}
const vFit = {
  mounted(el) {
    el._fitObserver = new ResizeObserver(() => fitText(el))
    el._fitObserver.observe(el.closest('.mtb-pad') || el)
    fitText(el)
  },
  updated(el, binding) { if (binding.value !== binding.oldValue) fitText(el) },
  unmounted(el) { el._fitObserver?.disconnect() },
}

// Right-click menu (edit mode): acts on the selection.
const bodyEl = ref(null)
const menu = ref(null)
function onContextMenu(event, w) {
  if (!editing.value) return
  if (!selectedIds.value.includes(w.id)) selectedIds.value = [w.id]
  const rect = bodyEl.value.getBoundingClientRect()
  menu.value = {
    x: Math.max(0, Math.min(event.clientX - rect.left, rect.width - 150)),
    y: Math.max(0, Math.min(event.clientY - rect.top, rect.height - 90)),
  }
  window.addEventListener('pointerdown', closeMenu)
  window.addEventListener('keydown', onMenuKey)
}
function onMenuKey(event) { if (event.key === 'Escape') closeMenu() }
function closeMenu() {
  menu.value = null
  window.removeEventListener('pointerdown', closeMenu)
  window.removeEventListener('keydown', onMenuKey)
}
function duplicateSelected() {
  const copies = widgets.value
    .filter((w) => selectedIds.value.includes(w.id))
    .map((w) => ({ ...JSON.parse(JSON.stringify(w)), id: newId(), x: w.x + GRID, y: w.y + GRID }))
  widgets.value.push(...copies)
  selectedIds.value = copies.map((w) => w.id)
  closeMenu()
}

function onParamChange(w, value) {
  const param = defFor(w).param
  const previousLabel = defaultLabel(w)
  w.param = Math.max(param.min, Math.min(param.max, Math.round(Number(value)) || param.default))
  if (w.label === previousLabel) w.label = defaultLabel(w)
}

// Optional per-element "logarithmic" curve (w.log): the value changes slowly over the first part
// of the travel and faster towards the end - position p maps to value ratio p^LOG_CURVE.
// Functions centred on 0 (rotation, position, ...: min = -max) get the curve mirrored around
// the centre instead, so they're fine near 0 and speed up towards both ends.
const LOG_CURVE = 3
const isBipolar = (def) => !def.toRatio && def.min < 0 && def.min === -def.max
function curve(def, x, exponent) {
  if (!isBipolar(def)) return Math.pow(x, exponent)
  const u = 2 * x - 1
  return 0.5 + 0.5 * Math.sign(u) * Math.pow(Math.abs(u), exponent)
}
const toPosition = (w, def, r) => (w.log ? curve(def, r, 1 / LOG_CURVE) : r)
const fromPosition = (w, def, p) => (w.log ? curve(def, p, LOG_CURVE) : p)

// Control position (0..1) for the current value of value function `def`. `invert` flips the
// direction (w.inv for the main/X function, w.invY for an X/Y pad's vertical function).
function axisRatio(w, def, invert) {
  const v = Number(laserState[def.key]) || 0
  const r = def.toRatio ? def.toRatio(v) : (v - def.min) / (def.max - def.min)
  const p = toPosition(w, def, Math.max(0, Math.min(1, r)))
  return invert ? 1 - p : p
}
function sendAxis(w, def, invert, position) {
  const p = Math.max(0, Math.min(1, position))
  const r = fromPosition(w, def, invert ? 1 - p : p)
  sendValue(def.key, def.fromRatio ? def.fromRatio(r) : def.min + r * (def.max - def.min))
}
const ratio = (w) => axisRatio(w, defFor(w), w.inv)
const percent = (w) => ratio(w) * 100
const display = (w) => defFor(w).format(Number(laserState[defFor(w).key]) || 0)

// Throttled send of the latest value over REST; markLocalChange guards against the status poll overwriting it.
let pending = {}
let timer = null
function sendValue(key, value) {
  markLocalChange()
  laserState[key] = value
  pending[key] = value
  if (timer) return
  timer = setTimeout(() => {
    const update = pending
    pending = {}
    timer = null
    updateState(update).catch(console.error)
  }, 70)
}
const sendRatio = (w, position) => sendAxis(w, defFor(w), w.inv, position)
const displayY = (w) => defForY(w).format(Number(laserState[defForY(w).key]) || 0)

let drag = null // { kind: 'move' | 'resize', id, startX, startY, orig }
const guides = ref([])
const GRID = 24
const SNAP = 8

// Snaps one axis: an edge/center of the dragged box aligns to other elements first, else its start edge snaps to the grid.
// `fractions` picks which points of the box (0 = start, 0.5 = center, 1 = end) take part in alignment.
function snapAxis(pos, size, others, fractions) {
  let best = null
  for (const f of fractions) {
    const point = pos + size * f
    for (const [start, length] of others) {
      for (const target of [start, start + length / 2, start + length]) {
        const diff = target - point
        if (Math.abs(diff) <= SNAP && (!best || Math.abs(diff) < Math.abs(best.diff))) best = { diff, target }
      }
    }
  }
  if (best) return { pos: Math.round(pos + best.diff), guides: [best.target] }
  return { pos: Math.round(pos / GRID) * GRID, guides: [] }
}

function onWidgetDown(event, w) {
  if (!editing.value || event.button !== 0) return
  if (event.shiftKey) {
    // Shift toggles membership; a freshly added element can be dragged right away.
    if (selectedIds.value.includes(w.id)) {
      selectedIds.value = selectedIds.value.filter((id) => id !== w.id)
      return
    }
    selectedIds.value = [...selectedIds.value, w.id]
  } else if (!selectedIds.value.includes(w.id)) {
    selectedIds.value = [w.id]
  }
  const origs = {}
  for (const item of widgets.value) if (selectedIds.value.includes(item.id)) origs[item.id] = { x: item.x, y: item.y }
  drag = { kind: 'move', id: w.id, startX: event.clientX, startY: event.clientY, origs }
  attachDrag()
}
function onResizeDown(event, w) {
  selectedIds.value = [w.id]
  drag = { kind: 'resize', id: w.id, startX: event.clientX, startY: event.clientY, orig: { w: w.w, h: w.h } }
  attachDrag()
}
function attachDrag() {
  window.addEventListener('pointermove', onDragMove)
  window.addEventListener('pointerup', endDrag)
  window.addEventListener('pointercancel', endDrag)
}
function endDrag() {
  drag = null
  guides.value = []
  window.removeEventListener('pointermove', onDragMove)
  window.removeEventListener('pointerup', endDrag)
  window.removeEventListener('pointercancel', endDrag)
}
function onDragMove(event) {
  if (!drag) return
  const w = widgets.value.find((item) => item.id === drag.id)
  if (!w) return
  const dx = event.clientX - drag.startX
  const dy = event.clientY - drag.startY
  if (drag.kind === 'move') {
    // The grabbed element snaps against everything not being moved; the rest of the selection follows its offset.
    const moving = Object.keys(drag.origs)
    const fixed = widgets.value.filter((o) => !moving.includes(o.id))
    const orig = drag.origs[w.id]
    const sx = snapAxis(orig.x + dx, w.w, fixed.map((o) => [o.x, o.w]), [0, 0.5, 1])
    const sy = snapAxis(orig.y + dy, w.h, fixed.map((o) => [o.y, o.h]), [0, 0.5, 1])
    const minX = Math.min(...moving.map((id) => drag.origs[id].x))
    const minY = Math.min(...moving.map((id) => drag.origs[id].y))
    const deltaX = Math.max(sx.pos - orig.x, -minX)
    const deltaY = Math.max(sy.pos - orig.y, -minY)
    for (const item of widgets.value) {
      const start = drag.origs[item.id]
      if (!start) continue
      item.x = start.x + deltaX
      item.y = start.y + deltaY
    }
    guides.value = [...sx.guides.map((x) => ({ x })), ...sy.guides.map((y) => ({ y }))]
  } else {
    const others = widgets.value.filter((o) => o.id !== w.id)
    const [minW, minH] = MIN_SIZE[w.type]
    const sx = snapAxis(w.x + Math.max(minW, drag.orig.w + dx), 0, others.map((o) => [o.x, o.w]), [0])
    const sy = snapAxis(w.y + Math.max(minH, drag.orig.h + dy), 0, others.map((o) => [o.y, o.h]), [0])
    w.w = Math.max(minW, sx.pos - w.x)
    w.h = Math.max(minH, sy.pos - w.y)
    guides.value = [...sx.guides.map((x) => ({ x })), ...sy.guides.map((y) => ({ y }))]
  }
}

// ── Multitouch ──────────────────────────────────────────────────────────────
// Every finger is its own gesture, keyed by pointerId: several faders, knobs
// and X/Y pads can be moved at once while buttons are held or tapped. Each
// control captures its own pointer, so a finger keeps controlling the element
// it started on even if it slides off it.
const gestures = new Map() // pointerId -> { w, startY?, startRatio? }
function beginGesture(event, w, extra = {}) {
  gestures.set(event.pointerId, { w, ...extra })
  event.currentTarget.setPointerCapture(event.pointerId)
}
function endGesture(event) { gestures.delete(event.pointerId) }
const gestureOf = (event, w) => {
  const g = gestures.get(event.pointerId)
  return g && g.w.id === w.id ? g : null
}

// Fader: absolute position along the track.
function setFromPointer(event, w) {
  const rect = event.currentTarget.getBoundingClientRect()
  sendRatio(w, 1 - (event.clientY - rect.top) / rect.height)
}
function onFaderDown(event, w) {
  if (editing.value) return
  event.stopPropagation()
  beginGesture(event, w)
  setFromPointer(event, w)
}
function onFaderMove(event, w) { if (gestureOf(event, w)) setFromPointer(event, w) }

// X/Y pad: absolute position on the field sets both functions at once (right = more X, up = more Y).
function setXYFromPointer(event, w) {
  const rect = event.currentTarget.getBoundingClientRect()
  sendAxis(w, defFor(w), w.inv, (event.clientX - rect.left) / rect.width)
  sendAxis(w, defForY(w), w.invY, 1 - (event.clientY - rect.top) / rect.height)
}
function onXYDown(event, w) {
  if (editing.value) return
  event.stopPropagation()
  if (isDoubleTap(event, w)) {
    resetDef(defFor(w), w)
    resetDef(defForY(w), w)
    return
  }
  beginGesture(event, w)
  setXYFromPointer(event, w)
}
function onXYMove(event, w) { if (gestureOf(event, w)) setXYFromPointer(event, w) }

// Double tap on a knob or X/Y pad: back to 0 - the centre for functions centred on 0,
// otherwise 0 or the lowest value the function allows (X/Y pads reset both functions).
// Rotation instead does a full rotation reset (angle to 0 + stop), see VALUE_FNS.rotation.
const DOUBLE_TAP_MS = 300
const lastTaps = new Map() // widget id -> { at, x, y } (per widget, so taps on other controls don't interfere)
function isDoubleTap(event, w) {
  const now = performance.now()
  const last = lastTaps.get(w.id)
  lastTaps.set(w.id, { at: now, x: event.clientX, y: event.clientY })
  if (!last || now - last.at >= DOUBLE_TAP_MS) return false
  if (Math.hypot(event.clientX - last.x, event.clientY - last.y) >= 30) return false
  lastTaps.delete(w.id)
  return true
}
// Functions with a `tapReset` (Rotation) run their own reset action instead,
// unless it returns null (element opted out) - then the plain reset applies.
function resetDef(def, w) {
  if (!def) return
  const custom = def.tapReset?.(w)
  if (custom) { custom.catch?.(console.error); return }
  sendValue(def.key, def.toRatio ? def.fromRatio(0) : Math.min(def.max, Math.max(def.min, 0)))
}

// Knob: relative vertical drag, full range over 200px.
function onKnobDown(event, w) {
  if (editing.value) return
  event.stopPropagation()
  if (isDoubleTap(event, w)) {
    resetDef(defFor(w), w)
    return
  }
  beginGesture(event, w, { startY: event.clientY, startRatio: ratio(w) })
}
function onKnobMove(event, w) {
  const g = gestureOf(event, w)
  if (g) sendRatio(w, g.startRatio + (g.startY - event.clientY) / 200)
}

// Button: momentary functions stay active while held; trigger functions fire once on touch
// down. Any number of buttons can be held at once (one per finger).
const isHeld = (w) => [...held.values()].some((h) => h.id === w.id)
const isLit = (w) => isHeld(w) || !!defFor(w).active?.(w)
const buttonMode = (w, def) => (def.momentaryOption && w.momentary ? 'momentary' : def.mode)
function onButtonDown(event, w) {
  if (editing.value) return
  const def = defFor(w)
  if (buttonMode(w, def) === 'trigger') {
    def.press(w)?.catch?.(console.error)
    return
  }
  if (isHeld(w)) return // already held by another finger
  event.currentTarget.setPointerCapture(event.pointerId)
  held.set(event.pointerId, w)
  def.press(w).catch(console.error)
}
function onButtonUp(event, w) {
  const h = held.get(event.pointerId)
  if (!h || h.id !== w.id) return
  held.delete(event.pointerId)
  defFor(w)?.release(w)?.catch?.(console.error)
}
// Releases every held button (page switch, edit mode, leaving Magic Touch).
function releaseHeldButton() {
  for (const [pointerId, w] of [...held]) {
    held.delete(pointerId)
    defFor(w)?.release(w)?.catch?.(console.error)
  }
  gestures.clear()
}

// ── Shared layout sync ──────────────────────────────────────────────────────
// Edits stay local while this client is in edit mode and are published to the backend
// when it switches back to run mode (or leaves the page), so other clients keep their
// current layout until the editor is done. A client in run mode checks every second for
// a newer published revision and applies it. While editing, remote revisions are ignored;
// the last client to publish wins.
let lastSynced = null
let saving = false
let retryTimer = null
const serialize = () => JSON.stringify({ pages: pages.value })

async function publish() {
  clearTimeout(retryTimer)
  retryTimer = null
  if (saving) { retryTimer = setTimeout(publish, 300); return }
  const body = serialize()
  if (body === lastSynced) return
  saving = true
  try {
    await saveMagicTouch(body)
    lastSynced = body
    localStorage.removeItem(LEGACY_STORAGE_KEY)
  } catch (error) {
    console.error(error)
    retryTimer = setTimeout(publish, 2000)
  } finally {
    saving = false
  }
}
watch(editing, (on) => { if (!on && loaded.value) publish() })
// Closing or reloading the tab mid-edit still publishes (keepalive survives the unload).
function publishOnUnload() {
  const body = serialize()
  if (!editing.value || !loaded.value || body === lastSynced) return
  fetch('/api/magic-touch', { method: 'POST', body, keepalive: true, headers: { 'Content-Type': 'application/json' } })
}
window.addEventListener('pagehide', publishOnUnload)

function applyServerLayout() {
  const fromServer = parsePages(magicTouch.layout)
  // Empty backend: seed it with this browser's old local layout, or the defaults.
  const next = fromServer || loadLegacyPages() || defaultPages()
  pages.value = next
  if (!next.some((p) => p.id === activePageId.value)) activePageId.value = next[0].id
  selectedIds.value = []
  lastSynced = fromServer ? serialize() : null
  loaded.value = true
  if (!fromServer) publish()
}
const busy = () => editing.value || saving || retryTimer !== null || serialize() !== lastSynced
async function pollLayout() {
  if (loaded.value && busy()) return
  try {
    const changed = await fetchMagicTouch()
    if ((changed || !loaded.value) && !(loaded.value && busy())) applyServerLayout()
  } catch { /* backend unreachable: keep the current layout and retry */ }
}
// Remount: start from the cached layout right away, then check for a newer one.
if (magicTouch.layout) applyServerLayout()
pollLayout()
const layoutPoll = setInterval(pollLayout, 1000)

// ── Two-finger scrolling ────────────────────────────────────────────────────
// Run mode: fingers on a control always operate that control (multitouch, see
// above) and never scroll; two fingers on *free* canvas space pan it.
// Edit mode: as soon as a second finger touches the canvas anywhere, a running
// element drag is cancelled and both fingers pan. Mouse wheel / trackpad
// scrolling is native and unaffected.
const CONTROL_SELECTOR = '.mtb-fader, .mtb-knob, .mtb-xy, .mtb-pad'
const touches = new Map() // pointerId -> { x, y } (fingers that may pan)
let panning = false
const midpoint = () => {
  const pts = [...touches.values()]
  return { x: pts.reduce((a, p) => a + p.x, 0) / pts.length, y: pts.reduce((a, p) => a + p.y, 0) / pts.length }
}
function onPanDown(event) {
  if (event.pointerType !== 'touch') return
  if (!editing.value && event.target.closest?.(CONTROL_SELECTOR)) return
  touches.set(event.pointerId, { x: event.clientX, y: event.clientY })
  if (touches.size === 2 && !panning) {
    panning = true
    if (editing.value) endDrag()
  }
  if (panning) event.stopPropagation()
}
function onPanMove(event) {
  if (event.pointerType !== 'touch' || !touches.has(event.pointerId)) return
  if (!panning) {
    touches.set(event.pointerId, { x: event.clientX, y: event.clientY })
    return
  }
  event.stopPropagation()
  if (touches.size < 2) {
    touches.set(event.pointerId, { x: event.clientX, y: event.clientY })
    return
  }
  const before = midpoint()
  touches.set(event.pointerId, { x: event.clientX, y: event.clientY })
  const after = midpoint()
  canvasEl.value.scrollLeft -= after.x - before.x
  canvasEl.value.scrollTop -= after.y - before.y
}
function onPanUp(event) {
  if (event.pointerType !== 'touch' || !touches.has(event.pointerId)) return
  touches.delete(event.pointerId)
  if (panning) event.stopPropagation()
  if (!touches.size) panning = false
}

onBeforeUnmount(() => {
  clearInterval(layoutPoll)
  window.removeEventListener('pagehide', publishOnUnload)
  // Leaving the Magic Touch page counts as done editing.
  if (loaded.value) publish()
  closeMenu()
  endDrag()
  releaseHeldButton()
  clearTimeout(timer)
  if (Object.keys(pending).length) updateState(pending).catch(console.error)
})
</script>

<style scoped>
.mtb { display: flex; flex-direction: column; height: 100%; min-height: 0; color: #e7f0ed; }
.mtb.loading > * { visibility: hidden; }
.mtb.loading::before { content: 'Loading layout from the laser server…'; position: absolute; padding: 16px; opacity: .6; }
.mtb-bar { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 4px 10px; border-bottom: 1px solid rgba(159,190,187,.16); }
.mtb-bar .mtb-btn { min-height: 28px; padding: 2px 10px; font-size: 12px; }
.mtb-bar-left { display: flex; gap: 8px; }
.mtb-hint, .mtb-muted { margin: 0; color: #91a6a2; font-size: 12px; }
.mtb-btn { min-height: 38px; padding: 6px 14px; color: inherit; background: rgba(255,255,255,.04); border: 1px solid rgba(159,190,187,.16); border-radius: 7px; font: inherit; font-size: 13px; cursor: pointer; touch-action: manipulation; }
.mtb-btn.active { color: #eafff3; background: rgba(101,230,165,.13); border-color: #65e6a5; }
.mtb-btn.danger { color: #ffaaa1; border-color: rgba(255,124,111,.45); }
.mtb-body { position: relative; flex: 1 1 auto; display: flex; min-height: 0; }
.mtb-canvas { position: relative; flex: 1 1 auto; min-width: 0; overflow: auto; touch-action: none; overscroll-behavior: contain; }
.mtb-spacer { position: absolute; width: 1px; height: 1px; pointer-events: none; }
.editing .mtb-canvas { background-image: radial-gradient(rgba(159,190,187,.22) 1px, transparent 1px); background-size: 24px 24px; }
.mtb-empty { margin: 24px; color: #91a6a2; font-size: 13px; pointer-events: none; }
.mtb-widget { position: absolute; display: flex; flex-direction: column; box-sizing: border-box; container-type: size; padding: clamp(3px, 7cqmin, 10px); background: #11191f; border: 1px solid rgba(159,190,187,.16); border-radius: 8px; user-select: none; }
.editing .mtb-widget { cursor: move; border-style: dashed; }
.mtb-widget.selected { border-color: #65e6a5; box-shadow: 0 0 0 2px rgba(101,230,165,.25); }
.mtb-label { flex: 0 1 auto; min-height: 0; font-size: clamp(6px, 9cqh, 12px); line-height: 1.15; text-align: center; color: #c7d4d0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mtb-fader, .mtb-knob { flex: 1 1 auto; display: flex; justify-content: center; min-height: 0; padding: clamp(1px, 4cqmin, 10px) 0; touch-action: none; cursor: pointer; }
.editing .mtb-fader, .editing .mtb-knob, .editing .mtb-pad, .editing .mtb-xy { pointer-events: none; }
.mtb-xy { position: relative; flex: 1 1 auto; min-height: 0; margin: 2px 0; overflow: hidden; background: #0c1318; background-image: linear-gradient(rgba(159,190,187,.08) 1px, transparent 1px), linear-gradient(90deg, rgba(159,190,187,.08) 1px, transparent 1px); background-size: 25% 25%; background-position: -1px -1px; border: 1px solid rgba(159,190,187,.22); border-radius: 6px; touch-action: none; cursor: crosshair; }
.mtb-xy-line { position: absolute; background: rgba(101,230,165,.35); pointer-events: none; }
.mtb-xy-line.v { top: 0; bottom: 0; width: 1px; }
.mtb-xy-line.h { left: 0; right: 0; height: 1px; }
.mtb-xy-dot { position: absolute; width: calc(22px * var(--cs, 1)); height: calc(22px * var(--cs, 1)); margin: calc(-11px * var(--cs, 1)) 0 0 calc(-11px * var(--cs, 1)); background: #65e6a5; border: 2px solid #eafff3; border-radius: 50%; box-shadow: 0 0 12px rgba(101,230,165,.7); box-sizing: border-box; pointer-events: none; }
.mtb-track { position: relative; width: 12px; height: 100%; background: rgba(255,255,255,.08); border-radius: 999px; }
.mtb-fill { position: absolute; left: 0; right: 0; bottom: 0; background: #65e6a5; border-radius: 999px; }
.mtb-thumb { position: absolute; left: 50%; width: 36px; height: 20px; margin-left: -18px; background: #eafff3; border-radius: 5px; box-shadow: 0 0 0 3px rgba(101,230,165,.25); pointer-events: none; }
.mtb-knob svg { width: 100%; height: 100%; }
.knob-body { fill: #1a252c; stroke: rgba(159,190,187,.25); stroke-width: 1.5; }
.knob-track, .knob-arc { fill: none; stroke-width: 5; stroke-linecap: round; }
.knob-track { stroke: rgba(255,255,255,.08); stroke-dasharray: 75 200; }
.knob-arc { stroke: #65e6a5; }
.knob-pointer { stroke: #eafff3; stroke-width: 4; stroke-linecap: round; }
.mtb-value { flex: 0 1 auto; min-height: 0; text-align: center; font-size: clamp(6px, 9cqh, 12px); line-height: 1.15; color: #65e6a5; font-variant-numeric: tabular-nums; }
.mtb-pad { display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 4px; flex: 1 1 auto; min-height: 0; padding: 4px; color: #fff3cc; background: linear-gradient(135deg, #7d461c, #3e291b); border: 1px solid rgba(255,188,89,.55); border-radius: 8px; font: inherit; font-size: 12px; font-weight: 700; text-transform: uppercase; cursor: pointer; touch-action: none; user-select: none; }
.mtb-pad.down { background: #e59036; color: #17120c; }
.mtb-pad.cue { position: relative; overflow: hidden; align-items: flex-end; justify-content: flex-end; color: #6a7090; background: rgba(255,255,255,0.05); border: 1px solid rgba(255,255,255,0.15); font-size: 13px; font-weight: 500; text-shadow: 0 0 2px rgba(0,0,0,.9); }
.mtb-pad.cue > span { position: relative; padding: 0 4px 2px; line-height: 1; }
.mtb-pad.cue.populated { background: rgba(20,22,32,.9); border-color: #48e07a; color: #48e07a; }
.mtb-pad.cue.down { border: 3px solid #a8ff60; box-shadow: inset 0 0 0 1px rgba(168,255,96,.65), 0 0 12px rgba(168,255,96,.75); color: #c2ff89; background: rgba(20,22,32,.9); }
.mtb-pad.swatch { color: #fff; text-shadow: 0 1px 4px rgba(0,0,0,.9); text-transform: none; font-size: 11px; background: linear-gradient(180deg, transparent 30%, rgba(0,0,0,.6)), var(--swatch); border: 2px solid transparent; }
.mtb-pad.swatch.down { background: linear-gradient(180deg, transparent 30%, rgba(0,0,0,.6)), var(--swatch); color: #fff; border-color: #fff; box-shadow: 0 0 0 2px #65e6a5; }
.mtb-row { display: flex; gap: 8px; }
.mtb-row .mtb-btn { flex: 1 1 0; padding-inline: 6px; }
.mtb-btn:disabled { opacity: .4; cursor: default; }
.mtb-pages {
  display: flex; gap: 6px; overflow-x: auto; scrollbar-width: none; color: #e7f0ed;
  /* Scroll shadows: edge shading appears only where more tabs are hidden (swipe the row). */
  background:
    linear-gradient(90deg, #0f161b 30%, transparent) left / 28px 100% no-repeat local,
    linear-gradient(270deg, #0f161b 30%, transparent) right / 28px 100% no-repeat local,
    radial-gradient(farthest-side at 0 50%, rgba(101,230,165,.45), transparent) left / 12px 100% no-repeat scroll,
    radial-gradient(farthest-side at 100% 50%, rgba(101,230,165,.45), transparent) right / 12px 100% no-repeat scroll;
}
.mtb-pages::-webkit-scrollbar { display: none; }
.mtb-page-tab { flex: 0 0 auto; min-width: 56px; min-height: 36px; padding: 6px 12px; color: #a7b9b5; background: transparent; border: 1px solid rgba(159,190,187,.16); border-radius: 7px; font: inherit; font-size: 14px; cursor: pointer; touch-action: manipulation; }
.mtb-page-tab.active { color: #eafff3; background: rgba(101,230,165,.13); border-color: #65e6a5; }
.mtb-page-tab.add { min-width: 44px; padding-inline: 12px; }
.mtb-pad-icon { display: grid; place-items: center; flex: 1 1 auto; width: 100%; min-height: 0; }
.mtb-pad-icon :deep(svg) { width: 100%; height: 100%; max-width: 64px; max-height: 64px; stroke: currentColor; stroke-width: 1.5; stroke-linecap: round; stroke-linejoin: round; }
.mtb-pad-text { max-width: 100%; }
.mtb-fit { display: inline-block; font-size: calc(1em * var(--fit, 1)); line-height: 1.1; text-align: center; white-space: normal; overflow-wrap: normal; word-break: normal; }
.mtb-pad-text-small { font-size: clamp(5px, 11cqh, 9px); font-weight: 500; line-height: 1.1; }
.mtb-size { display: grid; grid-template-columns: 1fr auto; gap: 4px 8px; align-items: center; color: #c7d4d0; font-size: 12px; }
.mtb-size input[type="range"] { grid-column: 1 / -1; grid-row: 2; width: 100%; margin: 0; accent-color: #65e6a5; }
.mtb-size-auto { padding: 2px 8px; color: inherit; background: rgba(255,255,255,.06); border: 1px solid rgba(159,190,187,.16); border-radius: 6px; font: inherit; font-size: 11px; cursor: pointer; }
.mtb-size-auto:disabled { opacity: .5; cursor: default; }
.mtb-track { width: calc(12px * var(--cs, 1)); }
.mtb-thumb { width: calc(36px * var(--cs, 1)); height: calc(20px * var(--cs, 1)); margin-left: calc(-18px * var(--cs, 1)); }
.mtb-knob svg { transform: scale(var(--cs, 1)); }
.mtb-pad-icon :deep(svg) { transform: scale(var(--cs, 1)); }
.mtb-menu { position: absolute; z-index: 6; display: flex; flex-direction: column; min-width: 140px; padding: 4px; background: rgba(17,25,31,.96); border: 1px solid rgba(159,190,187,.3); border-radius: 8px; box-shadow: 0 8px 28px rgba(0,0,0,.5); }
.mtb-menu button { padding: 8px 12px; color: #e7f0ed; background: transparent; border: 0; border-radius: 6px; font: inherit; font-size: 13px; text-align: left; cursor: pointer; }
.mtb-menu button:hover { background: rgba(101,230,165,.13); }
.mtb-menu button.danger { color: #ffaaa1; }
.mtb-check { flex-direction: row !important; align-items: center; gap: 8px !important; }
.mtb-guide { position: absolute; pointer-events: none; background: #ff4fd8; }
.mtb-guide.v { top: 0; bottom: 0; width: 1px; }
.mtb-guide.h { left: 0; right: 0; height: 1px; }
.mtb-resize { position: absolute; right: 0; bottom: 0; width: 18px; height: 18px; cursor: nwse-resize; background: linear-gradient(135deg, transparent 50%, #65e6a5 50%); border-bottom-right-radius: 8px; touch-action: none; }
.mtb-editor { position: absolute; z-index: 5; display: flex; flex-direction: column; width: 240px; max-height: calc(100% - 32px); background: rgba(17,25,31,.82); backdrop-filter: blur(6px); border: 1px solid rgba(159,190,187,.3); border-radius: 8px; box-shadow: 0 8px 28px rgba(0,0,0,.45); }
.mtb-editor-head { display: flex; align-items: center; justify-content: space-between; padding: 8px 8px 8px 12px; font-size: 12px; font-weight: 700; letter-spacing: .5px; text-transform: uppercase; color: #91a6a2; cursor: move; touch-action: none; user-select: none; border-bottom: 1px solid rgba(159,190,187,.16); }
.mtb-editor-collapse { width: 28px; height: 28px; color: inherit; background: rgba(255,255,255,.06); border: 1px solid rgba(159,190,187,.16); border-radius: 6px; font: inherit; cursor: pointer; }
.mtb-editor-body { display: flex; flex-direction: column; gap: 14px; padding: 12px; overflow-y: auto; }
.mtb-editor-body section { display: flex; flex-direction: column; gap: 10px; }
.mtb-editor-body section + section { padding-top: 12px; border-top: 1px solid rgba(159,190,187,.16); }
.mtb-editor-body h2 { margin: 0; font-size: 14px; }
.mtb-editor-body label { display: flex; flex-direction: column; gap: 5px; color: #c7d4d0; font-size: 12px; }
.mtb-field { min-height: 36px; padding: 6px 8px; color: inherit; background: rgba(12,17,22,.85); border: 1px solid rgba(159,190,187,.16); border-radius: 6px; font: inherit; font-size: 13px; }
</style>
