#!/usr/bin/env python3
"""Resumable chunked copy for flaky source drives, with optional SHA-1 check.

Usage: salvage_copy.py <src> <dst> [expected_sha1]

Resumes from the current size of <dst>. On a read error it backs off and
retries the same chunk (up to MAX_TRIES), so a drive that drops out briefly
does not restart the whole copy.
"""
import hashlib
import os
import sys
import time

CHUNK = 8 << 20
MAX_TRIES = 30


def copy(src, dst):
    total = os.path.getsize(src)
    done = os.path.getsize(dst) if os.path.exists(dst) else 0
    print(f"resuming at {done}/{total}", flush=True)
    with open(dst, "ab") as out:
        while done < total:
            for attempt in range(1, MAX_TRIES + 1):
                try:
                    with open(src, "rb", buffering=0) as f:
                        f.seek(done)
                        data = f.read(min(CHUNK, total - done))
                    if not data:
                        raise OSError("short read")
                    break
                except OSError as e:
                    wait = min(2 * attempt, 30)
                    print(f"read error at {done}: {e}; retry {attempt} in {wait}s", flush=True)
                    time.sleep(wait)
            else:
                raise SystemExit(f"giving up at offset {done}")
            out.write(data)
            out.flush()
            done += len(data)
            if done % (512 << 20) < CHUNK:
                print(f"{done * 100 // total}% ({done >> 20} MiB)", flush=True)
    print("copy complete", flush=True)


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        while chunk := f.read(16 << 20):
            h.update(chunk)
    return h.hexdigest()


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        sys.exit(1)
    copy(sys.argv[1], sys.argv[2])
    if len(sys.argv) == 4:
        got = sha1(sys.argv[2])
        ok = got == sys.argv[3].lower()
        print(f"sha1 {got} {'OK' if ok else 'MISMATCH (expected ' + sys.argv[3] + ')'}", flush=True)
        sys.exit(0 if ok else 2)
