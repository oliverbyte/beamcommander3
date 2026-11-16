<template>
  <!-- Which lasers the look controls, cues and Magic Touch act on (this browser only). -->
  <div v-if="lasers.length > 1" class="laser-selection" role="group" aria-label="Lasers these controls apply to">
    <span class="selection-label">Applies to</span>
    <button
      type="button"
      class="selection-button"
      :class="{ active: !selected.length }"
      :aria-pressed="!selected.length"
      title="All lasers"
      @click="setLaserSelection([])"
    >All</button>
    <button
      v-for="l in lasers" :key="l.id"
      type="button"
      class="selection-button"
      :class="{ active: selected.includes(l.id) }"
      :aria-pressed="selected.includes(l.id)"
      :title="selected.includes(l.id) ? `Remove ${l.name} from the selection` : `Add ${l.name} to the selection`"
      @click="toggleLaserSelected(l.id)"
    >
      <span class="armed-dot" :class="{ armed: l.armed }" :title="l.armed ? 'armed' : 'not armed'"></span>{{ l.name }}
    </button>
  </div>
</template>

<script setup>
// Laser selection bar: "All" or any set of lasers. Kept per browser (see
// laserSelection in useLaserSocket.js); every look change and cue action
// then only affects the selected lasers' looks.
import { computed } from 'vue'
import { lasers, laserSelection, selectedLaserIds, setLaserSelection, toggleLaserSelected } from '../composables/useLaserSocket.js'

// Depends on laserSelection so it updates when the selection changes.
const selected = computed(() => (laserSelection.ids.length, selectedLaserIds()))
</script>

<style scoped>
.laser-selection { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; margin-top: 10px; }
.selection-label { font-size: 11px; letter-spacing: .08em; text-transform: uppercase; color: #91a6a2; margin-right: 4px; }
.selection-button {
  display: inline-flex; align-items: center; gap: 6px; min-height: 34px; padding: 4px 12px;
  color: #a7b9b5; background: rgba(255,255,255,.04); border: 1px solid rgba(159,190,187,.22); border-radius: 8px;
  font: inherit; font-size: 13px; cursor: pointer; touch-action: manipulation;
}
.selection-button:hover { background: rgba(255,255,255,.08); color: #e7f0ed; }
.selection-button.active { color: #0c1116; background: #65e6a5; border-color: #65e6a5; font-weight: 600; }
.armed-dot { width: 7px; height: 7px; border-radius: 50%; background: #5a6470; }
.armed-dot.armed { background: #48e07a; box-shadow: 0 0 4px #48e07a; }
.selection-button.active .armed-dot { background: #0c1116; box-shadow: none; }
</style>
