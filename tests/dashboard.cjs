const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

const dashboard = fs.readFileSync(path.join(__dirname, '../dashboard/index.html'));
let frame = { version: 1, sequence: 15, values: { rpm: 2850, boost_psi: 9.4, iat_c: 34, coolant_c: 91 } };
const server = http.createServer((req, res) => {
  if (req.url === '/') {
    res.setHeader('content-type', 'text/html; charset=utf-8');
    res.end(dashboard);
  } else if (req.url === '/v1/telemetry' && frame) {
    res.setHeader('content-type', 'application/json');
    res.end(JSON.stringify(frame));
  } else {
    res.writeHead(204).end();
  }
});

async function run() {
  let browser;
  try {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    browser = await chromium.launch({ headless: true, executablePath: process.env.DASHBOARD_BROWSER });
    const page = await browser.newPage({ viewport: { width: 800, height: 480 } });
    const errors = [];
    page.on('pageerror', error => errors.push(error));
    await page.goto('http://127.0.0.1:' + server.address().port);
    const state = name => page.locator('#status[data-state="' + name + '"]').waitFor({ timeout: 6000 });
    const texts = () => page.locator('#rpm, #boost, #iat, #coolant').allTextContents();
    await state('live');
    assert.deepEqual(await texts(), ['2,850', '+9.4 psi', '34 °C', '91 °C']);
    const tach = await page.locator('.tachometer').boundingBox();
    const thermometers = await page.locator('.temperatures').boundingBox();
    assert.ok(tach.x + tach.width < thermometers.x, 'Tachometer sits to the left in landscape');
    assert.ok(thermometers.y + thermometers.height < 480, 'All gauges fit a short landscape radio');
    frame = { version: 1, sequence: 16, demo: true, values: { rpm: 4500, boost_psi: -4.1, iat_c: 45, coolant_c: 102 } };
    await state('demo');
    assert.deepEqual(await texts(), ['4,500', '-4.1 psi', '45 °C', '102 °C']);
    frame = { version: 1, sequence: 17, values: { rpm: 900 } };
    await page.getByText('900', { exact: true }).waitFor();
    assert.deepEqual(await texts(), ['900', '—', '—', '—']);
    await state('lost');
    assert.deepEqual(await texts(), ['—', '—', '—', '—']);
    frame = { version: 1, sequence: 18, values: { rpm: 0, boost_psi: 0, iat_c: 0, coolant_c: 0 } };
    await state('live');
    assert.deepEqual(await texts(), ['0', '0.0 psi', '0 °C', '0 °C']);
    await page.setViewportSize({ width: 360, height: 720 });
    const portraitTach = await page.locator('.tachometer').boundingBox();
    const portraitBoost = await page.locator('.boost').boundingBox();
    assert.ok(portraitBoost.y >= portraitTach.y + portraitTach.height, 'Gauges stack in portrait');
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
    frame = null;
    await state('waiting');
    assert.deepEqual(await texts(), ['—', '—', '—', '—']);
    assert.deepEqual(errors, []);
    console.log('PASS four live gauges, demo, stale and missing values, recovery, and radio-sized layouts');
  } finally {
    await browser?.close();
    await new Promise(resolve => server.close(resolve));
  }
}

run().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
