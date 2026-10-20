#!/usr/bin/env python3
"""Compare Sidecar with a real Behringer WING console, parameter by parameter.

Python 3.8+, standard library only (macOS, Windows, Linux). It talks to the
consoles over the WING's documented network protocols (Patrick-Gilles
Maillot's *WING Remote Protocols* document): discovery on UDP 2222 and OSC on
UDP 2223. It never sends anything that could change a parameter.

  python3 wing_compare.py discover
      Broadcasts "WING?" on UDP 2222 and lists every console that answers
      (ip, name, model, serial, firmware). Sidecar answers with model "sidecar".

  python3 wing_compare.py dump HOST -o wing.json
      Reads the whole parameter tree of the console at HOST, read-only, and
      writes every parameter (leaf) path with its value as the console reports
      it (text, and the numeric forms when the console gives them) and its
      description (type, range, unit, choices) to a JSON file.

  python3 wing_compare.py diff wing.json sidecar.json -o report.md
      Lists the paths only one console has, the value differences and the
      description (type / range / choices) differences, grouped by top-level
      node (ch, aux, bus, main, mtx, dca, fx, io, cfg, ...), as markdown.

  python3 wing_compare.py compare WING_IP SIDECAR_IP
      Dumps both consoles and diffs them; the files go to a new folder named
      wing_compare-YYYYMMDD-HHMMSS (wing.json, sidecar.json, report.md).

For a meaningful comparison load the same show / snapshot on both consoles
first (Sidecar: LIBRARY > SHOW > IMPORT WING). The tree depends on the models
chosen (EQ, dynamics, FX...): a different model shows up as paths only on one
side.

How the dump reads the tree (all requests are OSC messages without a value,
or with the single string "?" or "*", which the protocol defines as queries):
  1. node listings: a path without arguments returns its children's names;
  2. node descriptions: `<node> ,s ?` describes each child (node or parameter,
     type, range, unit, choices, r/o); children it doesn't describe are
     classified by a plain get;
  3. values: a plain get of every parameter (--values get, the default), or
     node dumps `<node> ,s *` (--values dump), walking down to smaller nodes
     when a dump doesn't come back (too big for one UDP packet) or is
     incomplete, plus gets for what the dumps leave out (read-only values).
Requests are pipelined (--window) with timeouts and retries; the window shrinks
when the console drops requests.
"""

import argparse
import datetime
import json
import os
import re
import socket
import struct
import sys
import time
from collections import OrderedDict, deque

DISCOVERY_PORT = 2222
OSC_PORT = 2223
FORMAT_VERSION = 1

# --------------------------------------------------------------------------
# OSC codec


def _pad_str(s):
    b = s.encode("utf-8") + b"\0"
    return b + b"\0" * ((4 - len(b) % 4) % 4)


def osc_encode(addr, args=()):
    out = _pad_str(addr)
    if not args:
        return out  # a bare address, as the protocol document's examples
    tags = ","
    body = b""
    for a in args:
        if isinstance(a, str):
            tags += "s"
            body += _pad_str(a)
        else:
            raise ValueError("only string arguments are ever sent")
    return out + _pad_str(tags) + body


def _read_str(b, i):
    end = b.index(b"\0", i)
    s = b[i:end].decode("utf-8", "replace")
    return s, i + ((end - i + 1 + 3) // 4) * 4


def osc_decode(b):
    """Returns (address, [args]) or None. Tolerates a missing type tag string."""
    try:
        addr, i = _read_str(b, 0)
        if not addr.startswith("/"):
            return None
        if i >= len(b):
            return addr, []
        tags, i = _read_str(b, i)
        if not tags.startswith(","):
            return addr, []
        args = []
        for t in tags[1:]:
            if t == "i":
                args.append(struct.unpack(">i", b[i:i + 4])[0])
                i += 4
            elif t == "f":
                args.append(struct.unpack(">f", b[i:i + 4])[0])
                i += 4
            elif t == "h":
                args.append(struct.unpack(">q", b[i:i + 8])[0])
                i += 8
            elif t == "d":
                args.append(struct.unpack(">d", b[i:i + 8])[0])
                i += 8
            elif t == "s" or t == "S":
                s, i = _read_str(b, i)
                args.append(s)
            elif t == "b":
                n = struct.unpack(">i", b[i:i + 4])[0]
                i += 4
                args.append({"blob": b[i:i + n].hex()})
                i += n + (4 - n % 4) % 4
            elif t == "T":
                args.append(True)
            elif t == "F":
                args.append(False)
            elif t == "N":
                args.append(None)
            else:
                break  # unknown tag: keep what was decoded
        return addr, args
    except (ValueError, struct.error, IndexError):
        return None


def tidy(v):
    """A float32 as JSON: rounded to what float32 can tell apart."""
    if isinstance(v, float):
        if v != v or v in (float("inf"), float("-inf")):
            return str(v)
        return float("%.7g" % v)
    return v


def norm_addr(a):
    if a.endswith("/") and len(a) > 1:
        a = a.rstrip("/") or "/"
    return a or "/"


def child_path(parent, name):
    return ("/" if parent == "/" else parent + "/") + name


def top_of(path):
    p = path.strip("/").split("/", 1)[0]
    return p or "/"


# --------------------------------------------------------------------------
# Progress


class Progress:
    def __init__(self, quiet=False):
        self.quiet = quiet
        self.tty = sys.stderr.isatty()
        self.last = 0.0
        self.width = 0

    def show(self, text, force=False):
        if self.quiet:
            return
        now = time.time()
        if not force and now - self.last < (0.2 if self.tty else 5.0):
            return
        self.last = now
        if self.tty:
            pad = max(0, self.width - len(text))
            sys.stderr.write("\r" + text + " " * pad)
            self.width = len(text)
        else:
            sys.stderr.write(text + "\n")
        sys.stderr.flush()

    def done(self, text):
        if self.quiet:
            return
        if self.tty:
            sys.stderr.write("\r" + text + " " * max(0, self.width - len(text)) + "\n")
            self.width = 0
        else:
            sys.stderr.write(text + "\n")
        sys.stderr.flush()


# --------------------------------------------------------------------------
# Read-only OSC client


class ReadOnlyOsc:
    """Pipelined OSC queries. The only messages it can send are a bare address
    (a get / node listing) or one string argument "?" or "*" (node description
    and node dump); anything else raises before it reaches the network."""

    def __init__(self, host, port=OSC_PORT, timeout=0.5, retries=3, window=32,
                 progress=None):
        self.dest = (socket.gethostbyname(host), port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", 0))
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        except OSError:
            pass
        self.timeout = timeout
        self.retries = retries
        self.max_window = max(1, window)
        self.window = self.max_window
        self.progress = progress or Progress(True)
        self.sent = 0
        self.received = 0
        self.timeouts = 0
        self.nodes = set(["/"])  # paths known to be nodes ("*" may only go there)

    def close(self):
        self.sock.close()

    def _send(self, addr, args):
        if args not in ([], ["?"], ["*"]):
            raise AssertionError("refusing to send a value: %r %r" % (addr, args))
        if args == ["*"] and addr not in self.nodes:
            raise AssertionError("refusing to dump a path not known as a node: %r" % addr)
        self.sock.sendto(osc_encode(addr, args), self.dest)
        self.sent += 1

    def _recv(self, wait):
        self.sock.settimeout(max(0.001, wait))
        try:
            data, src = self.sock.recvfrom(65535)
        except socket.timeout:
            return None
        except OSError:
            # Windows reports an ICMP "port unreachable" as a reset.
            time.sleep(min(wait, 0.05))
            return None
        if src[0] != self.dest[0]:
            return None
        self.received += 1
        return osc_decode(data)

    def query(self, addr, args=(), timeout=None, retries=None):
        r = self.run([(addr, addr, list(args))], "", timeout=timeout, retries=retries)
        return r.get(addr)

    def run(self, items, label, timeout=None, retries=None, adapt=True, window=None):
        """items: [(key, addr, args)]; returns {key: (addr, args) | ("ERROR", text) | None}.
        Replies are matched by address; an address is never in flight twice.
        adapt=False: unanswered requests are expected (probes), so they don't
        shrink the window; window: a fixed window for this batch."""
        saved = self.window
        if window:
            self.window = window
        timeout = self.timeout if timeout is None else timeout
        retries = self.retries if retries is None else retries
        results = {}
        pending = deque(items)
        inflight = OrderedDict()  # norm addr -> [key, addr, args, sent_at, tries]
        waiting = deque()  # same address as one in flight
        total = len(items)
        streak = 0
        last_shrink = 0.0
        start = time.time()
        while pending or inflight or waiting:
            while waiting and norm_addr(waiting[0][1]) not in inflight:
                pending.appendleft(waiting.popleft())
            while pending and len(inflight) < self.window:
                key, addr, args = pending.popleft()
                na = norm_addr(addr)
                if na in inflight:
                    waiting.append((key, addr, args))
                    continue
                self._send(addr, args)
                inflight[na] = [key, addr, args, time.time(), 1]
            if not inflight:
                continue
            oldest = min(e[3] for e in inflight.values())
            msg = self._recv(oldest + timeout - time.time())
            if msg is not None:
                raddr, rargs = msg
                na = norm_addr(raddr)
                err = False
                if na not in inflight and na.endswith("*") and norm_addr(na[:-1]) in inflight:
                    na, err = norm_addr(na[:-1]), True
                if na not in inflight and len(inflight) == 1 and self.window == 1:
                    # Lenient: one request in flight, take its answer whatever
                    # the address looks like (a console that canonicalises paths).
                    na = next(iter(inflight))
                if na in inflight:
                    e = inflight.pop(na)
                    results[e[0]] = ("ERROR", " ".join(str(a) for a in rargs)) if err else (raddr, rargs)
                    streak += 1
                    if streak >= 64 and self.window < self.max_window:
                        self.window += 1
                        streak = 0
            now = time.time()
            for na, e in list(inflight.items()):
                if now - e[3] < timeout:
                    continue
                if adapt:
                    self.timeouts += 1
                    streak = 0
                    if now - last_shrink > timeout:  # once per burst of losses
                        self.window = max(1, self.window // 2)
                        last_shrink = now
                if e[4] > retries:
                    results[e[0]] = None
                    del inflight[na]
                else:
                    e[3] = now
                    e[4] += 1
                    self._send(e[1], e[2])
            if label:
                done = len(results)
                rate = done / max(0.001, time.time() - start)
                self.progress.show("%s %d/%d  %.0f/s  window %d  timeouts %d"
                                   % (label, done, total, rate, self.window, self.timeouts))
        if window:
            self.window = saved
        return results


# --------------------------------------------------------------------------
# Discovery


def parse_info(text):
    parts = text.strip().strip("\0").split(",")
    keys = ["tag", "ip", "name", "model", "serial", "firmware"]
    info = {k: (parts[i].strip() if i < len(parts) else "") for i, k in enumerate(keys)}
    if len(parts) > len(keys):
        info["extra"] = ",".join(parts[len(keys):])
    return info


def local_ipv4():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))  # no packet is sent for a UDP connect
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def discover(targets, port, wait):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("", 0))
    dests = list(targets) or ["255.255.255.255"]
    if not targets:
        ip = local_ipv4()
        if ip and not ip.startswith("127."):
            dests.append(".".join(ip.split(".")[:3] + ["255"]))
        dests.append("127.0.0.1")
    found = OrderedDict()
    for attempt in range(2):
        for d in dests:
            try:
                sock.sendto(b"WING?", (d, port))
            except OSError as e:
                if attempt == 0:
                    sys.stderr.write("cannot send to %s: %s\n" % (d, e))
        end = time.time() + wait / 2.0
        while time.time() < end:
            sock.settimeout(max(0.01, end - time.time()))
            try:
                data, src = sock.recvfrom(2048)
            except socket.timeout:
                break
            except OSError:
                continue
            text = data.decode("utf-8", "replace")
            if not text.startswith("WING"):
                continue
            info = parse_info(text)
            info["from"] = src[0]
            found[(src[0], text)] = info
    sock.close()
    return list(found.values())


def cmd_discover(a):
    found = discover(a.target or [], a.port, a.timeout)
    if a.json:
        print(json.dumps(found, indent=2))
        return 0
    if not found:
        print("No console answered. Check the network, firewall (UDP 2222), or try "
              "--target <subnet broadcast or console IP>.")
        return 1
    print("%-15s  %-20s  %-12s  %-14s  %s" % ("IP (answered from)", "NAME", "MODEL", "SERIAL", "FIRMWARE"))
    for f in found:
        ip = f["from"] + ("" if f["ip"] in ("", f["from"]) else " (says %s)" % f["ip"])
        print("%-15s  %-20s  %-12s  %-14s  %s" % (ip, f["name"], f["model"], f["serial"], f["firmware"]))
    print("\nSidecar answers with model 'sidecar'; a real console with its own model.")
    return 0


# --------------------------------------------------------------------------
# Dump

TYPE_WORDS = ("node", "lin", "log", "fader", "int", "list", "flist", "string")


def parse_describe(args):
    """`<node> ,s ?` -> {child name: (kind, ro, desc)}; kind is "node", "leaf" or None."""
    text = "\n".join(a for a in args if isinstance(a, str))
    out = {}
    for line in text.splitlines():
        toks = line.split()
        if not toks:
            continue
        name = toks[0]
        rest = line.strip()[len(name):].strip()
        t = [x for x in toks[1:] if x != "r/o"]
        ro = "r/o" in toks[1:]
        kind = None
        if t:
            kind = "node" if t[0] == "node" else "leaf"
        out[name] = (kind, ro, rest)
    return out


def split_dump(text):
    """Splits a node dump on commas outside single quotes; None if a quote is open
    (a truncated reply)."""
    parts, cur, quoted = [], [], False
    for ch in text:
        if ch == "'":
            quoted = not quoted
            cur.append(ch)
        elif ch == "," and not quoted:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if quoted:
        return None
    if cur:
        parts.append("".join(cur))
    return [p for p in parts if p.strip()]


def parse_dump(base, text):
    """`a.b=v,c=w,..d.e=x`: leading dots go up from the last element's parent.
    Returns {absolute path: value text} or None when it doesn't parse."""
    els = split_dump(text)
    if els is None:
        return None
    out = {}
    pos = []
    for el in els:
        el = el.strip()
        if el.startswith("/"):
            pos = []
            el = el.lstrip("/")
            rel_root = True
        else:
            rel_root = False
        ups = len(el) - len(el.lstrip("."))
        el = el[ups:]
        if "=" not in el:
            return None
        path, val = el.split("=", 1)
        parts = [p for p in path.split(".") if p]
        if not parts:
            return None
        pos = pos[:max(0, len(pos) - ups)] + parts[:-1]
        val = val.strip()
        if len(val) >= 2 and val[0] == "'" and val[-1] == "'":
            val = val[1:-1]
        root = "/" if rel_root else base
        p = root
        for seg in pos + [parts[-1]]:
            p = child_path(p, seg)
        out[p] = val
    return out


def leaf_value(args):
    return [tidy(a) for a in args]


class Dumper:
    def __init__(self, osc, progress, describe=True, values="get", root="/", log=None):
        self.osc = osc
        self.progress = progress
        self.describe = describe
        self.values_mode = values
        self.root = norm_addr(root)
        self.log = log or (lambda s: None)
        self.children = {}  # node path -> [child paths]
        self.leaves = OrderedDict()  # path -> {"value", "desc", "ro"}
        self.missing = []  # paths whose value never came back
        self.errors = {}  # path -> error text from the console
        self.unlisted = []  # nodes whose listing never came back
        self.dump_stats = {"dumps": 0, "dump_failed": 0, "dump_extra_keys": 0}

    def _leaf(self, path):
        return self.leaves.setdefault(path, {})

    def walk(self):
        level = [self.root]
        depth = 0
        while level:
            depth += 1
            for p in level:
                self.osc.nodes.add(p)
            res = self.osc.run([(p, p, []) for p in level], "tree depth %d: listing" % depth)
            listed = {}
            for p in level:
                r = res.get(p)
                if r is None or r[0] == "ERROR":
                    self.unlisted.append(p)
                    continue
                names = [a for a in r[1] if isinstance(a, str)]
                if len(names) != len(r[1]):
                    # Not a listing after all: a parameter (only reachable here
                    # through the lenient single-request path or a root leaf).
                    self._leaf(p)["value"] = leaf_value(r[1])
                    continue
                listed[p] = names
                self.children[p] = [child_path(p, n) for n in names]
            descs = {}
            if self.describe and listed:
                dres = self.osc.run([(p, p, ["?"]) for p in listed], "tree depth %d: describe" % depth)
                for p in listed:
                    r = dres.get(p)
                    if r and r[0] != "ERROR":
                        descs[p] = parse_describe(r[1])
            nxt, unknown = [], []
            for p, names in listed.items():
                d = descs.get(p, {})
                for n in names:
                    c = child_path(p, n)
                    kind, ro, text = d.get(n, (None, False, ""))
                    if kind == "node":
                        nxt.append(c)
                    elif kind == "leaf":
                        e = self._leaf(c)
                        e["desc"] = text
                        if ro:
                            e["ro"] = True
                    else:
                        unknown.append(c)
            nxt += self._classify(unknown, depth)
            self.progress.done("tree depth %d: %d nodes, %d parameters so far"
                               % (depth, len(self.children), len(self.leaves)))
            level = nxt

    def _classify(self, paths, depth):
        """Children the description didn't cover: a get tells a parameter (numbers,
        or a single string) from a node (a list of names)."""
        if not paths:
            return []
        res = self.osc.run([(p, p, []) for p in paths], "tree depth %d: classify" % depth)
        nodes, maybe = [], []
        for p in paths:
            r = res.get(p)
            if r is None:
                self.unlisted.append(p)
                continue
            if r[0] == "ERROR":
                self.errors[p] = r[1]
                continue
            args = r[1]
            if all(isinstance(a, str) for a in args) and len(args) > 1:
                nodes.append(p)
            elif len(args) == 1 and isinstance(args[0], str) and args[0] and "/" not in args[0]:
                maybe.append((p, args))
            else:
                self._leaf(p)["value"] = leaf_value(args)
        if maybe:
            # One string: a string parameter, or a node with one child?
            probe = [(p, child_path(p, a[0]), []) for p, a in maybe]
            pres = self.osc.run(probe, "tree depth %d: probe" % depth, retries=1, adapt=False,
                                window=self.osc.max_window * 4)
            for p, args in maybe:
                r = pres.get(p)
                if r is not None and r[0] != "ERROR":
                    nodes.append(p)
                else:
                    self._leaf(p)["value"] = leaf_value(args)
        return nodes

    def _subtree_leaves(self, node):
        out = []
        stack = [node]
        while stack:
            n = stack.pop()
            for c in self.children.get(n, []):
                if c in self.children:
                    stack.append(c)
                elif c in self.leaves:
                    out.append(c)
        return out

    def _expected_in_dump(self, node):
        # A dump holds what a snapshot saves: no read-only parameters, no "$" nodes.
        exp = []
        for p in self._subtree_leaves(node):
            rel = p[len(node):] if node != "/" else p
            if "/$" in rel or self.leaves[p].get("ro"):
                continue
            exp.append(p)
        return exp

    def read_values(self):
        if self.values_mode == "dump":
            self._read_dumps()
        todo = [p for p, e in self.leaves.items() if "value" not in e]
        if todo:
            res = self.osc.run([(p, p, []) for p in todo], "values (get)")
            for p in todo:
                r = res.get(p)
                if r is None:
                    self.missing.append(p)
                elif r[0] == "ERROR":
                    self.errors[p] = r[1]
                else:
                    self.leaves[p]["value"] = leaf_value(r[1])
            self.progress.done("values (get): %d read, %d missing" % (len(todo) - len(self.missing), len(self.missing)))

    def _read_dumps(self):
        level = [c for c in self.children.get(self.root, []) if c in self.children]
        if self.root != "/":
            level = [self.root]
        while level:
            res = self.osc.run([(p, p, ["*"]) for p in level], "values (dump)", retries=1,
                               timeout=max(self.osc.timeout, 1.0))
            nxt = []
            for p in level:
                self.dump_stats["dumps"] += 1
                r = res.get(p)
                parsed = None
                if r is not None and r[0] != "ERROR":
                    text = "".join(a for a in r[1] if isinstance(a, str))
                    parsed = parse_dump(p, text)
                expected = self._expected_in_dump(p)
                if parsed is not None and all(e in parsed for e in expected):
                    for k, v in parsed.items():
                        if k in self.leaves:
                            if "value" not in self.leaves[k]:
                                self.leaves[k]["value"] = [v]
                        else:
                            self.dump_stats["dump_extra_keys"] += 1
                    continue
                # Too big for one packet, truncated or unreadable: go one level down.
                self.dump_stats["dump_failed"] += 1
                nxt += [c for c in self.children.get(p, []) if c in self.children]
            level = nxt
        self.progress.done("values (dump): %d dumps, %d walked down" %
                           (self.dump_stats["dumps"], self.dump_stats["dump_failed"]))


def dump_console(host, a, progress):
    osc = ReadOnlyOsc(host, a.port, a.timeout, a.retries, a.window, progress)
    started = datetime.datetime.now().isoformat(timespec="seconds")
    t0 = time.time()
    info = {}
    r = osc.query("/?", timeout=max(a.timeout, 1.0), retries=3)
    if r is not None and r[0] != "ERROR" and r[1] and isinstance(r[1][0], str):
        info = parse_info(r[1][0])
        progress.done("%s: %s, model %s, serial %s, firmware %s"
                      % (host, info.get("name"), info.get("model"), info.get("serial"), info.get("firmware")))
    else:
        progress.done("%s: no answer to /? (console info); continuing" % host)
    d = Dumper(osc, progress, describe=not a.no_describe, values=a.values, root=a.root)
    d.walk()
    if not d.children:
        osc.close()
        raise SystemExit("%s: no answer on OSC port %d (is it the right address? "
                         "is OSC enabled / not blocked by a firewall?)" % (host, a.port))
    d.read_values()
    secs = time.time() - t0
    osc.close()
    leaves = OrderedDict(sorted(d.leaves.items()))
    return OrderedDict([
        ("tool", "wing_compare"),
        ("format", FORMAT_VERSION),
        ("host", host),
        ("console", info),
        ("started", started),
        ("seconds", round(secs, 1)),
        ("method", {"values": a.values, "describe": not a.no_describe, "root": a.root}),
        ("stats", OrderedDict([
            ("nodes", len(d.children)),
            ("leaves", len(leaves)),
            ("with_value", sum(1 for e in leaves.values() if "value" in e)),
            ("with_desc", sum(1 for e in leaves.values() if "desc" in e)),
            ("missing", len(d.missing)),
            ("unlisted", len(d.unlisted)),
            ("errors", len(d.errors)),
            ("requests", osc.sent),
            ("replies", osc.received),
            ("timeouts", osc.timeouts),
        ] + list(d.dump_stats.items() if a.values == "dump" else []))),
        ("missing", sorted(d.missing)),
        ("unlisted", sorted(d.unlisted)),
        ("errors", OrderedDict(sorted(d.errors.items()))),
        ("leaves", leaves),
    ])


def write_json(data, path):
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write("\n")


def cmd_dump(a):
    progress = Progress(a.quiet)
    data = dump_console(a.host, a, progress)
    out = a.output or "wing-dump-%s.json" % a.host.replace(":", "_")
    write_json(data, out)
    s = data["stats"]
    print("%s: %d parameters (%d with values, %d described) in %d nodes, %.1f s -> %s"
          % (a.host, s["leaves"], s["with_value"], s["with_desc"], s["nodes"], data["seconds"], out))
    if s["missing"] or s["unlisted"]:
        print("  %d values and %d nodes did not answer (see 'missing' / 'unlisted' in the file)"
              % (s["missing"], s["unlisted"]))
    return 0


# --------------------------------------------------------------------------
# Diff


def _num_close(x, y):
    if isinstance(x, bool) or isinstance(y, bool):
        return x == y
    if isinstance(x, (int, float)) and isinstance(y, (int, float)):
        return abs(x - y) <= 1e-4 * max(1.0, abs(x), abs(y))
    return x == y


def values_differ(va, vb):
    """Compares what both sides report: the text form, and the numeric forms
    when both have them (a node dump only has the text)."""
    if va is None or vb is None:
        return (va is None) != (vb is None)
    sa = va[0] if va and isinstance(va[0], str) else None
    sb = vb[0] if vb and isinstance(vb[0], str) else None
    if sa is not None and sb is not None and sa.strip() != sb.strip():
        return True
    na, nb = va[1:] if sa is not None else va, vb[1:] if sb is not None else vb
    if na and nb:
        if len(na) != len(nb):
            return True
        return not all(_num_close(x, y) for x, y in zip(na, nb))
    if sa is None or sb is None:
        return not all(_num_close(x, y) for x, y in zip(va, vb)) or len(va) != len(vb)
    return False


def fmt_value(v):
    if v is None:
        return "(no value)"
    parts = []
    for x in v:
        if isinstance(x, str):
            parts.append(json.dumps(x, ensure_ascii=False))
        else:
            parts.append(str(x))
    return " ".join(parts) if parts else "(empty)"


def md_cell(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def diff_dumps(A, B, ignore=()):
    rx = [re.compile(x) for x in ignore]
    la, lb = A["leaves"], B["leaves"]

    def keep(p):
        return not any(r.search(p) for r in rx)

    groups = {}

    def g(p):
        return groups.setdefault(top_of(p), {"only_a": [], "only_b": [], "value": [], "desc": [], "same": 0})

    for p in sorted(set(la) | set(lb)):
        if not keep(p):
            continue
        if p not in lb:
            g(p)["only_a"].append(p)
        elif p not in la:
            g(p)["only_b"].append(p)
        else:
            ea, eb = la[p], lb[p]
            changed = False
            if values_differ(ea.get("value"), eb.get("value")):
                g(p)["value"].append((p, ea.get("value"), eb.get("value")))
                changed = True
            da, db = ea.get("desc"), eb.get("desc")
            if da is not None and db is not None and " ".join(da.split()) != " ".join(db.split()):
                g(p)["desc"].append((p, da, db))
                changed = True
            elif bool(ea.get("ro")) != bool(eb.get("ro")) and da is not None and db is not None:
                g(p)["desc"].append((p, ("r/o " if ea.get("ro") else "") + da, ("r/o " if eb.get("ro") else "") + db))
                changed = True
            if not changed:
                g(p)["same"] += 1
    return groups


ORDER = ["ch", "aux", "bus", "main", "mtx", "dca", "mgrp", "fx", "io", "cfg", "play", "rec", "$ctl"]


def group_order(names):
    return sorted(names, key=lambda n: (ORDER.index(n) if n in ORDER else len(ORDER), n))


def label(D, fallback):
    c = D.get("console") or {}
    bits = [c.get("name"), c.get("model"), c.get("firmware")]
    bits = [b for b in bits if b]
    return "%s (%s)" % (D.get("host", fallback), ", ".join(bits)) if bits else D.get("host", fallback)


def report_md(A, B, groups, name_a, name_b, limit):
    la, lb = label(A, name_a), label(B, name_b)
    tot = {k: sum(len(v[k]) for v in groups.values()) for k in ("only_a", "only_b", "value", "desc")}
    same = sum(v["same"] for v in groups.values())
    out = []
    w = out.append
    w("# WING parameter comparison\n")
    w("- **A:** %s, %s, %d parameters, read %s (%s s)" % (md_cell(la), name_a, len(A["leaves"]), A.get("started", "?"), A.get("seconds", "?")))
    w("- **B:** %s, %s, %d parameters, read %s (%s s)" % (md_cell(lb), name_b, len(B["leaves"]), B.get("started", "?"), B.get("seconds", "?")))
    for D, n in ((A, "A"), (B, "B")):
        s = D.get("stats", {})
        if s.get("missing") or s.get("unlisted"):
            w("- %s: %d values and %d nodes did not answer during the dump (not counted as differences)."
              % (n, s.get("missing", 0), s.get("unlisted", 0)))
    w("")
    w("**%d identical, %d only on A, %d only on B, %d value differences, %d description differences.**\n"
      % (same, tot["only_a"], tot["only_b"], tot["value"], tot["desc"]))
    w("| node | identical | only A | only B | value | description |")
    w("|---|---:|---:|---:|---:|---:|")
    for n in group_order(groups):
        v = groups[n]
        w("| %s | %d | %d | %d | %d | %d |" % (md_cell(n), v["same"], len(v["only_a"]), len(v["only_b"]), len(v["value"]), len(v["desc"])))
    w("")

    def cap(items):
        if limit and len(items) > limit:
            return items[:limit], len(items) - limit
        return items, 0

    for n in group_order(groups):
        v = groups[n]
        if not (v["only_a"] or v["only_b"] or v["value"] or v["desc"]):
            continue
        w("## %s\n" % md_cell(n))
        for key, title in (("only_a", "Only on A"), ("only_b", "Only on B")):
            if v[key]:
                items, more = cap(v[key])
                w("### %s (%d)\n" % (title, len(v[key])))
                src = A["leaves"] if key == "only_a" else B["leaves"]
                w("| path | value | description |")
                w("|---|---|---|")
                for p in items:
                    e = src[p]
                    w("| `%s` | %s | %s |" % (md_cell(p), md_cell(fmt_value(e.get("value"))), md_cell(e.get("desc", ""))))
                if more:
                    w("\n... and %d more (use --limit 0 for all)" % more)
                w("")
        if v["value"]:
            items, more = cap(v["value"])
            w("### Value differences (%d)\n" % len(v["value"]))
            w("| path | A | B |")
            w("|---|---|---|")
            for p, x, y in items:
                w("| `%s` | %s | %s |" % (md_cell(p), md_cell(fmt_value(x)), md_cell(fmt_value(y))))
            if more:
                w("\n... and %d more (use --limit 0 for all)" % more)
            w("")
        if v["desc"]:
            items, more = cap(v["desc"])
            w("### Description differences: type, range, unit, choices (%d)\n" % len(v["desc"]))
            w("| path | A | B |")
            w("|---|---|---|")
            for p, x, y in items:
                w("| `%s` | %s | %s |" % (md_cell(p), md_cell(x), md_cell(y)))
            if more:
                w("\n... and %d more (use --limit 0 for all)" % more)
            w("")
    return "\n".join(out) + "\n", tot


def load(path):
    with open(path, encoding="utf-8") as f:
        D = json.load(f)
    if D.get("tool") != "wing_compare" or "leaves" not in D:
        raise SystemExit("%s: not a wing_compare dump" % path)
    return D


def write_report(A, B, name_a, name_b, out, ignore, limit):
    groups = diff_dumps(A, B, ignore)
    md, tot = report_md(A, B, groups, name_a, name_b, limit)
    if out:
        d = os.path.dirname(os.path.abspath(out))
        os.makedirs(d, exist_ok=True)
        with open(out, "w", encoding="utf-8") as f:
            f.write(md)
        print("only on A %d, only on B %d, value differences %d, description differences %d -> %s"
              % (tot["only_a"], tot["only_b"], tot["value"], tot["desc"], out))
    else:
        sys.stdout.write(md)
    return tot


def cmd_diff(a):
    A, B = load(a.a), load(a.b)
    write_report(A, B, os.path.basename(a.a), os.path.basename(a.b), a.output, a.ignore or [], a.limit)
    return 0


def cmd_compare(a):
    folder = a.output or "wing_compare-%s" % datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    os.makedirs(folder, exist_ok=True)
    progress = Progress(a.quiet)
    files = []
    dumps = []
    for host, name in ((a.wing, "wing.json"), (a.sidecar, "sidecar.json")):
        progress.done("== dumping %s" % host)
        D = dump_console(host, a, progress)
        p = os.path.join(folder, name)
        write_json(D, p)
        s = D["stats"]
        print("%s: %d parameters in %d nodes, %.1f s -> %s" % (host, s["leaves"], s["nodes"], D["seconds"], p))
        files.append(p)
        dumps.append(D)
    write_report(dumps[0], dumps[1], "WING " + a.wing, "Sidecar " + a.sidecar,
                 os.path.join(folder, "report.md"), a.ignore or [], a.limit)
    return 0


# --------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="wing_compare.py",
        description="Compare Sidecar with a real Behringer WING over the WING's network "
                    "protocols (discovery UDP 2222, OSC UDP 2223). Read-only: it only sends "
                    "queries (gets, node listings, '?' descriptions, '*' dumps), never values.",
        epilog="Example: wing_compare.py discover; wing_compare.py compare 192.168.1.62 192.168.1.20",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")

    def dump_opts(p):
        p.add_argument("--port", type=int, default=OSC_PORT, help="OSC port (default %(default)s)")
        p.add_argument("--timeout", type=float, default=0.5, help="seconds to wait for an answer (default %(default)s)")
        p.add_argument("--retries", type=int, default=3, help="resends of an unanswered request (default %(default)s)")
        p.add_argument("--window", type=int, default=32,
                       help="requests in flight at once; shrinks by itself when the console drops "
                            "requests (default %(default)s; use 1-4 for a slow network)")
        p.add_argument("--values", choices=("get", "dump"), default="get",
                       help="read values with a get per parameter (text and numbers, default) or with "
                            "node dumps ',s *' (text only, fewer packets)")
        p.add_argument("--no-describe", action="store_true",
                       help="skip the ',s ?' descriptions (type, range, choices)")
        p.add_argument("--root", default="/", help="only read this subtree, e.g. /ch/1 (default: all)")
        p.add_argument("-q", "--quiet", action="store_true", help="no progress output")

    p = sub.add_parser("discover", help="list the consoles answering 'WING?' on UDP 2222")
    p.add_argument("--target", action="append", metavar="ADDR",
                   help="where to send WING? (repeatable; default: 255.255.255.255, the local /24 "
                        "broadcast and 127.0.0.1); a subnet broadcast or a console IP")
    p.add_argument("--port", type=int, default=DISCOVERY_PORT, help="discovery port (default %(default)s)")
    p.add_argument("--timeout", type=float, default=2.0, help="seconds to listen (default %(default)s)")
    p.add_argument("--json", action="store_true", help="print JSON")
    p.set_defaults(func=cmd_discover)

    p = sub.add_parser("dump", help="read a console's whole parameter tree into a JSON file (read-only)")
    p.add_argument("host", help="console IP address or host name")
    p.add_argument("-o", "--output", help="JSON file (default wing-dump-HOST.json)")
    dump_opts(p)
    p.set_defaults(func=cmd_dump)

    def diff_opts(p):
        p.add_argument("--ignore", action="append", metavar="REGEX",
                       help="leave out paths matching this regular expression (repeatable), "
                            "e.g. --ignore '/\\$' for the read-only state values")
        p.add_argument("--limit", type=int, default=500,
                       help="rows per table before '... and N more' (0 = all, default %(default)s)")

    p = sub.add_parser("diff", help="compare two dumps, as markdown")
    p.add_argument("a", help="dump A (e.g. the real WING)")
    p.add_argument("b", help="dump B (e.g. Sidecar)")
    p.add_argument("-o", "--output", help="markdown file (default: print)")
    diff_opts(p)
    p.set_defaults(func=cmd_diff)

    p = sub.add_parser("compare", help="dump a real WING and Sidecar, then diff them")
    p.add_argument("wing", help="the real WING's IP address")
    p.add_argument("sidecar", help="Sidecar's IP address")
    p.add_argument("-o", "--output", help="folder (default wing_compare-YYYYMMDD-HHMMSS)")
    dump_opts(p)
    diff_opts(p)
    p.set_defaults(func=cmd_compare)

    a = ap.parse_args(argv)
    if not getattr(a, "func", None):
        ap.print_help()
        return 2
    try:
        return a.func(a)
    except KeyboardInterrupt:
        sys.stderr.write("\ninterrupted\n")
        return 130
    except socket.gaierror as e:
        sys.stderr.write("cannot resolve host: %s\n" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
