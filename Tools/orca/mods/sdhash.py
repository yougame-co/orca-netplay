#!/usr/bin/env python3
"""The SD image pin of an Orca flat bundle (Source/Core/Core/Orca/Bundle.h SD_HASH_HEADER).

  sdhash.py <file>             prints its chunked SHA-256 and its plain SHA-256
  sdhash.py vector <out.json>  the shared test vector (OrcaBundle.ChunkedSha256)

The pin is SHA-256 over "orca-chunked-sha256 1\\n", "<bytes> 67108864\\n" and the lowercase hex
SHA-256 of each 64 MiB piece of the file in order, one per line (the last piece may be shorter).
The pieces hash in parallel, so Orca checks a 2 GB card at every boot in about the time XXH3 took.
"""

import hashlib
import json
import os
import sys
from concurrent.futures import ThreadPoolExecutor

HEADER = b"orca-chunked-sha256 1\n"
CHUNK = 64 << 20


def chunked_sha256(path, size=None):
    """The pin of the first `size` bytes of path (all of it by default)."""
    if size is None:
        size = os.path.getsize(path)
    if size <= 0:
        raise ValueError("an empty file has no pin")
    pieces = (size + CHUNK - 1) // CHUNK

    def piece(i):
        h = hashlib.sha256()
        with open(path, "rb") as f:
            f.seek(i * CHUNK)
            left = min(CHUNK, size - i * CHUNK)
            while left:
                data = f.read(min(left, 4 << 20))
                if not data:
                    raise ValueError(f"{path} is shorter than {size} bytes")
                h.update(data)
                left -= len(data)
        return h.hexdigest()

    # hashlib lets go of the GIL while it hashes, so the pieces hash on every core.
    with ThreadPoolExecutor(max_workers=min(8, pieces)) as pool:
        digests = list(pool.map(piece, range(pieces)))
    text = HEADER + f"{size} {CHUNK}\n".encode() + "".join(d + "\n" for d in digests).encode()
    return hashlib.sha256(text).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(8 << 20), b""):
            h.update(block)
    return h.hexdigest()


def vector(out):
    """A file of CHUNK + 4097 bytes, byte i = (i * 131 + 7) % 251, and its pin."""
    size = CHUNK + 4097
    data = bytes((i * 131 + 7) % 251 for i in range(251)) * (size // 251 + 1)
    # The pattern repeats every 251 bytes: (i * 131 + 7) % 251 depends on i % 251 only.
    data = data[:size]
    tmp = out + ".card"
    with open(tmp, "wb") as f:
        f.write(data)
    try:
        pin = chunked_sha256(tmp)
    finally:
        os.remove(tmp)
    with open(out, "w") as f:
        json.dump({"comment": "sdhash.py vector: byte i of the card is (i * 131 + 7) % 251",
                   "bytes": size, "chunkedSha256": pin}, f, indent=2)
        f.write("\n")
    print(f"{out}: {size} bytes, {pin}")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "vector":
        vector(sys.argv[2])
    elif len(sys.argv) == 2:
        print(f"chunked-sha256 {chunked_sha256(sys.argv[1])}")
        print(f"sha256         {sha256_file(sys.argv[1])}")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
