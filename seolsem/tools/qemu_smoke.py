#!/usr/bin/env python3
import argparse
import subprocess
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser(description="QEMU smoke boot for Seolsem images")
    ap.add_argument("--qemu", default="qemu-system-i386", help="qemu binary")
    ap.add_argument("--kernel-img", default="seolsem.img", help="kernel floppy image")
    ap.add_argument("--programs-img", default="seolsem_programs.img", help="programs floppy image")
    ap.add_argument("--timeout", type=float, default=8.0, help="seconds to keep guest running")
    ap.add_argument("--no-program-disk", action="store_true", help="boot without programs disk")
    args = ap.parse_args()

    cmd = [
        args.qemu,
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "none",
        "-no-reboot",
        "-no-shutdown",
        "-fda",
        args.kernel_img,
    ]
    if not args.no_program_disk:
        cmd.extend(["-fdb", args.programs_img])

    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    except FileNotFoundError:
        print(f"qemu smoke: binary not found: {args.qemu}", file=sys.stderr)
        return 2

    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        rc = proc.poll()
        if rc is not None:
            err = proc.stderr.read() if proc.stderr else ""
            print("qemu smoke: guest exited early", file=sys.stderr)
            if err:
                print(err.strip(), file=sys.stderr)
            return 1
        time.sleep(0.2)

    proc.terminate()
    try:
        proc.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=2.0)

    print("qemu smoke: boot window passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
