"""Read Static Instancing counters and the overlay draw-call breakdown from a running game.

Talks to the devbench MCP server that the Remote Control feature hosts inside SkyrimSE
(streamable HTTP JSON-RPC on 127.0.0.1:8920 by default). Prints one line per sample so the
collapse ratio and the Utility draw count can be compared with the feature on and off.

Usage:
    python tools/static_instancing_probe.py [--port 8920] [--samples 10] [--interval 2]
    python tools/static_instancing_probe.py --set Enabled=false      # measure-only mode
    python tools/static_instancing_probe.py --set Enabled=true
    python tools/static_instancing_probe.py --set InstanceSingles=true
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request


class DevBench:
    def __init__(self, port: int):
        self.url = f"http://127.0.0.1:{port}/mcp"
        self.session = None
        self.next_id = 1
        self._initialize()

    def _post(self, payload: dict) -> dict | None:
        data = json.dumps(payload).encode("utf-8")
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        req = urllib.request.Request(self.url, data=data, headers=headers, method="POST")
        with urllib.request.urlopen(req, timeout=10) as resp:
            sid = resp.headers.get("Mcp-Session-Id")
            if sid:
                self.session = sid
            body = resp.read().decode("utf-8", errors="replace")
        if not body.strip():
            return None
        # Streamable HTTP may answer as SSE; take the last data: line.
        if body.lstrip().startswith("event:") or "\ndata:" in body or body.startswith("data:"):
            last = None
            for line in body.splitlines():
                if line.startswith("data:"):
                    last = line[5:].strip()
            body = last or "{}"
        return json.loads(body)

    def _rpc(self, method: str, params: dict | None = None) -> dict:
        payload = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            payload["params"] = params
        self.next_id += 1
        result = self._post(payload)
        if result is None:
            return {}
        if "error" in result:
            raise RuntimeError(result["error"])
        return result.get("result", {})

    def _initialize(self):
        self._rpc("initialize", {
            "protocolVersion": "2025-03-26",
            "capabilities": {},
            "clientInfo": {"name": "static_instancing_probe", "version": "1.0"},
        })
        self._post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def call(self, tool: str, arguments: dict) -> dict:
        result = self._rpc("tools/call", {"name": tool, "arguments": arguments})
        content = result.get("content") or []
        for item in content:
            if item.get("type") == "text":
                try:
                    return json.loads(item["text"])
                except json.JSONDecodeError:
                    return {"text": item["text"]}
        return result


def utility_row(drawcalls: dict) -> dict:
    for entry in drawcalls.get("shaderTypes", []):
        if entry.get("type") == "Utility":
            return entry
    return {}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8920)
    parser.add_argument("--samples", type=int, default=10)
    parser.add_argument("--interval", type=float, default=2.0)
    parser.add_argument("--set", action="append", default=[], metavar="KEY=VALUE",
                        help="set a StaticInstancing setting before sampling (Enabled, IncludeAlphaTested, InstanceSingles, MinBatchSize)")
    args = parser.parse_args()

    bench = DevBench(args.port)

    for assignment in args.set:
        key, _, value = assignment.partition("=")
        lowered = value.strip().lower()
        if lowered in ("true", "false"):
            parsed: object = lowered == "true"
        else:
            try:
                parsed = int(value)
            except ValueError:
                parsed = value
        reply = bench.call("communityshaders.feature", {"action": "set", "shortName": "StaticInstancing", "settings": {key: parsed}})
        print("set", key, "=", parsed, "->", json.dumps(reply)[:200])

    first = bench.call("communityshaders.inspect", {"kind": "drawcalls"})
    if "error" in first:
        print("drawcalls kind unavailable:", first.get("error"))
    elif not first.get("overlayVisible", True):
        print("NOTE: the performance overlay is closed; per-type draw counts and costs are stale until it is opened")
    print("sample  frameMs   fps  utilDraws  utilUs/draw  eligible  unique  collapsible  collapsed  instanced  ratio%")
    for i in range(args.samples):
        inst = bench.call("communityshaders.inspect", {"kind": "instancing"})
        draws = bench.call("communityshaders.inspect", {"kind": "drawcalls"})
        util = utility_row(draws)
        eligible = inst.get("eligible", 0)
        collapsible = inst.get("collapsible", 0)
        ratio = 100.0 * collapsible / eligible if eligible else 0.0
        print(f"{i:6d}  {draws.get('frameMs', 0.0):7.2f}  {draws.get('fps', 0.0):5.1f}  {util.get('drawCalls', 0):9d}  {util.get('costPerCallUs', 0.0):11.2f}  "
              f"{eligible:8d}  {inst.get('uniqueKeys', 0):6d}  {collapsible:11d}  {inst.get('collapsed', 0):9d}  {inst.get('instancedDraws', 0):9d}  {ratio:6.1f}")
        if i + 1 < args.samples:
            time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print("error:", exc, file=sys.stderr)
        sys.exit(1)
