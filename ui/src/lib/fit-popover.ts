/** Keep opened details panels inside the viewport without changing document flow. */
export function fitPopover(node: HTMLDetailsElement) {
	let frame = 0;
	function position() {
		if (!node.open) return;
		cancelAnimationFrame(frame);
		frame = requestAnimationFrame(() => {
			const panel = node.querySelector<HTMLElement>('[data-popover]');
			if (!panel || !node.open) return;
			panel.style.transform = '';
			const box = panel.getBoundingClientRect();
			const shift =
				box.left < 12
					? 12 - box.left
					: box.right > innerWidth - 12
						? innerWidth - 12 - box.right
						: 0;
			panel.style.transform = `translateX(${shift}px)`;
		});
	}
	node.addEventListener('toggle', position);
	window.addEventListener('resize', position);
	return {
		destroy() {
			cancelAnimationFrame(frame);
			node.removeEventListener('toggle', position);
			window.removeEventListener('resize', position);
		},
	};
}
