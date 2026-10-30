import {test, expect} from '@playwright/test';
import {execFileSync} from 'node:child_process';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const evidence = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../docs/parity/evidence/learning-home');
const database = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../tmp-learning-home-live/lexica.db');

function backdate() {
	const script = `
import sqlite3
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
	await expect(page.getByText(/已记录查词 \d+ 次/)).toBeVisible();
	await expect(page.getByText(/已安排复习/)).toBeVisible();
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
