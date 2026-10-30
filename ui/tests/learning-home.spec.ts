import { expect, test, type Page } from '@playwright/test';

type Entry = ReturnType<typeof entry>;
function entry(
	id: number,
	word: string,
	gloss: string,
): {
	id: number;
	word: string;
	display: string;
	phonetic: null;
	frequency_rank: null;
	frequency_source: null;
	senses: {
		id: number;
		pos: string;
		english_definition: string;
		chinese_definition: string;
		level: null;
		level_source: null;
		examples: { id: number; text: string; translation: string }[];
	}[];
	collocations: [];
	synonyms: [];
	antonyms: [];
	word_family: [];
	tags: [];
	collins: null;
	oxford: false;
	mnemonic: null;
	roots: [];
	derivatives: [];
	synonyms_detailed: [];
	antonyms_detailed: [];
	lookalikes: [];
	forms: [];
	exam_example: null;
} {
	return {
		id,
		word,
		display: word,
		phonetic: null,
		frequency_rank: null,
		frequency_source: null,
		senses: [
			{
				id: id * 10,
				pos: 'adj.',
				english_definition: `Definition of ${word}`,
				chinese_definition: gloss,
				level: null,
				level_source: null,
				examples: [
					{ id: id * 100, text: `${word} sits in a short example.`, translation: 'Examples' },
				],
			},
		],
		collocations: [],
		synonyms: [],
		antonyms: [],
		word_family: [],
		tags: [],
		collins: null,
		oxford: false,
		mnemonic: null,
		roots: [],
		derivatives: [],
		synonyms_detailed: [],
		antonyms_detailed: [],
		lookalikes: [],
		forms: [],
		exam_example: null,
	};
}

const entries: Entry[] = [
	entry(1, 'meticulous', '一丝不苟的'),
	entry(2, 'subtle', '微妙的'),
	entry(3, 'resilient', '有韧性的'),
	entry(4, 'profound', '深刻的'),
];

const emptyStats = {
	encountered: 0,
	learning: 0,
	familiar: 0,
	stable: 0,
	ignored: 0,
	due_now: 0,
	high_priority: 0,
};

async function install(page: Page) {
	await page.addInitScript(
		(packed) => {
			const words = packed.entries as any[];
			const byId = (id: number) => words.find((item) => item.id === id);
			const byWord = (word: string) => words.find((item) => item.word === word);
			const win = window as any;
			win.__calls = [];
			const state = () => JSON.parse(localStorage.getItem('test.home.state') ?? '{}');
			const fail = (message: string) => {
				const error = new Error(message) as Error & { code: string };
				error.code = 'TEMPORARY';
				throw error;
			};
			const card = (id: number, isNew: boolean) => {
				const item = byId(id);
				const sense = item.senses[0];
				return {
					entry: item,
					item: {
						word_id: item.id,
						prompt: sense.chinese_definition,
						kind: 'definition_zh',
						source: 'dictionary',
						sense_id: sense.id,
						example_id: null,
					},
					is_new: isNew,
					bookmarked: false,
					in_custom_wordbook: false,
				};
			};
			const handle = async (command: string, args: any = {}) => {
				const current = state();
				if (current.failCommand === command) fail(`${command} failed`);
				switch (command) {
					case 'system_capabilities':
						return { automatic_dictionary_setup: false, commands: [] };
					case 'app_info':
						if (current.appFail) fail('app_info failed');
						return {
							word_count: current.wordCount ?? 55,
							schema_version: 6,
							fts_ok: true,
							dictionary_ready: false,
						};
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
						return words
							.filter((item) => item.word.startsWith(String(args.query ?? '')))
							.slice(0, 8)
							.map((item) => ({
								word: item.word,
								display: item.display,
								phonetic: item.phonetic,
								frequency_rank: null,
							}));
					case 'lookup_word': {
						const found = byWord(args.word);
						if (!found)
							return {
								outcome: { type: 'miss', suggestions: [] },
								encounter_id: null,
								memory: null,
								comprehension_level: null,
							};
						const spec = current.lookups?.[args.word] ?? {};
						const encounter = spec.encounter_id === null ? null : (spec.encounter_id ?? found.id);
						return {
							outcome: { type: 'hit', entry: found },
							encounter_id: encounter,
							memory:
								encounter === null
									? null
									: {
											status: spec.status ?? 'learning',
											visit_count: spec.visit_count ?? 1,
											high_priority: false,
										},
							comprehension_level: 'unknown',
						};
					}
					case 'library_words': {
						if (current.libraryFail) fail('library_words failed');
						const rows = (current.library ?? []).filter(
							(item: any) => !args.query || String(item.word).includes(args.query),
						);
						return { total: rows.length, words: rows };
					}
					case 'word_note':
						return { bookmarked: false, note: '', ignored: false };
					case 'ai_config_get':
						return { enabled: false, base_url: '', model: '', has_key: false };
					case 'ai_generate_examples':
						return [];
					case 'ai_enrich_words':
						return 0;
					case 'study_group': {
						if (current.studyFail) fail('study_group failed');
						const ids: number[] = current.newIds ?? [4];
						const cards = [
							...(current.studyDueIds ?? []).map((id: number) => card(id, false)),
							...ids.map((id) => card(id, true)),
						];
						return { cards, due_count: 0, new_count: cards.length, today_learned_new: 0 };
					}
					case 'study_grade':
						if (current.gradeSlow) {
							await new Promise<void>((resolve) => {
								const timer = setInterval(() => {
									if (localStorage.getItem('test.grade.release') === '1') {
										clearInterval(timer);
										resolve();
									}
								}, 20);
							});
						}
						win.__gradeReturned = true;
						return {
							correct: args.grade !== 'forgot',
							quality: 'good',
							answer: '',
							hints_used: 0,
							memory: null,
						};
					case 'review_group': {
						const batches: number[][] = current.reviewBatches ?? [[1]];
						const n = Number(sessionStorage.getItem('review.n') ?? '0');
						sessionStorage.setItem('review.n', String(n + 1));
						const ids = batches[Math.min(n, batches.length - 1)] ?? [];
						return ids.map((id) => {
							const full = card(id, false);
							return { entry: full.entry, item: full.item };
						});
					}
					case 'start_review_item':
						return args.wordId;
					case 'submit_review':
						return {
							correct: true,
							quality: 'excellent',
							answer: args.answer,
							hints_used: 0,
							memory: null,
						};
					case 'request_hint':
						return { kind: 'mask', text: 'm_________' };
					case 'practice_check':
						return true;
					case 'practice_hint':
						return { kind: 'mask', text: 'm_________' };
					default:
						return null;
				}
			};
			const original = window.fetch.bind(window);
			window.fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
				const url =
					typeof input === 'string' ? input : input instanceof URL ? input.href : input.url;
				if (!url.includes('/rpc')) return original(input, init);
				const request = JSON.parse(String(init?.body ?? '{}'));
				win.__calls.push({ command: request.command, args: request.args });
				try {
					const result = await handle(request.command, request.args ?? {});
					return new Response(
						JSON.stringify({
							protocol_version: 1,
							request_id: request.request_id,
							ok: true,
							result,
						}),
						{ status: 200, headers: { 'Content-Type': 'application/json' } },
					);
				} catch (error) {
					const coded = error as { code?: string; message?: string };
					return new Response(
						JSON.stringify({
							protocol_version: 1,
							request_id: request.request_id,
							ok: false,
							error: {
								code: coded.code ?? 'INTERNAL_ERROR',
								message: coded.message ?? 'Command failed',
							},
						}),
						{ status: 200, headers: { 'Content-Type': 'application/json' } },
					);
				}
			};
		},
		{ entries, emptyStats },
	);
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
	return page.evaluate(() => (window as any).__calls as { command: string; args: any }[]);
}

test.beforeEach(async ({ page }) => {
	await install(page);
});

test('dictionary home starts with search in English, even when reviews are due', async ({
	page,
}) => {
	for (const state of [
		{ statsFail: true, recent: [], wordCount: 55 },
		{
			stats: { ...emptyStats, encountered: 4, due_now: 4 },
			recent: [
				{
					word: 'meticulous',
					display: 'meticulous',
					phonetic: null,
					visited_at: '2026-10-30T00:00:00.000Z',
				},
			],
		},
	]) {
		await boot(page, state);
		await expect(page.getByRole('combobox', { name: 'Search for a word' })).toBeVisible();
		await expect(page.getByRole('heading', { name: 'Recent searches' })).toBeVisible();
		await expect(page.locator('main h1')).toHaveCount(0);
		expect((await page.getByLabel('Dictionary search').boundingBox())!.y).toBeLessThan(150);
		expect(await page.locator('main').innerText()).not.toMatch(/\p{Script=Han}/u);
		expect((await calls(page)).some((item) => item.command === 'stats_summary')).toBe(false);
		await page.setViewportSize({ width: 480, height: 820 });
		expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
			true,
		);
		await page.setViewportSize({ width: 1180, height: 820 });
	}
});

test('recent-search failures do not block lookup and can be retried', async ({ page }) => {
	await boot(page, { recentFail: true });
	await expect(page.getByRole('combobox', { name: 'Search for a word' })).toBeEnabled();
	await expect(page.getByRole('alert')).toContainText('recent_words failed');
	await page.evaluate(() =>
		localStorage.setItem('test.home.state', JSON.stringify({ recent: [] })),
	);
	await page.getByRole('button', { name: 'Retry recent searches' }).click();
	await expect(page.getByText('Words you look up will appear here.')).toBeVisible();
});

test('lookup keeps dictionary content and unsaved notes without a learning banner', async ({
	page,
}) => {
	await boot(page, { stats: emptyStats });
	await page.goto('/?word=meticulous');
	const notes = page.getByLabel('Word notes');
	await expect(notes).toBeEnabled();
	await notes.fill('Keep this draft');
	await page.getByRole('combobox', { name: 'Search for a word' }).press('Enter');
	await expect(notes).toHaveValue('Keep this draft');
	expect((await calls(page)).filter((item) => item.command === 'lookup_word')).toHaveLength(1);
	expect((await calls(page)).some((item) => item.command === 'library_words')).toBe(false);
	await expect(page.locator('.hint')).toHaveCount(0);
	await expect(page.locator('.hero-card')).toBeVisible();
});
test('finishing one due group still loads the remaining due words', async ({ page }) => {
	await boot(page, {
		stats: { ...emptyStats, encountered: 3, learning: 3, due_now: 3 },
		reviewBatches: [[1, 2], [3]],
		newIds: [4],
	});
	await page.goto('/review?mode=due');
	await expect(page.getByRole('heading', { name: 'meticulous', exact: true })).toBeVisible();
	await page.getByRole('button', { name: 'Know', exact: true }).click();
	await expect(page.getByRole('heading', { name: 'subtle', exact: true })).toBeVisible();
	await page.getByRole('button', { name: 'Know', exact: true }).click();
	await expect(page.getByRole('heading', { name: 'Round complete' })).toBeVisible();
	await expect(page.getByText('3 words are still due.')).toBeVisible();
	await page.getByRole('button', { name: 'Get next due group' }).click();
	await expect(page.getByRole('heading', { name: 'resilient', exact: true })).toBeVisible();
	const names = (await calls(page)).map((call) => call.command);
	expect(names.filter((name) => name === 'review_group')).toHaveLength(2);
	expect(names).not.toContain('study_group');
});

test('due mode survives reload and history changes without mixing new words', async ({ page }) => {
	await boot(page, { stats: emptyStats, reviewBatches: [[1]], newIds: [4] });
	await page.goto('/review?mode=due');
	await expect(page.getByRole('heading', { name: 'meticulous', exact: true })).toBeVisible();
	await page.reload();
	await expect(page.getByRole('heading', { name: 'meticulous', exact: true })).toBeVisible();
	await page.evaluate(() => ((window as any).__calls = []));
	await page.goto('/review?mode=new');
	await expect(page.getByRole('heading', { name: 'profound', exact: true })).toBeVisible();
	const learned = (await calls(page)).map((call) => call.command);
	expect(learned).toContain('study_group');
	expect(learned).not.toContain('review_group');
	expect(learned).not.toContain('study_grade');
	await page.evaluate(() => ((window as any).__calls = []));
	await page.goBack();
	await expect(page).toHaveURL(/mode=due/);
	await expect(page.getByRole('heading', { name: 'meticulous', exact: true })).toBeVisible();
	const returned = (await calls(page)).map((call) => call.command);
	expect(returned).toContain('review_group');
	expect(returned).not.toContain('study_group');
	expect(returned).not.toContain('study_grade');
});

test('new-word entry filters due cards from the shared study API', async ({ page }) => {
	await boot(page, { stats: emptyStats, studyDueIds: [1], newIds: [4] });
	await page.goto('/review?mode=new');
	await expect(page.getByRole('heading', { name: 'profound', exact: true })).toBeVisible();
	await expect(page.getByRole('heading', { name: 'meticulous', exact: true })).toHaveCount(0);
	const request = (await calls(page)).find((item) => item.command === 'study_group');
	expect(request?.args.priority).toBe('new_first');
});

test('late grade response cannot advance a newly selected learning session', async ({ page }) => {
	await boot(page, { stats: emptyStats, reviewBatches: [[1]], newIds: [3, 4], gradeSlow: true });
	await page.goto('/review?mode=due');
	await expect(page.getByRole('heading', { name: 'meticulous', exact: true })).toBeVisible();
	await page.getByRole('button', { name: 'Know', exact: true }).click();
	await expect
		.poll(async () => (await calls(page)).filter((item) => item.command === 'study_grade').length)
		.toBe(1);
	await page.evaluate(() => {
		const link = document.createElement('a');
		link.href = '/review?mode=new';
		link.textContent = 'Switch test mode';
		document.querySelector('main')?.prepend(link);
	});
	await page.getByRole('link', { name: 'Switch test mode' }).click();
	await expect(page.getByRole('heading', { name: 'resilient', exact: true })).toBeVisible();
	await page.evaluate(() => localStorage.setItem('test.grade.release', '1'));
	await page.waitForFunction(() => (window as any).__gradeReturned === true);
	await expect(page.getByRole('button', { name: 'Know', exact: true })).toBeEnabled();
	await expect(page.getByRole('heading', { name: 'resilient', exact: true })).toBeVisible();
});
