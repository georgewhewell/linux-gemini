#!/usr/bin/env python3
"""Create a local derivative DT, never edit the immutable admitted input.

This is a flattened-DT transform: /delete-property/ in a compiled plugin
cannot remove properties from an existing carrier. No device access.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess

SOURCE_SHA = "7ca55149138e0ba5a682b8fca0c7ed9e3c9b49404a6b56e7f16deb2b55fa0af4"
GPU = "/gpu@13040000"
SCP = "/power-controller@10006000"
VGPU = "/i2c@11010000/regulator@1c"
FLAG = "mediatek,gemini-gpuldo-owner"

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    data = args.source.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA:
        raise SystemExit("refuse: input is not the reviewed exact T7 carrier")
    get = os.environ.get("FDTGET", "fdtget")
    put = os.environ.get("FDTPUT", "fdtput")
    def read(path, node, prop, kind="x"):
        return subprocess.check_output([get, "-t", kind, str(path), node, prop], text=True).strip()
    assert read(args.source, GPU, "mali-supply") == "c"
    assert read(args.source, SCP, "mfg-supply") == "c"
    assert read(args.source, VGPU, "phandle") == "c"
    assert read(args.source, VGPU, "reg") == "1c"
    # Exclusive creation prevents replacing a prior candidate/output.
    with args.output.open("xb") as handle:
        handle.write(data)
    try:
        for node, prop in [(GPU, "mali-supply"), (SCP, "mfg-supply")]:
            subprocess.run([put, "-d", str(args.output), node, prop], check=True)
        subprocess.run([put, "-t", "x", str(args.output), SCP, "mfg_async-supply", "c"], check=True)
        for node in [GPU, SCP]:
            subprocess.run([put, str(args.output), node, FLAG], check=True)
        assert read(args.output, SCP, "mfg_async-supply") == "c"
        for node, prop in [(GPU, "mali-supply"), (SCP, "mfg-supply")]:
            props = subprocess.check_output([get, "-p", str(args.output), node], text=True).splitlines()
            assert prop not in props and FLAG in props
    except Exception:
        # Keep a failed derivative for inspection; it must not be admitted.
        raise SystemExit("failed derivative retained for inspection; DO NOT USE")
    print("source_sha256=" + SOURCE_SHA)
    print("derivative_sha256=" + hashlib.sha256(args.output.read_bytes()).hexdigest())
    print("property_delta=delete_gpu_mali_supply,delete_mfg_supply,add_async_supply,add_two_owner_flags")
    print("requires=mtk_scpsys.mt6797_gpuldo_owner=1; physical admission still mandatory")

if __name__ == "__main__":
    main()
