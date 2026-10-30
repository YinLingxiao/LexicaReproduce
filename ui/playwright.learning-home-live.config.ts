import {defineConfig} from '@playwright/test';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import fs from 'node:fs';

process.env.NO_PROXY = [process.env.NO_PROXY, '127.0.0.1', 'localhost'].filter(Boolean).join(',');
const ui = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(ui, '..');
const port = Number(process.env.LEXICA_HOME_LIVE_PORT ?? 18773);
const exe = process.env.LEXICA_SERVER_EXE || path.join(root, 'build-parity-msvc/src/parity/Debug/lexica_parity_server.exe');
const buildUi = process.env.LEXICA_UI_DIR || path.join(ui, 'build');
const data = process.env.LEXICA_HOME_LIVE_DATA ?? path.join(root, 'build-parity-msvc/test-output/learning-home-live', `${process.pid}-${Date.now()}`);
fs.mkdirSync(data, {recursive: true});
process.env.LEXICA_HOME_LIVE_DATA = data;

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
		command: `"${exe}" --port ${port} --no-auto-setup --data-dir "${data}" --ui-dir "${buildUi}"`,
		url: `http://127.0.0.1:${port}`,
		reuseExistingServer: false,
		timeout: 20000
	},
	reporter: 'list'
});
