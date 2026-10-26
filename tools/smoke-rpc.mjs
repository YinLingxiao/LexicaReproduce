import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const exe=path.resolve(root,process.argv[2]??'build-parity-msvc/src/parity/Debug/lexica_parity_server.exe');
const data=path.join(root,'build-parity-msvc/test-output/rpc',`${process.pid}-${Date.now()}`,'中文数据库');
fs.mkdirSync(data,{recursive:true});
const port=await new Promise(resolve=>{const server=net.createServer();server.listen(0,'127.0.0.1',()=>{const p=server.address().port;server.close(()=>resolve(p));});});
const child=spawn(exe,['--port',String(port),'--data-dir',data,'--ui-dir',path.join(root,'ui/build')],{cwd:root,windowsHide:true,stdio:['ignore','pipe','pipe']});
let errors='';child.stderr.on('data',chunk=>errors+=chunk);
try{
  await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('Backend startup timeout: '+errors)),10000);child.on('error',reject);child.once('exit',code=>{clearTimeout(timer);reject(Error(`Backend exited ${code}: ${errors}`));});child.stdout.on('data',chunk=>{if(String(chunk).includes('development server')){clearTimeout(timer);resolve();}});});
  const base=`http://127.0.0.1:${port}`;let sequence=0;
  const rpc=async(command,args={})=>{const request_id=`rpc-${++sequence}`;const r=await fetch(base+'/rpc',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({protocol_version:1,request_id,command,args})});assert.equal(r.status,200);const result=await r.json();assert.equal(result.request_id,request_id);return result;};
  const info=await rpc('app_info');assert.equal(info.ok,true);assert.equal(info.result.word_count,55);assert.equal(info.result.schema_version,5);assert.equal(info.result.fts_ok,true);
  const caps=await rpc('system_capabilities');assert.equal(caps.ok,true);assert.equal(caps.result.contract_version,'1.1.0');
  const note=caps.result.commands.find(command=>command.name==='save_word_note');
  if(!note.available)assert.equal((await rpc('save_word_note',{wordId:1,bookmarked:true,note:'test'})).error.code,'NOT_IMPLEMENTED');
  const hit=await rpc('lookup_word',{word:'meticulous'});assert.equal(hit.result.outcome.entry.word,'meticulous');
  assert.equal((await rpc('lookup_word',{word:'there-is-no-such-word'})).result.outcome.type,'miss');
  assert.equal((await rpc('search_suggest',{query:'met'})).result[0].word,'meticulous');
  assert.equal((await rpc('search_suggest',{query:'met',limit:'wrong'})).error.code,'BAD_REQUEST');
  assert.equal((await rpc('unknown')).error.code,'UNKNOWN_COMMAND');
  assert.equal((await fetch(base+'/rpc',{method:'POST',headers:{'Content-Type':'application/json',Origin:'https://untrusted.example'},body:'{}'})).status,403);
  assert.equal((await fetch(base+'/rpc',{method:'POST',headers:{'Content-Type':'text/plain'},body:'{}'})).status,415);
  const malformed=await fetch(base+'/rpc',{method:'POST',headers:{'Content-Type':'application/json'},body:'not-json'});assert.equal((await malformed.json()).error.code,'BAD_REQUEST');
  const page=await fetch(base+'/library');assert.equal(page.status,200);assert.match(await page.text(),/<title>Lexica<\/title>/);assert.match(page.headers.get('content-security-policy'),/sha256-/);
  const stream=await fetch(base+'/events');assert.equal(stream.status,200);assert.match(stream.headers.get('content-type')??'',/text\/event-stream/);
  const reader=stream.body.getReader();const decoder=new TextDecoder();let text='';const began=Date.now();
  while(!text.includes('ready')&&Date.now()-began<5000){const chunk=await reader.read();if(chunk.done)break;text+=decoder.decode(chunk.value,{stream:true});}
  assert.match(text,/ready/);
  const during=await rpc('lookup_word',{word:'meticulous'});assert.equal(during.ok,true);assert.equal(during.result.outcome.type,'hit');
  await reader.cancel();
  const result={ok:true,checks:14,transport:'real HTTP / C++ / SQLite',schema_version:info.result.schema_version,seed_words:info.result.word_count,unicode_path:true,events:true,contract_version:caps.result.contract_version};
  fs.writeFileSync(path.join(data,'report.json'),JSON.stringify(result,null,2));console.log(JSON.stringify(result));
}finally{child.kill();}
