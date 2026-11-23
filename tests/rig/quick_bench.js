#!/usr/bin/env node
/**
 * Keypad updates on an open connection: what a client waits and what the device itself
 * took (durationMs of the result), to tell the Bluetooth side from the network.
 *
 *   MQTT_URL=mqtt://user:pass@host:1883 PREFIX=test KIND=opener CODE_ID=99 COUNT=40 node tests/rig/quick_bench.js
 *
 * Updates the entry CODE_ID with the values it has, COUNT times one after the other.
 */
const mqtt = require('mqtt');

const base = `${process.env.PREFIX || 'test'}/${process.env.KIND || 'opener'}`;
const count = Number(process.env.COUNT || 40);
const codeId = Number(process.env.CODE_ID || 99);
const client = mqtt.connect(process.env.MQTT_URL || 'mqtt://localhost:1883');
let waiting = null;
let entry = null;
client.on('message', (topic, payload, packet) => {
  let json = null;
  try { json = JSON.parse(payload.toString()); } catch { return; }
  if (topic === `${base}/keypad/json` && Array.isArray(json)) entry = json.find((e) => e.codeId === codeId) || entry;
  if (topic === `${base}/keypad/result` && !packet.retain && waiting && json.requestId === waiting.id) waiting.resolve(json);
  if (topic === `${base}/debug/bleState`) console.log('connection interval', json.interval, 'ms');
});
const stats = (a) => { const s = [...a].sort((x, y) => x - y); return `avg ${Math.round(s.reduce((x, y) => x + y, 0) / s.length)} median ${s[Math.floor(s.length / 2)]} p95 ${s[Math.floor(s.length * 0.95)]} max ${s[s.length - 1]}`; };
client.on('connect', async () => {
  client.subscribe(`${base}/#`);
  await new Promise((r) => setTimeout(r, 1500));
  if (!entry) { console.log(`no entry ${codeId} in keypad/json`); process.exit(2); }
  const seen = { client: [], device: [] };
  let failed = 0;
  for (let i = 0; i < count + 2; i++) {
    const id = `q${Date.now().toString(36)}${i}`;
    const answer = new Promise((resolve) => { waiting = { id, resolve }; setTimeout(() => resolve(null), 20000); });
    const started = Date.now();
    client.publish(`${base}/keypad/actionJson`, JSON.stringify({ action: 'update', codeId, name: entry.name, code: entry.code, enabled: entry.enabled, requestId: id }));
    const result = await answer;
    if (!result || result.result !== 'success') { failed++; continue; }
    if (i < 2) continue; // the first ones open the connection
    seen.client.push(Date.now() - started);
    seen.device.push(result.durationMs);
  }
  client.publish(`${base}/debug/bleInfo`, '1');
  await new Promise((r) => setTimeout(r, 500));
  console.log(`client ms: ${stats(seen.client)}`);
  console.log(`device ms: ${stats(seen.device)}`);
  console.log(`${seen.client.length} measured, ${failed} failed`);
  process.exit(failed ? 1 : 0);
});
