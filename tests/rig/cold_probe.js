/**
 * The first command after a pause against the ones that follow: a keypad update and a log
 * range read, each after IDLE ms of silence and again right after, ROUNDS times.
 *
 *   MQTT_URL=mqtt://user:pass@host:1883 IDLE=9000 ROUNDS=3 node tests/rig/cold_probe.js
 *
 * Uses the keypad entry with codeId 99 of the device at test/opener.
 */
const mqtt = require('mqtt');
const base = 'test/opener';
const c = mqtt.connect(process.env.MQTT_URL);
const last = {}; const waits = new Map();
c.on('message', (t, p, pk) => {
  const s = p.toString(); last[t] = s; let j; try { j = JSON.parse(s); } catch { return; }
  if (!pk.retain && j && j.result && j.requestId && waits.has(j.requestId)) { waits.get(j.requestId)(j); waits.delete(j.requestId); }
});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let n = 0;
const cmd = (topic, body, label) => {
  const id = `c${Date.now().toString(36)}${n++}`; const t0 = Date.now();
  const p = new Promise((res) => { waits.set(id, res); setTimeout(() => res(null), 30000); });
  c.publish(`${base}${topic}`, JSON.stringify({ ...body, requestId: id }));
  return p.then((r) => { console.log(`${(Date.now() % 1000000 / 1000).toFixed(3)} ${label}: client ${Date.now() - t0} ms, device ${r && r.durationMs} ms, ${r ? r.result : 'NO ANSWER'}${r && r.entries ? ' entries ' + r.entries.length : ''}`); return r; });
};
c.on('connect', async () => {
  c.subscribe(`${base}/#`); await sleep(1500);
  const e = JSON.parse(last[`${base}/keypad/json`]).find((x) => x.codeId === 99);
  const marker = Number(last[`${base}/lastRollingLog`]);
  console.log('marker', marker);
  const upd = { action: 'update', codeId: 99, name: e.name, code: e.code, enabled: e.enabled };
  const idle = Number(process.env.IDLE || 9000);
  for (let i = 0; i < Number(process.env.ROUNDS || 3); i++) {
    await sleep(idle); await cmd('/keypad/actionJson', upd, 'cold update');
    await cmd('/keypad/actionJson', upd, 'warm update');
    await cmd('/query/logRange', { from: marker - 2, to: marker }, 'warm range');
    await sleep(idle); await cmd('/query/logRange', { from: marker - 2, to: marker }, 'cold range');
    await Promise.all([0, 1, 2].map((k) => cmd('/query/logRange', { from: marker - 2 - k, to: marker - k }, `queued range ${k}`)));
  }
  process.exit(0);
});
