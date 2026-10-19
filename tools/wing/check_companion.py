#!/usr/bin/env python3
"""Check that every OSC path the Bitfocus Companion WING module builds is
answered by a WING OSC server (the Sidecar remote or its headless emulator).

The module's command helpers (src/commands/*.ts) are tiny functions that
return template strings such as `${Node(channel)}/fdr`. This script expands
them with index 1, sends an OSC GET for each path and sorts the answers into
answered and unanswered paths. The module sources are read only;
nothing from them is copied into Sidecar.

  python3 -I tools/wing/check_companion.py <companion-module>/src/commands \
      [host:port] [--list]
"""

import os
import re
import socket
import struct
import sys

FUNC = re.compile(r"export function (\w+)\(([^)]*)\)[^{]*\{")
NAMESPACE = re.compile(r"export namespace (\w+)\s*\{")
SAMPLE = {"talkback": "A", "aes": "A", "band": "lo", "s": "x"}


def body_at(text, start):
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
    return ""


def load(commands_dir):
    funcs = {}
    for name in sorted(os.listdir(commands_dir)):
        if not name.endswith(".ts"):
            continue
        with open(os.path.join(commands_dir, name), encoding="utf-8") as f:
            text = f.read()
        ns = NAMESPACE.search(text)
        space = ns.group(1) if ns else name[:-3]
        for m in FUNC.finditer(text):
            params = [p.split(":")[0].strip() for p in m.group(2).split(",") if p.strip()]
            body = body_at(text, m.end() - 1)
            ret = re.search(r"return\s+(`[^`]*`|'[^']*')", body)
            if ret:
                funcs[(space, m.group(1))] = (params, ret.group(1)[1:-1])
    return funcs


def expand(funcs, space, name, args, depth=0):
    params, template = funcs[(space, name)]
    env = dict(zip(params, args))
    out = ""
    pos = 0
    for m in re.finditer(r"\$\{([^}]*)\}", template):
        out += template[pos:m.start()]
        pos = m.end()
        expr = m.group(1).strip()
        call = re.fullmatch(r"(?:(\w+)\.)?(\w+)\(([^)]*)\)", expr)
        if expr in env:
            out += str(env[expr])
        elif call and depth < 8:
            target = (call.group(1) or space, call.group(2))
            if target not in funcs:
                return None
            inner = [env.get(a.strip(), a.strip()) for a in call.group(3).split(",") if a.strip()]
            sub = expand(funcs, target[0], target[1], inner, depth + 1)
            if sub is None:
                return None
            out += sub
        else:
            return None
    return out + template[pos:]


def paths(funcs):
    found = set()
    for (space, name), (params, template) in funcs.items():
        args = [SAMPLE.get(p, 1) for p in params]
        p = expand(funcs, space, name, args)
        if p and p.startswith("/") and "${" not in p:
            found.add(p.rstrip("/") or "/")
    return sorted(found)


def osc_string(s):
    b = s.encode() + b"\0"
    return b + b"\0" * (-len(b) % 4)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    funcs = load(sys.argv[1])
    target = next((a for a in sys.argv[2:] if not a.startswith("--")), "127.0.0.1:2223")
    host, port = target.rsplit(":", 1)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.3)
    answered, missing = [], []
    for p in paths(funcs):
        sock.sendto(osc_string(p), (host, int(port)))
        try:
            data, _ = sock.recvfrom(65536)
        except socket.timeout:
            missing.append(p)
            continue
        if data.startswith(osc_string(p)):
            answered.append(p)
        else:
            missing.append(p)
    print(f"{len(funcs)} helpers, {len(answered) + len(missing)} paths: "
          f"{len(answered)} answered, {len(missing)} unanswered")
    if "--list" in sys.argv:
        for p in missing:
            print("unanswered", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
