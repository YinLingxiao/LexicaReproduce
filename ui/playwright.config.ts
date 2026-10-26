import {defineConfig} from '@playwright/test';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const ui=path.dirname(fileURLToPath(import.meta.url));
const root=path.resolve(ui,'..');
process.env.LEXICA_TEST_PORT ??= String(18760+process.pid%10000);
const port=Number(process.env.LEXICA_TEST_PORT);
const exe=path.join(root,'build-parity-msvc/src/parity/Debug/lexica_parity_server.exe');
const data=path.join(root,'build-parity-msvc/test-output/playwright',`${process.pid}-${Date.now()}`);
export default defineConfig({
  testDir:'./tests', workers:1, timeout:30000,
  use:{baseURL:`http://127.0.0.1:${port}`,channel:'msedge',headless:true,viewport:{width:1180,height:820}},
  webServer:{command:`"${exe}" --port ${port} --data-dir "${data}" --ui-dir "${path.join(ui,'build')}"`,url:`http://127.0.0.1:${port}`,reuseExistingServer:false,timeout:15000},
  reporter:'list'
});
