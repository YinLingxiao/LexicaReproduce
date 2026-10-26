import {defineConfig} from '@playwright/test';
process.env.NO_PROXY = [process.env.NO_PROXY, '127.0.0.1', 'localhost'].filter(Boolean).join(',');
const port=Number(process.env.LEXICA_FIXTURE_PORT??5199);
export default defineConfig({
  testDir:'./tests', testMatch:'**/parity.spec.ts', workers:1, timeout:45000,
  use:{baseURL:`http://127.0.0.1:${port}`,channel:'msedge',headless:true,viewport:{width:1180,height:820},colorScheme:'light'},
  webServer:{command:`npm run dev -- --host 127.0.0.1 --port ${port} --strictPort`,url:`http://127.0.0.1:${port}`,reuseExistingServer:false,timeout:120000},
  reporter:'list'
});
