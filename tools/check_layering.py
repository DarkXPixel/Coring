#!/usr/bin/env python3
"""Verify that coring5 #includes only point DOWN the layer graph.

The allowed dependencies mirror src/coring5/CMakeLists.txt and
docs/ARCHITECTURE.md ("Layering"). CMake enforces the *link* graph; this checks
the *include* graph, which also catches header-only violations that link fine.

Usage: tools/check_layering.py [repo_root]      (exit code 1 on violations)
"""

import re
import sys
from pathlib import Path

# module -> modules it may include (besides itself). Keep in sync with CMake.
ALLOWED = {
    "base": set(),
    "mem": {"base"},
    "config": {"base"},
    "obs": {"base"},
    "io": {"base", "mem", "config", "obs"},
    "net": {"base", "io", "upstream"},
    "proto/http1": {"base", "config"},
    "proto/l4": {"base", "io", "config", "obs", "mem", "net", "upstream"},
    "proto/http1conn": {"base", "io", "config", "obs", "mem", "proto/http1"},
    "proto/http2": {"base", "io", "config", "obs", "mem"},
    "proto/http3": {"base", "io", "config", "obs", "mem"},
    "upstream": {"base", "config"},
    "tls": {"base"},
    "handlers": {"base", "proto/http1", "upstream"},
    "control": set(),
    "runtime": {
        "base",
        "mem",
        "config",
        "obs",
        "io",
        "net",
        "upstream",
        "proto/l4",
        "proto/http1",
        "proto/http1conn",
        "proto/http2",
        "proto/http3",
        "control",
    },
}

INCLUDE = re.compile(r'^\s*#\s*include\s+"coring5/([^"]+)"')


def module_of(rel: str) -> str:
    """'proto/http1/Parser.hpp' -> 'proto/http1'; 'io/Ring.hpp' -> 'io'."""
    parts = rel.split("/")
    if parts[0] == "proto" and len(parts) > 2:
        return f"proto/{parts[1]}"
    return parts[0]


def main() -> int:
    root = (
        Path(sys.argv[1])
        if len(sys.argv) > 1
        else Path(__file__).resolve().parent.parent
    )
    violations = []
    unknown = set()
    scanned = 0
    for base in (root / "include" / "coring5", root / "src" / "coring5"):
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".hpp", ".cpp", ".h"):
                continue
            rel = path.relative_to(base).as_posix()
            if rel.startswith("apps/"):
                continue  # applications may use everything
            mod = module_of(rel)
            if mod not in ALLOWED:
                unknown.add(mod)
                continue
            scanned += 1
            for lineno, line in enumerate(path.read_text().splitlines(), 1):
                m = INCLUDE.match(line)
                if not m:
                    continue
                dep = module_of(m.group(1))
                if dep != mod and dep not in ALLOWED[mod]:
                    violations.append(
                        f"{path.relative_to(root)}:{lineno}: '{mod}' must not include '{dep}' ({m.group(1)})"
                    )
    for u in sorted(unknown):
        violations.append(
            f"unknown module '{u}': add it to ALLOWED in tools/check_layering.py"
        )
    for v in violations:
        print(v)
    print(f"checked {scanned} files, {len(violations)} violation(s)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
