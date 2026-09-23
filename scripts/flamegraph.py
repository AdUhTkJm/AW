#!/usr/bin/env python3
"""Render folded stacks (pprof --collapsed) as an SVG flamegraph.

Reads lines of the form

    frame1;frame2;...;frameN <count>

from stdin and writes an SVG to stdout. This is deliberately small and
dependency-free: the repo needs no FlameGraph checkout, only Python 3. The
result is a static flamegraph -- each frame is labelled and carries a <title>
tooltip with its name, sample count and share of the run.

Usage:
    pprof --collapsed build/awr_inspect plan.prof | scripts/flamegraph.py > plan.svg
"""

import argparse
import sys
import zlib
from html import escape


class Node:
    """One frame in the call trie. `value` is the samples that pass through it."""

    __slots__ = ("value", "children")

    def __init__(self):
        self.value = 0.0
        self.children = {}


def parse_folded(stream):
    root = Node()
    for line in stream:
        line = line.strip()
        if not line:
            continue
        # The count is the last whitespace-separated token; frame names may
        # contain spaces, so split from the right.
        head, sep, tail = line.rpartition(" ")
        if not sep:
            continue
        try:
            count = float(tail)
        except ValueError:
            continue
        if count <= 0:
            continue

        node = root
        node.value += count
        for frame in head.split(";"):
            child = node.children.get(frame)
            if child is None:
                child = Node()
                node.children[frame] = child
            child.value += count
            node = child
    return root


def color_for(name):
    # [unknown], [vdso] and kernel frames get a neutral grey so they do not
    # compete with real functions for attention.
    if name.startswith("[") and name.endswith("]"):
        return "rgb(205,205,205)"
    digest = zlib.crc32(name.encode("utf-8", "replace")) & 0xFFFFFFFF
    v1 = (digest & 0xFF) / 255.0
    v2 = ((digest >> 8) & 0xFF) / 255.0
    v3 = ((digest >> 16) & 0xFF) / 255.0
    return "rgb(%d,%d,%d)" % (int(205 + 50 * v1),
                              int(100 + 155 * v2),
                              int(50 + 100 * v3))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", nargs="?",
                        help="folded stack file (default: stdin)")
    parser.add_argument("--width", type=int, default=1200,
                        help="image width in pixels (default 1200)")
    parser.add_argument("--frame-height", type=int, default=16,
                        help="height of one frame row (default 16)")
    parser.add_argument("--title", default="Flamegraph",
                        help="image title (default: Flamegraph)")
    parser.add_argument("--min-width", type=float, default=0.5,
                        help="skip frames narrower than this many pixels (default 0.5)")
    args = parser.parse_args()

    if hasattr(sys.stdin, "reconfigure"):
        sys.stdin.reconfigure(encoding="utf-8", errors="replace")

    if args.input:
        with open(args.input, encoding="utf-8", errors="replace") as handle:
            root = parse_folded(handle)
    else:
        root = parse_folded(sys.stdin)

    total = root.value
    if total <= 0:
        sys.stderr.write("flamegraph: no samples in input\n")
        return 1

    width = max(240, args.width)
    frame_height = max(8, args.frame_height)
    font_size = max(8, min(12, frame_height - 4))

    rects = []  # (x, depth, width, name, value)
    max_depth = 0

    # Depth-first layout. Children are packed from the left in name order, so
    # the space past the last child is the frame's own (self) samples -- the
    # same convention FlameGraph.pl uses.
    def layout(node, name, x0, depth):
        nonlocal max_depth
        node_width = node.value / total * width
        if depth > 0:
            d = depth - 1
            max_depth = max(max_depth, d)
            if node_width >= args.min_width:
                rects.append((x0, d, node_width, name, node.value))
        cursor = x0
        for child_name, child in sorted(node.children.items()):
            layout(child, child_name, cursor, depth + 1)
            cursor += child.value / total * width

    layout(root, "", 0.0, 0)

    top = 30
    bottom = 12
    height = top + (max_depth + 1) * frame_height + bottom
    title = "%s \u2014 %d samples" % (args.title, round(total))

    out = []
    out.append('<?xml version="1.0" standalone="no"?>')
    out.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
               'viewBox="0 0 %d %d" font-family="Verdana,Helvetica,sans-serif">'
               % (width, height, width, height))
    out.append('<rect x="0" y="0" width="%d" height="%d" fill="#ffffff"/>'
               % (width, height))
    out.append('<text x="8" y="19" font-size="14" font-weight="bold" fill="#000000">%s</text>'
               % escape(title))

    for (x, depth, rect_width, name, value) in rects:
        # Root at the bottom, leaves growing upward: the same orientation
        # flamegraph.pl uses, so the fallback looks like the interactive SVG.
        y = top + (max_depth - depth) * frame_height
        share = value / total * 100.0
        out.append('<rect x="%.2f" y="%d" width="%.2f" height="%d" fill="%s" '
                   'stroke="#eeeeee" stroke-width="0.5"><title>%s (%d samples, %.2f%%)'
                   '</title></rect>'
                   % (x, y, rect_width, frame_height - 1, color_for(name),
                      escape(name), round(value), share))
        # Approximate how many characters fit; skip labels for thin frames.
        chars = int(rect_width / (font_size * 0.58))
        if chars >= 3:
            label = name if len(name) <= chars else name[:chars - 1] + "\u2026"
            out.append('<text x="%.2f" y="%d" font-size="%d" fill="#000000">%s</text>'
                       % (x + 3.0, y + frame_height - 5, font_size, escape(label)))

    out.append('</svg>')
    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
