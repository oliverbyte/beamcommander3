<template>
  <div class="touch-dock">
    <button
      v-for="item in ITEMS" :key="item.view"
      class="dock-btn"
      :class="{ active: open[item.view] }"
      @click="toggle(item.view)"
    >
      <span class="dock-icon">{{ item.icon }}</span>
      <span class="dock-label">{{ item.label }}</span>
    </button>
  </div>

  <FloatingPanel
    v-if="open.settings"
    title="Settings"
    :width="300"
    :initial-x="pos(40)"
    :initial-y="pos(40, true)"
    @close="open.settings = false"
    @popout="popout('settings')"
  >
    <ControlPanel popout @update:persistence="$emit('update:persistence', $event)" />
  </FloatingPanel>

  <FloatingPanel
    v-if="open.cues"
    title="Cues"
    :width="260"
    :initial-x="pos(360)"
    :initial-y="pos(40, true)"
    @close="open.cues = false"
    @popout="popout('cues')"
  >
    <CuePanel popout />
  </FloatingPanel>
</template>

<script setup>
// Big, touch-friendly dock (bottom center) that opens Settings/Cues
// as draggable in-page panels (FloatingPanel) - not window.open - so simply
// tapping a dock button can never collide with a popup blocker. Each panel
// then gets its own "⤢" icon (see FloatingPanel.vue) to pop *that* one out
// into a real, separate browser window, for dragging onto another monitor.
import { reactive } from 'vue'
import FloatingPanel from './FloatingPanel.vue'
import ControlPanel from './ControlPanel.vue'
import CuePanel from './CuePanel.vue'

defineEmits(['update:persistence'])

const ITEMS = [
  { view: 'settings', icon: '⚙️', label: 'Settings' },
  { view: 'cues',     icon: '🎬', label: 'Cues' },
]

const open = reactive({ settings: false, cues: false })

// Cascaded default position for a panel, clamped to whatever the actual
// viewport is (a small/secondary screen would otherwise place a panel
// well off-screen). `margin` keeps at least a bit of the panel visible
// on-screen so it stays reachable/draggable.
function pos(preferred, vertical = false) {
  const size = vertical ? window.innerHeight : window.innerWidth
  const margin = 60
  return Math.max(0, Math.min(preferred, size - margin))
}

// Keep track of already-opened popout windows per view - re-clicking the
// dock button, or the panel's own "⤤" icon, then just focuses the existing
// real window instead of also opening a redundant in-page duplicate.
const openWindows = {}
function toggle(view) {
  const existing = openWindows[view]
  if (existing && !existing.closed) {
    existing.focus()
    return
  }
  open[view] = !open[view]
}

function popout(view) {
  open[view] = false
  const existing = openWindows[view]
  if (existing && !existing.closed) {
    existing.focus()
    return
  }
  const url = `${location.origin}${location.pathname}?popup=${view}`
  const win = window.open(url, `beamcommander-${view}`, 'width=460,height=820,resizable=yes')
  if (win) openWindows[view] = win
}
</script>

<style scoped>
.touch-dock {
  position:fixed; left:50%; bottom:16px; transform:translateX(-50%);
  display:flex; gap:12px; z-index:30;
  background:rgba(8,10,18,0.88); border:1px solid rgba(120,130,200,0.25);
  border-radius:14px; padding:10px; backdrop-filter:blur(6px);
}
.dock-btn {
  display:flex; flex-direction:column; align-items:center; justify-content:center;
  gap:4px; min-width:88px; min-height:72px; padding:8px 14px;
  background:rgba(255,255,255,0.07); border:1px solid rgba(255,255,255,0.18);
  color:#cfd3e6; border-radius:10px; cursor:pointer;
  font-family:-apple-system,"Segoe UI",Roboto,sans-serif;
  touch-action:manipulation;
}
.dock-btn:hover { background:rgba(255,255,255,0.14); }
.dock-btn.active { background:rgba(72,224,122,0.18); border-color:#48e07a; color:#48e07a; }
.dock-icon { font-size:24px; line-height:1; }
.dock-label { font-size:12px; letter-spacing:0.5px; }
</style>
