import {test, expect, type Page} from '@playwright/test';
import {answerFor, calls, edgeEntries, entries, installContractFixture} from './contract-fixture';

const surgeryId = entries.find((entry) => entry.word === 'surgery')!.id;

async function onStudy(page: Page, ids?: number[]) {
	await page.goto('/');
	await page.evaluate((value) => {
		localStorage.setItem('test.study.on', '1');
		if (value) localStorage.setItem('test.study.ids', JSON.stringify(value));
	}, ids ?? null);
}

test.beforeEach(async ({page}) => {
	await installContractFixture(page);
});

test('motion respects reduced motion and keeps navigation usable on compact screens', async ({page}) => {
	await page.emulateMedia({reducedMotion: 'no-preference'});
	await page.goto('/stats');
	await expect(page.locator('.metric').first()).toBeVisible();
	await expect(page.locator('.page')).toHaveCSS('animation-name', 'lexica-enter');
	await expect(page.locator('.chart .bar').first()).toHaveCSS('animation-name', 'lexica-grow');
	await page.emulateMedia({reducedMotion: 'reduce'});
	await expect(page.locator('.page')).toHaveCSS('animation-name', 'none');
	await expect(page.locator('.chart .bar').first()).toHaveCSS('animation-name', 'none');
	await page.setViewportSize({width: 480, height: 820});
	await page.getByRole('navigation', {name: 'Main (compact)', exact: true}).getByRole('link', {name: 'Settings', exact: true}).click();
	await expect(page).toHaveURL(/\/settings/);
	await expect(page.locator('.page')).toHaveCSS('animation-name', 'none');
	expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});

test('study settings disclosure opens and closes without changing the card', async ({page}) => {
	await onStudy(page, [1, 2]);
	await page.goto('/review');
	const heading = page.getByRole('heading', {name: 'meticulous', exact: true});
	await expect(heading).toBeVisible();
	for (const reducedMotion of ['no-preference', 'reduce'] as const) {
		await page.emulateMedia({reducedMotion});
		await page.locator('.settings-btn').click();
		await expect(page.locator('#study-settings')).toBeVisible();
		await page.getByRole('button', {name: '关闭学习设置'}).click();
		await expect(page.locator('#study-settings')).toHaveCount(0);
		await expect(heading).toBeVisible();
	}
	expect((await calls(page)).filter(call => call.command === 'study_grade')).toHaveLength(0);
});

test('home hit, miss, load, error retry, keyboard and deep link', async ({page}) => {
	await page.goto('/');
	await expect(page.getByText(`${entries.length} entries · offline`)).toBeVisible();
	await page.screenshot({path: test.info().outputPath('home-1180.png'), fullPage: true, animations: 'disabled'});
	await expect(page.locator('body')).toHaveCSS('font-family', /Lexica Sans/);
	const input = page.getByRole('combobox', {name: 'Search for a word'});
	await input.fill('met');
	await expect(page.getByRole('option', {name: /meticulous/})).toBeVisible();
	await input.press('ArrowDown');
	await input.press('Enter');
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await expect(page).toHaveURL(/word=meticulous/);
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toHaveCSS('font-family', /Newsreader/);
	await page.screenshot({path: test.info().outputPath('word-1180.png'), fullPage: true, animations: 'disabled'});
	await page.locator('a.back').click();
	await expect(input).toBeVisible();
	await page.keyboard.press('Control+k');
	await expect(input).toBeFocused();
	await page.goto('/?word=zzzzzzzz');
	await expect(page.getByRole('heading', {name: /No entry for “zzzzzzzz”/})).toBeVisible();
	await page.evaluate(() => localStorage.setItem('test.lookup.fail', '1'));
	await page.goto('/?word=meticulous');
	await expect(page.getByRole('alert')).toContainText('lookup_word failed');
	await page.evaluate(() => localStorage.removeItem('test.lookup.fail'));
	await page.getByRole('button', {name: 'Try again'}).click();
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await page.goto('/');
	await page.evaluate(() => localStorage.setItem('test.lookup.slow', '1'));
	await page.goto('/?word=meticulous');
	await expect(page.locator('.skeleton')).toBeVisible();
	await page.goto('/?word=elaborate');
	await expect(page.getByRole('heading', {name: 'elaborate', exact: true})).toBeVisible();
	await page.waitForTimeout(800);
	await expect(page.getByRole('heading', {name: 'elaborate', exact: true})).toBeVisible();
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toHaveCount(0);
});

test('IME composition does not commit or replace suggestions early', async ({page}) => {
	await page.goto('/');
	const input = page.getByRole('combobox', {name: 'Search for a word'});
	await input.focus();
	await input.dispatchEvent('compositionstart');
	await input.fill('meticulous');
	await input.press('Enter');
	await expect(page).not.toHaveURL(/word=/);
	expect((await calls(page)).some((call) => call.command === 'lookup_word')).toBe(false);
	await input.dispatchEvent('compositionend');
	await expect(page.getByRole('option', {name: /meticulous/})).toBeVisible();
});

test('slow suggestions cannot replace a newer query', async ({page}) => {
	await page.goto('/');
	const input = page.getByRole('combobox', {name: 'Search for a word'});
	await input.fill('m');
	await expect.poll(() => page.evaluate(() => (window as any).__calls.some((call: any) => call.command === 'search_suggest' && call.args.query === 'm'))).toBe(true);
	await input.fill('sub');
	await expect(page.getByRole('option', {name: /subtle/})).toBeVisible();
	await page.waitForTimeout(500);
	await expect(page.getByRole('option', {name: /subtle/})).toBeVisible();
	await expect(page.getByRole('option', {name: /meticulous/})).toHaveCount(0);
	await input.press('Escape');
	await expect(page.getByRole('listbox')).toHaveCount(0);
});

test('notes keep unsaved drafts and retry after a failed save', async ({page}) => {
	await page.goto('/?word=meticulous');
	const notes = page.getByLabel('Word notes');
	await expect(notes).toBeEnabled();
	await notes.fill('不能丢失的笔记');
	await page.getByRole('combobox', {name: 'Search for a word'}).press('Enter');
	await expect(notes).toHaveValue('不能丢失的笔记');
	await page.evaluate(() => ((window as any).__saveFails = true));
	await page.getByRole('button', {name: 'Save', exact: true}).click();
	await expect(page.getByRole('alert')).toContainText('笔记保存失败');
	await expect(notes).toHaveValue('不能丢失的笔记');
	await page.evaluate(() => ((window as any).__saveFails = false));
	await page.getByRole('button', {name: 'Save', exact: true}).click();
	await expect(page.getByRole('status').filter({hasText: 'Saved'})).toBeVisible();
	await page.getByRole('button', {name: 'Copy entry'}).click();
	await expect.poll(() => page.evaluate(() => (window as any).__copied as string)).toContain('meticulous');
	await page.reload();
	await expect(page.getByLabel('Word notes')).toHaveValue('不能丢失的笔记');
});

test('bookmark, pause, library filter, search race and pagination', async ({page}) => {
	await page.goto('/?word=meticulous');
	await page.getByRole('button', {name: 'Bookmark this word'}).click();
	await expect(page.getByRole('button', {name: 'Remove bookmark'})).toHaveAttribute('aria-pressed', 'true');
	await page.getByLabel('Word notes').fill('联想到检查每一个细节。');
	await page.getByRole('button', {name: 'Save', exact: true}).click();
	await page.getByRole('button', {name: 'Pause reminders'}).click();
	await expect(page.getByRole('button', {name: 'Resume reminders'})).toBeVisible();
	await page.getByRole('link', {name: 'Words', exact: true}).click();
	await page.getByRole('button', {name: 'Saved', exact: true}).click();
	await expect(page.getByText('meticulous', {exact: true})).toBeVisible();
	await page.getByLabel('Search words and notes').fill('每一个细节');
	await expect(page.getByText('meticulous', {exact: true})).toBeVisible();
	await page.getByRole('button', {name: 'Resume', exact: true}).click();
	await expect(page.getByRole('button', {name: 'Resume', exact: true})).toHaveCount(0);
	await page.evaluate(() => localStorage.setItem('test.library.seeded', '1'));
	await page.getByRole('button', {name: 'All', exact: true}).click();
	const filter = page.getByLabel('Search words and notes');
	await filter.fill('ela');
	await page.waitForTimeout(220);
	await filter.fill('met');
	await page.waitForTimeout(900);
	await expect(page.getByText('meticulous', {exact: true})).toBeVisible();
	await expect(page.getByText('elaborate', {exact: true})).toHaveCount(0);
	await page.evaluate(() => localStorage.setItem('test.library.many', '1'));
	await filter.fill('');
	await expect(page.getByText(/1 \/ 2/)).toBeVisible();
	await page.getByRole('button', {name: 'Next', exact: true}).click();
	await expect(page.getByText(/2 \/ 2/)).toBeVisible();
	await page.evaluate(() => localStorage.setItem('test.library.fail', '1'));
	await page.reload();
	await expect(page.getByRole('alert')).toContainText('library_words');
	await page.evaluate(() => localStorage.removeItem('test.library.fail'));
	await page.getByRole('button', {name: 'Retry'}).click();
	await expect(page.getByRole('heading', {name: /Words/})).toBeVisible();
	await expect(page.getByRole('alert')).toHaveCount(0);
});

test('stats empty, fading, error and retry', async ({page}) => {
	await page.goto('/stats');
	await expect(page.getByRole('heading', {name: 'Stats', exact: true})).toBeVisible();
	await expect(page.getByText('No activity yet.')).toBeVisible();
	await page.evaluate(() => {
		localStorage.setItem('test.stats.activity', '1');
		localStorage.setItem('test.stats.fading', '1');
	});
	await page.reload();
	await expect(page.getByText('No activity yet.')).toHaveCount(0);
	await expect(page.getByText('meticulous', {exact: true})).toBeVisible();
	await page.evaluate(() => localStorage.setItem('test.stats.fail', '1'));
	await page.reload();
	await expect(page.getByRole('alert')).toContainText('stats_summary');
	await page.evaluate(() => localStorage.removeItem('test.stats.fail'));
	await page.getByRole('button', {name: 'Retry'}).click();
	await expect(page.getByText('Encountered')).toBeVisible();
});

test('settings styles, illegal study values, extension order and dictionary error', async ({page}) => {
	await page.goto('/settings');
	await expect(page.getByText('Seed only')).toBeVisible();
	await expect(page.getByText(String(entries.length), {exact: true}).first()).toBeVisible();
	for (const [label, accent] of [['留白沙龙', '#7a541d'], ['极地群青', '#284c82'], ['牛津辞书', '#9e3324']] as const) {
		await page.getByRole('button', {name: label, exact: true}).click();
		await expect.poll(() => page.evaluate(() => getComputedStyle(document.documentElement).getPropertyValue('--accent').trim())).toBe(accent);
	}
	await page.getByRole('button', {name: 'Dark', exact: true}).click();
	await page.getByRole('button', {name: 'Large', exact: true}).click();
	await page.getByRole('button', {name: 'Full entry', exact: true}).click();
	await page.reload();
	await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
	await expect(page.locator('html')).toHaveAttribute('data-edition', 'vermilion');
	await expect(page.locator('html')).toHaveAttribute('data-font-size', 'large');
	await page.screenshot({path: test.info().outputPath('settings-dark-large.png'), fullPage: true});
	await page.evaluate(() => localStorage.setItem('lexica.preferences', JSON.stringify({
		theme: 'nope', edition: 'nope', reading: 'nope', fontSize: 'huge',
		study: {wordbook: 'nope', dailyNewLimit: 7, cardMode: 'nope', planStyle: 'nope', priority: 'nope', accent: 'AU', speechRate: 2, exampleStyle: 'nope', exampleOrder: 'nope', pronounceRepeat: 'always', extensionTabs: [{id: 'nope', enabled: true}, {id: 'forms', enabled: 'yes'}, {id: 'roots', enabled: false}]}
	})));
	await page.reload();
	await expect(page.locator('html')).toHaveAttribute('data-theme', 'system');
	await expect(page.locator('html')).toHaveAttribute('data-edition', 'obsidian');
	await page.locator('summary').filter({hasText: '学习设置'}).click();
	await expect(page.getByRole('radio', {name: /大学英语四级/})).toHaveAttribute('aria-checked', 'true');
	await expect(page.getByRole('button', {name: '40', exact: true})).toHaveAttribute('aria-pressed', 'true');
	await expect(page.getByRole('checkbox', {name: '词根词缀'})).toHaveAttribute('aria-checked', 'false');
	const order = () => page.locator('.tab-list .handle').evaluateAll((nodes) => nodes.map((node) => node.getAttribute('aria-label')));
	expect((await order())[0]).toContain('词形变化');
	await page.locator('.tab-list .handle').first().focus();
	await page.keyboard.press('ArrowDown');
	expect((await order())[0]).toContain('词根词缀');
	await page.reload();
	await page.locator('summary').filter({hasText: '学习设置'}).click();
	expect((await order())[0]).toContain('词根词缀');
	await page.evaluate(() => ((window as any).__ensureLeft = 1));
	await page.getByRole('button', {name: /Download dictionary|Re-check dictionary/}).click();
	await expect(page.getByRole('dialog')).toContainText('Dictionary setup failed');
	await page.getByRole('button', {name: 'Retry'}).click();
	await expect(page.getByRole('dialog')).toHaveCount(0);
});

test('sidebar collapse, breakpoints and reduced motion', async ({page}) => {
	await page.goto('/');
	const sidebar = page.locator('aside.sidebar');
	await page.getByRole('button', {name: 'Collapse sidebar'}).click();
	await expect(sidebar).toHaveClass(/collapsed/);
	await page.reload();
	await expect(sidebar).toHaveClass(/collapsed/);
	await page.getByRole('button', {name: 'Expand sidebar'}).focus();
	await page.keyboard.press('Enter');
	await expect(sidebar).not.toHaveClass(/collapsed/);
	for (const width of [480, 759, 760, 761, 1180]) {
		await page.setViewportSize({width, height: 820});
		for (const route of ['/', '/?word=meticulous', '/library', '/review', '/stats', '/settings']) {
			await page.goto(route);
			await expect(page.locator('main')).toBeVisible();
			expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
		}
		const bottom = page.locator('.bottom-nav');
		if (width <= 760) await expect(bottom).toBeVisible();
		else await expect(bottom).toBeHidden();
		if (width === 480) await page.screenshot({path: test.info().outputPath('settings-480.png'), fullPage: true});
	}
});

test('theme circle follows the pointer, keyboard and reduced motion', async ({page}) => {
	await page.goto('/settings');
	await page.getByRole('button', {name: 'Dark', exact: true}).click();
	const toggle = page.locator('button.theme');
	await toggle.click({position: {x: 4, y: 4}});
	await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
	const box = await toggle.boundingBox();
	const origin = await page.evaluate(() => ({x: document.documentElement.style.getPropertyValue('--reveal-x'), y: document.documentElement.style.getPropertyValue('--reveal-y'), vt: (window as any).__vt}));
	expect(origin.vt).toEqual(['expand']);
	expect(Math.abs(parseFloat(origin.x) - (box!.x + 4))).toBeLessThan(1.5);
	expect(Math.abs(parseFloat(origin.y) - (box!.y + 4))).toBeLessThan(1.5);
	await toggle.focus();
	await page.keyboard.press('Enter');
	await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
	await toggle.click();
	await toggle.click();
	await expect.poll(() => page.evaluate(() => document.documentElement.dataset.revealing ?? null)).toBe(null);
	await page.emulateMedia({reducedMotion: 'reduce'});
	await page.reload();
	await page.locator('button.theme').click();
	expect(await page.evaluate(() => (window as any).__vt)).toEqual([]);
});

test('study card browse is read-only until graded, and forgot returns to the end', async ({page}) => {
	await onStudy(page, [1, 2]);
	await page.goto('/review');
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await expect(page.getByText(/Browse · /)).toContainText('1');
	await expect(page.getByRole('button', {name: 'Previous'})).toBeDisabled();
	await expect(page.getByRole('button', {name: 'Start practice'})).toBeDisabled();
	await page.keyboard.press('ArrowRight');
	await expect(page.getByRole('heading', {name: 'elaborate', exact: true})).toBeVisible();
	expect((await calls(page)).filter((call) => call.command === 'study_grade')).toHaveLength(0);
	await page.getByRole('button', {name: 'Previous'}).click();
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await page.getByRole('button', {name: '认识', exact: true}).click();
	await expect(page.getByRole('heading', {name: 'elaborate', exact: true})).toBeVisible();
	await page.keyboard.press('2');
	await expect(page.getByRole('heading', {name: 'Group complete'})).toBeVisible();
	await onStudy(page, [1]);
	await page.goto('/review');
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await page.keyboard.press('3');
	await expect(page.getByText(/Browse · /)).toContainText('2');
	await page.getByRole('button', {name: '认识', exact: true}).click();
	await expect(page.getByRole('heading', {name: 'Group complete'})).toBeVisible();
	await expect(page.locator('.summary').getByText('忘记')).toBeVisible();
	await expect(page.locator('.summary').getByText('认识')).toBeVisible();
	const grades = (await calls(page)).filter((call) => call.command === 'study_grade').map((call) => call.args.grade);
	expect(grades).toEqual(['forgot', 'know']);
});

test('study grade failure keeps the card and a second click does not double-write', async ({page}) => {
	await onStudy(page, [1]);
	await page.evaluate(() => localStorage.setItem('test.grade.fail', '1'));
	await page.goto('/review');
	await page.getByRole('button', {name: '认识', exact: true}).click();
	await expect(page.getByRole('alert')).toContainText('评分没有写入');
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await page.evaluate(() => {
		localStorage.removeItem('test.grade.fail');
		localStorage.setItem('test.grade.slow', '1');
		(window as any).__calls = [];
	});
	await page.getByRole('button', {name: '认识', exact: true}).dblclick();
	await expect(page.getByRole('heading', {name: 'Group complete'})).toBeVisible();
	expect((await calls(page)).filter((call) => call.command === 'study_grade' && call.args.grade === 'know')).toHaveLength(1);
});

test('bookmark does not overwrite notes when reading them fails', async ({page}) => {
	await onStudy(page, [1]);
	await page.goto('/review');
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await page.evaluate(() => {
		(window as any).__calls = [];
		(window as any).__failNext = {command: 'word_note', message: 'notes unavailable'};
	});
	await page.getByRole('button', {name: '收藏', exact: true}).click();
	await expect(page.getByRole('alert')).toContainText('notes unavailable');
	await expect(page.getByRole('button', {name: '收藏', exact: true})).toHaveAttribute('aria-pressed', 'false');
	expect((await calls(page)).filter(call => call.command === 'save_word_note')).toHaveLength(0);
});

test('study card content, context mode, syllables, speech and example preference', async ({page}) => {
	await onStudy(page, [surgeryId, 1]);
	await page.goto('/review');
	await expect(page.getByRole('heading', {name: 'surgery', exact: true})).toBeVisible();
	await expect(page.locator('.headword')).toContainText('sur·ge·ry');
	await expect(page.locator('.syllable-dot').first()).toHaveCSS('animation-name', /syllable-split/);
	await page.emulateMedia({reducedMotion: 'reduce'});
	await expect(page.locator('.syllable-dot').first()).toHaveCSS('animation-name', 'none');
	await page.emulateMedia({reducedMotion: 'no-preference'});
	await expect(page.locator('.pos-pill')).toHaveText('noun');
	await expect(page.locator('.level-pill').first()).toHaveText('B2');
	await expect(page.getByText('He needs surgery to repair his broken leg.')).toBeVisible();
	await expect(page.getByText('他需要手术来修复骨折的腿。')).toBeVisible();
	await expect(page.locator('.translation-zh')).toContainText('外科手术');
	await page.getByRole('tab', {name: '助记'}).click();
	await expect(page.locator('.mnemonic-text')).toContainText('cheirourgia');
	await page.getByRole('tab', {name: '词根词缀'}).click();
	await expect(page.locator('.root-part').first()).toHaveText('surg');
	await page.getByRole('tab', {name: '派生'}).click();
	await expect(page.getByText('microsurgery')).toBeVisible();
	await expect(page.getByText('显微外科')).toBeVisible();
	await page.getByRole('tab', {name: '形近词'}).click();
	await expect(page.getByText('sugary')).toBeVisible();
	await expect(page.getByText('surely')).toBeVisible();
	await expect(page.getByText('surety')).toBeVisible();
	await page.getByRole('tab', {name: '词形变化'}).click();
	await expect(page.getByText('Plural')).toBeVisible();
	await expect(page.getByText('surgeries')).toBeVisible();
	await expect.poll(async () => (await calls(page)).some((item) => item.command === 'tts_synthesize' && item.args.text === 'surgery' && item.args.accent === 'UK')).toBe(true);
	await page.locator('.dots-btn').click();
	await page.getByRole('switch', {name: '自动拆分音节'}).click();
	await expect(page.locator('.headword')).toHaveText('surgery');
	await page.getByRole('button', {name: '真题例句'}).click();
	await page.getByRole('button', {name: '语境卡片'}).click();
	await page.reload();
	await expect(page.locator('.context-card')).toBeVisible();
	await expect(page.locator('.pos-pill')).toHaveCount(0);
	await page.getByRole('button', {name: '点击显示完整释义'}).click();
	await expect(page.locator('.pos-pill')).toBeVisible();
	await page.screenshot({path: test.info().outputPath('study-card.png'), fullPage: true});
});

test('study practice hints, formal attempt, retry and summary', async ({page}) => {
	await onStudy(page, [1]);
	await page.goto('/review');
	await page.getByRole('button', {name: 'Start practice'}).click();
	await expect(page.getByLabel('Which word?')).toBeFocused();
	await expect(page.locator('.kind')).toHaveText('Chinese definition');
	await page.getByRole('button', {name: /First letter/}).click();
	await expect(page.getByText('m_________', {exact: true})).toBeVisible();
	await page.evaluate(() => ((window as any).__failNext = {command: 'request_hint', message: 'temporary hint failure'}));
	await page.getByRole('button', {name: /English definition/}).click();
	await expect(page.getByRole('alert')).toContainText('temporary hint failure');
	await page.getByRole('button', {name: 'Retry', exact: true}).click();
	await expect(page.getByText('m_________', {exact: true})).toBeVisible();
	await page.getByRole('button', {name: /English definition/}).click();
	await expect(page.getByText('very careful about details', {exact: true})).toBeVisible();
	await page.getByRole('button', {name: /Chinese definition/}).click();
	await expect(page.getByText('一丝不苟的', {exact: true})).toBeVisible();
	await expect(page.getByRole('button', {name: /No hints left/})).toBeDisabled();
	const answer = page.getByLabel('Which word?');
	await answer.fill('wrongword');
	await page.getByRole('button', {name: 'Check'}).click();
	await expect(page.getByText('Forgotten')).toBeVisible();
	await page.getByRole('button', {name: /Next|Finish/}).click();
	await answer.fill('meticulous');
	await page.getByRole('button', {name: 'Check'}).click();
	await expect(page.getByText('Correct — recalled on retry')).toBeVisible();
	await page.getByRole('button', {name: /Next|Finish/}).click();
	await expect(page.getByRole('heading', {name: 'Group complete'})).toBeVisible();
	await expect(page.getByText('needed practice')).toBeVisible();
	const names = (await calls(page)).map((call) => call.command);
	expect(names.filter((name) => name === 'submit_review')).toHaveLength(1);
	expect(names.filter((name) => name === 'practice_check')).toHaveLength(1);
	expect(names.filter((name) => name === 'start_review_item')).toHaveLength(1);
});

test('legacy review group still browses read-only and asks each word once', async ({page}) => {
	await page.goto('/');
	await page.evaluate(() => localStorage.setItem('test.review.ids', JSON.stringify([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12])));
	await page.goto('/review');
	await expect(page.getByText(/Browse · 1 \/ 10/)).toBeVisible();
	expect((await calls(page)).map((call) => call.command)).not.toContain('lookup_word');
	for (let index = 1; index < 10; index++) await page.getByRole('button', {name: 'Next', exact: true}).click();
	await page.getByRole('button', {name: 'Start practice'}).click();
	const asked = new Set<string>();
	for (let index = 0; index < 10; index++) {
		const word = answerFor(await page.locator('.prompt').innerText());
		expect(asked.has(word)).toBe(false);
		asked.add(word);
		await page.getByLabel('Which word?').fill(word);
		await page.getByRole('button', {name: 'Check'}).click();
		await expect(page.getByRole('status').filter({hasText: word})).toBeVisible();
		await page.getByRole('button', {name: /Next|Finish/}).click();
	}
	expect(asked.size).toBe(10);
	await expect(page.getByRole('heading', {name: 'Group complete'})).toBeVisible();
});

test('review empty, load and command errors', async ({page}) => {
	await page.goto('/');
	await page.evaluate(() => localStorage.setItem('test.review.ids', '[]'));
	await page.goto('/review');
	await expect(page.getByRole('heading', {name: 'Nothing due'})).toBeVisible();
	await page.evaluate(() => {
		localStorage.removeItem('test.review.ids');
		localStorage.setItem('test.study.on', '1');
		localStorage.setItem('test.study.empty', '1');
	});
	await page.goto('/review');
	await expect(page.getByRole('heading', {name: 'Nothing due'})).toBeVisible();
	expect((await calls(page)).some((call) => call.command === 'study_group')).toBe(true);
	await page.evaluate(() => {
		localStorage.removeItem('test.study.on');
		localStorage.removeItem('test.study.empty');
		localStorage.setItem('test.review.fail', '1');
	});
	await page.goto('/review');
	await expect(page.getByRole('heading', {name: 'Review unavailable'})).toBeVisible();
	await page.evaluate(() => localStorage.removeItem('test.review.fail'));
	await page.getByRole('button', {name: 'Reload'}).click();
	await expect(page.getByText(/Browse · /)).toBeVisible();
});

test('AI settings and review prompts fall back without blocking practice', async ({page}) => {
	await page.goto('/settings');
	await page.getByRole('button', {name: 'On', exact: true}).click();
	await page.getByPlaceholder('https://api.deepseek.com/v1').fill('https://mock.local/v1');
	await page.getByPlaceholder('deepseek-chat').fill('mock-model');
	await page.getByPlaceholder('Paste API key').fill('sk-test');
	await page.getByRole('button', {name: 'Save', exact: true}).click();
	await expect(page.getByText('AI settings saved.')).toBeVisible();
	await page.reload();
	await expect(page.getByPlaceholder('https://api.deepseek.com/v1')).toHaveValue('https://mock.local/v1');
	await page.evaluate(() => localStorage.setItem('test.ai.mode', 'fail'));
	await page.getByRole('button', {name: 'Test connection'}).click();
	await expect(page.getByRole('alert')).toContainText('401');
	await page.getByRole('button', {name: 'Remove key'}).click();
	await expect(page.getByText('· stored')).toHaveCount(0);
	await page.evaluate(() => {
		localStorage.setItem('test.ai.enabled', '1');
		localStorage.setItem('test.ai.has_key', '1');
		localStorage.setItem('test.ai.mode', 'fail');
		localStorage.setItem('test.review.ids', JSON.stringify([1]));
	});
	await page.goto('/review');
	await expect(page.locator('.ai-note')).toContainText('AI examples unavailable');
	await page.getByRole('button', {name: 'Start practice'}).click();
	await expect(page.locator('.kind')).toHaveText('Chinese definition');
	await page.evaluate(() => {
		localStorage.setItem('test.ai.mode', 'success');
		localStorage.removeItem('test.ai.cache');
	});
	await page.goto('/review');
	await expect(page.getByText('AI examples ready for 1 word.')).toBeVisible();
	await page.getByRole('button', {name: 'Start practice'}).click();
	await expect(page.locator('.kind')).toHaveText(/Cloze · AI/);
	await expect(page.locator('.prompt')).toHaveText('The result was clearly ______ in the final data.');
});

test('long and sparse entries and themed screenshots stay inside the viewport', async ({page}) => {
	for (const word of ['longentry', 'sparse']) {
		await page.goto(`/?word=${word}`);
		await expect(page.getByRole('heading', {name: word === 'longentry' ? /pneumonoultramicroscopicsilicovolcanoconiosis/ : 'sparse'})).toBeVisible();
		expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
		await page.screenshot({path: test.info().outputPath(`${word}.png`), fullPage: true});
	}
	await page.goto('/?word=sparse');
	await expect(page.getByText('Only an English gloss.')).toBeVisible();
	await page.getByRole('button', {name: 'Full entry', exact: true}).click();
	await expect(page.getByRole('heading', {name: 'Definitions'})).toBeVisible();
});


test('unknown frequency and non-CEFR levels stay honest on lookup and study pages', async ({page}) => {
	await page.goto('/?word=inevitable%20hour');
	await expect(page.getByRole('heading', {name: 'inevitable hour', exact: true})).toBeVisible();
	await expect(page.locator('.syllable-headword')).toHaveText('in·ev·i·ta·ble hour');
	await expect(page.getByText('暂无词频数据')).toBeVisible();
	await expect(page.locator('.freq-ticks')).toHaveCount(0);
	await expect(page.getByText('暂无等级数据')).toBeVisible();
	await expect(page.getByText('CEFR-R')).toHaveCount(0);
	await expect(page.getByText('88%')).toHaveCount(0);
	await expect(page.getByText('死期')).toBeVisible();
	await expect(page.locator('.pos-pill')).toHaveText('词性未标注');
	await page.screenshot({path: test.info().outputPath('inevitable-hour-lookup.png'), fullPage: true});

	await onStudy(page, [950]);
	await page.goto('/review');
	await expect(page.getByRole('heading', {name: 'inevitable hour', exact: true})).toBeVisible();
	await expect(page.locator('.headword .syllable-headword')).toHaveText('in·ev·i·ta·ble hour');
	await expect(page.locator('.level-pill')).toHaveText('未标注');
	await expect(page.getByText('CEFR-R')).toHaveCount(0);
	await page.screenshot({path: test.info().outputPath('inevitable-hour-study.png'), fullPage: true});
});

test('frequency rank, source, Collins stars and estimated levels are not conflated', async ({page}) => {
	await page.goto('/?word=staronly');
	await expect(page.getByText('暂无词频数据')).toBeVisible();
	await expect(page.locator('.freq-ticks')).toHaveCount(0);
	await expect(page.getByText('Collins ★★★★☆')).toBeVisible();
	await expect(page.getByText('星级，不是词频排名')).toBeVisible();
	await expect(page.getByText('暂无等级数据')).toBeVisible();

	await page.goto('/?word=rankedword');
	await expect(page.locator('.freq-ticks')).toHaveCount(1);
	await expect(page.locator('.freq-tick.on')).toHaveCount(14);
	await expect(page.getByText(/第 1,234 名/)).toBeVisible();
	await expect(page.locator('.freq-metric .metric-note')).toContainText('排名越小，越常见');
	await expect(page.locator('.freq-metric .metric-note')).toContainText('ECDICT 当代语料库词频顺序');
	await expect(page.locator('.level-metric .metric-note')).toContainText('旧版推断');
	await expect(page.locator('.level-metric .metric-note')).toContainText('估算');
	await expect(page.locator('.level-pill').first()).toHaveText('B2');

	await page.goto('/?word=badlevel');
	await expect(page.getByText('暂无等级数据')).toBeVisible();
	await expect(page.getByText('G9')).toHaveCount(0);

	await page.goto('/?word=bnconly');
	await expect(page.locator('.freq-metric .metric-note')).toContainText('ECDICT 英国国家语料库词频顺序');
});

test('syllable splitting preserves spaces, hyphens, apostrophes, punctuation and digits', async ({page}) => {
	await page.goto(`/?word=${encodeURIComponent("don't-stop, 42 apples!")}`);
	await expect(page.getByRole('heading', {name: "don't-stop, 42 apples!", exact: true})).toBeVisible();
	const headword = page.locator('.syllable-headword');
	const text = await headword.textContent();
	expect(text).toContain("don't-stop, 42");
	expect(text).toContain('!');
	expect(text).not.toMatch(/[·][\s\-',!0-9]|[\s\-',!0-9][·]/);

	await page.addInitScript(() =>
		localStorage.setItem('lexica.preferences', JSON.stringify({study: {autoSplitSyllables: false}}))
	);
	await page.reload();
	await expect(page.locator('.syllable-headword')).toHaveText("don't-stop, 42 apples!");
});

test('edge fixtures are wired for lookup and study regressions', async ({page}) => {
	expect(edgeEntries.map((entry) => entry.word)).toContain('inevitable hour');
	await page.goto('/?word=staronly');
	await expect(page.getByRole('heading', {name: 'staronly', exact: true})).toBeVisible();
});
