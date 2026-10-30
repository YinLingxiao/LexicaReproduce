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

	const accessible = $derived(
		phase === 'loading'
			? 'Preparing audio'
			: phase === 'playing'
				? 'Stop playback'
				: label,
	);
	const noticeId = $derived(`speech-info-${playKey.replace(/[^A-Za-z0-9_-]/g, '_')}`);
</script>

<span class="speak-control">
	<button
		type="button"
		class:busy={phase === 'loading'}
		class:active={phase === 'playing'}
		aria-label={accessible}
		aria-describedby={notice ? noticeId : undefined}
		title={[accessible, notice].filter(Boolean).join(' · ')}
		aria-busy={phase === 'loading'}
		aria-pressed={phase === 'playing'}
		onclick={activate}
		><Icon name="sound" size={18} /></button
	>
	{#if notice}<span class="speech-description" id={noticeId}>{notice}</span>{/if}
	{#if message}<details class="speech-details" use:fitPopover>
			<summary
				class:error={Boolean(message)}
				aria-label="Pronunciation error details"
				title={message}
			>
				!
			</summary>
			<span class="speech-popover" data-popover>
				<span role="alert">{message}</span>
				<a href="/settings#pronunciation">Pronunciation settings</a>
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
		width: 28px;
		height: 28px;
		min-height: 28px;
		padding: 4px;
		border: 0;
		border-radius: 50%;
		background: transparent;
		box-shadow: none;
		color: var(--accent);
		display: inline-flex;
		align-items: center;
		justify-content: center;
		flex-shrink: 0;
	}
	button:hover,
	button.active {
		background: color-mix(in srgb, var(--accent) 12%, transparent);
	}
	button:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
	button.busy { animation: speaking-pulse 0.8s ease-in-out infinite alternate; }
	@keyframes speaking-pulse { to { opacity: 0.35; } }
	@media (prefers-reduced-motion: reduce) { button.busy { animation: none; } }
	.speech-details {
		position: relative;
		font-size: 11px;
		color: var(--muted);
		text-align: left;
	}
	.speech-description {
		position: absolute;
		width: 1px;
		height: 1px;
		padding: 0;
		margin: -1px;
		overflow: hidden;
		clip-path: inset(50%);
		white-space: nowrap;
		border: 0;
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
