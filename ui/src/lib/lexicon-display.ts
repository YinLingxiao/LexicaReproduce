import type { Sense, WordEntry } from '$lib/api';

export const CEFR_LEVELS = ['A1', 'A2', 'B1', 'B2', 'C1', 'C2'] as const;
export type CefrLevel = (typeof CEFR_LEVELS)[number];

export const NO_LEVEL_LABEL = '暂无等级数据';
export const NO_FREQUENCY_LABEL = '暂无词频数据';

const KNOWN_FREQUENCY_SOURCES = new Set(['ecdict.frq', 'ecdict.bnc']);

export function normalizeCefrLevel(raw: string | null | undefined): CefrLevel | null {
	const value = raw?.trim().toUpperCase();
	return CEFR_LEVELS.includes(value as CefrLevel) ? (value as CefrLevel) : null;
}

/** Returns the level only when it is one of the six standard CEFR bands. */
export function senseLevel(sense: Sense | null | undefined): CefrLevel | null {
	return normalizeCefrLevel(sense?.level);
}

export function levelSourceLabel(source: string | null | undefined): string {
	switch (source?.trim()) {
		case 'seed':
			return '词表标注';
		case 'ecdict_inferred':
			return '估算';
		case 'legacy_unverified':
			return '旧版推断';
		case 'official':
			return '官方分级';
		default:
			return '来源未记录';
	}
}

/** True unless the level has an explicit, non-inferred source. */
export function isEstimatedLevel(source: string | null | undefined): boolean {
	const value = source?.trim();
	return value !== 'seed' && value !== 'official';
}

export interface LevelDisplay {
	level: CefrLevel | null;
	source: string | null;
	sourceLabel: string;
	estimated: boolean;
	label: string;
}

export function describeLevel(sense: Sense | null | undefined): LevelDisplay {
	const level = senseLevel(sense);
	if (!level) {
		return {
			level: null,
			source: null,
			sourceLabel: levelSourceLabel(null),
			estimated: false,
			label: NO_LEVEL_LABEL
		};
	}
	const source = sense?.level_source?.trim() || null;
	const sourceLabel = levelSourceLabel(source);
	const estimated = isEstimatedLevel(source);
	return {
		level,
		source,
		sourceLabel,
		estimated,
		label: estimated ? `${level} · 估算` : `${level} · ${sourceLabel}`
	};
}

export function cefrPosition(level: CefrLevel | null): number {
	return level ? CEFR_LEVELS.indexOf(level) : -1;
}

export interface FrequencyDisplay {
	rank: number | null;
	source: string | null;
	sourceLabel: string;
	hasRank: boolean;
	/** False for seed values that are only a rough suggestion-ordering hint. */
	isCorpusRank: boolean;
	ticks: number;
	label: string;
	explanation: string;
}

const FREQUENCY_TICKS = 18;

export function frequencyTicks(rank: number): number {
	if (rank <= 1000) return 16;
	if (rank <= 2500) return 14;
	if (rank <= 5000) return 12;
	if (rank <= 8000) return 10;
	if (rank <= 12000) return 7;
	if (rank <= 20000) return 5;
	return 3;
}

export function describeFrequency(entry: WordEntry | null | undefined): FrequencyDisplay {
	const rawRank = entry?.frequency_rank;
	const source = entry?.frequency_source?.trim() || null;
	const rank =
		typeof rawRank === 'number' && Number.isFinite(rawRank) && rawRank > 0 ? rawRank : null;
	const isCorpusRank =
		rank !== null && source !== null && KNOWN_FREQUENCY_SOURCES.has(source);
	const sourceLabel =
		source === 'ecdict.frq'
			? 'ECDICT 当代语料库词频顺序'
			: source === 'ecdict.bnc'
				? 'ECDICT 英国国家语料库词频顺序'
				: source === 'seed_approx'
					? '种子词库近似排序值'
					: source && KNOWN_FREQUENCY_SOURCES.has(source)
						? source
						: rank
							? '来源未记录'
							: '暂无来源';
	if (!rank || source === 'seed_approx') {
		return {
			rank: source === 'seed_approx' ? null : rank,
			source,
			sourceLabel,
			hasRank: false,
			isCorpusRank: false,
			ticks: 0,
			label: NO_FREQUENCY_LABEL,
			explanation:
				source === 'seed_approx'
					? '种子词库只有近似排序值，不作为词频展示。'
					: '没有可用的词频排名。'
		};
	}
	return {
		rank,
		source,
		sourceLabel,
		hasRank: true,
		isCorpusRank,
		ticks: frequencyTicks(rank),
		label: `第 ${rank.toLocaleString('zh-CN')} 名`,
		explanation: '排名越小，越常见。'
	};
}

export interface CollinsDisplay {
	stars: number;
	label: string;
}

export function describeCollins(entry: WordEntry | null | undefined): CollinsDisplay | null {
	const value = entry?.collins;
	if (typeof value !== 'number' || !Number.isInteger(value) || value < 1 || value > 5)
		return null;
	return { stars: value, label: `Collins ${'★'.repeat(value)}${'☆'.repeat(5 - value)}` };
}

export function displayPos(pos: string | null | undefined): string | null {
	const value = pos?.trim();
	return value ? value : null;
}

export { FREQUENCY_TICKS };
