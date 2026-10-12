#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host-only wire checks, optionally compared to retained isolated stock oracles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = Path(__file__).with_suffix(".c")
header = root / "drivers/net/wwan/mediatek/mtk_mt6797_ccci_speech.h"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--stock-power", type=Path)
parser.add_argument("--stock-ack", type=Path)
args = parser.parse_args()
if bool(args.stock_power) != bool(args.stock_ack):
    parser.error("supply both stock oracle files or neither")
with tempfile.TemporaryDirectory(prefix="speech-codec-fixture-") as temporary:
    binary = Path(temporary) / "fixture"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(source), "-o", str(binary)], check=True, timeout=30)
    command = [str(binary)]
    if args.stock_power:
        command += [str(args.stock_power.resolve()), str(args.stock_ack.resolve())]
    subprocess.run(command, check=True, timeout=30)
print(json.dumps({"result": "PASS", "hardware_qualified": False,
                  "stock_oracles_compared": bool(args.stock_power),
                  "sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in [source, header, Path(__file__)]}}, sort_keys=True))
