import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
process.env.NO_PROXY = [process.env.NO_PROXY, '127.0.0.1', 'localhost'].filter(Boolean).join(',');
const {chromium} = createRequire(path.join(root, 'ui/package.json'))('@playwright/test');
const exe = path.resolve(root, process.argv[2] ?? 'build-parity-msvc/src/parity/Debug/lexica_desktop.exe');
const ui = path.resolve(root, process.argv[3] ?? 'ui/build');
const testRoot = path.join(root, 'build-parity-msvc/test-output');
const data = process.argv[4] ? path.resolve(process.argv[4]) : path.join(testRoot, 'tts-desktop-review', `${Date.now()}-${process.pid}`);
assert(data.toLowerCase().startsWith(testRoot.toLowerCase() + path.sep), 'Desktop test data must stay inside test-output');
fs.mkdirSync(data, {recursive: true});
const port = await new Promise((resolve) => {
  const listener = net.createServer();
  listener.listen(0, '127.0.0.1', () => {
    const port = listener.address().port;
    listener.close(() => resolve(port));
  });
});
const child = spawn(exe, ['--data-dir', data, '--ui-dir', ui, '--no-auto-setup'], {
  windowsHide: true, stdio: 'ignore', env: {...process.env,
    WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}`}
});
let launchError;
child.on('error', (error) => { launchError = error; });
let browser;
try {
  const endpoint = `http://127.0.0.1:${port}`;
  let ready = false;
  for (let i = 0; i < 100; i++) {
    if (launchError) throw launchError;
    if (child.exitCode !== null) throw Error(`Desktop exited ${child.exitCode}`);
    try { if ((await fetch(`${endpoint}/json/version`)).ok) { ready = true; break; } } catch {}
    await new Promise((resolve) => setTimeout(resolve, 200));
  }
  assert(ready, 'WebView2 debugging endpoint did not become ready');
  browser = await chromium.connectOverCDP(endpoint);
  const context = browser.contexts()[0];
  for (let i = 0; i < 100 && !context.pages().length; ++i)
    await new Promise(resolve => setTimeout(resolve, 100));
  const page = context.pages()[0];
  assert(page, 'Host WebView2 page did not appear');
  // The debugging endpoint can exist before the host has configured its real
  // WebView. Wait for that navigation instead of racing it with a second one.
  await page.waitForURL('https://lexica.local/**', {waitUntil: 'domcontentloaded', timeout: 20000});
  await page.locator('.app-shell').waitFor();
  const errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.addInitScript(() => {
    window.__ttsPlaying = 0;
    const play = HTMLMediaElement.prototype.play;
    HTMLMediaElement.prototype.play = function () {
      this.addEventListener('playing', () => { window.__ttsPlaying++; }, {once: true});
      return play.call(this);
    };
  });
  await page.goto('https://lexica.local/settings');
  assert(await page.evaluate(() => Boolean(window.chrome?.webview)), 'Native bridge missing');
  await page.getByRole('button', {name: 'Preview voice', exact: true}).click();
  await page.waitForFunction(() => window.__ttsPlaying > 0, null, {timeout: 15000});
  await page.screenshot({path: path.join(data, 'settings-playing.png'), fullPage: true});
  await page.goto('https://lexica.local/?word=meticulous');
  await page.getByRole('button', {name: 'Pronounce word', exact: true}).click();
  await page.waitForFunction(() => window.__ttsPlaying > 0);
  await page.getByRole('button', {name: 'Read example', exact: true}).first().click();
  await page.waitForFunction(() => window.__ttsPlaying > 1);
  const exampleBox = await page.locator('.lead-example .example').boundingBox();
  assert(exampleBox.width > 200 && exampleBox.height < 250, 'Voice notice squeezed the example text');
  await page.locator('.context').screenshot({path: path.join(data, 'sentence-playing.png')});
  await page.evaluate(() => window.scrollTo(0, 0));
  await page.screenshot({path: path.join(data, 'word-sentence-playing.png'), fullPage: true});
  await page.reload();
  await page.getByRole('button', {name: 'Pronounce word', exact: true}).click();
  await page.waitForFunction(() => window.__ttsPlaying > 0);
  assert.deepEqual(errors, []);
  const result = {ok: true, transport: 'native WebView2', real_media_playing_events: true,
    settings_preview: true, word: true, sentence: true, reload: true,
    sentence_layout: {width: exampleBox.width, height: exampleBox.height}, human_listening: false};
  fs.writeFileSync(path.join(data, 'report.json'), JSON.stringify(result, null, 2));
  console.log(JSON.stringify({...result, data}));
} finally {
  try { await browser?.close(); }
  finally { child.kill(); }
}
