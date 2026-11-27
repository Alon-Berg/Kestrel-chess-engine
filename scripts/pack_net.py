r"""Packs the Kestrel network for the play page: gzip level 9 (no timestamp, so the output is reproducible).
The 5.0 network (KSTRNET4: int8 feature rows with one integer scale per neuron) is stored as it is.  The 3.2 network (int16 weights) was split into a plane of low bytes and a plane of
high bytes first; for int8 data that does not help, so the page copies the unpacked data straight into the engine's memory (shuffleCount = 0 in docs/play/play.js).
usage: python scripts/pack_net.py kestrel_net_5_0.bin docs/play/kestrel_net_5_0.pk.gz"""
import gzip
import hashlib
import os
import sys
import time

src, dst = sys.argv[1], sys.argv[2]
raw = open(src, "rb").read()
assert raw[:8] == b"KSTRNET4", "not a Kestrel 5.0 network"
t = time.time()
with open(dst, "wb") as f:
    with gzip.GzipFile(filename="", mode="wb", fileobj=f, compresslevel=9, mtime=0) as g:
        g.write(raw)
size = os.path.getsize(dst)
print("packed %d -> %d bytes (%.1f %%) in %.0f s" % (len(raw), size, 100.0 * size / len(raw), time.time() - t))
print("sha256 of the unpacked network:", hashlib.sha256(raw).hexdigest())
# self-check: unpack and compare
with gzip.open(dst, "rb") as g:
    assert g.read() == raw, "unpacking does not give the original file"
print("self-check ok")
