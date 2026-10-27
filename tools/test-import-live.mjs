// Import a real ECDICT corpus while repeatedly exercising the learning lookup.
// All writes go to a unique test database, never to the source corpus.
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const source=process.argv[2];
assert.ok(source && fs.existsSync(source),'Pass an existing ECDICT SQLite source');
const exe=path.resolve(root,process.argv[3]??'build-parity-msvc/src/parity/Debug/lexica_parity_server.exe');
const output=path.join(root,'build-parity-msvc/test-output/import-live',`${Date.now()}-${process.pid}`);
fs.mkdirSync(output,{recursive:true});
const data=path.join(output,'中文数据库');
const port=await new Promise(resolve=>{const server=net.createServer();server.listen(0,'127.0.0.1',()=>{const port=server.address().port;server.close(()=>resolve(port));});});
let child;
async function start(){
  child=spawn(exe,['--port',String(port),'--no-auto-setup','--data-dir',data,'--ui-dir',path.join(root,'ui/build')],{cwd:root,windowsHide:true,stdio:['ignore','pipe','pipe']});
  await new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>{reject(Error('Server startup timed out'));},15000);
    child.once('error',error=>{clearTimeout(timer);reject(error);});
    child.once('exit',code=>{clearTimeout(timer);reject(Error(`Server exited ${code}`));});
    child.stdout.on('data',chunk=>{if(String(chunk).includes('development server')){clearTimeout(timer);resolve();}});
    child.stderr.on('data',chunk=>fs.appendFileSync(path.join(output,'stderr.log'),chunk));
  });
}
async function stop(){if(!child || child.exitCode!==null)return;const done=new Promise(resolve=>child.once('exit',resolve));child.kill();await done;}
let sequence=0;
async function rpc(command,args={},timeout=10000){
  const response=await fetch(`http://127.0.0.1:${port}/rpc`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({protocol_version:1,request_id:`import-live-${++sequence}`,command,args}),signal:AbortSignal.timeout(timeout)});
  assert.equal(response.status,200);
  return response.json();
}
const report={ok:false,source:path.resolve(source),data,lookups:0,hits:0,failures:[],max_lookup_ms:0};
try{
  await start();
  let finished=false;
  const began=Date.now();
  // Convert rejection to a value immediately, so the lookup loop cannot leave
  // an unhandled rejection while an import failure is waiting to be reported.
  const imported=rpc('import_ecdict',{sourcePath:path.resolve(source)},1200000)
    .then(value=>({value}),error=>({error:String(error)})).finally(()=>{finished=true;});
  while(!finished){
    const started=Date.now();
    try{
      const result=await rpc('lookup_word',{word:'meticulous'},12000);
      report.lookups++;
      if(result.ok && result.result?.outcome?.type==='hit')report.hits++;
      else report.failures.push({elapsed_ms:Date.now()-began,result});
    }catch(error){report.lookups++;report.failures.push({elapsed_ms:Date.now()-began,error:String(error)});}
    report.max_lookup_ms=Math.max(report.max_lookup_ms,Date.now()-started);
    if(report.lookups%25===0)console.log(`lookups=${report.lookups} hits=${report.hits} failures=${report.failures.length} elapsed=${Math.round((Date.now()-began)/1000)}s`);
    if(!finished)await new Promise(resolve=>setTimeout(resolve,200));
  }
  const result=await imported;
  report.import_ms=Date.now()-began;
  assert.ok(!result.error,result.error);
  assert.equal(result.value.ok,true,JSON.stringify(result.value));
  report.imported=result.value.result;
  assert.ok(report.imported>1000000,'Use a complete ECDICT corpus for this test');
  const info=await rpc('app_info');
  assert.equal(info.ok,true);assert.equal(info.result.dictionary_ready,true);
  report.info=info.result;
  await stop();
  await start();
  const reopened=await rpc('app_info');
  assert.deepEqual(reopened.result,report.info);
  report.restart_verified=true;
  assert.equal(report.failures.length,0,'Lookups failed during import');
  assert.equal(report.hits,report.lookups);
  report.ok=true;
}catch(error){report.error=String(error);process.exitCode=1;}
finally{await stop();fs.writeFileSync(path.join(output,'report.json'),JSON.stringify(report,null,2));console.log(JSON.stringify(report));}
