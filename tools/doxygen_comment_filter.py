#!/usr/bin/env python3
"""Doxygen INPUT_FILTER: promotes eligible plain /* */ comments to /** */.

Placeholder (Phase 2 of the docs plan): passthrough only, so the Doxygen
pipeline itself can be verified end-to-end before the real promotion
algorithm lands in Phase 6. Doxygen invokes this as `<filter> <file>` and
uses stdout as the parsed content; the file on disk is never modified.
"""
import sys

with open(sys.argv[1], "r", encoding="utf-8", errors="replace") as f:
    sys.stdout.write(f.read())
