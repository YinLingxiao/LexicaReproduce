<script lang="ts">
	import { onMount } from 'svelte';
	import { wordbooksList, errorMessage, type WordbookInfo } from '$lib/api';
	import {
		preferences,
		applyPreferences,
		DAILY_NEW_LIMITS,
		type StudyPreferences,
	} from '$lib/preferences.svelte';
	import Icon from './Icon.svelte';

	let { onClose }: { onClose?: () => void } = $props();

	type Wordbook = StudyPreferences['wordbook'];
	const WORDBOOK_NAMES: Record<Wordbook, string> = {
		cet4: 'CET-4',
		cet6: 'CET-6',
		ky: 'Postgraduate English',
		toefl: 'TOEFL',
		ielts: 'IELTS',
		gre: 'GRE',
		gk: 'Gaokao English',
		oxford: 'Oxford 3000',
		custom: 'Custom wordbook',
		encountered: 'Search history',
	};
	const WORDBOOK_ORDER = Object.keys(WORDBOOK_NAMES) as Wordbook[];

	let books = $state<WordbookInfo[]>([]);
	let booksError = $state('');

	const bookRows = $derived(
		WORDBOOK_ORDER.map((id) => {
			const info = books.find((b) => b.id === id);
			return {
				id,
				name: WORDBOOK_NAMES[id],
				total: info?.total_words ?? null,
				learned: info?.learned_words ?? 0,
			};
		}),
	);

	onMount(async () => {
		try {
			books = await wordbooksList();
		} catch (e) {
			booksError = errorMessage(e);
		}
	});

	/** 每次修改学习偏好后立即持久化。 */
	function set<K extends keyof StudyPreferences>(key: K, value: StudyPreferences[K]) {
		preferences.study[key] = value;
		applyPreferences();
	}

	function toggleTab(index: number) {
		const tab = preferences.study.extensionTabs[index];
		tab.enabled = !tab.enabled;
		applyPreferences();
	}

	function moveTab(from: number, to: number) {
		const tabs = preferences.study.extensionTabs;
		if (to < 0 || to >= tabs.length || from === to) return;
		const next = [...tabs];
		const [item] = next.splice(from, 1);
		next.splice(to, 0, item);
		preferences.study.extensionTabs = next;
		applyPreferences();
	}

	let dragFrom = $state<number | null>(null);

	function handleKey(event: KeyboardEvent, index: number) {
		if (event.key === 'ArrowUp') {
			event.preventDefault();
			moveTab(index, index - 1);
		} else if (event.key === 'ArrowDown') {
			event.preventDefault();
			moveTab(index, index + 1);
		}
	}

	const cardModes = [
		['standard', 'Standard card'],
		['context', 'Context card'],
	] as const;
	const planStyles = [
		['standard', 'Standard'],
		['intensive', 'Intensive'],
		['casual', 'Casual'],
	] as const;
	const priorities = [
		['mixed', 'Mixed'],
		['new_first', 'New words first'],
		['review_first', 'Reviews first'],
	] as const;
	const repeats = [
		['never', 'Once'],
		['thrice', 'Three times'],
	] as const;
	const accents = [
		['US', 'US'],
		['UK', 'UK'],
	] as const;
	const rates = [
		[0.8, '0.8x'],
		[1, '1x'],
		[1.2, '1.2x'],
	] as const;
	const exampleStyles = [
		['standard', 'Dictionary examples'],
		['exam', 'Exam examples'],
	] as const;
	const exampleOrders = [
		['first', 'First'],
		['random', 'Random'],
	] as const;
</script>

{#snippet toggle(key: keyof StudyPreferences, label: string, last = false)}
	<div class="setting-row" class:last>
		<h3 id="study-{key}">{label}</h3>
		<button
			class="switch"
			role="switch"
			aria-checked={preferences.study[key] === true}
			aria-labelledby="study-{key}"
			onclick={() => set(key, !preferences.study[key] as never)}><span></span></button
		>
	</div>
{/snippet}

<div class="study-settings">
	{#if onClose}
		<div class="panel-head">
			<h2>Study settings</h2>
			<button class="icon-btn" aria-label="Close study settings" onclick={onClose}
				><Icon name="close" size={16} /></button
			>
		</div>
	{/if}

	<section class="panel">
		<div class="section-heading"><h2>Wordbook and plan</h2></div>
		<div class="books" role="radiogroup" aria-label="Wordbook">
			{#each bookRows as b}
				<button
					class="book"
					role="radio"
					aria-checked={preferences.study.wordbook === b.id}
					class:chosen={preferences.study.wordbook === b.id}
					onclick={() => set('wordbook', b.id)}
				>
					<span class="book-name">{b.name}</span>
					{#if b.total !== null}
						<span class="book-count">{b.learned.toLocaleString()} / {b.total.toLocaleString()}</span
						>
						<span class="book-bar" aria-hidden="true"
							><span style:width="{b.total ? Math.min(100, (b.learned / b.total) * 100) : 0}%"
							></span></span
						>
					{:else}<span class="book-count">—</span>{/if}
				</button>
			{/each}
		</div>
		{#if booksError}<p class="muted small">Wordbook progress unavailable:{booksError}</p>{/if}
		<p class="muted small blurb">
			Your custom wordbook contains bookmarked words. Manage them in
			<a href="/library?filter=bookmarked">Words · Bookmarked</a>。
		</p>
		<div class="setting-row">
			<h3>Card type</h3>
			<div class="segmented">
				{#each cardModes as [key, label]}<button
						aria-pressed={preferences.study.cardMode === key}
						class:chosen={preferences.study.cardMode === key}
						onclick={() => set('cardMode', key)}>{label}</button
					>{/each}
			</div>
		</div>
		<div class="setting-row">
			<h3>Study plan</h3>
			<div class="segmented">
				{#each planStyles as [key, label]}<button
						aria-pressed={preferences.study.planStyle === key}
						class:chosen={preferences.study.planStyle === key}
						onclick={() => set('planStyle', key)}>{label}</button
					>{/each}
			</div>
		</div>
		<div class="setting-row">
			<h3>Daily new words</h3>
			<div class="segmented">
				{#each DAILY_NEW_LIMITS as n}<button
						aria-pressed={preferences.study.dailyNewLimit === n}
						class:chosen={preferences.study.dailyNewLimit === n}
						onclick={() => set('dailyNewLimit', n)}>{n}</button
					>{/each}
			</div>
		</div>
		<div class="setting-row last">
			<h3>Study order</h3>
			<div class="segmented">
				{#each priorities as [key, label]}<button
						aria-pressed={preferences.study.priority === key}
						class:chosen={preferences.study.priority === key}
						onclick={() => set('priority', key)}>{label}</button
					>{/each}
			</div>
		</div>
	</section>

	<section class="panel">
		<div class="section-heading"><h2>Pronunciation</h2></div>
		{@render toggle('autoPronounceWord', 'Auto-play words')}
		<div class="setting-row">
			<h3>Repeat</h3>
			<div class="segmented">
				{#each repeats as [key, label]}<button
						aria-pressed={preferences.study.pronounceRepeat === key}
						class:chosen={preferences.study.pronounceRepeat === key}
						onclick={() => set('pronounceRepeat', key)}>{label}</button
					>{/each}
			</div>
		</div>
		{@render toggle('autoPronounceExample', 'Auto-play examples')}
		{@render toggle('autoPronounceContext', 'Auto-play context cards')}
		<div class="setting-row">
			<h3>Accent</h3>
			<div class="segmented">
				{#each accents as [key, label]}<button
						aria-pressed={preferences.study.accent === key}
						class:chosen={preferences.study.accent === key}
						onclick={() => set('accent', key)}>{label}</button
					>{/each}
			</div>
		</div>
		<div class="setting-row last">
			<h3>Playback speed</h3>
			<div class="segmented">
				{#each rates as [key, label]}<button
						aria-pressed={preferences.study.speechRate === key}
						class:chosen={preferences.study.speechRate === key}
						onclick={() => set('speechRate', key)}>{label}</button
					>{/each}
			</div>
		</div>
	</section>

	<section class="panel">
		<div class="section-heading"><h2>Examples</h2></div>
		<div class="setting-row">
			<h3>Example source</h3>
			<div class="segmented">
				{#each exampleStyles as [key, label]}<button
						aria-pressed={preferences.study.exampleStyle === key}
						class:chosen={preferences.study.exampleStyle === key}
						onclick={() => set('exampleStyle', key)}>{label}</button
					>{/each}
			</div>
		</div>
		<div class="setting-row last">
			<h3>Display</h3>
			<div class="segmented">
				{#each exampleOrders as [key, label]}<button
						aria-pressed={preferences.study.exampleOrder === key}
						class:chosen={preferences.study.exampleOrder === key}
						onclick={() => set('exampleOrder', key)}>{label}</button
					>{/each}
			</div>
		</div>
		<p class="muted small footnote">
			Exam examples support CET-4, CET-6, and postgraduate English exams.
		</p>
	</section>

	<section class="panel">
		<div class="section-heading"><h2>Cards and practice</h2></div>
		{@render toggle('autoSplitSyllables', 'Split syllables')}
		{@render toggle('fadeIn', 'Animate cards')}
		{@render toggle('hideChinese', 'Hide Chinese translations')}
		{@render toggle('inSessionQuiz', 'Spelling practice after browsing', true)}
	</section>

	<section class="panel">
		<div class="section-heading"><h2>Extra tabs</h2></div>
		<p class="muted small blurb">Choose tabs, then drag ≡ or use Up / Down to reorder them.</p>
		<ul class="tab-list">
			{#each preferences.study.extensionTabs as tab, i (tab.id)}
				<li
					class:dragging={dragFrom === i}
					ondragover={(e) => e.preventDefault()}
					ondrop={(e) => {
						e.preventDefault();
						if (dragFrom !== null) moveTab(dragFrom, i);
						dragFrom = null;
					}}
				>
					<button
						class="check"
						role="checkbox"
						aria-checked={tab.enabled}
						onclick={() => toggleTab(i)}
						><span class="box" class:on={tab.enabled}
							>{#if tab.enabled}<Icon name="check" size={12} />{/if}</span
						>{tab.label}</button
					>
					<button
						class="handle"
						draggable="true"
						aria-label="Move {tab.label} ( {i + 1}  of {preferences.study.extensionTabs
							.length} ) — use Up / Down to reorder"
						ondragstart={(e) => {
							dragFrom = i;
							e.dataTransfer?.setData('text/plain', tab.id);
						}}
						ondragend={() => (dragFrom = null)}
						onkeydown={(e) => handleKey(e, i)}>≡</button
					>
				</li>
			{/each}
		</ul>
		{@render toggle('autoReturnToExamples', 'Return to examples on each word', true)}
	</section>
</div>

<style>
	.study-settings {
		display: flex;
		flex-direction: column;
		gap: 20px;
	}
	.panel-head {
		display: flex;
		justify-content: space-between;
		align-items: center;
	}
	.panel-head h2 {
		font-size: 18px;
		margin: 0;
	}
	.icon-btn {
		display: inline-flex;
		padding: 6px;
		border: 0;
		background: transparent;
		color: var(--muted);
	}
	.setting-row {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 20px;
		padding: 16px 0;
		border-bottom: 1px solid var(--line);
	}
	.setting-row.last {
		border: 0;
		padding-bottom: 0;
	}
	.setting-row h3 {
		font-size: 13px;
		margin: 0;
		font-weight: 500;
	}
	.segmented {
		display: flex;
		gap: 4px;
		padding: 4px;
		background: var(--bg);
		border: 1px solid var(--line);
		border-radius: 10px;
	}
	.segmented button {
		white-space: nowrap;
		padding: 6px 12px;
		border: 0;
		background: transparent;
		color: var(--muted);
		font-size: 11px;
	}
	.segmented button.chosen {
		background: var(--card);
		color: var(--accent);
	}
	.switch {
		position: relative;
		width: 38px;
		height: 22px;
		padding: 0;
		border: 1px solid var(--line);
		border-radius: 99px;
		background: var(--bg);
		flex-shrink: 0;
	}
	.switch span {
		position: absolute;
		top: 2px;
		left: 2px;
		width: 16px;
		height: 16px;
		border-radius: 50%;
		background: var(--muted);
		transition: transform 0.16s var(--ease);
	}
	.switch[aria-checked='true'] {
		background: var(--soft);
		border-color: var(--accent);
	}
	.switch[aria-checked='true'] span {
		transform: translateX(16px);
		background: var(--accent);
	}
	.books {
		display: grid;
		grid-template-columns: repeat(auto-fill, minmax(170px, 1fr));
		gap: 8px;
		margin-bottom: 12px;
	}
	.book {
		display: flex;
		flex-direction: column;
		align-items: flex-start;
		gap: 4px;
		padding: 10px 12px;
		text-align: left;
		background: var(--bg);
		border: 1px solid var(--line);
		border-radius: 10px;
		color: var(--fg);
	}
	.book.chosen {
		border-color: var(--accent);
		background: var(--soft);
	}
	.book-name {
		font-size: 12px;
		font-weight: 500;
	}
	.book-count {
		font-size: 10px;
		color: var(--muted);
	}
	.book-bar {
		display: block;
		width: 100%;
		height: 3px;
		background: var(--line);
		border-radius: 3px;
		overflow: hidden;
	}
	.book-bar span {
		display: block;
		height: 100%;
		background: var(--accent);
	}
	.blurb {
		line-height: 1.7;
		margin: 0 0 6px;
	}
	.footnote {
		margin: 14px 0 0;
	}
	.tab-list {
		list-style: none;
		padding: 0;
		margin: 6px 0 0;
		border-bottom: 1px solid var(--line);
	}
	.tab-list li {
		display: flex;
		align-items: center;
		justify-content: space-between;
		padding: 4px 0;
		border-top: 1px solid var(--line);
	}
	.tab-list li.dragging {
		opacity: 0.5;
	}
	.check {
		display: flex;
		align-items: center;
		gap: 10px;
		border: 0;
		background: transparent;
		color: var(--fg);
		font-size: 13px;
		padding: 8px 0;
	}
	.box {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		width: 16px;
		height: 16px;
		border: 1px solid var(--line);
		border-radius: 4px;
		color: var(--card);
	}
	.box.on {
		background: var(--accent);
		border-color: var(--accent);
	}
	.handle {
		border: 0;
		background: transparent;
		color: var(--muted);
		font-size: 16px;
		padding: 4px 10px;
		cursor: grab;
	}
	@media (max-width: 600px) {
		.setting-row {
			align-items: flex-start;
			flex-direction: column;
			gap: 12px;
		}
		.setting-row:has(.switch) {
			flex-direction: row;
			align-items: center;
		}
		.segmented {
			width: 100%;
		}
		.segmented button {
			flex: 1;
		}
	}
</style>
