#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove mod.schema.json differs from a local git revision only in annotation text."""
import argparse
import json
from pathlib import Path
import subprocess


def strip_text(value):
    if isinstance(value, dict):
        return {key: None if key in ('title', 'description', '$comment') and isinstance(item, str)
                else strip_text(item) for key, item in value.items()}
    if isinstance(value, list):
        return [strip_text(item) for item in value]
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', help='local revision containing the original schema')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    path = 'Tools/orca/mods/mod.schema.json'
    old = json.loads(subprocess.check_output(['git', 'show', f'{args.base}:{path}'], cwd=root))
    new = json.loads((root / path).read_text())
    assert strip_text(old) == strip_text(new), 'Non-annotation schema content changed'
    print('PASS: schema identical after stripping title/description/$comment values (keys retained)')


if __name__ == '__main__':
    main()
