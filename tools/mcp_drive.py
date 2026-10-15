#!/usr/bin/env python3
"""mcp_drive.py — drive qc-mcp (stdio JSON-RPC) with timed steps.

Wer:      Martin.Lonkwitz
Was:      Startet qc-mcp, sendet initialize + eine Liste von tools/call-Schritten
          mit optionalen Wartezeiten dazwischen und schreibt Antworten als JSONL.
Wann:     2026-09-04
Warum:    Hardware-Experimente brauchen definierte Pausen zwischen Nachrichten
          (z. B. "12 s nach Connect warten, dann schreiben"); ein Pipe aus
          printf kann nicht warten. Analyse-Skript, wird versioniert.
Projekt:  Ampersand
Nutzung:  tools/mcp_drive.py out.jsonl 'qc_status' 'sleep:12' 'qc_set_param:{"row":0,"column":1,"param":0,"value":0.5}' ...
          Schritt-Syntax: <tool>[:<json-args>] | sleep:<sekunden>
          Umgebung: QC_MCP=<pfad zu qc-mcp> (Default build/default/qc-mcp), QC_MCP_ARGS
"""
import json
import os
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    out_path = sys.argv[1]
    steps = sys.argv[2:]
    exe = os.environ.get("QC_MCP", "build/default/qc-mcp")
    args = os.environ.get("QC_MCP_ARGS", "--verbose").split()
    proc = subprocess.Popen([exe, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=open(out_path + ".stderr", "w"), text=True, bufsize=1)
    out = open(out_path, "w")
    rid = 0

    def call(obj):
        nonlocal rid
        proc.stdin.write(json.dumps(obj) + "\n")
        proc.stdin.flush()
        if "id" not in obj:
            return None
        line = proc.stdout.readline()
        out.write(line)
        out.flush()
        return json.loads(line)

    rid += 1
    call({"jsonrpc": "2.0", "id": rid, "method": "initialize",
          "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "mcp_drive", "version": "0"}}})
    call({"jsonrpc": "2.0", "method": "notifications/initialized"})
    for step in steps:
        t0 = time.time()
        if step.startswith("sleep:"):
            time.sleep(float(step[6:]))
            print(f"[{time.strftime('%H:%M:%S')}] slept {step[6:]} s")
            continue
        name, _, arg = step.partition(":")
        params = {"name": name, "arguments": json.loads(arg) if arg else {}}
        rid += 1
        rep = call({"jsonrpc": "2.0", "id": rid, "method": "tools/call", "params": params})
        res = rep.get("result", rep.get("error", {}))
        text = res.get("content", [{}])[0].get("text", json.dumps(res)) if isinstance(res, dict) else str(res)
        flag = "ERR " if isinstance(res, dict) and res.get("isError") else ""
        print(f"[{time.strftime('%H:%M:%S')}] {name} ({time.time() - t0:.1f}s): {flag}{text[:160].replace(chr(10), ' ')}")
    proc.stdin.close()
    proc.wait(timeout=30)
    return 0


if __name__ == "__main__":
    sys.exit(main())
