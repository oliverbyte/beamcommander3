<script setup>
import { onMounted, ref } from 'vue'
import LaserScene from './components/LaserScene.vue'
import ControlPanel from './components/ControlPanel.vue'
import CuePanel from './components/CuePanel.vue'
import ZoningPanel from './components/ZoningPanel.vue'
import LasersPanel from './components/LasersPanel.vue'
import TouchDock from './components/TouchDock.vue'
import { connectSocket, fetchState, startStatusPolling } from './composables/useLaserSocket.js'

const persistenceMs = ref(5)

// A dedicated, separate browser window (opened via window.open from a
// FloatingPanel's popout icon - see TouchDock.vue) loads this same app with
// ?popup=<view> in the URL, and gets *only* that one panel full-screen so
// it can be dragged onto another monitor. Read once - this never changes
// for the lifetime of a given window.
const popupView = new URLSearchParams(window.location.search).get('popup')

// The popped-out panel has no in-content heading of its own (see
// ControlPanel/CuePanel) - its title lives only in the actual window/tab
// title instead, so it still identifies itself in the taskbar.
const POPUP_TITLES = { settings: 'Settings', cues: 'Cues', zoning: 'Zoning', lasers: 'Lasers' }
if (popupView && POPUP_TITLES[popupView]) {
  document.title = `BeamCommander3 – ${POPUP_TITLES[popupView]}`
}

onMounted(() => {
  connectSocket()
  fetchState().catch(() => {})
  startStatusPolling()
})
</script>

<template>
  <template v-if="popupView">
    <ControlPanel v-if="popupView === 'settings'" popout @update:persistence="persistenceMs = $event" />
    <CuePanel v-else-if="popupView === 'cues'" popout />
    <ZoningPanel v-else-if="popupView === 'zoning'" popout />
    <LasersPanel v-else-if="popupView === 'lasers'" popout />
  </template>
  <template v-else>
    <LaserScene :persistence-ms="persistenceMs" />
    <div class="logo">BeamCommander<span class="ver">3</span></div>
    <TouchDock @update:persistence="persistenceMs = $event" />
  </template>
</template>

<style scoped>
.logo {
  position:fixed; top:14px; left:16px; z-index:20;
  font-family:-apple-system,"Segoe UI",Roboto,sans-serif;
  font-size:14px; letter-spacing:2px; text-transform:uppercase;
  color:#8fe3ff; pointer-events:none; user-select:none;
  text-shadow:0 1px 3px rgba(0,0,0,0.8);
}
.logo .ver { color:#48e07a; }
</style>
