import adapter from '@sveltejs/adapter-static';
import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

/** @type {import('@sveltejs/kit').Config} */
const config = {
	preprocess: vitePreprocess(),
	kit: {
		csp: {
			mode: 'hash',
			directives: {
				'default-src': ['self'], 'script-src': ['self'],
				'style-src': ['self', 'unsafe-inline'], 'font-src': ['self'],
				'img-src': ['self', 'data:'], 'connect-src': ['self'],
				'object-src': ['none'], 'base-uri': ['self'], 'frame-src': ['none']
			}
		},
		// Tauri 以自定义协议服务静态文件。SPA 模式：所有路由 fallback 到 index.html。
		adapter: adapter({
			fallback: 'index.html'
		})
	}
};

export default config;
