#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Worst-case stack depth of the leaf's public entry points (issue #351).

Reads the call-graph files GCC writes with -fcallgraph-info=su (one `.ci` per
translation unit: every function's static frame size and its call edges) from a
leaf build tree, and walks the call graph from each public entry point to its
deepest path.

Indirect calls are resolved from the INDIRECT table, which names every
function-pointer call reachable from the leaf and what it can reach:

  - the application's callbacks (clock, random source, write sink) are not
    counted: their stack is the application's;
  - nanopb's field callbacks are resolved by context: the callback that
    encodes a message can reach only the callbacks of that message's fields,
    which is how the nesting Request > ResourceSpans > ScopeSpans > Span >
    Event > KeyValue > AnyValue bounds nanopb's recursion;
  - upb's allocator calls reach the backend's refusing allocator or upb's
    global (malloc) one.

Recursion is allowed only where RECURSION declares it, up to the message
nesting it follows. An indirect call the table does not resolve, or a cycle it
does not declare, fails the run, so the figure cannot silently leave a path out.

libc (memcpy, memset, ...) has no .ci file and counts as 0 bytes; the functions
are listed under the table.

Usage: leaf-stack.py <build-dir> <upb|nanopb> [label]
"""

import os
import re
import sys

ENTRY_POINTS = (
    "microtel_leaf_init",
    "microtel_leaf_span_start",
    "microtel_leaf_span_start_remote",
    "microtel_leaf_span_set_attribute",
    "microtel_leaf_span_add_event",
    "microtel_leaf_span_set_status",
    "microtel_leaf_span_end",
    "microtel_leaf_clock_sync",
    "microtel_leaf_encode",
    "microtel_leaf_encode_to",
    "microtel_leaf_encoded_size",
)

# The deepest OTLP nesting the leaf emits: Request > ResourceSpans > ScopeSpans
# > Span > Event > KeyValue > AnyValue.
MESSAGE_DEPTH = 7

USER = "user"  # an application callback: not counted
CONTEXT = "context"  # a nanopb field callback: resolved by the encoding context

# nanopb: which field callbacks each callback's message can reach. "root" is
# the ExportTraceServiceRequest that the backend's entry point encodes.
NANOPB_CALLBACKS = {
    "root": ("encode_resource_spans",),
    "encode_resource_spans": ("encode_attrs", "encode_scope_spans"),
    "encode_scope_spans": ("encode_str", "encode_spans"),
    "encode_spans": ("encode_str", "encode_attrs", "encode_events"),
    "encode_events": ("encode_str", "encode_attrs"),
    "encode_attrs": ("encode_kv_key", "encode_kv_string"),
    "encode_str": (),
    "encode_kv_key": (),
    "encode_kv_string": (),
}

UPB_ALLOCATORS = ("refuse_alloc", "upb_global_allocfunc")

# Every indirect call site reachable from the leaf, by calling function.
INDIRECT = {
    "read_clock": USER,  # config.now_ns
    "next_id_word": USER,  # config.random_bytes
    "sink_write": USER,  # the encode_to write callback (nanopb)
    "microtel_leaf_internal_encode_upb": USER,  # the encode_to write callback (upb)
    # nanopb
    "microtel_pb_write": ("buf_write", "sink_write"),  # the output stream
    "encode_field": ("microtel_pb_default_field_callback",),  # descriptor callback
    "microtel_pb_default_field_callback": CONTEXT,  # pb_callback_t.funcs
    "microtel_pb_encode": (),  # extensions: the OTLP descriptors have none
    "encode_basic_field": (),  # PB_LTYPE_SUBMSG_W_CB: the OTLP descriptors have none
    # upb: alloc.h's upb_malloc / upb_realloc / upb_free
    "microtel__upb_Arena_SlowMalloc_dont_copy_me__upb_internal_use_only": UPB_ALLOCATORS,
    "microtel_upb_Arena_Init": UPB_ALLOCATORS,
    "microtel_upb_Arena_Free": UPB_ALLOCATORS,
    "upb_Encoder_Encode": UPB_ALLOCATORS,
    "_upb_mapsorter_resize": UPB_ALLOCATORS,
}

# Declared recursion: the functions allowed to re-enter themselves, and how
# often per encoding context. Every other cycle in the call graph fails the run.
#
# upb's encoder enters encode_message once per nested message, and nothing else
# in the leaf recurses. nanopb enters pb_encode once for a callback's own
# message and once more for a static (non-callback) submessage inside it
# (ResourceSpans > Resource, ScopeSpans > InstrumentationScope, Span > Status,
# KeyValue > AnyValue); pb_check_proto3_default_value descends into such a
# submessage the same way.
NANOPB_CONTEXTS = ("root", "encode_resource_spans", "encode_scope_spans", "encode_spans",
                   "encode_events", "encode_attrs")
RECURSION = {
    "upb": {"encode_message": {"root": MESSAGE_DEPTH}},
    "nanopb": {
        "microtel_pb_encode": {
            "root": 1,
            "encode_resource_spans": 2,
            "encode_scope_spans": 2,
            "encode_spans": 2,
            "encode_events": 1,
            "encode_attrs": 2,
        },
        "pb_check_proto3_default_value": {ctx: 2 for ctx in NANOPB_CONTEXTS},
    },
}

NODE_RE = re.compile(r'^node: \{ title: "([^"]+)" label: "([^"]*)"')
EDGE_RE = re.compile(r'^edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')
SIZE_RE = re.compile(r"\\n(\d+) bytes \(([a-z,]+)\)")


def base_name(title):
    """`/path/file.c:name.isra.0` -> `name`."""
    name = title.rsplit(":", 1)[-1]
    return name.split(".", 1)[0]


class Graph:
    """Frame sizes and call edges of every function in the .ci files."""

    def __init__(self):
        self.frame = {}  # title -> bytes
        self.dynamic = set()
        self.edges = {}  # title -> [callee title]
        self.by_name = {}  # base name -> title (defined functions)

    def load(self, path):
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                node = NODE_RE.match(line)
                if node:
                    size = SIZE_RE.search(node.group(2))
                    if size:
                        self.frame[node.group(1)] = int(size.group(1))
                        if "dynamic" in size.group(2):
                            self.dynamic.add(node.group(1))
                    continue
                edge = EDGE_RE.match(line)
                if edge:
                    callees = self.edges.setdefault(edge.group(1), [])
                    if edge.group(2) not in callees:
                        callees.append(edge.group(2))

    def index(self):
        for title in self.frame:
            name = base_name(title)
            if name in self.by_name and self.frame[self.by_name[name]] >= self.frame[title]:
                continue
            self.by_name[name] = title


class Walker:
    """Depth-first search over (function, context, recursion counts) states.

    A state determines everything below it, so results are memoised by state.
    The context is the nanopb field callback whose message is being encoded
    ("root" outside any, and always for upb); the counts say how often each
    declared recursive function is already on the path in that context.
    """

    def __init__(self, graph, backend):
        self.graph = graph
        self.recursion = RECURSION[backend]
        self.external = set()
        self.unknown = set()
        self.cycles = set()
        self.memo = {}

    def targets(self, caller, callee, ctx):
        """(title, context) pairs a call edge can reach."""
        if callee != "__indirect_call":
            return [(callee, ctx)]
        rule = INDIRECT.get(base_name(caller))
        if rule is None:
            self.unknown.add(base_name(caller))
            return []
        if rule == USER:
            return []
        if rule == CONTEXT:
            return [(self.graph.by_name[c], c) for c in NANOPB_CALLBACKS[ctx]]
        return [(self.graph.by_name[name], ctx) for name in rule if name in self.graph.by_name]

    def enter(self, title, ctx, counts):
        """The recursion counts on entering `title`, or None past its cap."""
        name = base_name(title)
        if name not in self.recursion:
            return counts
        seen = dict(counts)
        seen[name] = seen.get(name, 0) + 1
        if seen[name] > self.recursion[name].get(ctx, 0):
            return None
        return tuple(sorted(seen.items()))

    def worst(self, title, ctx, counts, on_path):
        """(bytes, [titles]) of the deepest path from `title`."""
        if title not in self.graph.frame:
            self.external.add(base_name(title))
            return 0, []
        state = (title, ctx, counts)
        if state in self.memo:
            return self.memo[state]
        on_path.add(state)
        best, best_path = 0, []
        for callee in self.graph.edges.get(title, []):
            for target, target_ctx in self.targets(title, callee, ctx):
                # A new context starts its counts from zero.
                start = counts if target_ctx == ctx else ()
                next_counts = self.enter(target, target_ctx, start)
                if next_counts is None:
                    continue
                if (target, target_ctx, next_counts) in on_path:
                    self.cycles.add(base_name(target))
                    continue
                depth, sub = self.worst(target, target_ctx, next_counts, on_path)
                if depth > best:
                    best, best_path = depth, sub
        on_path.discard(state)
        result = (self.graph.frame[title] + best, [title] + best_path)
        self.memo[state] = result
        return result


def summarise(path):
    """The deepest path as the leaf's own frames, plus the frame count."""
    own = [base_name(t) for t in path[1:] if "/leaf/src/" in t or t.startswith("microtel_leaf")]
    return f"{' > '.join(own) or '-'} ({len(path)} frames)"


def load_graph(build_dir):
    graph = Graph()
    for root, _dirs, files in os.walk(build_dir):
        if "CompilerId" in root:
            continue
        for name in files:
            if name.endswith(".ci"):
                graph.load(os.path.join(root, name))
    graph.index()
    return graph


def main(argv):
    if len(argv) < 3 or argv[2] not in RECURSION:
        print(__doc__, file=sys.stderr)
        return 2
    build_dir, backend = argv[1], argv[2]
    label = argv[3] if len(argv) > 3 else backend
    graph = load_graph(build_dir)
    if not graph.frame:
        print(f"leaf-stack: no .ci files under {build_dir} (build with -fcallgraph-info=su)",
              file=sys.stderr)
        return 2

    walker = Walker(graph, backend)
    rows = []
    for entry in ENTRY_POINTS:
        if entry not in graph.frame:
            print(f"leaf-stack: {entry} not found in the call graph", file=sys.stderr)
            return 1
        depth, path = walker.worst(entry, "root", (), set())
        rows.append((entry, depth, path))
    if walker.unknown:
        print("leaf-stack: indirect calls not resolved in leaf-stack.py, from: "
              + ", ".join(sorted(walker.unknown)), file=sys.stderr)
        return 1
    if walker.cycles:
        print("leaf-stack: recursion not declared in leaf-stack.py, through: "
              + ", ".join(sorted(walker.cycles)), file=sys.stderr)
        return 1

    print(f"Worst-case stack, {label} (static call graph, `-fcallgraph-info=su`):\n")
    print("| entry point | bytes | deepest path through the leaf's own code |")
    print("|---|---:|---|")
    for entry, depth, path in rows:
        print(f"| `{entry}` | {depth:,} | {summarise(path)} |")
    print()
    print("Not counted (no call-graph data): "
          + ", ".join(f"`{n}`" for n in sorted(walker.external)) + ".")
    if graph.dynamic:
        print("Dynamic frames (the figure is a lower bound): "
              + ", ".join(sorted(base_name(t) for t in graph.dynamic)) + ".")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
