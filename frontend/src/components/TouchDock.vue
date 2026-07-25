<template>
  <div class="touch-dock">
    <button
      v-for="item in PANEL_ITEMS" :key="item.view"
      class="dock-btn"
      :class="{ active: open[item.view] }"
      @click="toggle(item.view)"
      :title="item.label"
    >
      <span class="dock-icon" v-html="ICONS[item.icon]"></span>
      <span class="dock-label">{{ item.label }}</span>
    </button>

    <!-- Local-only: pauses/hides the 3D preview in this browser tab alone.
         Never touches the backend, so hardware output and every other
         connected client's preview are completely unaffected. -->
    <button
      class="dock-btn"
      :class="{ active: previewEnabled }"
      @click="$emit('update:previewEnabled', !previewEnabled)"
      :title="previewEnabled ? 'Hide preview (this browser only)' : 'Show preview (this browser only)'"
    >
      <span class="dock-icon" v-html="previewEnabled ? ICONS.eye : ICONS.eyeOff"></span>
      <span class="dock-label">Preview</span>
    </button>

    <!-- Settings is deliberately the rightmost/last dock button. -->
    <button
      class="dock-btn"
      :class="{ active: open.settings }"
      @click="toggle('settings')"
      title="Settings"
    >
      <span class="dock-icon" v-html="ICONS.settings"></span>
      <span class="dock-label">Settings</span>
    </button>
  </div>

  <FloatingPanel
    v-if="open.settings"
    title="Settings"
    :width="300"
    :initial-x="pos(40, false, 300)"
    :initial-y="pos(40, true)"
    @close="open.settings = false"
    @popout="popout('settings')"
  >
    <ControlPanel popout @update:persistence="$emit('update:persistence', $event)" />
  </FloatingPanel>

  <FloatingPanel
    v-if="open.cues"
    title="Cues"
    maximized
    @close="open.cues = false"
    @popout="popout('cues')"
  >
    <CuePanel popout />
  </FloatingPanel>

  <FloatingPanel
    v-if="open.zoning"
    title="Zoning"
    :width="320"
    :initial-x="pos(1400, false, 320)"
    :initial-y="pos(40, true)"
    @close="open.zoning = false"
    @popout="popout('zoning')"
  >
    <ZoningPanel popout />
  </FloatingPanel>

  <FloatingPanel
    v-if="open.lasers"
    title="Lasers"
    :width="320"
    :initial-x="pos(1740, false, 320)"
    :initial-y="pos(40, true)"
    @close="open.lasers = false"
    @popout="popout('lasers')"
  >
    <LasersPanel popout />
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
import ZoningPanel from './ZoningPanel.vue'
import LasersPanel from './LasersPanel.vue'

defineProps({
  previewEnabled: { type: Boolean, default: true },
})
defineEmits(['update:persistence', 'update:previewEnabled'])

// Settings is intentionally not in this list - it's rendered as its own
// explicit, always-last button in the template so it's guaranteed to stay
// the rightmost dock icon regardless of panel order here.
const PANEL_ITEMS = [
  { view: 'cues',   icon: 'cues',   label: 'Cues' },
  { view: 'zoning', icon: 'zoning', label: 'Zoning' },
  { view: 'lasers', icon: 'lasers', label: 'Lasers' },
]

// Minimal, single-color (monochrome, via CSS currentColor) stroke icons -
// deliberately plain line art instead of colourful/oversized emoji, in
// keeping with the rest of the dock's understated dark UI.
const ICONS = {
  cues: `<svg viewBox="0 0 24 24" fill="none"><rect x="3" y="3" width="18" height="18" rx="2"/><path d="M8 3v18M16 3v18M3 8h5M16 8h5M3 16h5M16 16h5"/></svg>`,
  zoning: `<svg viewBox="0 0 24 24" fill="none"><polygon points="2 6 9 3 15 6 22 3 22 18 15 21 9 18 2 21"/><path d="M9 3v15M15 6v15"/></svg>`,
  lasers: `<svg viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="2"/><path d="M16.2 7.8a6 6 0 010 8.4M7.8 7.8a6 6 0 000 8.4M19 4.9a10 10 0 010 14.2M5 4.9a10 10 0 000 14.2"/></svg>`,
  eye: `<svg viewBox="0 0 24 24" fill="none"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>`,
  eyeOff: `<svg viewBox="0 0 24 24" fill="none"><path d="M17.9 17.9A10.1 10.1 0 0112 20c-7 0-11-8-11-8a18.4 18.4 0 015-5.9M9.9 4.2A9 9 0 0112 4c7 0 11 8 11 8a18.5 18.5 0 01-2.2 3.2"/><path d="M14.1 14.1a3 3 0 10-4.2-4.2"/><path d="M1 1l22 22"/></svg>`,
  settings: `<svg viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 00.33 1.82l.06.06a2 2 0 11-2.83 2.83l-.06-.06a1.65 1.65 0 00-1.82-.33 1.65 1.65 0 00-1 1.51V21a2 2 0 11-4 0v-.09A1.65 1.65 0 009 19.4a1.65 1.65 0 00-1.82.33l-.06.06a2 2 0 11-2.83-2.83l.06-.06A1.65 1.65 0 004.6 15a1.65 1.65 0 00-1.51-1H3a2 2 0 110-4h.09A1.65 1.65 0 004.6 9a1.65 1.65 0 00-.33-1.82l-.06-.06a2 2 0 112.83-2.83l.06.06A1.65 1.65 0 009 4.6a1.65 1.65 0 001-1.51V3a2 2 0 114 0v.09a1.65 1.65 0 001 1.51 1.65 1.65 0 001.82-.33l.06-.06a2 2 0 112.83 2.83l-.06.06A1.65 1.65 0 0019.4 9a1.65 1.65 0 001.51 1H21a2 2 0 110 4h-.09a1.65 1.65 0 00-1.51 1z"/></svg>`,
}

const open = reactive({ settings: false, cues: false, zoning: false, lasers: false })

// Cascaded default position for a panel, clamped to whatever the actual
// viewport is (a small/secondary screen, e.g. an iPad, would otherwise
// place a panel well off-screen). `panelSize` is the panel's own
// width/height (in the same axis as `vertical`) so the clamp keeps the
// *entire* panel on-screen, not just its top-left corner - previously this
// only accounted for a flat margin, so a panel positioned near the right
// edge of a narrower viewport than the desktop-tuned defaults (e.g.
// Zoning/Lasers's cascaded x offsets) could end up only partially visible.
function pos(preferred, vertical = false, panelSize = 0) {
  const viewport = vertical ? window.innerHeight : window.innerWidth
  const edgeGap = 12
  const maxStart = Math.max(0, viewport - panelSize - edgeGap)
  return Math.max(0, Math.min(preferred, maxStart))
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

// Requested (width, height) per popped-out view, before being clamped to
// what's actually available below. Cues (null) requests the full
// available area (16x8 grid of double-size buttons, see CuePanel.vue) -
// the rest just want a reasonably large default window.
const REQUESTED_POPUP_SIZE = { cues: null, settings: [460, 820], zoning: [460, 820], lasers: [460, 820] }
function popout(view) {
  open[view] = false
  const existing = openWindows[view]
  if (existing && !existing.closed) {
    existing.focus()
    return
  }
  const url = `${location.origin}${location.pathname}?popup=${view}`

  // Always size/position against the *current* screen's actual available
  // area (screen.avail*, excludes OS taskbar/menu bar) and explicitly
  // center the window in it, rather than a fixed desktop-sized guess with
  // no left/top - a plain 'width=460,height=820' with no position left it
  // up to the browser/OS to place the window, which can render it partly
  // off-screen on a smaller display (e.g. an iPad) that the app wasn't
  // tuned for. Clamping width/height to the available area too means it
  // can never request a window bigger than the screen it'll open on.
  const availW = screen.availWidth, availH = screen.availHeight
  const availLeft = screen.availLeft || 0, availTop = screen.availTop || 0
  const [reqW, reqH] = REQUESTED_POPUP_SIZE[view] || [availW, availH]
  const w = Math.min(reqW, availW)
  const h = Math.min(reqH, availH)
  const left = availLeft + Math.max(0, (availW - w) / 2)
  const top = availTop + Math.max(0, (availH - h) / 2)

  const win = window.open(url, `beamcommander-${view}`, `width=${w},height=${h},left=${left},top=${top},resizable=yes`)
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
.dock-icon { display:flex; align-items:center; justify-content:center; }
.dock-icon :deep(svg) {
  width:22px; height:22px;
  stroke:currentColor; fill:none;
  stroke-width:1.7; stroke-linecap:round; stroke-linejoin:round;
}
.dock-label { font-size:12px; letter-spacing:0.5px; }
</style>
