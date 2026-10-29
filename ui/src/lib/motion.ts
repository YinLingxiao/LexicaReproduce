import { slide } from 'svelte/transition';
import { cubicOut } from 'svelte/easing';

/** Read the preference at interaction time, including changes made while open. */
export function disclosure(node: Element) {
	return slide(node, {
		duration: typeof matchMedia !== 'undefined' && matchMedia('(prefers-reduced-motion: reduce)').matches ? 0 : 220,
		easing: cubicOut
	});
}
