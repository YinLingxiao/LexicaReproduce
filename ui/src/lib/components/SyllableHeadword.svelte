<script lang="ts">
	import { headwordPieces } from '$lib/syllables';

	let { word, split = true }: { word: string; split?: boolean } = $props();

	const pieces = $derived(headwordPieces(word, split));
	const dots = $derived(pieces.map((piece) => (piece.dotBefore ? 1 : 0)));
</script>

{#key word}<span class="syllable-headword" aria-label={word}
		>{#each pieces as piece, i}{#if piece.dotBefore}<span
					class="syllable-dot"
					style:--i={pieces.slice(0, i).filter((item) => item.dotBefore).length}
					aria-hidden="true">·</span
				>{/if}<span aria-hidden="true">{piece.text}</span>{/each}</span
	>{/key}

<style>
	.syllable-headword {
		display: inline;
		white-space: pre-wrap;
	}
	.syllable-dot {
		display: inline-block;
		width: 0.3em;
		margin: 0 0.04em;
		text-align: center;
		overflow: clip;
		vertical-align: baseline;
		font-weight: 400;
		color: color-mix(in srgb, var(--accent) 65%, var(--kraft));
		animation: syllable-split 640ms var(--ease) calc(380ms + var(--i, 0) * 80ms) backwards;
	}
	@keyframes syllable-split {
		from {
			width: 0;
			margin: 0;
			opacity: 0;
			transform: translateY(0.18em) scale(0.3);
		}
		60% {
			opacity: 1;
		}
	}
</style>
