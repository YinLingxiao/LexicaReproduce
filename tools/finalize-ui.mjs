// Hash the actual SPA bootstrap, including whitespace. The static fallback does
// not consistently include SvelteKit CSP metadata; the native host serves this
// generated policy as a response header. No unsafe-inline script permission.
import fs from 'node:fs';
import crypto from 'node:crypto';
const base = new URL('../ui/build/', import.meta.url);
const html = fs.readFileSync(new URL('index.html', base), 'utf8');
const hashes = [...html.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/gi)]
  .filter(([, attributes]) => !/\bsrc\s*=/.test(attributes))
  .map(([, , body]) => `'sha256-${crypto.createHash('sha256').update(body.replace(/\r\n/g, '\n')).digest('base64')}'`);
const policy = `default-src 'self'; script-src 'self' ${hashes.join(' ')}; style-src 'self' 'unsafe-inline'; font-src 'self'; img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'self'; frame-src 'none'`;
fs.writeFileSync(new URL('csp.txt', base), policy);
console.log('SPA content security policy generated.');
