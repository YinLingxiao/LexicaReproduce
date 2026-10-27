import seed from '../../resources/seed.json' with {type: 'json'};
import type {Page} from '@playwright/test';

const FORM_LABELS: Record<string, string> = {
	plural: 'Plural', past: 'Past', past_participle: 'Past participle', third_person: '3rd person singular',
	ing: 'Present participle', comparative: 'Comparative', superlative: 'Superlative',
	noun: 'Noun', verb: 'Verb', adjective: 'Adjective', adverb: 'Adverb'
};
const LOOKALIKES: Record<string, {word: string; pos: string | null; gloss: string}[]> = {
	surgery: [
		{word: 'sugary', pos: 'adj.', gloss: '含糖的，过于甜蜜的'},
		{word: 'surely', pos: 'adv.', gloss: '肯定地，不会吧，必定，当然'},
		{word: 'surety', pos: 'n.', gloss: '保证金，担保人'}
	]
};

function formsOf(raw: unknown) {
	if (!raw || typeof raw !== 'object') return [];
	return Object.entries(raw as Record<string, unknown>)
		.filter(([, form]) => typeof form === 'string' && form.trim().length > 0)
		.map(([label, form]) => ({label: FORM_LABELS[label] ?? label, form: String(form)}));
}
function derivativesOf(word: string, family: string[]) {
	const out: {word: string; pos: string | null; gloss: string}[] = [];
	for (const item of family) {
		const inline = /^(\S+)\s+((?:n|v|vt|vi|adj|adv|prep|conj|pron)\.)\s*(.*)$/i.exec(item.trim());
		if (inline) out.push({word: inline[1], pos: inline[2], gloss: inline[3].trim() || '派生词'});
		else if (item.trim() && item.trim() !== word) out.push({word: item.trim(), pos: null, gloss: '派生词'});
	}
	return out;
}

type Sense = {id: number; pos: string | null; english_definition: string; chinese_definition: string | null; level: string | null; level_source: string | null; examples: {id: number; text: string; translation: string | null}[]};
export type FixtureEntry = {
	id: number; word: string; display: string; phonetic: string | null; frequency_rank: number | null; frequency_source: string | null;
	senses: Sense[]; collocations: {text: string; gloss: string | null}[]; synonyms: string[]; antonyms: string[];
	word_family: string[]; tags: string[]; collins: number | null; oxford: boolean; mnemonic: string | null;
	roots: {part: string; meaning: string}[]; derivatives: {word: string; pos: string | null; gloss: string}[];
	synonyms_detailed: {word: string; pos: string | null; gloss: string}[];
	antonyms_detailed: {word: string; pos: string | null; gloss: string}[];
	lookalikes: {word: string; pos: string | null; gloss: string}[];
	forms: {label: string; form: string}[]; exam_example: {text: string; translation: string | null} | null;
};

export const entries: FixtureEntry[] = (seed.words as any[]).map((w, i) => {
	const senses: Sense[] = w.senses.map((s: any, j: number) => ({
		id: i * 10 + j, pos: s.pos ?? null, english_definition: s.english, chinese_definition: s.chinese ?? null, level: s.level ?? null,
		level_source: s.level ? 'seed' : null,
		examples: (s.examples ?? []).map((ex: any, k: number) => ({id: i * 100 + j * 10 + k, text: ex.text, translation: ex.translation ?? null}))
	}));
	const forms = formsOf(w.forms);
	const pos = senses[0]?.pos ?? null;
	const gloss = senses[0]?.chinese_definition ?? '';
	return {
		id: i + 1, word: w.word, display: w.word, phonetic: w.phonetic ?? null, frequency_rank: w.frequency_rank ?? null,
		frequency_source: w.frequency_rank === null || w.frequency_rank === undefined ? null : 'seed_approx', senses,
		collocations: w.collocations ?? [], synonyms: w.synonyms ?? [], antonyms: w.antonyms ?? [], word_family: w.word_family ?? [],
		tags: w.tags ?? [], collins: w.collins ?? null, oxford: !!w.oxford, mnemonic: w.mnemonic ?? null, roots: w.roots ?? [],
		derivatives: derivativesOf(w.word, w.word_family ?? []),
		synonyms_detailed: (w.synonyms ?? []).map((word: string) => ({word, pos, gloss: gloss || '近义词'})),
		antonyms_detailed: (w.antonyms ?? []).map((word: string) => ({word, pos, gloss: '反义词'})),
		lookalikes: LOOKALIKES[w.word] ?? [], forms,
		exam_example: senses[0]?.examples[0] ? {text: senses[0].examples[0].text, translation: senses[0].examples[0].translation} : null
	};
});

const EDGE_TEMPLATE = entries[0];
function edgeEntry(
	id: number,
	word: string,
	patch: Partial<FixtureEntry> & {sense?: Partial<Sense>}
): FixtureEntry {
	const sense: Sense = {
		id: id * 10,
		pos: null,
		english_definition: '',
		chinese_definition: null,
		level: null,
		level_source: null,
		examples: [],
		...patch.sense
	};
	const entryPatch = {...patch};
	delete entryPatch.sense;
	return {
		...EDGE_TEMPLATE,
		id,
		word,
		display: word,
		phonetic: null,
		frequency_rank: null,
		frequency_source: null,
		senses: [sense],
		collocations: [],
		synonyms: [],
		antonyms: [],
		word_family: [],
		derivatives: [],
		synonyms_detailed: [],
		antonyms_detailed: [],
		lookalikes: [],
		forms: [],
		exam_example: null,
		tags: [],
		collins: null,
		oxford: false,
		mnemonic: null,
		roots: [],
		...entryPatch
	};
}

export const edgeEntries: FixtureEntry[] = [
	edgeEntry(950, 'inevitable hour', {
		sense: {chinese_definition: '死期', level: 'R', level_source: 'legacy_unverified'}
	}),
	edgeEntry(951, 'staronly', {
		collins: 4,
		sense: {chinese_definition: '只有柯林斯星级的词'}
	}),
	edgeEntry(952, 'rankedword', {
		frequency_rank: 1234,
		frequency_source: 'ecdict.frq',
		sense: {chinese_definition: '有排名的词', level: 'B2', level_source: 'legacy_unverified'}
	}),
	edgeEntry(953, 'badlevel', {
		frequency_rank: 5000,
		frequency_source: 'ecdict.bnc',
		sense: {chinese_definition: '非法等级的词', level: 'G9', level_source: 'legacy_unverified'}
	}),
	edgeEntry(954, "don't-stop, 42 apples!", {
		sense: {chinese_definition: '标点与数字测试'}
	}),
	edgeEntry(955, 'bnconly', {
		frequency_rank: 8765,
		frequency_source: 'ecdict.bnc',
		sense: {chinese_definition: '只有 BNC 排名的词'}
	})
];

export function answerFor(prompt: string) {
	for (const entry of entries) for (const sense of entry.senses)
		if (sense.chinese_definition === prompt || sense.english_definition === prompt) return entry.word;
	throw new Error(`No fixture word for prompt: ${prompt}`);
}

const BOOKS = [
	{id: 'cet4', name: 'CET-4 四级核心词', total_words: 3849, learned_words: 826},
	{id: 'cet6', name: 'CET-6 六级核心词', total_words: 2540, learned_words: 418},
	{id: 'ky', name: '考研大纲词', total_words: 5500, learned_words: 1204},
	{id: 'custom', name: '我的生词本', total_words: 128, learned_words: 64},
	{id: 'encountered', name: '查词本', total_words: 96, learned_words: 96}
];

export async function installContractFixture(page: Page) {
	await page.addInitScript((packed) => {
		const entries = packed.entries as any[];
		const edgeEntries = packed.edgeEntries as any[];
		const books = packed.books as any[];
		const edgeById = new Map(edgeEntries.map((entry: any) => [entry.id, entry]));
		const entryById = (id: number) => edgeById.get(id) ?? entries[id - 1];
		const entryByWord = (word: string) =>
			edgeEntries.find((entry: any) => entry.word === word) ??
			entries.find((entry: any) => entry.word === word);
		const win = window as any;
		win.__calls = [];
		win.__uttered = [];
		const origStart = document.startViewTransition?.bind(document);
		win.__vt = [];
		if (origStart) document.startViewTransition = ((cb: Parameters<Document['startViewTransition']>[0]) => {
			win.__vt.push(document.documentElement.dataset.revealing ?? null);
			return origStart(cb);
		}) as Document['startViewTransition'];
		try {
			const synth = window.speechSynthesis;
			const speak = synth.speak.bind(synth);
			synth.speak = (utterance: SpeechSynthesisUtterance) => {
				win.__uttered.push({text: utterance.text, lang: utterance.lang, rate: utterance.rate});
				try { speak(utterance); } catch { utterance.onend?.(new SpeechSynthesisEvent('end', {utterance})); }
			};
		} catch { /* headless hosts can omit a writable speech engine */ }
		try {
			Object.defineProperty(navigator, 'clipboard', {configurable: true, value: {
				writeText: async (text: string) => { win.__copied = text; },
				readText: async () => String(win.__copied ?? '')
			}});
		} catch { /* clipboard may already be fixed */ }
		const flag = (key: string) => localStorage.getItem(key) === '1';
		const wait = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
		const fail = (code: string, message: string) => { const error = new Error(message) as Error & {code: string}; error.code = code; throw error; };
		const blank = (text: string, word: string) => text.replace(new RegExp(`\\b${word}\\b`, 'gi'), '______');
		const studyItem = (entry: any) => {
			const sense = entry.senses.find((item: any) => item.chinese_definition?.trim()) ?? entry.senses[0];
			return {word_id: entry.id, prompt: sense?.chinese_definition ?? sense?.english_definition ?? entry.word, kind: sense?.chinese_definition ? 'definition_zh' : 'definition', source: 'dictionary', sense_id: sense?.id ?? null, example_id: null};
		};
		const hintFor = (wordId: number, hintNo: number) => {
			const entry = entries[wordId - 1];
			if (entry?.word === 'meticulous') return [{kind: 'mask', text: 'm_________'}, {kind: 'english', text: 'very careful about details'}, {kind: 'chinese', text: '一丝不苟的'}][hintNo - 1];
			const sense = entry?.senses?.[0];
			const mask = `${entry?.word?.[0] ?? '?'}_________`;
			return [{kind: 'mask', text: mask}, {kind: 'english', text: sense?.english_definition ?? ''}, {kind: 'chinese', text: (sense?.chinese_definition ?? '').split('；')[0]}][hintNo - 1];
		};
		const handle = async (command: string, args: any = {}) => {
			const notes = JSON.parse(localStorage.getItem('test.notes') ?? '{}');
			const visited = JSON.parse(localStorage.getItem('test.visited') ?? '[]');
			const save = (key: string, value: unknown) => localStorage.setItem(key, JSON.stringify(value));
			if (win.__failNext?.command === command) { const message = win.__failNext.message; win.__failNext = null; fail('TEMPORARY', message); }
			switch (command) {
				case 'system_capabilities': return {automatic_dictionary_setup: false, commands: []};
				case 'app_info':
					if (flag('test.app.fail')) fail('INTERNAL_ERROR', 'app_info failed');
					return {word_count: entries.length, schema_version: 6, fts_ok: true, dictionary_ready: false};
				case 'stats_summary':
					if (flag('test.stats.fail')) fail('NOT_IMPLEMENTED', 'stats_summary is scheduled for the next development phase.');
					return {encountered: visited.length, learning: visited.length, familiar: 0, stable: 0, ignored: 0, due_now: 2, high_priority: 0};
				case 'recent_words':
					return visited.slice(-6).reverse().filter((id: number) => entryById(id)).map((id: number) => ({word: entryById(id).word, display: entryById(id).display, phonetic: entryById(id).phonetic, visited_at: '2026-10-26T08:00:00.000Z', comprehension_level: 'unknown'}));
				case 'search_suggest': {
					await wait(args.query === 'm' ? 450 : args.query === 'slow' ? 700 : 25);
					if (flag('test.suggest.fail')) fail('UNAVAILABLE', 'Suggestions unavailable');
					return entries.filter((entry) => entry.word.startsWith(String(args.query ?? ''))).slice(0, 8).map((entry) => ({word: entry.word, display: entry.display, phonetic: entry.phonetic, frequency_rank: entry.frequency_rank}));
				}
				case 'lookup_word': {
					if (flag('test.lookup.fail')) fail('NOT_IMPLEMENTED', 'lookup_word failed for this attempt');
					if (args.word === 'meticulous' && flag('test.lookup.slow')) await wait(700);
					if (args.word === 'longentry') return {outcome: {type: 'hit', entry: {...entries[0], id: 900, frequency_rank: null, frequency_source: null, word: 'longentry', display: 'pneumonoultramicroscopicsilicovolcanoconiosis-extra', phonetic: '/ˌnjuːmənoʊˌʌltrəˌmaɪkrəˈskɒpɪk/', senses: [{id: 1, pos: 'n.', english_definition: 'A '.repeat(80) + 'very long definition that must wrap inside the column.', chinese_definition: '超长中文释义'.repeat(30), level: 'C2', level_source: 'legacy_unverified', examples: [{id: 1, text: 'This longentry sentence is intentionally long enough to wrap. '.repeat(6), translation: '超长例句译文'.repeat(12)}]}], synonyms: ['alpha', 'beta'], antonyms: [], word_family: [], collocations: [], roots: [], derivatives: [], synonyms_detailed: [], antonyms_detailed: [], lookalikes: [], forms: [], exam_example: null, tags: [], mnemonic: null, collins: null, oxford: false}}, encounter_id: 900, memory: {status: 'learning', visit_count: 1, high_priority: false}, comprehension_level: 'unknown'};
					if (args.word === 'sparse') return {outcome: {type: 'hit', entry: {...entries[0], id: 901, frequency_rank: null, frequency_source: null, word: 'sparse', display: 'sparse', phonetic: null, senses: [{id: 1, pos: null, english_definition: 'Only an English gloss.', chinese_definition: null, level: null, level_source: null, examples: []}], synonyms: [], antonyms: [], word_family: [], collocations: [], roots: [], derivatives: [], synonyms_detailed: [], antonyms_detailed: [], lookalikes: [], forms: [], exam_example: null, tags: [], mnemonic: null, collins: null, oxford: false}}, encounter_id: null, memory: null, comprehension_level: null};
					let entry = entryByWord(args.word);
					if (entry && flag('test.lookup.noexamples')) entry = {...entry, senses: entry.senses.map((sense: any) => ({...sense, examples: []})), exam_example: null};
					if (entry && !visited.includes(entry.id)) { visited.push(entry.id); save('test.visited', visited); }
					return {outcome: entry ? {type: 'hit', entry} : {type: 'miss', suggestions: []}, encounter_id: entry?.id ?? null, memory: entry ? {status: 'learning', visit_count: 1, high_priority: false} : null, comprehension_level: entry ? 'unknown' : null};
				}
				case 'word_note': return notes[args.wordId] ?? {bookmarked: false, note: '', ignored: false};
				case 'save_word_note':
					if (win.__saveFails) fail('DB_ERROR', '测试：笔记保存失败，请重试');
					await wait(80);
					notes[args.wordId] = {...notes[args.wordId], bookmarked: args.bookmarked, note: args.note, ignored: notes[args.wordId]?.ignored ?? false};
					save('test.notes', notes);
					return null;
				case 'set_word_ignored':
					notes[args.wordId] = {...notes[args.wordId], bookmarked: notes[args.wordId]?.bookmarked ?? false, note: notes[args.wordId]?.note ?? '', ignored: args.ignored};
					save('test.notes', notes);
					return null;
				case 'set_comprehension': return null;
				case 'library_words': {
					if (flag('test.library.fail')) fail('NOT_IMPLEMENTED', 'library_words is scheduled for the next development phase.');
					if (args.query === 'ela') await wait(600);
					let words = entries.filter((entry) => visited.includes(entry.id) || flag('test.library.seeded')).map((entry) => ({
						id: entry.id, word: entry.word, display: entry.display, phonetic: entry.phonetic, definition: entry.senses[0].chinese_definition ?? entry.senses[0].english_definition,
						status: notes[entry.id]?.ignored ? 'ignored' : 'learning', bookmarked: notes[entry.id]?.bookmarked ?? false, note: notes[entry.id]?.note ?? '', visit_count: 1, next_review_at: null
					}));
					if (flag('test.library.many')) words = entries.slice(0, 30).map((entry) => ({id: entry.id, word: entry.word, display: entry.display, phonetic: entry.phonetic, definition: entry.senses[0].english_definition, status: 'learning', bookmarked: false, note: '', visit_count: 1, next_review_at: null}));
					words = words.filter((word) => (args.filter === 'all' || (args.filter === 'bookmarked' && word.bookmarked) || args.filter === word.status) && (!args.query || word.word.includes(args.query) || word.note.includes(args.query)));
					return {total: words.length, words: words.slice(args.offset, args.offset + 24)};
				}
				case 'activity_days':
					return Array.from({length: 14}, (_, index) => ({date: `2026-10-${String(index + 10).padStart(2, '0')}`, lookups: flag('test.stats.activity') && index === 13 ? 2 : 0, reviews: 0}));
				case 'fading_words':
					return flag('test.stats.fading') ? [{word: 'meticulous', display: 'meticulous', high_priority: true, next_review_at: '2026-10-20T00:00:00.000Z'}] : [];
				case 'study_group': {
					if (!flag('test.study.on')) fail('NOT_IMPLEMENTED', 'study_group is scheduled for the next development phase.');
					if (flag('test.study.fail')) fail('NOT_IMPLEMENTED', 'study_group is scheduled for the next development phase.');
					if (flag('test.study.slow')) await wait(800);
					if (flag('test.study.empty')) return {cards: [], due_count: 0, new_count: 0, today_learned_new: 0};
					const raw = localStorage.getItem('test.study.ids');
					const ids: number[] = raw ? JSON.parse(raw) : [];
					const surgery = entries.find((entry) => entry.word === 'surgery');
					const pool = ids.length ? ids.map((id) => entryById(id)).filter(Boolean) : [surgery, ...entries.filter((entry) => entry.word !== 'surgery')].filter(Boolean).slice(0, Math.max(1, Math.min(entries.length, Number(args.dailyNewLimit) || 20)));
					const cards = pool.map((entry, index) => ({entry, item: studyItem(entry), is_new: index === 0, bookmarked: false, in_custom_wordbook: false}));
					return {cards, due_count: cards.filter((card) => !card.is_new).length, new_count: cards.filter((card) => card.is_new).length, today_learned_new: 0};
				}
				case 'study_grade':
					if (flag('test.grade.slow')) await wait(400);
					if (flag('test.grade.fail')) fail('DB_ERROR', '评分没有写入，请重试');
					return {correct: args.grade !== 'forgot', quality: args.grade === 'forgot' ? 'forgotten' : args.grade === 'hazy' ? 'hard' : 'good', answer: '', hints_used: 0, memory: null};
				case 'wordbooks_list':
					if (flag('test.books.fail')) fail('NOT_IMPLEMENTED', 'wordbooks_list is scheduled for the next development phase.');
					return books;
				case 'custom_wordbook_toggle': return null;
				case 'ai_enrich_words': return Array.isArray(args.wordIds) ? args.wordIds.length : 0;
				case 'review_group': {
					if (flag('test.review.fail') || flag('test.study.empty')) {
						if (flag('test.review.fail')) fail('NOT_IMPLEMENTED', 'review_group is scheduled for the next development phase.');
						return [];
					}
					const raw = localStorage.getItem('test.review.ids');
					const ids: number[] = raw ? JSON.parse(raw) : [1, 3];
					const defOnly = new Set(JSON.parse(localStorage.getItem('test.review.defonly') ?? '[]'));
					const out = [];
					for (const id of ids.slice(0, 10)) {
						const original = entryById(id);
						if (!original) continue;
						const entry = defOnly.has(id) ? {...original, senses: original.senses.map((sense: any) => ({...sense, examples: []}))} : original;
						const sense = entry.senses.find((item: any) => item.chinese_definition?.trim()) ?? entry.senses[0];
						out.push({entry, item: {word_id: entry.id, prompt: sense.chinese_definition ?? sense.english_definition, kind: sense.chinese_definition ? 'definition_zh' : 'definition', source: 'dictionary', sense_id: sense.id, example_id: null}});
					}
					return out;
				}
				case 'start_review_item':
					await wait(40);
					return args.wordId;
				case 'request_hint':
				case 'practice_hint':
					await wait(40);
					return hintFor(command === 'practice_hint' ? args.wordId : args.reviewId, args.hintNo);
				case 'submit_review': {
					await wait(40);
					const entry = entryById(args.reviewId);
					const correct = args.answer === entry.word;
					return {correct, quality: correct ? 'excellent' : 'forgotten', answer: entry.word, hints_used: 0, memory: {word_id: entry.id, strength_days: 1, difficulty: 5, review_count: 1, lapse_count: correct ? 0 : 1, visit_count: 1, weak_streak: 0, high_priority: !correct, first_visit_at: '2026-10-26T00:00:00.000Z', last_visit_at: null, last_review_at: '2026-10-26T00:00:00.000Z', last_reinforcement_at: null, next_review_at: '2026-10-27T00:00:00.000Z'}};
				}
				case 'practice_check':
					return String(args.answer ?? '').trim().toLowerCase() === entryById(args.wordId).word;
				case 'ai_config_get':
					return {enabled: flag('test.ai.enabled'), base_url: localStorage.getItem('test.ai.base_url') ?? 'https://api.deepseek.com/v1', model: localStorage.getItem('test.ai.model') ?? 'deepseek-chat', has_key: flag('test.ai.has_key')};
				case 'ai_config_save': {
					localStorage.setItem('test.ai.enabled', args.enabled ? '1' : '0');
					localStorage.setItem('test.ai.base_url', args.baseUrl);
					localStorage.setItem('test.ai.model', args.model);
					if (args.key === '') localStorage.removeItem('test.ai.has_key');
					else if (args.key) localStorage.setItem('test.ai.has_key', '1');
					return {enabled: args.enabled, base_url: args.baseUrl, model: args.model, has_key: flag('test.ai.has_key')};
				}
				case 'ai_test_connection':
					if (localStorage.getItem('test.ai.mode') === 'fail') fail('NETWORK', 'mock: 401 Unauthorized');
					return null;
				case 'ai_generate_examples': {
					const items: {word_id: number; sense_id: number | null}[] = args.items ?? [];
					if (!flag('test.ai.enabled')) return [];
					const cache = JSON.parse(localStorage.getItem('test.ai.cache') ?? '{}');
					const missing = items.filter((item) => cache[item.word_id] === undefined);
					if (missing.length) {
						sessionStorage.setItem('test.aiNetwork', String(Number(sessionStorage.getItem('test.aiNetwork') ?? 0) + 1));
						const mode = localStorage.getItem('test.ai.mode') ?? 'success';
						if (mode === 'fail') fail('NETWORK', 'mock: connection refused');
						if (mode === 'hang') return new Promise(() => {});
						if (mode !== 'invalid') {
							for (const item of missing) cache[item.word_id] = blank(`The result was clearly ${entryById(item.word_id).word} in the final data.`, entryById(item.word_id).word);
							localStorage.setItem('test.ai.cache', JSON.stringify(cache));
						}
					}
					return items.filter((item) => cache[item.word_id]).map((item) => ({word_id: item.word_id, sense_id: item.sense_id, sentence: cache[item.word_id]}));
				}
				case 'ensure_dictionary':
					if (win.__ensureLeft > 0) { win.__ensureLeft -= 1; fail('NOT_IMPLEMENTED', 'ensure_dictionary is scheduled for the next development phase.'); }
					return {ready: false, word_count: entries.length, imported: 0, skipped: true};
				case 'import_ecdict':
					fail('NOT_IMPLEMENTED', 'import_ecdict is scheduled for the next development phase.');
				default:
					fail('NOT_IMPLEMENTED', `${command} is scheduled for the next development phase.`);
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
	}, {entries, edgeEntries, books: BOOKS});
}

export function calls(page: Page) {
	return page.evaluate(() => (window as any).__calls as {command: string; args: any}[]);
}
