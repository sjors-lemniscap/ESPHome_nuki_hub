#!/usr/bin/env node
/**
 * Timing of the common operations against a real device, as a client sees them over MQTT.
 *
 *   MQTT_URL=mqtt://user:pass@host:1883 PREFIX=test KIND=opener node tests/rig/benchmark.js [rounds]
 *
 * Creates and deletes keypad entries named bench_*. Prints a table, and JSON with --json.
 */
const mqtt = require('mqtt');

const url = process.env.MQTT_URL || 'mqtt://localhost:1883';
const base = `${process.env.PREFIX || 'test'}/${process.env.KIND || 'opener'}`;
const hub = process.env.PREFIX || 'test';
const rounds = Number(process.argv.find((a) => /^\d+$/.test(a)) || 20);
const asJson = process.argv.includes('--json');
const client = mqtt.connect(url);
const waiting = [];
const last = {};
let seq = 0;
const rid = () => `b${Date.now().toString(36)}${++seq}`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

client.on('message', (topic, payload, packet) => {
  const text = payload.toString();
  let json = null;
  try { json = JSON.parse(text); } catch { /* plain */ }
  last[topic] = { text, json };
  for (let i = waiting.length - 1; i >= 0; i--) {
    const w = waiting[i];
    if (w.topic === topic && !packet.retain && w.match(json, text)) { waiting.splice(i, 1); w.resolve(json ?? text); }
  }
});
function expect(topic, match = () => true, ms = 60000) {
  return new Promise((resolve) => {
    const w = { topic: `${base}${topic}`, match, resolve };
    waiting.push(w);
    setTimeout(() => { const i = waiting.indexOf(w); if (i >= 0) { waiting.splice(i, 1); resolve(null); } }, ms);
  });
}
const send = (topic, data) => client.publish(`${base}${topic}`, typeof data === 'string' ? data : JSON.stringify(data));
async function command(topic, resultTopic, data, ms) {
  const requestId = rid();
  const answer = expect(resultTopic, (j) => j && j.requestId === requestId, ms);
  const t0 = Date.now();
  send(topic, { ...data, requestId });
  const r = (await answer) || { result: '<timeout>' };
  return { ...r, ms: Date.now() - t0 };
}
const stats = (values) => {
  const v = [...values].sort((a, b) => a - b);
  if (!v.length) return null;
  const q = (p) => v[Math.min(v.length - 1, Math.floor(v.length * p))];
  return { n: v.length, avg: Math.round(v.reduce((a, b) => a + b, 0) / v.length), p50: q(0.5), p95: q(0.95), max: v[v.length - 1] };
};
const code = (i) => Number(String(300000 + ((Date.now() + i * 104729) % 600000)).replace(/0/g, '9'));

(async () => {
  await new Promise((resolve, reject) => { client.once('connect', resolve); client.once('error', reject); });
  client.subscribe([`${base}/#`, `${hub}/maintenance/#`]);
  await sleep(2500);
  const results = {};
  const failures = {};
  const note = (name, r) => { (results[name] ||= []).push(r.ms); if (r.result !== 'success') failures[name] = (failures[name] || 0) + 1; };

  // leftovers
  const p0 = expect('/keypad/json'); send('/query/keypad', '1');
  for (const e of ((await p0) || []).filter((x) => x.name.startsWith('bench_'))) {
    await command('/keypad/actionJson', '/keypad/result', { action: 'delete', codeId: e.codeId });
  }

  // --- single keypad commands, one after the other
  const ids = [];
  for (let i = 0; i < rounds; i++) {
    const r = await command('/keypad/actionJson', '/keypad/result', { action: 'add', name: `bench_${i}`, code: code(i), enabled: 1, timeLimited: 0 });
    note('keypad add', r);
    if (r.codeId) ids.push(r.codeId);
  }
  for (const codeId of ids) note('keypad update', await command('/keypad/actionJson', '/keypad/result', { action: 'update', codeId, enabled: 0 }));
  for (const codeId of ids) note('keypad delete', await command('/keypad/actionJson', '/keypad/result', { action: 'delete', codeId }));

  // --- reads
  for (let i = 0; i < Math.min(rounds, 10); i++) {
    await sleep(2500);   // past the open connection of the previous command, like a first command after a pause
    let t0 = Date.now(); let p = expect('/keypad/json'); send('/query/keypad', '1');
    note('keypad list read (cold)', { ms: (await p) ? Date.now() - t0 : 60000, result: 'success' });
    t0 = Date.now(); p = expect('/json'); send('/query/lockstate', '1');
    note('state read (warm)', { ms: (await p) ? Date.now() - t0 : 60000, result: 'success' });
  }

  // --- the same number of adds as one batch, and as a sync
  const many = Array.from({ length: rounds }, (_, i) => ({ action: 'add', name: `bench_b${i}`, code: code(1000 + i), enabled: 1, timeLimited: 0 }));
  let r = await command('/keypad/batchJson', '/keypad/batchResult', { commands: many }, 180000);
  results[`batch of ${rounds} adds (total)`] = [r.ms];
  if (r.succeeded !== rounds) failures.batch = rounds - (r.succeeded || 0);
  const del = (r.results || []).filter((x) => x.codeId).map((x) => ({ action: 'delete', codeId: x.codeId }));
  r = await command('/keypad/batchJson', '/keypad/batchResult', { commands: del }, 180000);
  results[`batch of ${del.length} deletes (total)`] = [r.ms];

  const entries = Array.from({ length: rounds }, (_, i) => ({ name: `bench_s${i}`, code: code(2000 + i), enabled: 1, timeLimited: 0 }));
  r = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'bench_', entries }, 180000);
  results[`sync adding ${rounds} (total)`] = [r.ms];
  r = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'bench_', entries }, 180000);
  results['sync, nothing to do'] = [r.ms];
  r = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'bench_', entries: [] }, 180000);
  results[`sync deleting ${rounds} (total)`] = [r.ms];

  const out = { at: new Date().toISOString(), rounds, operations: {}, failures,
    device: { bluetooth: last[`${base}/bluetoothJson`]?.json, status: last[`${base}/status`]?.json,
      freeHeapInternal: Number(last[`${hub}/maintenance/freeHeapInternal`]?.text), minFreeHeapInternal: Number(last[`${hub}/maintenance/minFreeHeapInternal`]?.text),
      wifiRssi: Number(last[`${hub}/maintenance/wifiRssi`]?.text) } };
  for (const [name, values] of Object.entries(results)) out.operations[name] = stats(values);
  if (asJson) console.log(JSON.stringify(out, null, 2));
  else {
    console.log(`${'operation'.padEnd(34)} ${'n'.padStart(3)} ${'avg'.padStart(7)} ${'p50'.padStart(7)} ${'p95'.padStart(7)} ${'max'.padStart(7)}  ms`);
    for (const [name, s] of Object.entries(out.operations)) console.log(`${name.padEnd(34)} ${String(s.n).padStart(3)} ${String(s.avg).padStart(7)} ${String(s.p50).padStart(7)} ${String(s.p95).padStart(7)} ${String(s.max).padStart(7)}`);
    console.log(`failures: ${JSON.stringify(failures)}`);
    console.log(`device: ${JSON.stringify(out.device)}`);
  }
  client.end();
  process.exit(Object.keys(failures).length ? 1 : 0);
})().catch((err) => { console.error(err); process.exit(2); });
