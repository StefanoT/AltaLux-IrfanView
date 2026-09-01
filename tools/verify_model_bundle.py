#!/usr/bin/env python3
"""Verify the filenames and SHA-256 values in an AltaLux model bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    manifest_path = args.directory / "AltaLuxSegmentation.models.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schemaVersion") != 1:
        raise SystemExit("Unsupported model manifest schema")
    for key in ("encoder", "decoder"):
        entry = manifest[key]
        path = args.directory / entry["file"]
        actual = sha256(path)
        if actual.lower() != entry["sha256"].lower():
            raise SystemExit(f"SHA-256 mismatch for {path.name}")
        print(f"verified {path.name}: {actual}")


if __name__ == "__main__":
    main()
