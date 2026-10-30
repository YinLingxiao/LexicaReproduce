<script lang="ts">
	import Icon from './Icon.svelte';
	import { fitPopover } from '$lib/fit-popover';
	import {
		getSpeechSnapshot,
		speakText,
		stopSpeaking,
		subscribeSpeech,
	} from '$lib/tts';

	let {
		text,
		playKey,
		label,
		accent = undefined,
		rate = undefined,
		repeat = 1,
		voiceId = undefined,
	}: {
		text: string;
		playKey: string;
		label: string;
		accent?: 'US' | 'UK';
		rate?: number;
		repeat?: number;
		voiceId?: string | null;
	} = $props();

	let phase = $state<'idle' | 'loading' | 'playing' | 'error'>('idle');
	let message = $state('');
	let notice = $state('');
	let actualAccent = $state('');

	function sync(): void {
		const snap = getSpeechSnapshot();
		const mine = snap.playKey === playKey;
		phase = mine ? snap.phase : 'idle';
		message = mine && snap.phase === 'error' ? snap.error : '';
		notice = mine ? snap.notice : '';
		actualAccent = mine ? snap.actualAccent : '';
	}

	$effect(() => {
		const key = playKey;
		sync();
		const unsubscribe = subscribeSpeech(sync);
		return () => {
			unsubscribe();
			const snap = getSpeechSnapshot();
			if (
				snap.playKey === key &&
				(snap.phase === 'loading' || snap.phase === 'playing')
			)
				stopSpeaking();
		};
	});

	function activate(): void {
		const snap = getSpeechSnapshot();
		if (
			snap.playKey === playKey &&
			(snap.phase === 'loading' || snap.phase === 'playing')
		) {
			stopSpeaking();
			return;
		}
		speakText(text, { playKey, accent, rate, repeat, voiceId });
	}

	const caption = $derived(
		phase === 'loading'
			? '合成中'
			: phase === 'playing'
				? '停止'
				: label === '朗读单词'
					? '朗读'
					: label,
	);
	const accessible = $derived(
		phase === 'loading'
			? `正在合成${label}`
			: phase === 'playing'
				? `停止${label}`
				: label,
	);
</script>

<span class="speak-control">
	<button
		type="button"
		class:busy={phase === 'loading'}
		class:active={phase === 'playing'}
		aria-label={accessible}
		aria-busy={phase === 'loading'}
		aria-pressed={phase === 'playing'}
		onclick={activate}
		><Icon
			name={phase === 'playing' ? 'close' : 'sound'}
			size={14}
		/>{caption}</button
	>
	{#if message || notice}<details class="speech-details" use:fitPopover>
			<summary
				class:error={Boolean(message)}
				aria-label={message ? '查看发音错误' : '查看发音信息'}
				title={message || notice}
			>
				{message
					? '朗读失败'
					: actualAccent === 'US'
						? '美音'
						: actualAccent === 'UK'
							? '英音'
							: '音色'}
			</summary>
			<span class="speech-popover" data-popover>
				<span role={message ? 'alert' : undefined}>{message || notice}</span>
				<a href="/settings#pronunciation">发音设置</a>
			</span>
		</details>{/if}
</span>

<style>
	.speak-control {
		display: inline-flex;
		align-items: center;
		gap: 8px;
		position: relative;
		max-width: 100%;
	}
	button {
		min-height: 32px;
		padding: 4px 10px;
		font-size: 13px;
		display: inline-flex;
		align-items: center;
		gap: 5px;
		white-space: nowrap;
	}
	button.busy,
	button.active {
		border-color: var(--accent);
		color: var(--accent);
	}
	.speech-details {
		position: relative;
		font-size: 11px;
		color: var(--muted);
		text-align: left;
	}
	summary {
		cursor: pointer;
		list-style: none;
		border-bottom: 1px dotted var(--muted);
	}
	summary::-webkit-details-marker {
		display: none;
	}
	summary.error {
		color: var(--danger);
	}
	.speech-popover {
		position: absolute;
		top: calc(100% + 10px);
		right: 0;
		z-index: 35;
		width: min(300px, 75vw);
		display: grid;
		gap: 8px;
		padding: 12px 14px;
		background: var(--card);
		border: 1px solid var(--line);
		border-radius: 10px;
		box-shadow: 0 8px 28px #0002;
		line-height: 1.6;
		overflow-wrap: anywhere;
	}
	.speech-popover a {
		font-size: 11px;
		color: var(--accent);
	}
</style>
