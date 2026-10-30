<script lang="ts">
	import { onMount, untrack } from 'svelte';
	import { page } from '$app/state';
	import { goto } from '$app/navigation';
	import SearchBox from '$lib/components/SearchBox.svelte';
	import WordView from '$lib/components/WordView.svelte';
	import Icon from '$lib/components/Icon.svelte';
	import {
		lookupWord,
		recentWords,
		appInfo,
		relTime,
		errorMessage,
		type LookupWordResult,
		type RecentWord,
	} from '$lib/api';

	type LoadStatus = 'loading' | 'ready' | 'error';
	let result = $state<LookupWordResult | null>(null);
	let value = $state('');
	let recent = $state<RecentWord[]>([]);
	let recentStatus = $state<LoadStatus>('loading');
	let recentError = $state('');
	let wordCount = $state<number | null>(null);
	let error = $state('');
	let loading = $state(false);
	let request = 0;
	let refreshToken = 0;
	const query = $derived(page.url.searchParams.get('word') ?? '');
	async function refresh() {
		const token = ++refreshToken;
		try {
			const rows = await recentWords(8);
			if (token !== refreshToken) return;
			recent = rows;
			recentStatus = 'ready';
			recentError = '';
		} catch (cause) {
			if (token !== refreshToken) return;
			recentStatus = 'error';
			recentError = errorMessage(cause);
		}
	}
	async function load(word: string) {
		const token = ++request;
		loading = true;
		error = '';
		result = null;
		value = word;
		try {
			const found = await lookupWord(word);
			if (token !== request) return;
			result = found;
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

	$effect(() => {
		const word = query;
		untrack(() => {
			if (word) void load(word);
			else {
				++request;
				result = null;
				error = '';
				loading = false;
				value = '';
			}
		});
	});

	onMount(() => {
		void refresh();
		void appInfo()
			.then((info) => {
				wordCount = info.word_count;
			})
			.catch(() => {
				wordCount = null;
			});
	});
</script>

<main class="page home">
	{#if query}
		<a href="/" class="back"><Icon name="back" size={15} />Search</a>
		<SearchBox oncommit={doLookup} bind:value />
		<div class="word-area" aria-busy={loading}>
			{#if loading}<div class="skeleton"></div>{:else if error}<div
					class="error"
					role="alert"
				>
					{error}
				</div>
				<button onclick={() => load(query)}>Try again</button>{:else if result}
				{#key result}<WordView
						{result}
						{query}
						oncommit={doLookup}
						ondismiss={() => goto('/')}
						onupdate={refresh}
					/>{/key}{/if}
		</div>
	{:else}
		<section class="search-block" aria-label="Dictionary search">
			<SearchBox oncommit={doLookup} bind:value />
			<div class="search-note">
				<span
					>{wordCount === null ? '—' : wordCount.toLocaleString()} entries · offline</span
				>
				<span><kbd>/</kbd> or <kbd>Ctrl K</kbd> to search</span>
			</div>
		</section>
		<section class="recent" aria-labelledby="home-recent-title">
			<div class="section-heading">
				<h2 id="home-recent-title">Recent searches</h2>
				{#if recent.length}<a href="/library">All words</a>{/if}
			</div>
			{#if recentStatus === 'loading'}
				<p class="quiet" role="status">Loading recent searches…</p>
			{:else if recentStatus === 'error'}
				<p class="error" role="alert">{recentError}</p>
				<button
					onclick={() => {
						recentStatus = 'loading';
						void refresh();
					}}>Retry recent searches</button
				>
			{:else if recent.length === 0}
				<p class="quiet">Words you look up will appear here.</p>
			{:else}
				<div class="recent-grid">
					{#each recent as r}<button
							type="button"
							onclick={() => doLookup(r.word)}
							><span
								><strong>{r.display}</strong><small>{r.phonetic ?? ''}</small
								></span
							><span class="when">{relTime(r.visited_at)}</span></button
						>{/each}
				</div>
			{/if}
		</section>
	{/if}
</main>

<style>
	.home {
		max-width: 880px;
	}
	.search-block {
		padding-top: 3vh;
	}
	.search-note {
		display: flex;
		justify-content: space-between;
		flex-wrap: wrap;
		gap: 6px 18px;
		margin: 10px 2px 0;
		color: var(--muted);
		font-size: 11px;
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
	}
</style>
