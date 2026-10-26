// One-time source migration; re-run only with an unchanged upstream-api.ts snapshot.
import fs from 'node:fs';
import ts from '../ui/node_modules/typescript/lib/typescript.js';
const original = fs.readFileSync(new URL('../contracts/upstream-api.ts', import.meta.url), 'utf8');
const source = ts.createSourceFile('api.ts', original, ts.ScriptTarget.Latest, true);
const removed = [];
for (const node of source.statements) {
  const name = node.name?.text;
  if (ts.isInterfaceDeclaration(node) || ts.isTypeAliasDeclaration(node) || ts.isImportDeclaration(node) || (ts.isFunctionDeclaration(node) && name === 'invoke') || (ts.isVariableStatement(node) && node.declarationList.declarations.some(d => d.name.getText(source) === 'isPreview'))) removed.push([node.getFullStart(), node.end]);
}
let output = original;
for (const [start,end] of removed.reverse()) output = output.slice(0,start) + output.slice(end);
const models = JSON.parse(fs.readFileSync(new URL('../contracts/models.json', import.meta.url)));
const names = [...Object.keys(models.enums), ...Object.keys(models.models), ...Object.keys(models.unions)];
output = `// Adapted from Lexica ${JSON.parse(fs.readFileSync(new URL('../contracts/commands.json', import.meta.url))).upstream_sha}; MIT.\nimport { invoke, isPreview } from './transport';\nexport { isPreview } from './transport';\nimport type { ${names.join(', ')} } from './generated/contracts';\nexport type { ${names.join(', ')} } from './generated/contracts';\nexport type MemoryStateLite = MemoryState;\n` + output;
output = output.replace("return invoke('practice_check', { wordId, answer });", "return invoke<boolean>('practice_check', { wordId, answer }).then(correct => ({ correct }));");
fs.writeFileSync(new URL('../ui/src/lib/api.ts', import.meta.url), output);
