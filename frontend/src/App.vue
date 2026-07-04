<script setup>
import { onMounted, ref } from 'vue'
import LaserScene from './components/LaserScene.vue'
import ControlPanel from './components/ControlPanel.vue'
import { connectSocket, fetchState, startStatusPolling } from './composables/useLaserSocket.js'

const persistenceMs = ref(25)

onMounted(() => {
  connectSocket()
  fetchState().catch(() => {})
  startStatusPolling()
})
</script>

<template>
  <LaserScene :persistence-ms="persistenceMs" />
  <ControlPanel @update:persistence="persistenceMs = $event" />
</template>
