import type { CommandMap } from './generated/contracts';

type Response = {protocol_version: number; request_id: string; ok: boolean; result?: unknown; error?: {code: string; message: string}};
type EventMessage = {event: string; payload: unknown};
type Bridge = {postMessage: (message: unknown) => void; addEventListener: (name: 'message', fn: (event: MessageEvent) => void) => void};
const pending = new Map<string, {resolve: (value: unknown) => void; reject: (error: Error) => void; timer: ReturnType<typeof setTimeout>}>();
const events = new Map<string, Set<(event: {payload: unknown}) => void>>();
let listening = false;
let sequence = 0;
export const isPreview = () => import.meta.env.DEV && import.meta.env.VITE_LEXICA_PREVIEW === '1';

export class RpcError extends Error {
  constructor(public code: string, message: string) { super(message); this.name = 'RpcError'; }
}
function bridge(): Bridge | undefined {
  return typeof window === 'undefined' ? undefined : (window as unknown as {chrome?: {webview?: Bridge}}).chrome?.webview;
}
function attach(native: Bridge) {
  if (listening) return;
  listening = true;
  native.addEventListener('message', event => {
    const data = event.data as Response & EventMessage;
    if (data.protocol_version !== 1) return;
    if (typeof data.event === 'string') { for (const fn of events.get(data.event) ?? []) fn({payload: data.payload}); return; }
    const task = pending.get(data.request_id);
    if (!task) return;
    clearTimeout(task.timer); pending.delete(data.request_id);
    if (data.ok) task.resolve(data.result); else task.reject(new RpcError(data.error?.code ?? 'INTERNAL_ERROR', data.error?.message ?? 'Command failed'));
  });
  window.addEventListener('beforeunload', () => {
    for (const task of pending.values()) { clearTimeout(task.timer); task.reject(new RpcError('CANCELLED', 'Window closed')); }
    pending.clear(); events.clear();
  });
}
export async function invoke<T>(command: string, args: Record<string, unknown> = {}): Promise<T> {
  if (import.meta.env.DEV && import.meta.env.VITE_LEXICA_PREVIEW === '1') return (await import('./preview')).previewInvoke(command, args) as Promise<T>;
  const request = {protocol_version: 1, request_id: `${Date.now()}-${++sequence}`, command, args};
  const timeout = command === 'import_ecdict' || command === 'ensure_dictionary' ? 600_000 : 30_000;
  const native = bridge();
  if (native) {
    attach(native);
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {pending.delete(request.request_id); reject(new RpcError('TIMEOUT', 'Command timed out'));}, timeout);
      pending.set(request.request_id, {resolve: value => resolve(value as T), reject, timer});
      try { native.postMessage(request); } catch (error) {clearTimeout(timer); pending.delete(request.request_id); reject(error);}
    });
  }
  let response: globalThis.Response;
  try { response = await fetch('/rpc', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(request), signal: AbortSignal.timeout(timeout)}); }
  catch { throw new RpcError('NETWORK_ERROR', 'Start the C++ backend before opening this page.'); }
  if (!response.ok) throw new RpcError('HTTP_ERROR', `Local service returned ${response.status}`);
  const data = await response.json() as Response;
  if (data.protocol_version !== 1 || data.request_id !== request.request_id) throw new RpcError('PROTOCOL_MISMATCH', 'Invalid response envelope');
  if (!data.ok) throw new RpcError(data.error?.code ?? 'INTERNAL_ERROR', data.error?.message ?? 'Command failed');
  return data.result as T;
}
export function invokeCommand<K extends keyof CommandMap>(command: K, args: CommandMap[K]['args']): Promise<CommandMap[K]['result']> {
  return invoke(command, args as Record<string, unknown>);
}
export type UnlistenFn = () => void;
let eventSource: EventSource | undefined;
function ensureHttpEvents() {
  if (bridge() || eventSource || typeof EventSource === 'undefined') return;
  eventSource = new EventSource('/events');
  eventSource.onmessage = event => {
    const data = JSON.parse(String(event.data)) as EventMessage;
    if (typeof data.event !== 'string') return;
    for (const fn of events.get(data.event) ?? []) fn({payload: data.payload});
  };
}
export async function listen<T>(name: string, fn: (event: {payload: T}) => void): Promise<UnlistenFn> {
  const native = bridge(); if (native) attach(native); else ensureHttpEvents();
  const handler = fn as (event: {payload: unknown}) => void;
  const handlers = events.get(name) ?? new Set(); handlers.add(handler); events.set(name, handlers);
  return () => {handlers.delete(handler); if (!handlers.size) events.delete(name);};
}
function hostCall(action: string, extra: Record<string, unknown> = {}): Promise<string | null> {
  const native = bridge();
  if (!native) return Promise.resolve(null);
  attach(native);
  const request = {protocol_version: 1, request_id: `host-${Date.now()}-${++sequence}`, kind: 'host', action, ...extra};
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {pending.delete(request.request_id); reject(new RpcError('TIMEOUT', 'Host timed out'));}, 120_000);
    pending.set(request.request_id, {resolve: value => {
      const path = value && typeof value === 'object' && 'path' in value ? (value as {path: string | null}).path : null;
      resolve(path);
    }, reject, timer});
    try { native.postMessage(request); } catch (error) {clearTimeout(timer); pending.delete(request.request_id); reject(error);}
  });
}
export const pickFile = () => hostCall('pick-file');
export const pickDirectory = () => hostCall('pick-directory');
export async function openUrl(url: string): Promise<void> {
  const parsed = new URL(url);
  if (!['https:', 'http:'].includes(parsed.protocol)) throw new RpcError('BAD_REQUEST', 'Unsupported external URL');
  window.open(parsed.href, '_blank', 'noopener,noreferrer');
}
// Enabled only by the desktop smoke harness, before the document is created.
if (typeof window !== 'undefined' && (window as unknown as {__LEXICA_SMOKE__?: boolean}).__LEXICA_SMOKE__) {
  (window as unknown as {__LEXICA_INVOKE__: typeof invoke}).__LEXICA_INVOKE__ = invoke;
}
