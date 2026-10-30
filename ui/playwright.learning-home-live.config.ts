import {defineConfig} from '@playwright/test';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

process.env.NO_PROXY = [process.env.NO_PROXY, '127.0.0.1', 'localhost'].filter(Boolean).join(',');
const ui = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(ui, '..');
const port = Number(process.env.LEXICA_HOME_LIVE_PORT ?? 18773);
const exe = process.env.LEXICA_SERVER_EXE || 'D:\\C++自选项目\\build-parity-msvc\\src\\parity\\Release\\lexica_parity_server.exe';
const data = path.join(root, 'tmp-learning-home-live');

export default defineConfig({
	testDir: './tests',
	testMatch: '**/learning-home.live.mjs',
	workers: 1,
	timeout: 60000,
	use: {
		baseURL: `http://127.0.0.1:${port}`,
		channel: 'msedge',
		headless: true,
		viewport: {width: 1180, height: 820}
	},
	webServer: {
		command: `"${exe}" --port ${port} --no-auto-setup --data-dir "${data}" --ui-dir "${path.join(ui, 'build')}"`,
		url: `http://127.0.0.1:${port}`,
		reuseExistingServer: false,
		timeout: 20000
	},
	reporter: 'list'
});
