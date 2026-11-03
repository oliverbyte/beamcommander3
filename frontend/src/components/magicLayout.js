// Shared lists plus the factory layout for the Magic Touch builder (positions sit on the 24px grid).
export const COLORS = [
  ['Pure red', '#ff0000'], ['Pure green', '#00ff00'], ['Pure blue', '#0000ff'], ['Warm white', '#fff1d0'], ['White', '#ffffff'],
  ['Rose red', '#ff304f'], ['Amber', '#ff9f1c'], ['Yellow', '#ffe64d'], ['Lime', '#b7f531'], ['Mint green', '#30e878'],
  ['Cyan', '#37e5e5'], ['Sky', '#42a5ff'], ['Electric blue', '#4666ff'], ['Violet', '#9e63ff'], ['Magenta', '#f34bd7'],
]
export const SHAPES = ['circle', 'line', 'triangle', 'square', 'wave', 'staticwave']
export const MOVES = ['none', 'circle', 'pan', 'tilt', 'tiltup', 'tiltdown', 'eight', 'random']

export const colorFnId = (name) => `color_${name.toLowerCase().replace(/ /g, '_')}`

const PAD = 84
const PITCH = 96
const COLUMN = 144

const button = (fn, x, y, w = PAD, h = PAD, extra = {}) => ({ type: 'button', fn, x, y, w, h, ...extra })
const knob = (fn, x, y) => ({ type: 'knob', fn, x, y, w: 120, h: 150 })
const padRow = (items, makeFn, y) => items.map((item, i) => button(makeFn(item), COLUMN + i * PITCH, y, PAD, PAD, { label: Array.isArray(item) ? item[0] : item }))

export function buildDefaultPages() {
  const live = [
    { type: 'fader', fn: 'master_brightness', label: 'Master', x: 24, y: 24, w: 96, h: 540 },
    button('flash', COLUMN, 24, 192, 96),
    button('master_brightness_flash', 360, 24, 192, 96),
    button('blackout_toggle', 576, 24, 96, 96, { label: 'Blackout' }),
    button('gate_toggle', 696, 24, 96, 96, { label: 'Output gate' }),
    ...padRow(COLORS.slice(0, 8), (c) => colorFnId(c[0]), 144),
    ...padRow(SHAPES, (s) => `shape_${s}`, 240),
    ...padRow(MOVES, (m) => `move_${m}`, 336),
    ...['release_time', 'move_speed', 'move_size', 'dot_amount', 'rainbow_amount'].map((fn, i) => knob(fn, COLUMN + i * 144, 432)),
    ...Array.from({ length: 8 }, (_, i) => button('cue_recall', COLUMN + i * PITCH, 600, PAD, PAD, { param: i + 1, label: `${i + 1}` })),
  ]
  return [{ name: 'Live', widgets: live }]
}
