import {expect, test, type Page} from '@playwright/test';

const evidence = '../docs/parity/evidence/learning-home';

type Entry = ReturnType<typeof entry>;
function entry(id: number, word: string, gloss: string): {
	id: number; word: string; display: string; phonetic: null; frequency_rank: null; frequency_source: null;
	senses: {id: number; pos: string; english_definition: string; chinese_definition: string; level: null; level_source: null; examples: {id: number; text: string; translation: string}[]}[];
	collocations: []; synonyms: []; antonyms: []; word_family: []; tags: []; collins: null; oxford: false; mnemonic: null; roots: [];
	derivatives: []; synonyms_detailed: []; antonyms_detailed: []; lookalikes: []; forms: []; exam_example: null;
} {
	return {
		id, word, display: word, phonetic: null, frequency_rank: null, frequency_source: null,
		senses: [{id: id * 10, pos: 'adj.', english_definition: `Definition of ${word}`, chinese_definition: gloss, level: null, level_source: null, examples: [{id: id * 100, text: `${word} sits in a short example.`, translation: '例句'}]}],
		collocations: [], synonyms: [], antonyms: [], word_family: [], tags: [], collins: null, oxford: false, mnemonic: null, roots: [],
		derivatives: [], synonyms_detailed: [], antonyms_detailed: [], lookalikes: [], forms: [], exam_example: null
	};
}

const entries: Entry[] = [
	entry(1, 'meticulous', '一丝不苟的'),
	entry(2, 'subtle', '微妙的'),
	entry(3, 'resilient', '有韧性的'),
	entry(4, 'profound', '深刻的')
];

const emptyStats = {encountered: 0, learning: 0, familiar: 0, stable: 0, ignored: 0, due_now: 0, high_priority: 0};

async function install(page: Page) {
	await page.addInitScript((packed) => {
		const words = packed.entries as any[];
		const byId = (id: number) => words.find((item) => item.id === id);
		const byWord = (word: string) => words.find((item) => item.word === word);
		const win = window as any;
		win.__calls = [];
		const state = () => JSON.parse(localStorage.getItem('test.home.state') ?? '{}');
		const fail = (message: string) => { const error = new Error(message) as Error & {code: string}; error.code = 'TEMPORARY'; throw error; };
		const card = (id: number, isNew: boolean) => {
			const item = byId(id);
			const sense = item.senses[0];
			return {
				entry: item,
				item: {word_id: item.id, prompt: sense.chinese_definition, kind: 'definition_zh', source: 'dictionary', sense_id: sense.id, example_id: null},
				is_new: isNew,
				bookmarked: false,
				in_custom_wordbook: false
			};
		};
		const handle = async (command: string, args: any = {}) => {
			const current = state();
			if (current.failCommand === command) fail(`${command} failed`);
			switch (command) {
				case 'system_capabilities': return {automatic_dictionary_setup: false, commands: []};
				case 'app_info':
					if (current.appFail) fail('app_info failed');
					return {word_count: current.wordCount ?? 55, schema_version: 6, fts_ok: true, dictionary_ready: false};
				case 'stats_summary':
					if (current.statsFail) fail('stats_summary failed');
					if (current.statsSlow) {
						await new Promise((resolve) => {
							const timer = setInterval(() => {
								if (localStorage.getItem('test.home.release') === '1') {
									clearInterval(timer);
									resolve(null);
								}
							}, 40);
						});
					}
					return current.stats ?? packed.emptyStats;
				case 'recent_words':
					if (current.recentFail) fail('recent_words failed');
					return current.recent ?? [];
				case 'search_suggest':
					return words.filter((item) => item.word.startsWith(String(args.query ?? ''))).slice(0, 8).map((item) => ({word: item.word, display: item.display, phonetic: item.phonetic, frequency_rank: null}));
				case 'lookup_word': {
					const found = byWord(args.word);
					if (!found) return {outcome: {type: 'miss', suggestions: []}, encounter_id: null, memory: null, comprehension_level: null};
					const spec = current.lookups?.[args.word] ?? {};
					const encounter = spec.encounter_id === null ? null : spec.encounter_id ?? found.id;
					return {outcome: {type: 'hit', entry: found}, encounter_id: encounter, memory: encounter === null ? null : {status: spec.status ?? 'learning', visit_count: spec.visit_count ?? 1, high_priority: false}, comprehension_level: 'unknown'};
				}
				case 'library_words': {
					if (current.libraryFail) fail('library_words failed');
					const rows = (current.library ?? []).filter((item: any) => !args.query || String(item.word).includes(args.query));
					return {total: rows.length, words: rows};
				}
				case 'word_note': return {bookmarked: false, note: '', ignored: false};
				case 'ai_config_get': return {enabled: false, base_url: '', model: '', has_key: false};
				case 'ai_generate_examples': return [];
				case 'ai_enrich_words': return 0;
				case 'study_group': {
					if (current.studyFail) fail('study_group failed');
					const ids: number[] = current.newIds ?? [4];
					const cards = [...(current.studyDueIds ?? []).map((id: number) => card(id, false)), ...ids.map((id) => card(id, true))];
					return {cards, due_count: 0, new_count: cards.length, today_learned_new: 0};
				}
				case 'study_grade':
					if (current.gradeSlow) {
						await new Promise<void>((resolve) => {
							const timer = setInterval(() => {
								if (localStorage.getItem('test.grade.release') === '1') { clearInterval(timer); resolve(); }
							}, 20);
						});
					}
					win.__gradeReturned = true;
					return {correct: args.grade !== 'forgot', quality: 'good', answer: '', hints_used: 0, memory: null};
				case 'review_group': {
					const batches: number[][] = current.reviewBatches ?? [[1]];
					const n = Number(sessionStorage.getItem('review.n') ?? '0');
					sessionStorage.setItem('review.n', String(n + 1));
					const ids = batches[Math.min(n, batches.length - 1)] ?? [];
					return ids.map((id) => {
						const full = card(id, false);
						return {entry: full.entry, item: full.item};
					});
				}
				case 'start_review_item': return args.wordId;
				case 'submit_review': return {correct: true, quality: 'excellent', answer: args.answer, hints_used: 0, memory: null};
				case 'request_hint': return {kind: 'mask', text: 'm_________'};
				case 'practice_check': return true;
				case 'practice_hint': return {kind: 'mask', text: 'm_________'};
				default: return null;
			}
		};
		const original = window.fetch.bind(window);
		window.fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
			const url = typeof input === 'string' ? input : input instanceof URL ? input.href : input.url;
			if (!url.includes('/rpc')) return original(input, init);
			const request = JSON.parse(String(init?.body ?? '{}'));
			win.__calls.push({command: request.command, args: request.args});
			try {
				const result = await handle(request.command, request.args ?? {});
				return new Response(JSON.stringify({protocol_version: 1, request_id: request.request_id, ok: true, result}), {status: 200, headers: {'Content-Type': 'application/json'}});
			} catch (error) {
				const coded = error as {code?: string; message?: string};
				return new Response(JSON.stringify({protocol_version: 1, request_id: request.request_id, ok: false, error: {code: coded.code ?? 'INTERNAL_ERROR', message: coded.message ?? 'Command failed'}}), {status: 200, headers: {'Content-Type': 'application/json'}});
			}
		};
	}, {entries, emptyStats});
}

async function boot(page: Page, state: unknown) {
	await page.goto('/');
	await page.evaluate((value) => {
		localStorage.removeItem('test.home.release');
		localStorage.setItem('test.home.state', JSON.stringify(value));
	}, state);
	await page.reload();
}

function calls(page: Page) {
	return page.evaluate(() => (window as any).__calls as {command: string; args: any}[]);
}

test.beforeEach(async ({page}) => {
	await install(page);
});

test('new user sees a first lookup, not an empty review victory', async ({page}) => {
	await boot(page, {stats: emptyStats, recent: [], wordCount: 55});
	await expect(page.getByRole('heading', {name: '让查过的词真正被记住。'})).toBeVisible();
	await expect(page.getByRole('heading', {name: '先查一个词'})).toBeVisible();
	await expect(page.getByText('当前待复习')).toHaveCount(0);
	await expect(page.getByText('当前暂无到期复习')).toHaveCount(0);
	await expect(page.getByText('55 entries · offline')).toBeVisible();
	await page.getByRole('button', {name: '去查第一个词'}).click();
	await expect(page.getByRole('combobox', {name: 'Search for a word'})).toBeFocused();
	await page.screenshot({path: `${evidence}/home-new.png`, fullPage: true, animations: 'disabled'});
});

test('due reviews are the primary action and open the due queue', async ({page}) => {
	await boot(page, {
		stats: {...emptyStats, encountered: 4, learning: 4, due_now: 4},
		recent: [{word: 'meticulous', display: 'meticulous', phonetic: null, visited_at: '2026-10-30T00:00:00.000Z', comprehension_level: 'unknown'}],
		reviewBatches: [[1, 2]],
		newIds: [4]
	});
	await expect(page.getByRole('heading', {name: '当前待复习 4 个'})).toBeVisible();
	await expect(page.getByText('今日已完成')).toHaveCount(0);
	await expect(page.getByRole('heading', {name: '最近查过的词'})).toBeVisible();
	await expect(page.getByRole('button', {name: 'meticulous'})).toBeVisible();
	for (const width of [1180, 760, 480]) {
		await page.setViewportSize({width, height: 820});
		expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
	}
	await page.setViewportSize({width: 480, height: 820});
	await page.screenshot({path: `${evidence}/home-due-480.png`, fullPage: true, animations: 'disabled'});
	await page.setViewportSize({width: 1180, height: 820});
	await page.screenshot({path: `${evidence}/home-due.png`, fullPage: true, animations: 'disabled'});
	const start = page.getByRole('link', {name: '开始复习'});
	await start.focus();
	await expect(start).toBeFocused();
	await page.keyboard.press('Enter');
	await expect(page).toHaveURL(/\/review\?mode=due$/);
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await expect(page.getByText('到期复习', {exact: true})).toBeVisible();
	await expect(page.getByText('新词学习')).toHaveCount(0);
	const names = (await calls(page)).map((call) => call.command);
	expect(names).toContain('review_group');
	expect(names).not.toContain('study_group');
	expect(names).not.toContain('study_grade');
	expect(names).not.toContain('submit_review');
	expect(names).not.toContain('start_review_item');
});

test('recorded words with nothing due offer lookup, library, and separate new-word study', async ({page}) => {
	await boot(page, {
		stats: {...emptyStats, encountered: 2, learning: 2, due_now: 0},
		recent: [{word: 'subtle', display: 'subtle', phonetic: null, visited_at: '2026-10-30T00:00:00.000Z', comprehension_level: 'unknown'}],
		newIds: [4],
		reviewBatches: [[1]]
	});
	await expect(page.getByRole('heading', {name: '当前暂无到期复习'})).toBeVisible();
	await expect(page.getByText('学习新词来自当前词书，不是到期复习。')).toBeVisible();
	await page.screenshot({path: `${evidence}/home-clear.png`, fullPage: true, animations: 'disabled'});
	await page.getByRole('link', {name: '学习新词'}).click();
	await expect(page).toHaveURL(/mode=new/);
	await expect(page.getByRole('heading', {name: 'profound', exact: true})).toBeVisible();
	await expect(page.getByText('新词学习')).toBeVisible();
	const names = (await calls(page)).map((call) => call.command);
	expect(names).toContain('study_group');
	expect(names).not.toContain('review_group');
});

test('a failed summary stays failed until retry', async ({page}) => {
	await boot(page, {statsSlow: true, stats: {...emptyStats, encountered: 2, learning: 2, due_now: 2}, recent: []});
	await expect(page.getByRole('heading', {name: '正在读取学习记录'})).toBeVisible();
	await expect(page.getByText('当前待复习')).toHaveCount(0);
	await expect(page.getByText('当前暂无到期复习')).toHaveCount(0);
	await page.screenshot({path: `${evidence}/home-loading.png`, fullPage: true, animations: 'disabled'});
	await page.evaluate(() => localStorage.setItem('test.home.release', '1'));
	await expect(page.getByRole('heading', {name: '当前待复习 2 个'})).toBeVisible();
	await page.evaluate(() => {
		const state = JSON.parse(localStorage.getItem('test.home.state') ?? '{}');
		state.statsFail = true;
		state.statsSlow = false;
		localStorage.setItem('test.home.state', JSON.stringify(state));
	});
	await page.reload();
	await expect(page.getByRole('alert')).toContainText('stats_summary failed');
	await expect(page.getByText('当前待复习')).toHaveCount(0);
	await expect(page.getByText('当前暂无到期复习')).toHaveCount(0);
	await page.screenshot({path: `${evidence}/home-error.png`, fullPage: true, animations: 'disabled'});
	await page.evaluate(() => {
		const state = JSON.parse(localStorage.getItem('test.home.state') ?? '{}');
		state.statsFail = false;
		state.stats = {encountered: 3, learning: 3, familiar: 0, stable: 0, ignored: 0, due_now: 2, high_priority: 0};
		localStorage.setItem('test.home.state', JSON.stringify(state));
	});
	await page.getByRole('button', {name: '重试'}).click();
	await expect(page.getByRole('heading', {name: '当前待复习 2 个'})).toBeVisible();
});

test('finishing one due group still loads the remaining due words', async ({page}) => {
	await boot(page, {
		stats: {...emptyStats, encountered: 3, learning: 3, due_now: 3},
		reviewBatches: [[1, 2], [3]],
		newIds: [4]
	});
	await page.goto('/review?mode=due');
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await page.getByRole('button', {name: '认识', exact: true}).click();
	await expect(page.getByRole('heading', {name: 'subtle', exact: true})).toBeVisible();
	await page.getByRole('button', {name: '认识', exact: true}).click();
	await expect(page.getByRole('heading', {name: '本轮已完成'})).toBeVisible();
	await expect(page.getByText('统计显示仍有 3 个到期')).toBeVisible();
	await page.getByRole('button', {name: '继续获取到期词'}).click();
	await expect(page.getByRole('heading', {name: 'resilient', exact: true})).toBeVisible();
	const names = (await calls(page)).map((call) => call.command);
	expect(names.filter((name) => name === 'review_group')).toHaveLength(2);
	expect(names).not.toContain('study_group');
});

test('due mode survives reload and history changes without mixing new words', async ({page}) => {
	await boot(page, {stats: emptyStats, reviewBatches: [[1]], newIds: [4]});
	await page.goto('/review?mode=due');
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await page.reload();
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await page.evaluate(() => ((window as any).__calls = []));
	await page.goto('/review?mode=new');
	await expect(page.getByRole('heading', {name: 'profound', exact: true})).toBeVisible();
	const learned = (await calls(page)).map((call) => call.command);
	expect(learned).toContain('study_group');
	expect(learned).not.toContain('review_group');
	expect(learned).not.toContain('study_grade');
	await page.evaluate(() => ((window as any).__calls = []));
	await page.goBack();
	await expect(page).toHaveURL(/mode=due/);
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	const returned = (await calls(page)).map((call) => call.command);
	expect(returned).toContain('review_group');
	expect(returned).not.toContain('study_group');
	expect(returned).not.toContain('study_grade');
});

test('lookup hint uses the saved record and does not invent a review time', async ({page}) => {
	await boot(page, {
		stats: emptyStats,
		lookups: {
			meticulous: {visit_count: 2, status: 'learning', next_review_at: '2026-12-01T00:00:00.000Z'},
			subtle: {visit_count: 1, status: 'learning', next_review_at: null}
		},
		library: [
			{id: 1, word: 'meticulous', display: 'meticulous', phonetic: null, definition: '一丝不苟的', status: 'learning', bookmarked: false, note: '', visit_count: 2, next_review_at: '2026-12-01T00:00:00.000Z'},
			{id: 2, word: 'subtle', display: 'subtle', phonetic: null, definition: '微妙的', status: 'learning', bookmarked: false, note: '', visit_count: 1, next_review_at: null}
		]
	});
	await page.goto('/?word=meticulous');
	await expect(page.getByText('查词记录已保存')).toBeVisible();
	await expect(page.getByText('已记录查词 2 次')).toBeVisible();
	await expect(page.getByText('学习状态：学习中')).toBeVisible();
	await expect(page.getByText(/已安排复习/)).toBeVisible();
	expect((await calls(page)).filter((call) => call.command === 'lookup_word')).toHaveLength(1);
	expect((await calls(page)).some((call) => call.command === 'library_words')).toBe(true);
	await page.screenshot({path: `${evidence}/lookup-saved.png`, fullPage: true, animations: 'disabled'});
	await page.goto('/?word=subtle');
	await expect(page.getByText('查词记录已保存')).toBeVisible();
	await expect(page.getByText('词库记录里没有下次复习时间')).toBeVisible();
	await expect(page.getByText('已安排复习')).toHaveCount(0);
	expect((await calls(page)).filter((call) => call.command === 'lookup_word').map((call) => call.args.word)).toEqual(['subtle']);
});

test('repeat lookup and status refresh keep an unsaved note', async ({page}) => {
	await boot(page, {
		stats: {...emptyStats, encountered: 1, learning: 1},
		lookups: {meticulous: {visit_count: 1, status: 'learning'}},
		library: [{id: 1, word: 'meticulous', display: 'meticulous', phonetic: null, definition: '一丝不苟的', status: 'learning', bookmarked: false, note: '', visit_count: 1, next_review_at: null}]
	});
	await page.goto('/?word=meticulous');
	const notes = page.getByLabel('Word notes');
	await expect(notes).toBeEnabled();
	await notes.fill('未保存的笔记');
	await page.getByRole('combobox', {name: 'Search for a word'}).press('Enter');
	await expect(notes).toHaveValue('未保存的笔记');
	await page.evaluate(() => {
		const state = JSON.parse(localStorage.getItem('test.home.state') ?? '{}');
		state.library[0].status = 'familiar';
		localStorage.setItem('test.home.state', JSON.stringify(state));
	});
	await page.getByRole('button', {name: '刷新学习状态'}).click();
	await expect(notes).toHaveValue('未保存的笔记');
	await expect(page.getByText('学习状态：熟悉')).toBeVisible();
	await expect.poll(async () => (await calls(page)).filter((call) => call.command === 'lookup_word').length).toBe(1);
	expect((await calls(page)).some((call) => call.command === 'stats_summary')).toBe(true);
});

test('new-word entry filters due cards from the shared study API', async ({page}) => {
	await boot(page, {stats: emptyStats, studyDueIds: [1], newIds: [4]});
	await page.goto('/review?mode=new');
	await expect(page.getByRole('heading', {name: 'profound', exact: true})).toBeVisible();
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toHaveCount(0);
	const request = (await calls(page)).find((item) => item.command === 'study_group');
	expect(request?.args.priority).toBe('new_first');
});

test('late grade response cannot advance a newly selected learning session', async ({page}) => {
	await boot(page, {stats: emptyStats, reviewBatches: [[1]], newIds: [3, 4], gradeSlow: true});
	await page.goto('/review?mode=due');
	await expect(page.getByRole('heading', {name: 'meticulous', exact: true})).toBeVisible();
	await page.getByRole('button', {name: '认识', exact: true}).click();
	await expect.poll(async () => (await calls(page)).filter((item) => item.command === 'study_grade').length).toBe(1);
	await page.evaluate(() => {
		const link = document.createElement('a');
		link.href = '/review?mode=new'; link.textContent = '切换测试模式'; document.querySelector('main')?.prepend(link);
	});
	await page.getByRole('link', {name: '切换测试模式'}).click();
	await expect(page.getByRole('heading', {name: 'resilient', exact: true})).toBeVisible();
	await page.evaluate(() => localStorage.setItem('test.grade.release', '1'));
	await page.waitForFunction(() => (window as any).__gradeReturned === true);
	await expect(page.getByRole('button', {name: '认识', exact: true})).toBeEnabled();
	await expect(page.getByRole('heading', {name: 'resilient', exact: true})).toBeVisible();
});
