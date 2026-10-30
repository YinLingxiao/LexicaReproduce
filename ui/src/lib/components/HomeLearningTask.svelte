<script lang="ts">
	import Icon from './Icon.svelte';

	type TaskState =
		| { kind: 'loading' }
		| { kind: 'error'; message: string }
		| { kind: 'new' }
		| { kind: 'due'; count: number }
		| { kind: 'clear' };

	let {
		state,
		onRetry,
		onFocusSearch
	}: {
		state: TaskState;
		onRetry: () => void;
		onFocusSearch: () => void;
	} = $props();
</script>

<section class="task" aria-labelledby="home-task-title" aria-busy={state.kind === 'loading'} aria-live="polite">
	{#if state.kind === 'loading'}
		<h2 id="home-task-title" class="status">正在读取学习记录</h2>
	{:else if state.kind === 'error'}
		<h2 id="home-task-title">学习记录没有读出来</h2>
		<p class="error" role="alert">{state.message}</p>
		<button type="button" onclick={onRetry}>重试</button>
	{:else if state.kind === 'new'}
		<h2 id="home-task-title">先查一个词</h2>
		<p>还没有查词记录。查到的词会留下学习记录，之后可以反馈理解程度，并按计划复习。</p>
		<button type="button" class="primary" onclick={onFocusSearch}>去查第一个词</button>
	{:else if state.kind === 'due'}
		<h2 id="home-task-title">当前待复习 {state.count.toLocaleString()} 个</h2>
		<p>这些是已经到期的词。开始复习只会进入到期队列。</p>
		<a class="button primary" href="/review?mode=due">开始复习 <Icon name="arrow" size={16} /></a>
	{:else}
		<h2 id="home-task-title">当前暂无到期复习</h2>
		<p>已有学习记录，现在没有到期的词。</p>
		<div class="actions">
			<button type="button" class="primary" onclick={onFocusSearch}>继续查词</button>
			<a class="button" href="/library">查看词库</a>
			<a class="button" href="/review?mode=new">学习新词</a>
		</div>
		<p class="note">学习新词来自当前词书，不是到期复习。</p>
	{/if}
</section>

<style>
	.task {
		background: var(--card);
		border: 1px solid var(--line);
		border-radius: 16px;
		padding: 22px 24px;
	}
	.task h2 {
		font: 500 28px/1.2 var(--serif);
		margin-bottom: 8px;
	}
	.task p {
		color: var(--muted);
		font-size: 14px;
		line-height: 1.55;
		margin-bottom: 16px;
		max-width: 62ch;
	}
	.status {
		margin: 0;
		color: var(--muted);
		font: 500 18px/1.4 var(--serif);
	}
	.actions {
		display: flex;
		flex-wrap: wrap;
		gap: 10px;
	}
	.task :global(a.button),
	.task button {
		min-height: 44px;
	}
	.note {
		margin: 10px 0 0;
		font-size: 12px;
	}
	@media (max-width: 700px) {
		.task {
			padding: 18px;
		}
		.task h2 {
			font-size: 24px;
		}
		.actions {
			display: grid;
		}
		.task :global(a.button),
		.task button {
			width: 100%;
		}
	}
</style>
