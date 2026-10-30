<script lang="ts">
	import Icon from './Icon.svelte';
	type Schedule =
		| { kind: 'pending' | 'unknown' | 'unscheduled' | 'error' | 'paused' }
		| { kind: 'scheduled'; when: string };
	let {
		saved,
		miss,
		visitCount,
		statusText,
		schedule,
		onRefresh,
		onRetrySchedule,
	}: {
		saved: boolean;
		miss: boolean;
		visitCount: number | null;
		statusText: string | null;
		schedule: Schedule;
		onRefresh: () => void;
		onRetrySchedule: () => void;
	} = $props();
</script>

<aside class="hint" aria-label="查词学习记录">
	<div class="record-summary">
		{#if saved}
			<span class="saved"><Icon name="check" size={13} />查词记录已保存</span>
			{#if visitCount !== null}<span>查过 {visitCount.toLocaleString()} 次</span
				>{/if}
			{#if schedule.kind === 'paused'}<span>已暂停复习</span>
			{:else}
				{#if statusText}<span>{statusText}</span>{/if}
				{#if schedule.kind === 'scheduled'}<span>下次复习：{schedule.when}</span
					>{/if}
			{/if}
			{#if schedule.kind === 'error'}<button
					class="retry"
					type="button"
					onclick={onRetrySchedule}>重试读取复习安排</button
				>{/if}
		{:else if miss}<span>未找到词条</span>
		{:else}<span>本次查词未记录</span>{/if}
	</div>
	{#if saved}
		<div class="record-actions">
			<a href="/library">词库 <Icon name="arrow" size={12} /></a>
			<details>
				<summary aria-label="学习记录操作" title="学习记录操作">•••</summary>
				<div class="record-menu">
					<a href="/review?mode=due">进入到期复习</a>
					<button type="button" onclick={onRefresh}>刷新学习状态</button>
				</div>
			</details>
		</div>
	{/if}
</aside>

<style>
	.hint {
		display: flex;
		align-items: center;
		justify-content: space-between;
		flex-wrap: wrap;
		gap: 6px 14px;
		padding: 0 2px;
		margin: 12px 0 16px;
		min-height: 28px;
		color: var(--muted);
		font-size: 12px;
		line-height: 1.6;
	}
	.record-summary {
		display: flex;
		align-items: center;
		flex-wrap: wrap;
		gap: 4px 12px;
	}
	.record-summary > span:not(:first-child)::before {
		content: '·';
		margin-right: 12px;
		color: var(--line);
	}
	.saved {
		display: inline-flex;
		align-items: center;
		gap: 5px;
	}
	.record-actions {
		display: flex;
		align-items: center;
		gap: 12px;
		margin-left: auto;
	}
	a {
		display: inline-flex;
		align-items: center;
		gap: 4px;
		color: var(--muted);
		text-decoration: none;
	}
	a:hover {
		color: var(--accent);
	}
	details {
		position: relative;
	}
	summary {
		cursor: pointer;
		list-style: none;
		padding: 0 4px;
		letter-spacing: 2px;
	}
	summary::-webkit-details-marker {
		display: none;
	}
	.record-menu {
		position: absolute;
		right: 0;
		top: calc(100% + 8px);
		z-index: 30;
		display: grid;
		gap: 4px;
		min-width: 160px;
		padding: 8px;
		background: var(--card);
		border: 1px solid var(--line);
		border-radius: 10px;
		box-shadow: 0 8px 28px #0002;
	}
	.record-menu a,
	.record-menu button {
		padding: 8px 10px;
		font-size: 12px;
		text-align: left;
		justify-content: flex-start;
	}
	.record-menu button,
	.retry {
		border: 0;
		background: transparent;
		color: var(--muted);
		box-shadow: none;
	}
	.retry {
		padding: 0;
		font-size: 12px;
		text-decoration: underline;
	}
	@media (max-width: 600px) {
		.hint {
			font-size: 11px;
		}
		.record-summary {
			gap: 3px 8px;
		}
		.record-summary > span:not(:first-child)::before {
			margin-right: 8px;
		}
	}
</style>
