#!/usr/bin/env python3
"""Every key shipped for the original languages must exist in the two added ones."""
import glob
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LANG = os.path.join(ROOT, "sonix-packer", "assets", "{model}", "usr", "resource", "sonix", "language")
NEW = ("Português PT.ini", "Ukrainian.ini")


def keys(path):
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "=" in line and not line.startswith(";"):
                k, v = line.split("=", 1)
                out[k.strip()] = v.rstrip("\n")
    return out


bad = 0
for model in ("R1", "R3PII"):
    reference = keys(os.path.join(LANG.format(model=model), "English.ini"))
    for name in NEW:
        target = keys(os.path.join(LANG.format(model=model), name))
        missing = sorted(set(reference) - set(target))
        if missing:
            bad += 1
            print(f"FAIL {model}/{name}: {len(missing)} keys missing")
            for k in missing[:12]:
                print(f"  {k}")
        else:
            print(f"ok {model}/{name}: {len(reference)} keys")

sys.exit(1 if bad else 0)
