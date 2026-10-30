// Adapted from Lexica c4b92a39402860566bf954f92efc8e59666aaa4a; MIT.
import { invoke, isPreview } from './transport';
export { isPreview } from './transport';
import type { ComprehensionLevel, MemoryStatus, RecallQuality, StudyPriority, StudyGrade, PromptKind, PromptSource, HintKind, SetupPhase, Example, Collocation, Sense, WordFormEntry, RelatedWordItem, RootPart, WordEntry, Suggestion, MemorySummary, LookupWordResult, AppInfo, SetupProgress, EnsureDictionaryResult, WordNote, LibraryWord, LibraryPage, ActivityDay, StatsSummary, RecentWord, ReviewItem, ReviewGroupEntry, Hint, MemoryState, ReviewFeedback, WordbookInfo, StudyCardEntry, StudySessionGroup, AiConfigView, AiExample, AiGenItem, FadingWord, LookupOutcome, TtsSynthesis, TtsVoiceList } from './generated/contracts';
export type { ComprehensionLevel, MemoryStatus, RecallQuality, StudyPriority, StudyGrade, PromptKind, PromptSource, HintKind, SetupPhase, Example, Collocation, Sense, WordFormEntry, RelatedWordItem, RootPart, WordEntry, Suggestion, MemorySummary, LookupWordResult, AppInfo, SetupProgress, EnsureDictionaryResult, WordNote, LibraryWord, LibraryPage, ActivityDay, StatsSummary, RecentWord, ReviewItem, ReviewGroupEntry, Hint, MemoryState, ReviewFeedback, WordbookInfo, StudyCardEntry, StudySessionGroup, AiConfigView, AiExample, AiGenItem, FadingWord, LookupOutcome, TtsSynthesis, TtsVoice, TtsVoiceList } from './generated/contracts';
export type MemoryStateLite = MemoryState;

export function errorMessage(error: unknown): string {
	if (error && typeof error === 'object' && 'message' in error) return String(error.message);
	return typeof error === 'string' ? error : 'Something went wrong. Try again.';
}
export const wordNote = (wordId: number) => invoke<WordNote>('word_note', { wordId });
export const saveWordNote = (wordId: number, bookmarked: boolean, note: string) =>
	invoke<void>('save_word_note', { wordId, bookmarked, note });
export const libraryWords = (query = '', filter = 'all', offset = 0) =>
	invoke<LibraryPage>('library_words', { query, filter, offset });
export const activityDays = () =>
	invoke<ActivityDay[]>('activity_days', { offsetMinutes: -new Date().getTimezoneOffset() });
export const statusLabel: Record<string, string> = {
	new: 'New',
	learning: 'Learning',
	familiar: 'Familiar',
	stable: 'Stable',
	ignored: 'Paused'
};

export async function appInfo(): Promise<AppInfo> {
	return invoke<AppInfo>('app_info');
}

/** Imports the full ECDICT database (stardict.db); returns newly added entries. Takes minutes. */
export async function importEcdict(sourcePath: string): Promise<number> {
	return invoke<number>('import_ecdict', { sourcePath });
}

/** Download ECDICT when missing, then import. Emits `dictionary-setup` progress events. */
export async function ensureDictionary(): Promise<EnsureDictionaryResult> {
	return invoke<EnsureDictionaryResult>('ensure_dictionary');
}

export async function searchSuggest(query: string, limit = 8): Promise<Suggestion[]> {
	return invoke<Suggestion[]>('search_suggest', { query, limit });
}

export async function lookupWord(word: string): Promise<LookupWordResult> {
	return invoke<LookupWordResult>('lookup_word', { word });
}

export async function setComprehension(
	encounterId: number,
	level: ComprehensionLevel
): Promise<void> {
	return invoke('set_comprehension', { encounterId, level });
}

export async function recentWords(limit = 6): Promise<RecentWord[]> {
	return invoke<RecentWord[]>('recent_words', { limit });
}

export async function statsSummary(): Promise<StatsSummary> {
	return invoke<StatsSummary>('stats_summary');
}

export async function setWordIgnored(wordId: number, ignored: boolean): Promise<void> {
	return invoke('set_word_ignored', { wordId, ignored });
}

export function studyGroup(
	wordbook: string,
	dailyNewLimit: number,
	priority: StudyPriority
): Promise<StudySessionGroup> {
	return invoke<StudySessionGroup>('study_group', { wordbook, dailyNewLimit, priority });
}

export function studyGrade(wordId: number, grade: StudyGrade): Promise<ReviewFeedback> {
	return invoke<ReviewFeedback>('study_grade', { wordId, grade });
}

export function wordbooksList(): Promise<WordbookInfo[]> {
	return invoke<WordbookInfo[]>('wordbooks_list');
}

export function customWordbookToggle(wordId: number, added: boolean): Promise<void> {
	return invoke<void>('custom_wordbook_toggle', { wordId, added });
}

export function aiEnrichWords(wordIds: number[], examStyle: boolean): Promise<number> {
	return invoke<number>('ai_enrich_words', { wordIds, examStyle });
}

export async function reviewQueue(): Promise<ReviewItem[]> {
	return invoke<ReviewItem[]>('review_queue');
}

/** Read-only group of due words (≤10): full entries + local prompts. No side effects. */
export async function reviewGroup(): Promise<ReviewGroupEntry[]> {
	return invoke<ReviewGroupEntry[]>('review_group');
}

export async function startReviewItem(item: ReviewItem): Promise<number> {
	return invoke<number>('start_review_item', {
		wordId: item.word_id,
		senseId: item.sense_id,
		exampleId: item.example_id
	});
}

/** In-group retry grading: read-only, reuses the Rust grader, never double-counts. */
export async function practiceCheck(
	wordId: number,
	answer: string | null
): Promise<{ correct: boolean }> {
	return invoke<boolean>('practice_check', { wordId, answer }).then(correct => ({ correct }));
}

/** In-group retry hints: read-only, same construction as the formal flow. */
export async function practiceHint(
	wordId: number,
	senseId: number | null,
	hintNo: number
): Promise<Hint> {
	return invoke('practice_hint', { wordId, senseId, hintNo });
}

export async function aiConfigGet(): Promise<AiConfigView> {
	return invoke<AiConfigView>('ai_config_get');
}

/**
 * Saves AI settings. `key`: null keeps the stored key, '' deletes it,
 * a non-empty string replaces it. The key lives in the OS credential store.
 */
export async function aiConfigSave(
	enabled: boolean,
	baseUrl: string,
	model: string,
	key: string | null
): Promise<AiConfigView> {
	return invoke<AiConfigView>('ai_config_save', { enabled, baseUrl, model, key });
}

export async function aiTestConnection(): Promise<void> {
	return invoke('ai_test_connection');
}

/** Generate and cache cloze-ready AI examples. Callers choose when generation is needed. */
export async function aiGenerateExamples(items: AiGenItem[]): Promise<AiExample[]> {
	return invoke<AiExample[]>('ai_generate_examples', { items });
}

export async function requestHint(reviewId: number, hintNo: number): Promise<Hint> {
	return invoke<Hint>('request_hint', { reviewId, hintNo });
}

export async function submitReview(
	reviewId: number,
	answer: string | null
): Promise<ReviewFeedback> {
	return invoke<ReviewFeedback>('submit_review', { reviewId, answer });
}

export async function fadingWords(limit = 20): Promise<FadingWord[]> {
	return invoke<FadingWord[]>('fading_words', { limit });
}

export function ttsVoices(): Promise<TtsVoiceList> {
	return invoke<TtsVoiceList>('tts_voices');
}

// A fresh document owns its cancellation sequence; reloads and other tabs are independent.
const ttsClientId = globalThis.crypto?.randomUUID?.() ??
	`tts-${Date.now()}-${Math.random().toString(36).slice(2)}`;

export function ttsSynthesize(args: {
	text: string;
	token: number;
	voiceId?: string | null;
	accent?: string | null;
	rate?: number | null;
}): Promise<TtsSynthesis> {
	return invoke<TtsSynthesis>('tts_synthesize', {
		text: args.text,
		token: args.token,
		clientId: ttsClientId,
		voiceId: args.voiceId ?? null,
		accent: args.accent ?? null,
		rate: args.rate ?? null
	});
}

export function ttsCancel(token: number): Promise<void> {
	return invoke<void>('tts_cancel', { token, clientId: ttsClientId });
}

export function dueLabel(iso: string | null): string {
	if (!iso) return '';
	const ms = new Date(iso).getTime() - Date.now();
	if (!Number.isFinite(ms)) return '';
	if (ms <= 0) return 'Due';
	if (ms < 3_600_000) return 'in under 1h';
	if (ms < 86_400_000) return `in ${Math.ceil(ms / 3_600_000)}h`;
	const d = Math.ceil(ms / 86_400_000);
	return `in ${d}d`;
}

export function relTime(iso: string): string {
	const ms = Date.now() - new Date(iso).getTime();
	if (!Number.isFinite(ms)) return '';
	const min = Math.floor(ms / 60_000);
	if (min < 1) return 'just now';
	if (min < 60) return `${min}m ago`;
	const h = Math.floor(min / 60);
	if (h < 24) return `${h}h ago`;
	return `${Math.floor(h / 24)}d ago`;
}
