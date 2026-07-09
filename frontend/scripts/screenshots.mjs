// Generates the README screenshots against a *live* backend (see
// release.yml's "screenshots" step in the build-macos job — it starts the
// real built backend/laser_daemon, pointed at the built frontend/dist, then
// runs this script against it). Not run in local dev.
//
// Fixed 1440x900 viewport (the default/logical MacBook Air resolution) so
// image dimensions stay identical release over release. Captures one shot
// of the idle preview (with a look applied via /api/state, so it isn't just
// a blank blackout) plus one per dock menu button
// (Settings/Cues/Zoning/Lasers), each named to match the release asset
// names referenced from README.md.
//
// Note: backend/cues.json is gitignored (it's local runtime data, not
// shipped) - a fresh checkout/CI runner has no saved cues at all, so this
// applies a look directly via /api/state instead of relying on any
// pre-existing cue, then saves it into slot 1 itself so the Cues panel
// screenshot also shows a populated slot instead of an empty grid.
import { chromium } from 'playwright'
import { mkdir } from 'node:fs/promises'

const BASE_URL = process.env.BASE_URL || 'http://localhost:8000'
const OUT_DIR = process.env.OUT_DIR || './screenshots'
const VIEWPORT = { width: 1440, height: 900 }

// A colorful, clearly-animated look for the screenshots - not just the
// default plain green circle.
const DEMO_STATE = {
  blackout: false,
  shape: 'circle',
  r: 0.1, g: 0.9, b: 0.4,
  rainbow_amount: 0.6,
  rainbow_speed: 0.4,
  move_mode: 'circle',
  move_speed: 0.3,
  move_size: 0.6,
  rotation_speed: 0.05,
}

// Matches TouchDock.vue's ITEMS — dock button label -> output file suffix.
const DOCK_VIEWS = [
  { view: 'settings', label: 'Settings' },
  { view: 'cues', label: 'Cues' },
  { view: 'zoning', label: 'Zoning' },
  { view: 'lasers', label: 'Lasers' },
]

async function main() {
  await mkdir(OUT_DIR, { recursive: true })

  const browser = await chromium.launch()
  const page = await browser.newPage({ viewport: VIEWPORT })

  await page.goto(BASE_URL, { waitUntil: 'load' })
  await page.waitForSelector('canvas.laser-canvas')

  await page.evaluate((state) => fetch('/api/state', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(state),
  }), DEMO_STATE)
  await page.evaluate(() => fetch('/api/cue/1/save', { method: 'POST' }))
  await page.waitForTimeout(4000) // let the WS-driven preview + beam persistence trail fill in
  await page.screenshot({ path: `${OUT_DIR}/screenshot-background.png` })

  for (const { view, label } of DOCK_VIEWS) {
    const button = page.locator('.dock-btn', { hasText: label })
    await button.click()
    await page.waitForTimeout(500) // panel mount + layout settle
    await page.screenshot({ path: `${OUT_DIR}/screenshot-${view}.png` })
    await button.click() // close it before opening the next one
    await page.waitForTimeout(200)
  }

  await browser.close()
}

main().catch((err) => {
  console.error(err)
  process.exit(1)
})
