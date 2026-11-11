// Screenshots docs/*-preview.html to PNG (needs Playwright).
//   node docs/render.js
const { chromium } = require('playwright');
const path = require('path');
(async () => {
  const b = await chromium.launch();
  for (const [name, wmm] of [['LongForm', 264.16], ['Mixxa', 294.64]]) {
    const pg = await b.newPage({ deviceScaleFactor: 2 });
    const W = Math.ceil(wmm * 96 / 25.4), H = Math.ceil(128.5 * 96 / 25.4);
    await pg.setViewportSize({ width: W, height: H });
    await pg.goto('file://' + path.join(__dirname, `${name}-preview.html`));
    await pg.waitForTimeout(300);
    await pg.screenshot({ path: path.join(__dirname, `${name}-panel.png`), clip: { x: 0, y: 0, width: W, height: H } });
  }
  await b.close();
})();
