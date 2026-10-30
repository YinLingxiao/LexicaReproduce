export type SpeechPhase = 'idle' | 'loading' | 'playing' | 'error';

export interface SpeechSnapshot {
	phase: SpeechPhase;
	error: string;
	notice: string;
	playKey: string;
	token: number;
	actualAccent: string;
}

export interface SynthesisRequest {
	text: string;
	token: number;
	voiceId: string | null;
	accent: 'US' | 'UK' | null;
	rate: number;
}

export interface SynthesisAudio {
	mime: string;
	bytes: Uint8Array;
	voiceId: string;
	voiceLabel: string;
	language: string;
	requestedAccent: string | null;
	actualAccent: string;
	substituted: boolean;
}

export class SpeechError extends Error {
	code: string;
	constructor(code: string, message: string) {
		super(message);
		this.name = 'SpeechError';
		this.code = code;
	}
}

export interface SpeechAudio {
	play(): Promise<void>;
	pause(): void;
	setCurrentTime(seconds: number): void;
	onended: (() => void) | null;
	onerror: (() => void) | null;
}

export interface SpeechDeps {
	synthesize(request: SynthesisRequest): Promise<SynthesisAudio>;
	cancel(token: number): Promise<void>;
	createAudio(url: string): SpeechAudio;
	createObjectURL(bytes: Uint8Array, mime: string): string;
	revokeObjectURL(url: string): void;
	setTimer(fn: () => void, ms: number): number;
	clearTimer(id: number): void;
}

export interface SpeakInput {
	text: string;
	playKey: string;
	accent?: 'US' | 'UK' | null;
	rate?: number;
	repeat?: number;
	voiceId?: string | null;
}

interface Job {
	token: number;
	text: string;
	playKey: string;
	voiceId: string | null;
	accent: 'US' | 'UK' | null;
	rate: number;
	repeat: number;
}

const REPEAT_GAP_MS = 350;

function accentName(accent: string, language: string): string {
	if (accent === 'US') return 'American English';
	if (accent === 'UK') return 'British English';
	return language;
}

function noticeFor(job: Job, audio: SynthesisAudio): string {
	const actual = `${audio.voiceLabel} (${accentName(audio.actualAccent, audio.language)})`;
	if (!audio.substituted) return `Voice: ${actual}`;
	const wanted =
		job.accent === 'US'
			? 'American English'
			: job.accent === 'UK'
				? 'British English'
				: 'preferred accent';
	if (job.voiceId)
		return `Selected voice: ${actual}. Your accent preference is ${wanted}.`;
	return `${wanted} is unavailable. Using ${actual}.`;
}

function blocked(error: unknown): boolean {
	if (!error || typeof error !== 'object') return false;
	const name = 'name' in error ? String(error.name) : '';
	const message = 'message' in error ? String(error.message) : '';
	return (
		name === 'NotAllowedError' ||
		/not allowed|user gesture|interact with the document/i.test(message)
	);
}

export function createSpeechController(deps: SpeechDeps) {
	let epoch = 0;
	let state: SpeechSnapshot = {
		phase: 'idle',
		error: '',
		notice: '',
		playKey: '',
		token: 0,
		actualAccent: '',
	};
	const listeners = new Set<() => void>();
	let queued: Job | null = null;
	let pumping = false;
	let inFlight = false;
	let cancelWanted = 0;
	let cancelSent = 0;
	let cancelScheduled = false;
	let playbackStop: (() => void) | null = null;
	let timer: number | null = null;

	function emit(): void {
		for (const listener of listeners) listener();
	}
	function set(partial: Partial<SpeechSnapshot>): void {
		state = { ...state, ...partial };
		emit();
	}
	function requestCancel(token: number): void {
		cancelWanted = Math.max(cancelWanted, token);
		if (cancelScheduled) return;
		cancelScheduled = true;
		queueMicrotask(() => {
			cancelScheduled = false;
			const target = cancelWanted;
			if (target <= cancelSent) return;
			cancelSent = target;
			void deps.cancel(target).catch(() => {});
		});
	}
	function stopPlayback(): void {
		if (timer !== null) {
			deps.clearTimer(timer);
			timer = null;
		}
		const stop = playbackStop;
		playbackStop = null;
		stop?.();
	}

	async function playRepeats(job: Job, audio: SynthesisAudio): Promise<void> {
		const url = deps.createObjectURL(audio.bytes, audio.mime);
		let element: SpeechAudio;
		try {
			element = deps.createAudio(url);
		} catch (error) {
			deps.revokeObjectURL(url);
			throw error;
		}
		await new Promise<void>((resolve) => {
			let closed = false;
			let played = 0;
			const finish = () => {
				if (closed) return;
				closed = true;
				if (timer !== null) {
					deps.clearTimer(timer);
					timer = null;
				}
				element.onended = null;
				element.onerror = null;
				element.pause();
				deps.revokeObjectURL(url);
				if (playbackStop === finish) playbackStop = null;
				resolve();
			};
			playbackStop = finish;
			const fail = (message: string) => {
				if (closed || job.token !== epoch) {
					finish();
					return;
				}
				set({ phase: 'error', error: message });
				finish();
			};
			const start = () => {
				if (closed || job.token !== epoch) {
					finish();
					return;
				}
				played += 1;
				set({ phase: 'playing', error: '' });
				try {
					if (played > 1) element.setCurrentTime(0);
					void element
						.play()
						.catch((error: unknown) =>
							fail(
								blocked(error)
									? 'Autoplay was blocked. Click the speaker to play.'
									: 'Audio playback failed.',
							),
						);
				} catch {
					fail('Audio playback failed.');
				}
			};
			element.onended = () => {
				if (closed || job.token !== epoch) {
					finish();
					return;
				}
				if (played >= job.repeat) {
					set({ phase: 'idle', error: '' });
					finish();
					return;
				}
				timer = deps.setTimer(() => {
					timer = null;
					start();
				}, REPEAT_GAP_MS);
			};
			element.onerror = () => fail('Audio playback failed.');
			start();
		});
	}

	async function pump(): Promise<void> {
		if (pumping) return;
		pumping = true;
		try {
			while (queued) {
				const job = queued;
				queued = null;
				if (job.token !== epoch) continue;
				inFlight = true;
				let audio: SynthesisAudio;
				try {
					audio = await deps.synthesize({
						text: job.text,
						token: job.token,
						voiceId: job.voiceId,
						accent: job.accent,
						rate: job.rate,
					});
				} catch (error) {
					inFlight = false;
					if (job.token !== epoch) continue;
					const speech =
						error instanceof SpeechError
							? error
							: new SpeechError(
									'RESOURCE_ERROR',
									error instanceof Error
										? error.message
										: 'Pronunciation failed.',
								);
					if (speech.code === 'CANCELLED') {
						set({ phase: 'idle', error: '' });
						continue;
					}
					set({ phase: 'error', error: speech.message, notice: '' });
					continue;
				}
				inFlight = false;
				if (job.token !== epoch) continue;
				set({
					notice: noticeFor(job, audio),
					actualAccent: audio.actualAccent,
				});
				try {
					await playRepeats(job, audio);
				} catch {
					if (job.token === epoch)
						set({
							phase: 'error',
							error: 'Audio playback failed.',
							notice: '',
						});
				}
			}
		} finally {
			inFlight = false;
			pumping = false;
			if (queued) void pump();
		}
	}

	function speak(input: SpeakInput): void {
		const text = input.text.trim();
		const token = ++epoch;
		stopPlayback();
		if (inFlight) requestCancel(token);
		const playKey = input.playKey;
		if (!text) {
			queued = null;
			set({
				phase: 'error',
				error: 'No text to read.',
				notice: '',
				playKey,
				token,
			});
			return;
		}
		if (new TextEncoder().encode(text).byteLength > 2000) {
			queued = null;
			set({
				phase: 'error',
				error: 'Text is too long.',
				notice: '',
				playKey,
				token,
			});
			return;
		}
		const job: Job = {
			token,
			text,
			playKey,
			voiceId: input.voiceId ?? null,
			accent:
				input.accent === 'US' || input.accent === 'UK' ? input.accent : null,
			rate:
				input.rate === 0.8 || input.rate === 1 || input.rate === 1.2
					? input.rate
					: 1,
			repeat: input.repeat === 3 ? 3 : 1,
		};
		queued = job;
		set({
			phase: 'loading',
			error: '',
			notice: '',
			actualAccent: '',
			playKey,
			token,
		});
		if (inFlight) requestCancel(token);
		void pump();
	}

	function stop(): void {
		const token = ++epoch;
		queued = null;
		stopPlayback();
		set({ phase: 'idle', error: '', token });
		requestCancel(token);
	}

	return {
		snapshot: (): SpeechSnapshot => ({ ...state }),
		subscribe(listener: () => void): () => void {
			listeners.add(listener);
			return () => listeners.delete(listener);
		},
		speak,
		stop,
	};
}
