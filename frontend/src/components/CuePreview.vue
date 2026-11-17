<template>
  <svg class="cue-preview" viewBox="0 0 24 24">
    <CueLookIcon v-for="item in looks" :key="item.key" :cue="item.look" />
  </svg>
</template>

<script setup>
// A cue's thumbnail: the looks of every laser saved in it - and of the cue
// layers playing on them - drawn on top of each other, so a cue that puts
// text on one laser and a circle on the others shows both. Identical looks
// (e.g. all lasers alike) are drawn once. Older cues have one look.
import { computed } from 'vue'
import CueLookIcon from './CueLookIcon.vue'

const props = defineProps({ cue: { type: Object, required: true } })

// What makes two looks look different in the icon.
const ICON_FIELDS = [
  'shape', 'ilda_file', 'text', 'font', 'text_bold', 'text_italic', 'text_underline', 'r', 'g', 'b',
  'rainbow_amount', 'rainbow_speed', 'dot_amount', 'shape_scale', 'pos_x', 'pos_y', 'rotation_speed',
  'move_mode', 'move_size', 'move_speed', 'wave_speed',
]
const looks = computed(() => {
  const c = props.cue
  const states = c.lasers && Object.keys(c.lasers).length ? Object.values(c.lasers) : [c]
  const out = []
  const seen = new Set()
  const add = (look) => {
    const key = JSON.stringify(ICON_FIELDS.map((f) => look[f]))
    if (seen.has(key)) return
    seen.add(key)
    out.push({ key, look })
  }
  for (const state of states) {
    add(state)
    for (const layer of state.layers || []) add(layer)
  }
  return out
})
</script>

<style scoped>
.cue-preview {
  position:absolute; inset:0; width:100%; height:100%;
  filter:drop-shadow(0 0 3px currentColor);
  pointer-events:none;
}
</style>
