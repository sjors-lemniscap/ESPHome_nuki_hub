#!/usr/bin/env node
/**
 * How the commands behave with many entries on the keypad.
 *
 *   MQTT_URL=mqtt://user:pass@host:1883 PREFIX=test KIND=opener COUNT=80 node tests/rig/large_keypad.js
 *
 * Syncs COUNT entries named largetest_* onto the keypad, measures a list read and single
 * commands with all of them there, and removes them again.
 */
const mqtt = require('mqtt');

const base = `${process.env.PREFIX || 'test'}/${process.env.KIND || 'opener'}`;
const count = Number(process.env.COUNT || 80);
const client = mqtt.connect(process.env.MQTT_URL || 'mqtt://localhost:1883');
const waiting = [];
let seq = 0;
let failed = 0;
const log = (m) => console.log(`${new Date().toISOString().slice(11, 23)} ${m}`);
const check = (name, ok, detail = '') => { if (!ok) failed++; log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? ` — ${detail}` : ''}`); };

client.on('message', (topic, payload, packet) => {
  let json = null;
  try { json = JSON.parse(payload.toString()); } catch { /* plain */ }
  for (let i = waiting.length - 1; i >= 0; i--) {
    const w = waiting[i];
    if (w.topic === topic && !packet.retain && w.match(json)) { waiting.splice(i, 1); w.resolve(json); }
  }
});
const expect = (topic, match, ms) => new Promise((resolve) => {
  const w = { topic: `${base}${topic}`, match, resolve };
  waiting.push(w);
  setTimeout(() => { const i = waiting.indexOf(w); if (i >= 0) { waiting.splice(i, 1); resolve(null); } }, ms);
});
async function command(topic, resultTopic, data, ms = 60000) {
  const requestId = `l${Date.now().toString(36)}${++seq}`;
  const answer = expect(resultTopic, (j) => j && j.requestId === requestId, ms);
  const started = Date.now();
  client.publish(`${base}${topic}`, JSON.stringify({ ...data, requestId }));
  const result = (await answer) || { result: '<timeout>' };
  return { result, ms: Date.now() - started };
}
// Six digits from 1 to 9, different for every entry and not starting with 12
const code = (i) => Number(String(i + 300000).split('').map((d) => String((Number(d) % 9) + 1)).join('')) + 0;

client.on('connect', async () => {
  client.subscribe(`${base}/#`);
  await new Promise((r) => setTimeout(r, 1500));
  const entries = [];
  const used = new Set();
  for (let i = 0; entries.length < count; i++) {
    const c = code(i * 7 + 11);
    if (used.has(c) || String(c).startsWith('12')) continue;
    used.add(c);
    entries.push({ name: `largetest_${entries.length}`, code: c, enabled: 1 });
  }
  // A sync does at most 50 commands and says how many are left: send it until none are
  const sync = async (wanted) => {
    let total = 0;
    let rounds = 0;
    let result;
    do {
      const round = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'largetest_', entries: wanted }, 300000);
      total += round.ms;
      rounds++;
      result = round.result;
    } while (result.result === 'success' && result.remaining > 0 && rounds < 10);
    return { result, ms: total, rounds };
  };
  let r = await sync(entries);
  check(`sync adds ${count} entries`, r.result.result === 'success' && !r.result.remaining && r.result.failed === 0,
    `${r.ms} ms in ${r.rounds} rounds, ${Math.round(r.ms / count)} ms per entry, ${JSON.stringify({ ...r.result, results: undefined })}`);

  // A board without the memory for the whole list sends it in pages, keypad/page/<n>
  const pagesSeen = {};
  const onPage = (topic, payload) => {
    const m = topic.match(/\/keypad\/page\/(\d+)$/);
    if (m && payload.length) { try { pagesSeen[m[1]] = JSON.parse(payload.toString()); } catch { /* not a page */ } }
  };
  client.on('message', onPage);
  const listed = expect('/keypad/json', (j) => Array.isArray(j), 20000);
  const paged = expect('/keypad/pages', (j) => Number(j) > 0, 20000);
  const started = Date.now();
  client.publish(`${base}/query/keypad`, '1');
  let list = await Promise.race([listed, paged.then((n) => (n ? null : new Promise(() => {})))]);
  let how = 'one message';
  if (!Array.isArray(list)) {
    const pages = Number(await paged) || 0;
    await new Promise((r) => setTimeout(r, 2000 + 300 * pages));
    list = [];
    for (let n = 0; n < pages; n++) list = list.concat(pagesSeen[n] || []);
    how = `${pages} pages`;
  }
  client.off('message', onPage);
  check('list read with all entries', !!list && list.filter((e) => e.name.startsWith('largetest_')).length === count,
    `${Date.now() - started} ms, ${list ? list.length : 0} entries in ${how}, ${list ? JSON.stringify(list).length : 0} bytes`);

  const times = { add: [], update: [], delete: [] };
  for (let i = 0; i < 5; i++) {
    r = await command('/keypad/actionJson', '/keypad/result', { action: 'add', name: `largetest_x${i}`, code: [987311, 988422, 989533, 976644, 991755][i], enabled: 1 });
    if (r.result.result !== 'success') { check('single add', false, JSON.stringify(r.result)); continue; }
    times.add.push(r.ms);
    const codeId = r.result.codeId;
    r = await command('/keypad/actionJson', '/keypad/result', { action: 'update', codeId, name: `largetest_x${i}`, code: [987311, 988422, 989533, 976644, 991755][i], enabled: 0 });
    if (r.result.result === 'success') times.update.push(r.ms); else check('single update', false, JSON.stringify(r.result));
    r = await command('/keypad/actionJson', '/keypad/result', { action: 'delete', codeId });
    if (r.result.result === 'success') times.delete.push(r.ms); else check('single delete', false, JSON.stringify(r.result));
  }
  const avg = (a) => (a.length ? Math.round(a.reduce((x, y) => x + y, 0) / a.length) : -1);
  check('single commands with a full keypad', times.add.length === 5 && times.update.length === 5 && times.delete.length === 5,
    `add ${avg(times.add)} ms, update ${avg(times.update)} ms, delete ${avg(times.delete)} ms`);

  r = await sync([]);
  check('sync removes them again', r.result.result === 'success' && !r.result.remaining && r.result.failed === 0,
    `${r.ms} ms in ${r.rounds} rounds, ${JSON.stringify({ ...r.result, results: undefined })}`);
  const after = expect('/keypad/json', (j) => Array.isArray(j), 60000);
  client.publish(`${base}/query/keypad`, '1');
  const left = ((await after) || []).filter((e) => e.name.startsWith('largetest_')).length;
  check('none left on the keypad', left === 0, `${left} left`);
  console.log(failed ? `${failed} check(s) FAILED` : 'all checks passed');
  client.end();
  process.exit(failed ? 1 : 0);
});
