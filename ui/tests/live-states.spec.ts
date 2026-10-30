import { test, expect } from '@playwright/test';

test('completed modules persist learning, notes and pause state through the real UI', async ({
	page,
	request,
}) => {
	const errors: string[] = [];
	let sequence = 0;
	page.on('pageerror', (error) => errors.push(error.message));
	const rpc = async (command: string, args = {}) => {
		const response = await request.post('/rpc', {
			data: { protocol_version: 1, request_id: `live-${Date.now()}-${++sequence}`, command, args },
		});
		expect(response.status()).toBe(200);
		const result = await response.json();
		expect(result.ok, JSON.stringify(result)).toBe(true);
		return result.result;
	};
	const caps = await rpc('system_capabilities');
	expect(caps.commands.every((c: { available: boolean }) => c.available)).toBe(true);
	const hit = await rpc('lookup_word', { word: 'meticulous' });
	const wordId = hit.outcome.entry.id;
	await rpc('set_comprehension', { encounterId: hit.encounter_id, level: 'english' });
	await rpc('save_word_note', { wordId, bookmarked: true, note: '保留我的笔记' });
	await rpc('custom_wordbook_toggle', { wordId, added: true });
	expect(await rpc('practice_check', { wordId, answer: 'meticulous' })).toBe(true);
	expect(await rpc('practice_check', { wordId, answer: 'wrong' })).toBe(false);
	const reviewId = await rpc('start_review_item', { wordId });
	await rpc('request_hint', { reviewId, hintNo: 1 });
	const feedback = await rpc('submit_review', { reviewId, answer: 'meticulous' });
	expect(feedback.correct).toBe(true);
	await page.goto('/library?filter=bookmarked');
	await expect(page.getByText('meticulous', { exact: true })).toBeVisible();
	await rpc('set_word_ignored', { wordId, ignored: true });
	await page.goto('/library?filter=ignored');
	await expect(page.getByText('meticulous', { exact: true })).toBeVisible();
	await page.getByRole('button', { name: 'Resume', exact: true }).click();
	await expect(page.getByText('meticulous', { exact: true })).toHaveCount(0);
	expect((await rpc('word_note', { wordId })).note).toBe('保留我的笔记');
	await page.goto('/stats');
	await expect(page.getByRole('heading', { name: 'Stats', exact: true })).toBeVisible();
	await expect(page.getByRole('heading', { name: 'Last 14 days' })).toBeVisible();
	await expect(page.getByRole('alert')).toHaveCount(0);
	await page.goto('/review');
	await expect(
		page
			.getByRole('button', { name: 'Start practice' })
			.or(page.getByRole('heading', { name: 'Nothing due' })),
	).toBeVisible();
	await expect(page.getByRole('alert')).toHaveCount(0);
	await page.goto('/settings');
	await expect(page.getByText('Seed only')).toBeVisible();
	await expect(page.getByText('55', { exact: true }).first()).toBeVisible();
	await page.goto('/?word=zzzzzzzz');
	await expect(page.getByRole('heading', { name: /No entry for “zzzzzzzz”/ })).toBeVisible();
	expect(errors).toEqual([]);
});

test('real C++ service keeps inevitable hour honest', async ({ page, request }) => {
	const sourcePath = '../tests/data/ecdict-inevitable.db';
	const response = await request.post('/rpc', {
		data: {
			protocol_version: 1,
			request_id: `import-${Date.now()}`,
			command: 'import_ecdict',
			args: { sourcePath },
		},
	});
	expect(response.status()).toBe(200);
	const imported = await response.json();
	expect(imported.ok, JSON.stringify(imported)).toBe(true);
	expect(imported.result).toBeGreaterThan(0);
	const lookup = await request.post('/rpc', {
		data: {
			protocol_version: 1,
			request_id: `lookup-${Date.now()}`,
			command: 'lookup_word',
			args: { word: 'inevitable hour' },
		},
	});
	const found = await lookup.json();
	expect(found.ok, JSON.stringify(found)).toBe(true);
	expect(found.result.outcome.type).toBe('hit');
	expect(found.result.outcome.entry.display).toBe('inevitable hour');
	expect(found.result.outcome.entry.frequency_rank).toBeNull();
	expect(found.result.outcome.entry.frequency_source).toBeNull();
	expect(found.result.outcome.entry.collins).toBeNull();
	expect(found.result.outcome.entry.senses[0].level).toBeNull();
	expect(found.result.outcome.entry.senses[0].level_source).toBeNull();
	expect(found.result.outcome.entry.senses[0].chinese_definition).toBe('死期');
	await page.goto(`/?word=${encodeURIComponent('inevitable hour')}`);
	await expect(page.getByRole('heading', { name: 'inevitable hour', exact: true })).toBeVisible();
	await expect(page.locator('.syllable-headword')).toHaveText('in·ev·i·ta·ble hour');
	await expect(page.getByText('Frequency unavailable')).toBeVisible();
	await expect(page.locator('.freq-ticks')).toHaveCount(0);
	await expect(page.getByText('Level unavailable')).toBeVisible();
	await expect(page.getByText('CEFR-R')).toHaveCount(0);
	await expect(page.getByText('88%')).toHaveCount(0);
	await expect(page.locator('.definition-preview')).toContainText('死期');
	await page.getByRole('button', { name: 'Full entry', exact: true }).click();
	await expect(page.locator('.translation-text')).toHaveText('死期');
	await expect(page.locator('.pos-pill')).toHaveText('Part of speech unavailable');
	await page.waitForTimeout(1400);
	await page.screenshot({
		path: test.info().outputPath('inevitable-hour-live.png'),
		fullPage: true,
	});
});
