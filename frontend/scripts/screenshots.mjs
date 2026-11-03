// Capture the README and website screenshots from a real saved test cue
// and its three tested preview positions. The release workflow runs the
// backend with an isolated HOME, local-only laser addresses, and blackout
// on, so this script cannot alter a user's show data or reach laser hardware.
import { chromium } from 'playwright'
import { mkdir, readFile } from 'node:fs/promises'

const BASE_URL = process.env.BASE_URL || 'http://localhost:8000'
const OUT_DIR = process.env.OUT_DIR || './screenshots'
const VIEWPORT = { width: 1440, height: 900 }
const PROFILE = JSON.parse(await readFile(new URL('./screenshot-profile.json', import.meta.url), 'utf8'))
const SCREENSHOTS = [
  { view: 'Shapes', file: 'settings' },
  { view: 'Cues', file: 'cues' },
  { view: 'Magic Touch', file: 'magictouch' },
  { view: 'Zoning', file: 'zoning' },
  { view: 'Lasers', file: 'lasers' },
]

async function prepareIsolatedBackend(page) {
  if (process.env.BC3_SCREENSHOT_ISOLATED !== '1') {
    throw new Error('Refusing to change backend state without BC3_SCREENSHOT_ISOLATED=1.')
  }
  if (!['localhost', '127.0.0.1', '::1'].includes(new URL(BASE_URL).hostname)) {
    throw new Error('Screenshot backend must run on localhost.')
  }

  await page.evaluate(async (profile) => {
    const request = async (path, body) => {
      const response = await fetch(path, {
        method: body === undefined ? 'GET' : 'POST',
        headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
        body: body === undefined ? undefined : JSON.stringify(body),
      })
      if (!response.ok) throw new Error(`${path} failed with HTTP ${response.status}`)
      return response.json()
    }

    const existingLasers = await request('/api/lasers')
    if (!Array.isArray(existingLasers) || existingLasers.length !== 0) {
      throw new Error('Screenshot backend must start with an empty laser list.')
    }

    await request('/api/state', { ...profile.cue, blackout: true })
    for (const laser of profile.lasers) {
      const created = await request('/api/lasers', { name: laser.name, ip: '127.0.0.1' })
      await request(`/api/lasers/${created.id}`, {
        armed: true,
        preview_x: laser.preview_x,
        preview_y: laser.preview_y,
      })
    }
    await request(`/api/cue/${profile.cueSlot}/save`, {})
  }, PROFILE)
}

async function selectView(page, label) {
  await page.locator('.page-nav-button').filter({ hasText: label }).click()
  await page.waitForTimeout(700)
}

async function main() {
  await mkdir(OUT_DIR, { recursive: true })

  const browser = await chromium.launch()
  try {
    const page = await browser.newPage({ viewport: VIEWPORT })
    await page.goto(BASE_URL, { waitUntil: 'load' })
    await page.waitForSelector('.page-nav-button')
    await page.waitForSelector('canvas.laser-canvas')
    await prepareIsolatedBackend(page)

    await selectView(page, 'Preview')
    await page.waitForTimeout(4000)
    await page.screenshot({ path: `${OUT_DIR}/screenshot-background.png` })

    for (const { view, file } of SCREENSHOTS) {
      await selectView(page, view)
      const screenshotOptions = { path: `${OUT_DIR}/screenshot-${file}.png` }
      if (view === 'Lasers') {
        screenshotOptions.mask = [page.locator('.laser-row input.ip')]
        screenshotOptions.maskColor = '#1b2025'
      }
      await page.screenshot(screenshotOptions)
    }
  } finally {
    await browser.close()
  }
}

main().catch((error) => {
  console.error(error)
  process.exit(1)
})
