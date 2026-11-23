#!/usr/bin/env node
/**
 * Checks what nuki_hub adds to Nuki Hub's MQTT contract, against a real device.
 *
 *   MQTT_URL=mqtt://user:pass@host:1883 PREFIX=test KIND=opener node tests/rig/additions.js
 *
 * Needs the `mqtt` package (NODE_PATH may point at another project's node_modules).
 * Creates and deletes keypad entries named addtest_* and synctest_*, one time control entry,
 * switches ledFlashEnabled forth and back and sends deactivateRTO. With STRIKE=1 it also
 * actuates the electric strike once. Nothing else is touched.
 */
const mqtt = require('mqtt');

const url = process.env.MQTT_URL || 'mqtt://localhost:1883';
const base = `${process.env.PREFIX || 'test'}/${process.env.KIND || 'opener'}`;
const hub = process.env.PREFIX || 'test';
const client = mqtt.connect(url);
const waiting = [];   // {topic, match, resolve}
const last = {};
let failed = 0;
let seq = 0;
const rid = () => `t${Date.now().toString(36)}${++seq}`;
const log = (m) => console.log(`${new Date().toISOString().slice(11, 23)} ${m}`);
const check = (name, ok, detail = '') => { if (!ok) failed++; log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? ` — ${detail}` : ''}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

client.on('message', (topic, payload, packet) => {
  const text = payload.toString();
  let json = null;
  try { json = JSON.parse(text); } catch { /* plain */ }
  last[topic] = { text, json, retained: packet.retain };
  for (let i = waiting.length - 1; i >= 0; i--) {
    const w = waiting[i];
    if (w.topic === topic && !packet.retain && w.match(json, text)) { waiting.splice(i, 1); w.resolve(json ?? text); }
  }
});

function expect(topic, match = () => true, ms = 40000) {
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
  send(topic, { ...data, requestId });
  return (await answer) || { result: '<timeout>' };
}
const list = () => (last[`${base}/keypad/json`]?.json || []);
async function freshList() {
  const p = expect('/keypad/json');
  send('/query/keypad', '1');
  return (await p) || list();
}

(async () => {
  await new Promise((resolve, reject) => { client.once('connect', resolve); client.once('error', reject); });
  client.subscribe([`${base}/#`, `${hub}/maintenance/#`, 'homeassistant/+/+/+/config']);
  await sleep(2500);

  // Leftovers of an interrupted run
  for (const e of (await freshList()).filter((x) => /^(addtest_|synctest_|expiretest_)/.test(x.name))) {
    await command('/keypad/actionJson', '/keypad/result', { action: 'delete', codeId: e.codeId });
  }

  // --- status and memory
  const status = last[`${base}/status`]?.json;
  check('status is published', !!status && status.paired === true && typeof status.available === 'boolean', JSON.stringify(status));
  check('internal heap is published', Number(last[`${hub}/maintenance/freeHeapInternal`]?.text) > 0
    && Number(last[`${hub}/maintenance/minFreeHeapInternal`]?.text) > 0,
  `${last[`${hub}/maintenance/freeHeapInternal`]?.text} free, ${last[`${hub}/maintenance/minFreeHeapInternal`]?.text} minimum`);

  // --- request id, code id, changed entry
  const code = 300000 + Math.floor(Math.random() * 600000);
  const safe = Number(String(code).replace(/0/g, '7'));
  const changed = expect('/keypad/changed', (j) => j && j.change === 'added');
  let r = await command('/keypad/actionJson', '/keypad/result', { action: 'add', name: 'addtest_one', code: safe, enabled: 1, timeLimited: 0 });
  check('add answers with the request id and success', r.result === 'success', JSON.stringify(r));
  check('add result carries the new codeId', Number.isInteger(r.codeId) && r.codeId > 0, `codeId=${r.codeId}`);
  const c = await changed;
  check('keypad/changed announces the added entry', !!c && c.codeId === r.codeId && c.entry?.name === 'addtest_one', JSON.stringify(c));
  const firstId = r.codeId;
  const listed = (await freshList()).find((e) => e.name === 'addtest_one');
  check('codeId of the result matches the keypad list', !!listed && listed.codeId === firstId, `list has ${listed?.codeId}`);

  // --- the same add again: the entry exists, the command did its job
  r = await command('/keypad/actionJson', '/keypad/result', { action: 'add', name: 'addtest_one', code: safe, enabled: 1, timeLimited: 0 });
  check('repeating an add reports the existing entry', r.result === 'success' && r.existed === true && r.codeId === firstId, JSON.stringify(r));

  // --- the same code under another name: refused, with the device's reason
  r = await command('/keypad/actionJson', '/keypad/result', { action: 'add', name: 'addtest_two', code: safe, enabled: 1, timeLimited: 0 });
  check('a code in use is refused with the device error', r.result === 'failed' && r.error === 'K_ERROR_CODE_ALREADY_EXISTS', JSON.stringify(r));

  // --- the same name and code with another time window is not "the same entry"
  r = await command('/keypad/actionJson', '/keypad/result', { action: 'add', name: 'addtest_one', code: safe, enabled: 1, timeLimited: 1, allowedFrom: '2030-01-01 08:00:00', allowedUntil: '2030-01-01 09:00:00' });
  check('repeating an add with another window is refused', r.result === 'failed' && !r.existed, JSON.stringify(r));
  r = await command('/keypad/actionJson', '/keypad/result', { action: 'delete', codeId: 65536 + firstId });
  check('a code id beyond 16 bits is not taken for another entry', r.result === 'noExistingCodeIdSet', JSON.stringify(r));
  check('and that entry is still there', !!(await freshList()).find((e) => e.codeId === firstId));

  // --- update and delete announce the entry
  let ch = expect('/keypad/changed', (j) => j && j.change === 'updated');
  r = await command('/keypad/actionJson', '/keypad/result', { action: 'update', codeId: firstId, enabled: 0 });
  const u = await ch;
  check('update: result and changed entry', r.result === 'success' && u?.entry?.enabled === 0, JSON.stringify(u));

  // --- a deadline that has passed
  const burst = [0, 1, 2].map((i) => command('/keypad/actionJson', '/keypad/result', { action: 'update', codeId: firstId, enabled: i % 2 }));
  const late = command('/keypad/actionJson', '/keypad/result', { action: 'update', codeId: firstId, enabled: 1, timeoutMs: 1 });
  await Promise.all(burst);
  r = await late;
  check('a command past its timeoutMs is not executed', r.result === 'expired', JSON.stringify(r));

  ch = expect('/keypad/changed', (j) => j && j.change === 'deleted');
  r = await command('/keypad/actionJson', '/keypad/result', { action: 'delete', codeId: firstId });
  const d = await ch;
  check('delete: result and changed entry', r.result === 'success' && d?.codeId === firstId, JSON.stringify(d));

  // --- batch
  const mk = (i) => Number(String(400000 + ((code + i * 7919) % 500000)).replace(/0/g, '8'));
  r = await command('/keypad/batchJson', '/keypad/batchResult', { commands: [
    { action: 'add', name: 'addtest_b1', code: mk(1), enabled: 1, timeLimited: 0 },
    { action: 'add', name: 'addtest_b2', code: mk(2), enabled: 1, timeLimited: 1, allowedFrom: '2030-01-01 08:00:00', allowedUntil: '2030-01-01 10:00:00' },
    { action: 'add', name: 'addtest_bad', code: 405678, enabled: 1, timeLimited: 0 },
    { action: 'delete', codeId: 64999 },
  ] }, 90000);
  check('batch: one combined result', r.result === 'partial' && r.succeeded === 2 && r.failed === 2 && r.results?.length === 4, JSON.stringify(r).slice(0, 300));
  check('batch: per command results in order', r.results?.[0]?.result === 'success' && r.results?.[1]?.codeId > 0
    && r.results?.[2]?.result === 'noValidCodeSet' && r.results?.[3]?.result === 'noExistingCodeIdSet');
  for (const x of (r.results || []).filter((x) => x.codeId && x.result === 'success')) {
    await command('/keypad/actionJson', '/keypad/result', { action: 'delete', codeId: x.codeId });
  }

  // --- desired state
  const want = (names) => names.map((n, i) => ({ name: `synctest_${n}`, code: mk(10 + n.charCodeAt(0) + i), enabled: 1, timeLimited: 1, allowedFrom: '2030-02-01 08:00:00', allowedUntil: '2030-02-01 10:00:00' }));
  const set1 = want(['a', 'b', 'c']);
  r = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'synctest_', entries: set1, dryRun: true }, 90000);
  check('sync dry run reports what it would do', r.result === 'dryRun' && r.adds === 3 && r.deletes === 0, JSON.stringify(r));
  r = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'synctest_', entries: set1 }, 120000);
  check('sync adds the missing entries', r.result === 'success' && r.adds === 3 && r.succeeded === 3, JSON.stringify(r).slice(0, 260));
  r = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'synctest_', entries: set1 }, 90000);
  check('sync of an unchanged list does nothing', r.result === 'success' && r.unchanged === 3 && !r.adds && !r.updates && !r.deletes, JSON.stringify(r));
  const set2 = [{ ...set1[0], allowedUntil: '2030-02-01 12:00:00' }, set1[1]];
  r = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'synctest_', entries: set2 }, 120000);
  check('sync updates what differs and deletes what is not wanted', r.result === 'success' && r.updates === 1 && r.deletes === 1 && r.unchanged === 1, JSON.stringify(r).slice(0, 260));
  const after = (await freshList()).filter((e) => e.name.startsWith('synctest_'));
  check('keypad holds exactly the wanted entries', after.length === 2 && after.find((e) => e.name === 'synctest_a')?.allowedUntil === '2030-02-01 12:00:00', after.map((e) => `${e.name}:${e.allowedUntil}`).join(', '));
  const foreign = list().filter((e) => !e.name.startsWith('synctest_')).length;
  r = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'synctest_', entries: [] }, 120000);
  const end = await freshList();
  check('sync with an empty list removes the prefixed entries only', r.deletes === 2 && !end.some((e) => e.name.startsWith('synctest_'))
    && end.filter((e) => !e.name.startsWith('synctest_')).length === foreign, `${end.length} entries left`);
  // The same state written in other ways must not look like a difference
  const loose = [{ name: 'synctest_t', code: mk(70), enabled: true, allowedFrom: '2030-03-01T08:00:00', allowedUntil: '2030-03-01T10:00:00' }];
  r = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'synctest_', entries: loose }, 120000);
  check('sync: a date implies a time limit', r.result === 'success' && r.adds === 1, JSON.stringify(r).slice(0, 200));
  const t = (await freshList()).find((e) => e.name === 'synctest_t');
  check('sync: the entry is time limited on the keypad', !!t && t.timeLimited === 1 && t.allowedUntil === '2030-03-01 10:00:00', JSON.stringify(t));
  r = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'synctest_', entries: loose }, 90000);
  check('sync: true/1 and T/space are the same state', r.unchanged === 1 && !r.updates, JSON.stringify(r));
  await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'synctest_', entries: [] }, 120000);

  r = await command('/keypad/syncJson', '/keypad/syncResult', { prefix: 'x', entries: [] });
  check('sync refuses a short prefix', r.result === 'invalidPrefix', JSON.stringify(r));

  // --- entries that ran out are removed (needs keypad_delete_expired with this name prefix)
  if (process.env.EXPIRE_PREFIX) {
    const pad = (n) => String(n).padStart(2, '0');
    const local = (d) => `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
    const now = Date.now();
    const gone = expect('/keypad/expiredResult', (j) => j && j.reason === 'expired', 60000);
    const name = `${process.env.EXPIRE_PREFIX}old`;
    r = await command('/keypad/batchJson', '/keypad/batchResult', { commands: [
      { action: 'add', name, code: mk(40), enabled: 1, timeLimited: 1, allowedFrom: local(new Date(now - 6 * 3600e3)), allowedUntil: local(new Date(now - 3 * 3600e3)) },
      { action: 'add', name: `${process.env.EXPIRE_PREFIX}new`, code: mk(41), enabled: 1, timeLimited: 1, allowedFrom: local(new Date(now - 3600e3)), allowedUntil: local(new Date(now + 3600e3)) },
    ] }, 60000);
    check('expiry: test entries added', r.succeeded === 2, JSON.stringify(r).slice(0, 200));
    const x = await gone;
    check('expiry: the entry that ran out is deleted by the device', !!x && x.succeeded === 1 && x.results?.length === 1, JSON.stringify(x));
    const left = (await freshList()).filter((e) => e.name.startsWith(process.env.EXPIRE_PREFIX)).map((e) => e.name);
    check('expiry: the entry still valid stays', left.length === 1 && left[0].endsWith('new'), left.join(', '));
    for (const e of list().filter((e) => e.name.startsWith(process.env.EXPIRE_PREFIX))) {
      await command('/keypad/actionJson', '/keypad/result', { action: 'delete', codeId: e.codeId });
    }
  }

  // --- log entries by index
  const marker = Number(last[`${base}/lastRollingLog`]?.text);
  if (marker > 3) {
    r = await command('/query/logRange', '/logRange', { from: marker - 2, to: marker }, 60000);
    check('log range returns the entries asked for', r.result === 'success' && r.entries?.length === 3
      && r.entries[0].index === marker - 2 && r.entries[2].index === marker, JSON.stringify(r).slice(0, 200));
  }

  // --- lock action as JSON
  r = await command('/action', '/actionResult', { action: 'deactivateRTO' }, 60000);
  check('action as JSON answers with the request id', r.result === 'success', JSON.stringify(r));
  r = await command('/action', '/actionResult', { action: 'noSuchAction' });
  check('unknown action is answered too', r.result === 'unknown_action', JSON.stringify(r));

  // An action the device logs (switching ring to open off while it is off is not logged):
  // the strike, set STRIKE=1 to allow it. Its log entry must arrive as an event.
  if (process.env.STRIKE) {
    const evp = expect('/event/action', (j) => j && /ElectricStrikeActuation/i.test(j.action || ''), 45000);
    r = await command('/action', '/actionResult', { action: 'electricStrikeActuation' }, 60000);
    check('strike action succeeds', r.result === 'success', JSON.stringify(r));
    const ev = await evp;
    check('event/action for the strike', !!ev && typeof ev.time === 'string' && ev.completionStatus === 'success', JSON.stringify(ev).slice(0, 220));
  }
  const audit = last[`${base}/audit`]?.json;
  check('audit names the command', !!audit && typeof audit.command === 'string', JSON.stringify(audit).slice(0, 160));
  const bt = last[`${base}/bluetoothJson`]?.json;
  check('bluetooth statistics are published', !!bt && bt.commands > 0, JSON.stringify(bt));
  const disc = Object.keys(last).filter((t) => t.startsWith('homeassistant/') && t.includes('/nuki_'));
  if (process.env.EXPECT_DISCOVERY) check('Home Assistant discovery is published', disc.length >= 6, `${disc.length} entities`);

  // --- time control
  await sleep(3000);
  // The list may be published before the answer to the command: wait for both from the start
  const tcListed = expect('/timecontrol/json', (j) => Array.isArray(j) && j.some((e) => e.time === '03:33'), 90000);
  r = await command('/timecontrol/action', '/timecontrol/result', { action: 'add', weekdays: ['mon'], time: '03:33', lockAction: 'deactivateRTO' }, 60000);
  check('time control: add', r.result === 'success', JSON.stringify(r));
  const tcList = await tcListed;
  const tc = (tcList || []).find((e) => e.time === '03:33');
  check('time control: entry appears in the list', !!tc, JSON.stringify(tcList));
  if (tc) {
    r = await command('/timecontrol/action', '/timecontrol/result', { action: 'update', entryId: tc.entryId, enabled: 0 }, 60000);
    check('time control: update', r.result === 'success', JSON.stringify(r));
    const tcGone = expect('/timecontrol/json', (j) => Array.isArray(j) && !j.some((e) => e.time === '03:33'), 90000);
    r = await command('/timecontrol/action', '/timecontrol/result', { action: 'delete', entryId: tc.entryId }, 60000);
    check('time control: delete', r.result === 'success', JSON.stringify(r));
    const gone = await tcGone;
    check('time control: entry is gone', gone !== null);
  }

  // --- authorizations: nothing is changed, only the refusal path
  r = await command('/authorization/action', '/authorization/result', { action: 'delete', authId: 4000000000 });
  check('authorization: unknown id is refused', r.result === 'noExistingAuthIdSet', JSON.stringify(r));

  // --- configuration
  const led = last[`${base}/configuration/basicJson`]?.json?.ledFlashEnabled;
  if (led === 0 || led === 1) {
    const set = async (value) => {
      const requestId = rid();
      const p = expect('/configuration/commandResult', (j) => j && j.requestId === requestId, 60000);
      send('/configuration/action', { ledFlashEnabled: value, requestId });
      return (await p) || {};
    };
    r = await set(led ? 0 : 1);
    check('configuration: a setting is written', r.ledFlashEnabled === 'success' && r.general === 'success', JSON.stringify(r));
    const cfg = await expect('/configuration/basicJson', (j) => j && j.ledFlashEnabled === (led ? 0 : 1), 30000);
    check('configuration: the device reports the new value', cfg !== null);
    r = await set(led);
    check('configuration: and back', r.ledFlashEnabled === 'success', JSON.stringify(r));
    r = await set(led);
    check('configuration: an unchanged value is not written', r.ledFlashEnabled === 'unchanged' && r.general === 'noChange', JSON.stringify(r));
  } else {
    check('configuration is published', false, 'no basicJson');
  }

  for (const e of (await freshList()).filter((x) => /^(addtest_|synctest_|expiretest_)/.test(x.name))) {
    await command('/keypad/actionJson', '/keypad/result', { action: 'delete', codeId: e.codeId });
  }
  console.log(failed ? `\n${failed} check(s) FAILED` : '\nall checks passed');
  client.end();
  process.exit(failed ? 1 : 0);
})().catch((err) => { console.error(err); process.exit(2); });
