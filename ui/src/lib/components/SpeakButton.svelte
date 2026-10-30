<script lang="ts">
	import { getSpeechSnapshot, speakText, stopSpeaking, subscribeSpeech } from '$lib/tts';

	let {
		text,
		playKey,
		label,
		accent = undefined,
		rate = undefined,
		repeat = 1,
		voiceId = undefined
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

	function sync(): void {
		const snap = getSpeechSnapshot();
		const mine = snap.playKey === playKey;
		phase = mine ? snap.phase : 'idle';
		message = mine && snap.phase === 'error' ? snap.error : '';
		notice = mine ? snap.notice : '';
	}

	$effect(() => {
		const key = playKey;
		sync();
		const unsubscribe = subscribeSpeech(sync);
		return () => {
			unsubscribe();
			const snap = getSpeechSnapshot();
			if (snap.playKey === key && (snap.phase === 'loading' || snap.phase === 'playing'))
				stopSpeaking();
		};
	});

	function activate(): void {
		const snap = getSpeechSnapshot();
		if (snap.playKey === playKey && (snap.phase === 'loading' || snap.phase === 'playing')) {
			stopSpeaking();
			return;
		}
		speakText(text, { playKey, accent, rate, repeat, voiceId });
	}

	const caption = $derived(phase === 'loading' ? '正在合成' : phase === 'playing' ? '停止' : label);
	const accessible = $derived(
		phase === 'loading' ? `正在合成${label}` : phase === 'playing' ? `停止${label}` : label
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
		onclick={activate}>{caption}</button
	>
	{#if message}<span class="speak-alert" role="alert">{message}</span>{/if}
	{#if notice}<span class="speak-note">{notice}</span>{/if}
</span>

<style>
	.speak-control {
		display: inline-flex;
		flex-direction: column;
		align-items: flex-start;
		gap: 4px;
		max-width: 100%;
	}
	button {
		min-height: 32px;
		padding: 4px 10px;
		font-size: 13px;
	}
	button.busy,
	button.active {
		border-color: var(--accent);
		color: var(--accent);
	}
	.speak-alert,
	.speak-note {
		font-size: 12px;
		line-height: 1.4;
		overflow-wrap: anywhere;
	}
	.speak-alert {
		color: var(--danger);
	}
	.speak-note {
		color: var(--muted);
	}
</style>
