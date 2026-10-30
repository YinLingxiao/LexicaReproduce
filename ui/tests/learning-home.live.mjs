import {test, expect} from '@playwright/test';
import {execFileSync} from 'node:child_process';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const evidence = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../docs/parity/evidence/learning-home');
const database = path.join(process.env.LEXICA_HOME_LIVE_DATA, 'lexica.db');

function backdate() {
	const script = `
import sqlite3
from pathlib import Path
assert Path(${JSON.stringify(database)}).is_file(), 'isolated test database missing'
conn = sqlite3.connect(${JSON.stringify(database)}, timeout=30)
conn.execute("PRAGMA busy_timeout=30000")
cur = conn.execute("UPDATE memory_states SET next_review_at='2020-01-01T00:00:00.000Z'")
print(cur.rowcount)
conn.commit()
`;
	const out = execFileSync('py', ['-3', '-c', script], {encoding: 'utf8'}).trim();
	if (Number(out) < 2) throw new Error(`expected two due rows, updated ${out}`);
}

test('isolated service: lookup, due review, feedback, refreshed home', async ({page, request}) => {
	const calls = [];
	await page.route('**/rpc', async (route) => {
		calls.push(route.request().postDataJSON().command);
		await route.continue();
	});
	const rpc = async (command, args = {}) => {
		const response = await request.post('/rpc', {
			data: {protocol_version: 1, request_id: `live-${Date.now()}-${calls.length}`, command, args}
		});
		expect(response.ok()).toBeTruthy();
		const body = await response.json();
		expect(body.ok, JSON.stringify(body)).toBe(true);
		return body.result;
	};

	await page.goto('/');
	await expect(page.getByRole('heading', {name: '先查一个词'})).toBeVisible();
	await expect(page.getByText('当前待复习')).toHaveCount(0);
	await page.screenshot({path: path.join(evidence, 'home-live-new.png'), fullPage: true, animations: 'disabled'});

	await page.getByRole('combobox', {name: 'Search for a word'}).fill('meticulous');
	await page.getByRole('button', {name: 'Look up'}).click();
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await expect(page.getByText('查词记录已保存')).toBeVisible();
	await expect(page.getByText(/查过 \d+ 次/)).toBeVisible();
	await expect(page.getByText(/下次复习/)).toBeVisible();
	await page.screenshot({path: path.join(evidence, 'home-live-lookup.png'), fullPage: true, animations: 'disabled'});

	await rpc('lookup_word', {word: 'subtle'});
	backdate();
	await page.goto('/');
	await expect(page.getByRole('heading', {name: '当前待复习 2 个'})).toBeVisible();
	await expect(page.getByRole('button', {name: 'meticulous'})).toBeVisible();
	await page.setViewportSize({width: 480, height: 820});
	expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
	await page.setViewportSize({width: 1180, height: 820});
	await page.screenshot({path: path.join(evidence, 'home-live-due.png'), fullPage: true, animations: 'disabled'});

	calls.length = 0;
	const start = page.getByRole('link', {name: '开始复习'});
	await start.focus();
	await expect(start).toBeFocused();
	await page.keyboard.press('Enter');
	await expect(page).toHaveURL(/\/review\?mode=due$/);
	await expect(page.getByText('到期复习', {exact: true})).toBeVisible();
	await expect(page.getByRole('heading', {name: /meticulous|subtle/})).toBeVisible();
	expect(calls).toContain('review_group');
	expect(calls).not.toContain('study_group');
	expect(calls).not.toContain('study_grade');
	expect(calls).not.toContain('submit_review');
	expect(calls).not.toContain('start_review_item');
	await page.screenshot({path: path.join(evidence, 'review-live-due.png'), fullPage: true, animations: 'disabled'});

	await page.getByRole('button', {name: '认识', exact: true}).click();
	await page.getByRole('button', {name: '认识', exact: true}).click();
	await expect(page.getByRole('heading', {name: '本轮已完成'})).toBeVisible();
	expect(calls.filter((name) => name === 'study_grade')).toHaveLength(2);

	await page.goto('/');
	await expect(page.getByRole('heading', {name: '当前暂无到期复习'})).toBeVisible();
	await expect(page.getByText('今日已完成')).toHaveCount(0);
	const summary = await rpc('stats_summary');
	expect(summary.due_now).toBe(0);
	expect(summary.encountered).toBeGreaterThanOrEqual(2);
	await page.screenshot({path: path.join(evidence, 'home-live-clear.png'), fullPage: true, animations: 'disabled'});
});

test('real system TTS plays words and sentences across reloads and tabs', async ({page, context, request}) => {
	const rpc = async (command, args = {}) => {
		const response = await request.post('/rpc', {data: {
			protocol_version: 1, request_id: `tts-live-${crypto.randomUUID()}`, command, args
		}});
		return response.json();
	};
	const catalog = await rpc('tts_voices');
	expect(catalog.ok).toBe(true);
	test.skip(!catalog.result.supported || !catalog.result.voices.length, 'No installed English voice in this environment');
	const errors = [];
	page.on('pageerror', (error) => errors.push(error.message));
	await context.addInitScript(() => {
		window.__realAudioPlaying = 0;
		const play = HTMLMediaElement.prototype.play;
		HTMLMediaElement.prototype.play = function () {
			this.addEventListener('playing', () => { window.__realAudioPlaying++; }, {once: true});
			return play.call(this);
		};
	});
	const synthRequests = [];
	context.on('request', (req) => {
		if (req.method() !== 'POST' || !req.url().endsWith('/rpc')) return;
		const data = req.postDataJSON();
		if (data.command === 'tts_synthesize') synthRequests.push(data.args);
	});
	await page.goto('/?word=meticulous');
	await page.getByRole('button', {name: '朗读单词', exact: true}).click();
	await expect.poll(() => page.evaluate(() => window.__realAudioPlaying)).toBeGreaterThan(0);
	expect(synthRequests.at(-1).text).toBe('meticulous');
	const firstClient = synthRequests.at(-1).clientId;
	expect(firstClient).toBeTruthy();
	await rpc('tts_cancel', {token: 999999, clientId: firstClient});
	const stale = await rpc('tts_synthesize', {text: 'hello', token: 1, clientId: firstClient});
	expect(stale.error.code).toBe('CANCELLED');
	await page.reload();
	await page.getByRole('button', {name: '朗读单词', exact: true}).click();
	await expect.poll(() => page.evaluate(() => window.__realAudioPlaying)).toBeGreaterThan(0);
	expect(synthRequests.at(-1).clientId).not.toBe(firstClient);
	const secondClient = synthRequests.at(-1).clientId;
	await page.evaluate(() => document.fonts.ready);
	const beforeExample = await page.locator('.lead-example .example').boundingBox();
	await page.getByRole('button', {name: '朗读例句', exact: true}).first().click();
	await expect.poll(() => page.evaluate(() => window.__realAudioPlaying)).toBeGreaterThan(1);
	expect(synthRequests.at(-1).text).not.toBe('meticulous');
	const afterExample = await page.locator('.lead-example .example').boundingBox();
	expect(afterExample.width).toBeGreaterThan(200);
	expect(afterExample.width).toBeGreaterThanOrEqual(beforeExample.width * 0.95);
	expect(afterExample.height).toBeLessThanOrEqual(beforeExample.height + 40);
	await page.setViewportSize({width: 480, height: 820});
	expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
	expect((await page.locator('.lead-example .example').boundingBox()).width).toBeGreaterThan(150);
	await page.locator('.context').screenshot({path: path.join(evidence, 'tts-http-480.png')});
	await page.setViewportSize({width: 1180, height: 820});
	await page.screenshot({path: path.join(evidence, 'tts-http-playing.png'), fullPage: true});
	const second = await context.newPage();
	await second.goto('/settings');
	await second.getByRole('button', {name: '试听', exact: true}).click();
	await expect.poll(() => second.evaluate(() => window.__realAudioPlaying)).toBeGreaterThan(0);
	expect(synthRequests.at(-1).clientId).not.toBe(secondClient);
	expect(errors).toEqual([]);
	await second.close();
});
