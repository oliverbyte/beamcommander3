<template>
  <svg class="cue-preview" viewBox="0 0 24 24">
    <defs v-if="cue.rainbow_amount > 0">
      <!-- userSpaceOnUse (fixed 0..24 coordinates): an objectBoundingBox gradient silently fails to paint on a
           zero-height bounding box such as the horizontal "line" shape. -->
      <linearGradient :id="gradientId" gradientUnits="userSpaceOnUse" x1="0" y1="12" x2="24" y2="12">
        <stop offset="0%"    stop-color="hsl(0,100%,55%)" />
        <stop offset="16.6%" stop-color="hsl(60,100%,55%)" />
        <stop offset="33.3%" stop-color="hsl(120,100%,55%)" />
        <stop offset="50%"   stop-color="hsl(180,100%,55%)" />
        <stop offset="66.6%" stop-color="hsl(240,100%,55%)" />
        <stop offset="83.3%" stop-color="hsl(300,100%,55%)" />
        <stop offset="100%"  stop-color="hsl(360,100%,55%)" />
      </linearGradient>
    </defs>
    <g :style="posStyle">
      <g class="cue-move-group" :style="moveStyle">
        <g class="cue-spin-group" :style="rotateStyle">
          <circle v-if="cue.shape === 'circle'" cx="12" cy="12" r="8"
            fill="none" :stroke="stroke" stroke-width="2" :stroke-dasharray="dashArray" :style="shapeStyle()" />
          <line v-else-if="cue.shape === 'line'" x1="3" y1="12" x2="21" y2="12"
            :stroke="stroke" stroke-width="2" :stroke-dasharray="dashArray" :style="shapeStyle()" />
          <polygon v-else-if="cue.shape === 'triangle'" points="12,4 20,20 4,20"
            fill="none" :stroke="stroke" stroke-width="2" :stroke-dasharray="dashArray" :style="shapeStyle()" />
          <rect v-else-if="cue.shape === 'square'" x="4" y="4" width="16" height="16"
            fill="none" :stroke="stroke" stroke-width="2" :stroke-dasharray="dashArray" :style="shapeStyle()" />
          <path v-else-if="cue.shape === 'wave'" d="M-6,12 Q-1.5,4 3,12 T12,12 T21,12 T30,12"
            fill="none" :stroke="stroke" stroke-width="2" :stroke-dasharray="dashArray" :style="shapeStyle('wave')" />
          <path v-else d="M3,12 Q7.5,4 12,12 T21,12"
            fill="none" :stroke="stroke" stroke-width="2" :stroke-dasharray="dashArray" :style="shapeStyle()" />
        </g>
      </g>
    </g>
  </svg>
</template>

<script>
let instanceCounter = 0
</script>

<script setup>
import { computed } from 'vue'

// Animated icon for a saved cue (the full saved state object): shape, beam color, rainbow, dotted stroke,
// position offset, rotation, movement mode and wave travel. Dot positions are deliberately not animated
// unless rotation/move/low scan rate say so - a steady dotted cue looks steady on real hardware too.
const props = defineProps({ cue: { type: Object, required: true } })
const gradientId = `cue-rainbow-${++instanceCounter}`

const MOVE_ANIM = { pan: 'cue-move-pan', tilt: 'cue-move-tilt', tiltup: 'cue-move-tiltup', tiltdown: 'cue-move-tiltdown', circle: 'cue-move-circle', eight: 'cue-move-eight', random: 'cue-move-random' }

const stroke = computed(() => {
  const c = props.cue
  if (c.rainbow_amount > 0) return `url(#${gradientId})`
  const to = (v) => Math.round(Math.max(0, Math.min(1, v)) * 255)
  return `rgb(${to(c.r)},${to(c.g)},${to(c.b)})`
})
// Only genuinely dotted cues (dot_amount < 1) get a dashed stroke; a solid cue's real output is continuous.
const dashArray = computed(() => (props.cue.dot_amount < 1 ? '2,2' : 'none'))
// pos_x/pos_y are -1..1; SVG y grows downward so pos_y flips sign.
const posStyle = computed(() => ({
  transform: `translate(${(props.cue.pos_x || 0) * 6}px, ${-(props.cue.pos_y || 0) * 6}px)`,
}))
const rotateStyle = computed(() => {
  const speed = props.cue.rotation_speed
  if (!speed) return {}
  const dur = 1 / Math.max(0.02, Math.abs(speed))
  return { animation: `cue-spin ${dur}s linear infinite`, animationDirection: speed < 0 ? 'reverse' : 'normal' }
})
// Keyframes mirror calc_movement() in laser_daemon.cpp; --amp carries move_size, duration comes from move_speed.
const moveStyle = computed(() => {
  const c = props.cue
  const anim = MOVE_ANIM[c.move_mode]
  if (!anim || !c.move_speed) return {}
  const amp = Math.min(6, (c.move_size || 0) * 3)
  const dur = 1 / Math.max(0.02, Math.abs(c.move_speed))
  return { '--amp': `${amp}px`, animation: `${anim} ${dur}s linear infinite` }
})
function shapeStyle(shape) {
  const c = props.cue
  const anims = []
  if (c.rainbow_amount > 0 && c.rainbow_speed) anims.push(`cue-hue ${1 / Math.abs(c.rainbow_speed)}s linear infinite`)
  // Dotted cues only visibly chase below ~5kpps; above that the real output looks steady.
  if (c.dot_amount < 1 && c.rate_kpps < 5) {
    anims.push(`cue-dash ${Math.max(0.4, 3 / Math.max(0.1, c.rate_kpps))}s linear infinite`)
  }
  // Only "wave" integrates a time-based phase in the backend; "staticwave" does not.
  if (shape === 'wave' && c.wave_speed) {
    anims.push(`cue-wave-travel ${1 / Math.max(0.05, Math.abs(c.wave_speed))}s linear infinite ${c.wave_speed < 0 ? 'reverse' : 'normal'}`)
  }
  return { animation: anims.join(', ') }
}
</script>

<style scoped>
.cue-preview {
  position:absolute; inset:0; width:100%; height:100%;
  filter:drop-shadow(0 0 3px currentColor);
  pointer-events:none;
}
.cue-spin-group { transform-box:fill-box; transform-origin:center; }
</style>

<!-- Unscoped: scoped CSS rewrites @keyframes names, but the animation names are plain strings built in JS above. -->
<style>
@keyframes cue-spin { from { transform:rotate(0deg); } to { transform:rotate(360deg); } }
@keyframes cue-hue  { from { filter:hue-rotate(0deg); } to { filter:hue-rotate(360deg); } }
@keyframes cue-dash { from { stroke-dashoffset:0; } to { stroke-dashoffset:-24; } }
@keyframes cue-wave-travel { from { transform:translateX(0); } to { transform:translateX(-18px); } }
@keyframes cue-move-pan {
  0%, 100% { transform:translateX(0); }
  25%      { transform:translateX(var(--amp)); }
  50%      { transform:translateX(0); }
  75%      { transform:translateX(calc(var(--amp) * -1)); }
}
@keyframes cue-move-tilt {
  0%, 100% { transform:translateY(0); }
  25%      { transform:translateY(calc(var(--amp) * -1)); }
  50%      { transform:translateY(0); }
  75%      { transform:translateY(var(--amp)); }
}
/* tiltup/tiltdown sweep continuously in one direction, then snap back. */
@keyframes cue-move-tiltup {
  0%    { transform:translateY(var(--amp)); }
  99.9% { transform:translateY(calc(var(--amp) * -1)); }
  100%  { transform:translateY(var(--amp)); }
}
@keyframes cue-move-tiltdown {
  0%    { transform:translateY(calc(var(--amp) * -1)); }
  99.9% { transform:translateY(var(--amp)); }
  100%  { transform:translateY(calc(var(--amp) * -1)); }
}
@keyframes cue-move-circle {
  0%, 100% { transform:translate(var(--amp), 0); }
  25%      { transform:translate(0, calc(var(--amp) * -1)); }
  50%      { transform:translate(calc(var(--amp) * -1), 0); }
  75%      { transform:translate(0, var(--amp)); }
}
@keyframes cue-move-eight {
  0%, 50%, 100% { transform:translate(0, 0); }
  25%           { transform:translate(var(--amp), calc(var(--amp) * -1)); }
  75%           { transform:translate(calc(var(--amp) * -1), var(--amp)); }
}
@keyframes cue-move-random {
  0%, 100% { transform:translate(0, 0); }
  20%      { transform:translate(var(--amp), calc(var(--amp) * -0.6)); }
  45%      { transform:translate(calc(var(--amp) * -0.7), var(--amp)); }
  70%      { transform:translate(calc(var(--amp) * 0.5), calc(var(--amp) * 0.8)); }
}
</style>
