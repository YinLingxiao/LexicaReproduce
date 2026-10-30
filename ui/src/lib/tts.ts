import { ttsCancel, ttsSynthesize, ttsVoices, type TtsVoiceList } from './api';
import {
	createSpeechController,
	SpeechError,
	type SpeakInput,
	type SpeechAudio,
	type SpeechSnapshot,
} from './speech-controller';
import { RpcError } from './transport';

const VOICE_KEY = 'lexica.tts.voiceId';

export interface SpeakOptions {
	accent?: 'US' | 'UK';
	rate?: number;
	repeat?: number;
	voiceId?: string | null;
	playKey?: string;
}

export function getPreferredVoiceId(): string | null {
	if (typeof localStorage === 'undefined') return null;
	try {
		return localStorage.getItem(VOICE_KEY) || null;
	} catch {
		return null;
	}
}

export function setPreferredVoiceId(id: string | null): void {
	if (typeof localStorage === 'undefined') return;
	try {
		if (id) localStorage.setItem(VOICE_KEY, id);
		else localStorage.removeItem(VOICE_KEY);
	} catch {
		/* The current session can still use the selected voice. */
	}
}

function bytesFromBase64(value: string): Uint8Array {
	const binary = atob(value);
	const bytes = new Uint8Array(binary.length);
	for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
	return bytes;
}

function browserAudio(url: string): SpeechAudio {
	const audio = new Audio();
	audio.preload = 'auto';
	audio.src = url;
	let ended: (() => void) | null = null;
	let failed: (() => void) | null = null;
	audio.onended = () => ended?.();
	audio.onerror = () => failed?.();
	return {
		play: () => audio.play(),
		pause: () => audio.pause(),
		setCurrentTime: (seconds) => {
			audio.currentTime = seconds;
		},
		get onended() {
			return ended;
		},
		set onended(fn) {
			ended = fn;
		},
		get onerror() {
			return failed;
		},
		set onerror(fn) {
			failed = fn;
		},
	};
}

const controller = createSpeechController({
	async synthesize(request) {
		try {
			const result = await ttsSynthesize({
				text: request.text,
				token: request.token,
				voiceId: request.voiceId,
				accent: request.accent,
				rate: request.rate,
			});
			return {
				mime: result.mime,
				bytes: bytesFromBase64(result.audio_base64),
				voiceId: result.voice_id,
				voiceLabel: result.voice_label,
				language: result.language,
				requestedAccent: result.requested_accent,
				actualAccent: result.actual_accent,
				substituted: result.substituted,
			};
		} catch (error) {
			if (error instanceof RpcError) throw new SpeechError(error.code, error.message);
			if (error instanceof SpeechError) throw error;
			if (error instanceof Error) throw new SpeechError('RESOURCE_ERROR', error.message);
			throw new SpeechError('RESOURCE_ERROR', 'Pronunciation failed.');
		}
	},
	cancel: async (token) => {
		await ttsCancel(token);
	},
	createAudio: browserAudio,
	createObjectURL: (bytes, mime) => {
		const copy = new ArrayBuffer(bytes.byteLength);
		new Uint8Array(copy).set(bytes);
		return URL.createObjectURL(new Blob([copy], { type: mime }));
	},
	revokeObjectURL: (url) => URL.revokeObjectURL(url),
	setTimer: (fn, ms) => window.setTimeout(fn, ms),
	clearTimer: (id) => window.clearTimeout(id),
});

export function speakText(text: string, opts: SpeakOptions = {}): void {
	const input: SpeakInput = {
		text,
		playKey: opts.playKey ?? text,
		accent: opts.accent,
		rate: opts.rate,
		repeat: opts.repeat,
		voiceId: opts.voiceId === undefined ? getPreferredVoiceId() : opts.voiceId,
	};
	controller.speak(input);
}

export function stopSpeaking(): void {
	controller.stop();
}

export function getSpeechSnapshot(): SpeechSnapshot {
	return controller.snapshot();
}

export function subscribeSpeech(listener: () => void): () => void {
	return controller.subscribe(listener);
}

export function loadTtsVoices(): Promise<TtsVoiceList> {
	return ttsVoices();
}

if (typeof window !== 'undefined') window.addEventListener('pagehide', () => controller.stop());
