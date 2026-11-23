"""Keypad commands over the ESPHome API transport (api_transport: true), without a broker.

usage: api_transport.py <host> <device> [rounds] [--press <button name>]
"""
import asyncio
import json
import statistics
import sys
import time

from aioesphomeapi import APIClient


async def main():
    host, device = sys.argv[1], sys.argv[2]
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 10
    client = APIClient(host, 6053, None)
    await client.connect(login=True)
    entities, services = await client.list_entities_services()
    if "--press" in sys.argv:
        name = sys.argv[sys.argv.index("--press") + 1]
        key = next(e.key for e in entities if e.name == name)
        client.button_command(key)
        await asyncio.sleep(1)
        print("pressed", name)
        return
    service = next(s for s in services if s.name == "nuki_hub_command")
    waiting = {}
    seen = {}

    def on_event(call):
        if not call.is_event or call.service != "esphome.nuki_hub" or call.data.get("device") != device:
            return
        topic, payload = call.data.get("topic"), call.data.get("payload")
        seen[topic] = seen.get(topic, 0) + 1
        if topic == "/keypad/result":
            try:
                result = json.loads(payload)
            except ValueError:
                return
            future = waiting.pop(result.get("requestId"), None)
            if future and not future.done():
                future.set_result(result)

    client.subscribe_service_calls(on_event)
    counter = 0

    async def command(data):
        nonlocal counter
        counter += 1
        request_id = f"api{int(time.time())}_{counter}"
        future = asyncio.get_running_loop().create_future()
        waiting[request_id] = future
        started = time.monotonic()
        await client.execute_service(
            service, {"device": device, "topic": "/keypad/actionJson", "payload": json.dumps({**data, "requestId": request_id})}
        )
        result = await asyncio.wait_for(future, 30)
        return result, (time.monotonic() - started) * 1000

    failures = 0
    times = {"add": [], "update": [], "delete": []}
    for i in range(rounds):
        name = f"apitest_{i}"
        code = int(f"{i % 9 + 1}35792")
        result, ms = await command({"action": "add", "name": name, "code": code, "enabled": 1})
        times["add"].append(ms)
        code_id = result.get("codeId")
        if result.get("result") != "success" or not code_id:
            failures += 1
            print("add failed:", result)
            continue
        result, ms = await command({"action": "update", "codeId": code_id, "name": name, "code": code, "enabled": 0})
        times["update"].append(ms)
        failures += result.get("result") != "success"
        result, ms = await command({"action": "delete", "codeId": code_id})
        times["delete"].append(ms)
        failures += result.get("result") != "success"
    for action, values in times.items():
        if values:
            print(f"{action:7} avg {statistics.mean(values):.0f} ms, median {statistics.median(values):.0f} ms, max {max(values):.0f} ms")
    print("events by topic:", dict(sorted(seen.items())))
    print("FAILED" if failures else "all commands succeeded", f"({rounds * 3} commands, {failures} failed)")
    await client.disconnect()
    sys.exit(1 if failures else 0)


asyncio.run(main())
