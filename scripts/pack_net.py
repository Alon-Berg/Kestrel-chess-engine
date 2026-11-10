r"""Packs the Kestrel network for the play page: splits the int16 weights into a plane of low bytes and a plane of high bytes (the high bytes are almost all 0 / -1, so
gzip then needs 58 % of the size instead of 69 %), then gzip level 9 (no timestamp, so the output is reproducible).
usage: python scripts/pack_net.py kestrel_net_3_2.bin docs/play/kestrel_net_3_2.pk.gz
The page (engine-worker.js) undoes the split while it copies the data into the engine; the layout is:
   [64-byte header][N low bytes][N high bytes][float head ...]   with N = 768 * 32 * 1536 + 1536 int16 values (input weights and biases)"""
import gzip
import hashlib
import io
import sys
import time

import numpy as np

src, dst = sys.argv[1], sys.argv[2]
NN_INB, NN_H = 768 * 32, 1536
N = NN_INB * NN_H + NN_H
raw = np.fromfile(src, dtype=np.uint8)
assert bytes(raw[:8]) == b"KSTRNET2", "not a Kestrel network"
start, end = 64, 64 + 2 * N
assert raw.size > end
reg = raw[start:end].reshape(-1, 2)
packed = np.concatenate([raw[:start], np.ascontiguousarray(reg[:, 0]), np.ascontiguousarray(reg[:, 1]), raw[end:]])
assert packed.size == raw.size
t = time.time()
with open(dst, "wb") as f:
    with gzip.GzipFile(filename="", mode="wb", fileobj=f, compresslevel=9, mtime=0) as g:
        g.write(packed.tobytes())
print("packed %d -> %d bytes (%.1f %%) in %.0f s" % (raw.size, io.open(dst, "rb").seek(0, 2), 100.0 * io.open(dst, "rb").seek(0, 2) / raw.size, time.time() - t))
print("sha256 of the unpacked network:", hashlib.sha256(raw.tobytes()).hexdigest())
print("N (int16 values) =", N)
# self-check: unpack and compare
with gzip.open(dst, "rb") as g:
    back = np.frombuffer(g.read(), dtype=np.uint8)
out = np.empty_like(back)
out[:start] = back[:start]
lo = back[start:start + N]; hi = back[start + N:start + 2 * N]
o = out[start:end].reshape(-1, 2); o[:, 0] = lo; o[:, 1] = hi
out[end:] = back[end:]
assert np.array_equal(out, raw), "unpacking does not give the original file"
print("self-check ok")
