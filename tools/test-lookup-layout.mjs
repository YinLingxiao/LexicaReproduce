import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
process.env.NO_PROXY = [process.env.NO_PROXY, '127.0.0.1', 'localhost'].filter(Boolean).join(',');
const {chromium} = createRequire(path.join(root, 'ui/package.json'))('@playwright/test');
const data = path.resolve(process.argv[2]);
assert(data.toLowerCase().startsWith(path.join(root, 'build-parity-msvc/test-output').toLowerCase()+path.sep));
const exe = path.resolve(root, process.argv[3] ?? 'build-parity-msvc/src/parity/Debug/lexica_parity_server.exe');
const ui = path.resolve(root, process.argv[4] ?? 'ui/build');
const port = await new Promise(resolve => { const s=net.createServer();s.listen(0,'127.0.0.1',()=>{const p=s.address().port;s.close(()=>resolve(p));}); });
const base = `http://127.0.0.1:${port}`;
const began = Date.now();
const child = spawn(exe,['--port',String(port),'--data-dir',data,'--no-auto-setup','--ui-dir',ui],{windowsHide:true,stdio:['ignore','pipe','pipe']});
let browser;
try {
 await new Promise((resolve,reject)=>{
  const timer=setTimeout(()=>reject(Error('Server startup timed out')),20000);
  child.on('error',reject);child.stdout.on('data',value=>{if(String(value).includes('development server')){clearTimeout(timer);resolve();}});
 });
 const startupMs=Date.now()-began;
 const rpc=async(command,args={})=>{
  const reply=await(await fetch(base+'/rpc',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({protocol_version:1,request_id:crypto.randomUUID(),command,args})})).json();
  assert(reply.ok,JSON.stringify(reply.error));return reply.result;
 };
 const info=await rpc('app_info');
 assert(info.word_count>3_000_000,'This regression requires the complete dictionary copy');
 const ai=await rpc('ai_config_get');
 if(ai.enabled) await rpc('ai_config_save',{enabled:false,baseUrl:ai.base_url,model:ai.model,key:null});
 const found=await rpc('lookup_word',{word:'happy'});
 const wordId=found.outcome.entry.id;
 await rpc('set_word_ignored',{wordId,ignored:true});
 await rpc('save_word_note',{wordId,bookmarked:true,note:'学习笔记'});
 browser=await chromium.launch({channel:'msedge',headless:true});
 const context=await browser.newContext({viewport:{width:1180,height:820},deviceScaleFactor:1.5,reducedMotion:'reduce'});
 await context.addInitScript(()=>{
  localStorage.setItem('lexica.preferences',JSON.stringify({theme:'dark',edition:'obsidian',reading:'guided'}));
  window.__playing=0;
  const play=HTMLMediaElement.prototype.play;
  HTMLMediaElement.prototype.play=function(){this.addEventListener('playing',()=>window.__playing++,{once:true});return play.call(this);};
 });
 const page=await context.newPage();
 const errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.goto(base+'/?word=happy');
 await page.getByText('已暂停复习',{exact:true}).waitFor();
 await page.evaluate(()=>document.fonts.ready);
 await page.getByRole('button',{name:'朗读单词',exact:true}).click();
 await page.waitForFunction(()=>window.__playing>0);
 const hint=await page.getByLabel('查词学习记录').boundingBox();
 const hero=await page.locator('.hero-card').boundingBox();
 const definition=await page.locator('.word-card').boundingBox();
 assert(hint.height<65,`Learning hint too large: ${hint.height}`);
 assert(hero.height<390,`Hero too large: ${hero.height}`);
 assert(definition.y<700,`Definitions pushed below the first viewport: ${definition.y}`);
 const meaning=await page.locator('.definition-preview p').textContent();
 assert(meaning.trim().length>2 && !meaning.includes('暂无释义'),'A word without examples must show its available definition');
 assert.equal(await page.getByText(/下次复习/).count(),0,'Paused word displays a stale review date');
 assert.equal(await page.locator('.speech-popover').isVisible(),false);
 assert.equal(await page.locator('.metadata-panel').isVisible(),false);
 await page.evaluate(()=>window.scrollTo(0,0));
 await page.screenshot({path:path.join(data,'happy-dark-desktop.png')});
 await page.getByLabel('查看发音信息').click();
 await page.locator('.speech-popover').waitFor();
 await page.getByLabel('查看发音信息').click();
 await page.setViewportSize({width:480,height:820});
 assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'Compact layout overflows');
 for (const selector of ['.speech-details summary','.metadata-details summary']) {
  await page.locator(selector).click();
  await page.waitForFunction(() => [...document.querySelectorAll('details[open] [data-popover]')].every(node=>{
   const box=node.getBoundingClientRect();return box.left>=10&&box.right<=innerWidth-10;
  }));
  await page.locator(selector).click();
 }
 await page.evaluate(()=>window.scrollTo(0,0));
 await page.screenshot({path:path.join(data,'happy-dark-480.png')});
 const feedback=found.comprehension_level==='chinese' ? null : page.waitForRequest(req=>req.method()==='POST'&&req.url().endsWith('/rpc')&&req.postDataJSON().command==='set_comprehension');
 await page.getByRole('button',{name:'Understood'}).click();
 if(feedback) assert.equal((await feedback).postDataJSON().args.level,'chinese','Visible Chinese fallback must not be recorded as English-only comprehension');
 await page.locator('.definitions').waitFor();
 assert.deepEqual(errors,[]);
 const result={ok:true,word_count:info.word_count,server_startup_ms:startupMs,hint_height:hint.height,hero_height:hero.height,definitions_top:definition.y,definition_visible_without_example:true,fallback_comprehension_chinese:true,paused_schedule_correct:true,real_audio_played:true,errors};
 fs.writeFileSync(path.join(data,'layout-report.json'),JSON.stringify(result,null,2));
 console.log(JSON.stringify({...result,data}));
} finally { try { await browser?.close(); } finally { child.kill(); } }
