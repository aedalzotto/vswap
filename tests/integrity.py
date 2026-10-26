import os, hashlib, time
CH = 64 << 20; N = 48  # 48 x 64 MiB = 3 GiB
def swap(): return {l.split()[0]: l.split()[3] for l in open('/proc/swaps').readlines()[1:]}
# pages that really hit a swap device (zswap-held pages count in /proc/swaps but not here)
def io(): v = dict(l.split() for l in open('/proc/vmstat')); return int(v['pswpout']), int(v['pswpin'])
io0 = io()
t = time.time(); bufs, h = [], []
for i in range(N):
    b = bytearray(os.urandom(CH)); bufs.append(b); h.append(hashlib.sha256(b).digest())
print(f"filled {N*CH>>20} MiB in {time.time()-t:.1f}s; swap KiB used: {swap()}", flush=True)
t = time.time(); bad = [i for i in range(N) if hashlib.sha256(bufs[i]).digest() != h[i]]
print(f"re-hashed in {time.time()-t:.1f}s; mismatches: {len(bad)} {bad}", flush=True)
o, i = (a - b for a, b in zip(io(), io0))
print(f"device I/O: {o*4>>10} MiB out, {i*4>>10} MiB in")
print("PASS" if not bad else "FAIL")
