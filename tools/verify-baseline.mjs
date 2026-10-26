import fs from 'node:fs';
import crypto from 'node:crypto';
const root=new URL('../',import.meta.url);
const baseline=JSON.parse(fs.readFileSync(new URL('contracts/baseline.json',root),'utf8').replace(/^\uFEFF/,''));
for(const [file,expected] of Object.entries(baseline.files)){
  const raw=fs.readFileSync(new URL(file,root));
  const bytes=/\.(ts|json|sql)$/.test(file)?Buffer.from(raw.toString('utf8').replace(/\r\n/g,'\n')):raw;
  const hash=crypto.createHash('sha256').update(bytes).digest('hex');
  if(hash!==expected)throw Error(`Pinned upstream asset changed: ${file}`);
}
console.log(`Pinned upstream ${baseline.upstream_sha}: ${Object.keys(baseline.files).length} files verified.`);
