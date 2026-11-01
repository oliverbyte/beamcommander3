<script setup>
import { onMounted, ref } from 'vue'
import LaserScene from './components/LaserScene.vue'
import MagicTouchPanel from './components/MagicTouchPanel.vue'
import { connectSocket, fetchLasers, fetchState, startStatusPolling } from './composables/useLaserSocket.js'

const persistenceMs = ref(20)
const activePage = ref('cues')

// Whether the 3D laser preview renders/updates - purely a local UI
// preference for this browser tab.
// Never sent to the backend, so it can never affect the real hardware
// output or any other connected browser client's preview.
const previewEnabled = ref(true)

onMounted(() => {
  connectSocket()
  fetchState().catch(() => {})
  fetchLasers().catch(() => {})
  startStatusPolling()
})
</script>

<template>
  <LaserScene :persistence-ms="persistenceMs" :enabled="previewEnabled" :show-layout="activePage === 'preview'" />
  <MagicTouchPanel
    :persistence-ms="persistenceMs"
    :preview-enabled="previewEnabled"
    @update:persistence="persistenceMs = $event"
    @update:preview-enabled="previewEnabled = $event"
    @page-change="activePage = $event"
  />
</template>
