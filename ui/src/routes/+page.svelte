<script lang="ts">
	import { onMount, untrack } from 'svelte';
	import { page } from '$app/state';
	import { goto } from '$app/navigation';
	import SearchBox from '$lib/components/SearchBox.svelte';
	import WordView from '$lib/components/WordView.svelte';
	import Icon from '$lib/components/Icon.svelte';
	import HomeLearningTask from '$lib/components/HomeLearningTask.svelte';
	import LookupRecordHint from '$lib/components/LookupRecordHint.svelte';
	import {
		lookupWord,
		recentWords,
		statsSummary,
		appInfo,
		libraryWords,
		relTime,
		dueLabel,
		errorMessage,
		type LookupWordResult,
		type RecentWord,
		type StatsSummary
	} from '$lib/api';

	type LoadStatus = 'loading' | 'ready' | 'error';
	type Schedule =
		| { kind: 'pending' }
		| { kind: 'unknown' }
		| { kind: 'scheduled'; when: string }
		| { kind: 'unscheduled' }
		| { kind: 'error' };

	const statusText: Record<string, string> = {
		learning: '学习中',
		familiar: '熟悉',
		stable: '稳定'
	};

	let result = $state<LookupWordResult | null>(null);
	let value = $state('');
	let recent = $state<RecentWord[]>([]);
	let stats = $state<StatsSummary | null>(null);
	let statsStatus = $state<LoadStatus>('loading');
	let statsError = $state('');
	let recentStatus = $state<LoadStatus>('loading');
	let recentError = $state('');
	let wordCount = $state<number | null>(null);
	let wordCountStatus = $state<LoadStatus>('loading');
	let error = $state('');
	let loading = $state(false);
	let request = 0;
	let schedule = $state<Schedule>({ kind: 'unknown' });
	let scheduleToken = 0;
	let refreshToken = 0;
	let recordMemory = $state<{ status: string; visit_count: number } | null>(null);
	const query = $derived(page.url.searchParams.get('word') ?? '');
	const hit = $derived(result?.outcome.type === 'hit' ? result.outcome.entry : null);
	const task = $derived.by(() => {
		if (statsStatus === 'loading') return { kind: 'loading' as const };
		if (statsStatus === 'error') return { kind: 'error' as const, message: statsError };
		if (!stats) return { kind: 'loading' as const };
		if (recentStatus === 'loading' && stats.encountered === 0 && stats.due_now === 0)
			return { kind: 'loading' as const };
		if (stats.due_now > 0) return { kind: 'due' as const, count: stats.due_now };
		if (stats.encountered === 0 && recentStatus === 'error')
			return { kind: 'error' as const, message: recentError };
		if (stats.encountered === 0 && recent.length === 0) return { kind: 'new' as const };
		return { kind: 'clear' as const };
	});

	async function refresh() {
		const token = ++refreshToken;
		const results = await Promise.allSettled([recentWords(8), statsSummary()]);
		if (token !== refreshToken) return;
		if (results[0].status === 'fulfilled') {
			recent = results[0].value;
			recentStatus = 'ready';
			recentError = '';
		} else {
			recentStatus = 'error';
			recentError = errorMessage(results[0].reason);
		}
		if (results[1].status === 'fulfilled') {
			stats = results[1].value;
			statsStatus = 'ready';
			statsError = '';
		} else {
			stats = null;
			statsStatus = 'error';
			statsError = errorMessage(results[1].reason);
		}
	}

	function retryHome() {
		statsStatus = 'loading';
		recentStatus = 'loading';
		stats = null;
		recent = [];
		statsError = '';
		recentError = '';
		void refresh();
	}

	async function readSchedule(wordId: number, word: string) {
		const token = ++scheduleToken;
		schedule = { kind: 'pending' };
		try {
			const rows = await libraryWords(word, 'all', 0);
			if (token !== scheduleToken) return;
			const row = rows.words.find((item) => item.id === wordId);
			if (!row) {
				schedule = { kind: 'unknown' };
				return;
			}
			recordMemory = { status: row.status, visit_count: row.visit_count };
			if (row.next_review_at) schedule = { kind: 'scheduled', when: dueLabel(row.next_review_at) };
			else schedule = { kind: 'unscheduled' };
		} catch {
			if (token !== scheduleToken) return;
			schedule = { kind: 'error' };
		}
	}

	async function load(word: string) {
		const token = ++request;
		++scheduleToken;
		loading = true;
		error = '';
		result = null;
		recordMemory = null;
		schedule = { kind: 'unknown' };
		value = word;
		try {
			const found = await lookupWord(word);
			if (token !== request) return;
			result = found;
			if (found.outcome.type === 'hit' && found.encounter_id !== null)
				void readSchedule(found.outcome.entry.id, found.outcome.entry.word);
		} catch (e) {
			if (token === request) error = errorMessage(e);
		} finally {
			if (token === request) {
				loading = false;
				void refresh();
			}
		}
	}

	function doLookup(word: string) {
		const q = word.trim();
		if (!q) return;
		if (q === query) {
			if (!result && !loading) void load(q);
			return;
		}
		void goto(`/?word=${encodeURIComponent(q)}`, { noScroll: true });
	}

	function focusSearch() {
		document.querySelector<HTMLInputElement>('input[aria-label="Search for a word"]')?.focus();
	}

	function refreshLearningStatus() {
		void refresh();
		if (hit && result?.encounter_id !== null) void readSchedule(hit.id, hit.word);
	}

	$effect(() => {
		const word = query;
		untrack(() => {
			if (word) void load(word);
			else {
				++request;
				++scheduleToken;
				result = null;
				error = '';
				loading = false;
				value = '';
				schedule = { kind: 'unknown' };
			}
		});
	});

	onMount(() => {
		void refresh();
		void appInfo()
			.then((info) => {
				wordCount = info.word_count;
				wordCountStatus = 'ready';
			})
			.catch(() => {
				wordCount = null;
				wordCountStatus = 'error';
			});
	});
</script>

<main class="page home">
	{#if query}
		<a href="/" class="back"><Icon name="back" size={15} />Search</a>
		<SearchBox oncommit={doLookup} bind:value />
		<div class="word-area" aria-busy={loading}>
			{#if loading}<div class="skeleton"></div>{:else if error}<div class="error" role="alert">
					{error}
				</div>
				<button onclick={() => load(query)}>Try again</button
				>{:else if result}
				<LookupRecordHint
					saved={result.encounter_id !== null && result.outcome.type === 'hit'}
					miss={result.outcome.type === 'miss'}
					visitCount={recordMemory?.visit_count ?? result.memory?.visit_count ?? null}
					statusText={statusText[recordMemory?.status ?? result.memory?.status ?? ''] ?? null}
					{schedule}
					onRefresh={refreshLearningStatus}
					onRetrySchedule={() => {
						if (hit) void readSchedule(hit.id, hit.word);
					}}
				/>
				{#key result}<WordView
						{result}
						{query}
						oncommit={doLookup}
						ondismiss={() => goto('/')}
						onupdate={refreshLearningStatus}
					/>{/key}{/if}
		</div>
	{:else}
		<header class="intro">
			<h1>让查过的词真正被记住。</h1>
			<ol class="flow">
				<li>查词</li>
				<li>留下学习记录</li>
				<li>反馈理解程度</li>
				<li>按计划复习</li>
			</ol>
		</header>
		<HomeLearningTask state={task} onRetry={retryHome} onFocusSearch={focusSearch} />
		<section class="search-block" aria-labelledby="home-search-title">
			<h2 id="home-search-title">查词</h2>
			<SearchBox oncommit={doLookup} bind:value />
			<p class="search-note"><kbd>/</kbd> 或 Ctrl K 聚焦搜索</p>
		</section>
		<section class="recent" aria-labelledby="home-recent-title">
			<div class="section-heading">
				<h2 id="home-recent-title">{recent.length ? '最近查过的词' : '查词记录'}</h2>
				{#if recent.length}<a href="/library">查看词库</a>{/if}
			</div>
			{#if recentStatus === 'loading'}
				<p class="quiet" role="status">最近查词读取中</p>
			{:else if recentStatus === 'error'}
				<p class="error" role="alert">{recentError}</p>
			{:else if recent.length === 0}
				<p class="quiet">查过的词会出现在这里。</p>
				{#if task.kind === 'new'}
					<div class="recent-grid">
						{#each ['meticulous', 'subtle', 'resilient', 'profound'] as word}<button
								type="button"
								onclick={() => doLookup(word)}><span><strong>{word}</strong></span></button
							>{/each}
					</div>
				{/if}
			{:else}
				<div class="recent-grid">
					{#each recent as r}<button type="button" onclick={() => doLookup(r.word)}
							><span><strong>{r.display}</strong><small>{r.phonetic ?? ''}</small></span><span
								class="when">{relTime(r.visited_at)}</span
							></button
						>{/each}
				</div>
			{/if}
		</section>
		<section class="secondary" aria-labelledby="home-stats-title">
			<h2 id="home-stats-title">学习统计</h2>
			{#if statsStatus === 'ready' && stats}
				<p>
					{stats.encountered.toLocaleString()} 个有学习记录的词 · 学习中 {stats.learning.toLocaleString()} · 熟悉 {stats.familiar.toLocaleString()} · 稳定 {stats.stable.toLocaleString()}
				</p>
			{:else if statsStatus === 'error'}
				<p>学习统计暂时没有读出来。</p>
			{:else}
				<p role="status">学习统计读取中</p>
			{/if}
			<p class="dict">
				{#if wordCountStatus === 'error'}离线词典数量暂不可用{:else if wordCount === null}—{:else}{wordCount.toLocaleString()} entries · offline{/if}
			</p>
		</section>
	{/if}
</main>

<style>
	.home {
		max-width: 880px;
	}
	.intro {
		margin-bottom: 22px;
	}
	.flow {
		display: flex;
		flex-wrap: wrap;
		gap: 8px 18px;
		margin: 0;
		padding: 0;
		list-style: none;
		color: var(--muted);
		font-size: 13px;
	}
	.flow li {
		display: flex;
		align-items: center;
		gap: 18px;
	}
	.flow li:not(:last-child)::after {
		content: '→';
		color: var(--kraft);
	}
	.search-block {
		margin-top: 28px;
	}
	.search-note {
		margin: 8px 2px 0;
		color: var(--muted);
		font-size: 12px;
	}
	.recent {
		margin-top: 34px;
	}
	.quiet {
		color: var(--muted);
		font-size: 14px;
	}
	.recent-grid {
		display: grid;
		grid-template-columns: 1fr 1fr;
		gap: 0 28px;
	}
	.recent-grid button {
		border: 0;
		border-top: 1px solid var(--line);
		border-radius: 0;
		justify-content: space-between;
		padding: 13px 2px;
		text-align: left;
		background: none;
		min-width: 0;
		min-height: 44px;
	}
	.recent-grid button:hover {
		background: none;
	}
	.recent-grid button:hover strong {
		color: var(--accent);
	}
	.recent-grid strong {
		font: 500 19px var(--serif);
		transition: color 0.16s var(--ease);
	}
	.recent-grid small {
		font-size: 10px;
		color: var(--muted);
		display: block;
		margin-top: 4px;
	}
	.when {
		color: var(--muted);
		font-size: 10px;
		white-space: nowrap;
	}
	.secondary {
		margin-top: 36px;
		padding-top: 16px;
		border-top: 1px solid var(--line);
		color: var(--muted);
		font-size: 12px;
	}
	.secondary h2 {
		font-size: 13px;
		color: var(--muted);
	}
	.secondary p {
		margin: 0 0 6px;
	}
	.back {
		display: inline-flex;
		align-items: center;
		gap: 8px;
		font-size: 12px;
		text-decoration: none;
		color: var(--muted);
		margin-bottom: 20px;
		min-height: 44px;
	}
	.word-area {
		margin-top: 24px;
	}
	@media (max-width: 700px) {
		.recent-grid {
			grid-template-columns: 1fr;
		}
		.flow {
			display: grid;
			gap: 4px;
		}
		.flow li:not(:last-child)::after {
			content: '';
		}
	}
</style>
