<script lang="ts">
	type Schedule =
		| { kind: 'pending' }
		| { kind: 'unknown' }
		| { kind: 'scheduled'; when: string }
		| { kind: 'unscheduled' }
		| { kind: 'error' };

	let {
		saved,
		miss,
		visitCount,
		statusText,
		schedule,
		onRefresh,
		onRetrySchedule
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
	{#if miss}
		<p>没有查到这个词，这次没有写入学习记录。</p>
	{:else if saved}
		<p class="saved">查词记录已保存</p>
		<ul>
			{#if visitCount !== null}<li>已记录查词 {visitCount.toLocaleString()} 次</li>{/if}
			{#if statusText}<li>学习状态：{statusText}</li>{/if}
			{#if schedule.kind === 'pending'}<li role="status">正在读取复习安排</li>{/if}
			{#if schedule.kind === 'scheduled'}
				<li>已安排复习{schedule.when ? ` · ${schedule.when}` : ''}</li>
			{/if}
			{#if schedule.kind === 'unscheduled'}<li>词库记录里没有下次复习时间</li>{/if}
			{#if schedule.kind === 'error'}
				<li class="error" role="alert">复习安排没有读出来</li>
			{/if}
		</ul>
		{#if schedule.kind === 'error'}
			<button type="button" onclick={onRetrySchedule}>重试读取复习安排</button>
		{/if}
		<div class="links">
			<a href="/library">查看个人词库</a>
			<a href="/review?mode=due">进入到期复习</a>
			<button type="button" class="ghost" onclick={onRefresh}>刷新学习状态</button>
		</div>
	{:else}
		<p>这次查词没有返回学习记录。</p>
	{/if}
</aside>

<style>
	.hint {
		margin-bottom: 18px;
		padding: 14px 16px;
		border: 1px solid var(--line);
		border-radius: 12px;
		background: var(--soft);
	}
	.saved {
		margin: 0 0 8px;
		font: 500 18px/1.3 var(--serif);
		color: var(--fg);
	}
	.hint p {
		margin: 0;
		color: var(--muted);
		font-size: 13px;
	}
	ul {
		margin: 0;
		padding-left: 18px;
		color: var(--fg);
		font-size: 13px;
		line-height: 1.6;
	}
	.links {
		display: flex;
		flex-wrap: wrap;
		gap: 8px 14px;
		align-items: center;
		margin-top: 10px;
	}
	.links a,
	.links button {
		min-height: 44px;
		font-size: 13px;
	}
	.hint > button {
		min-height: 44px;
		margin-top: 8px;
	}
	@media (max-width: 700px) {
		.links {
			display: grid;
		}
		.links a,
		.links button,
		.hint > button {
			width: 100%;
		}
	}
</style>
