import sys, zlib, struct
d = open(sys.argv[1], "rb").read()
i = 0; vals = []
while len(vals) < 4:
    while d[i:i+1].isspace(): i += 1
    j = i
    while not d[j:j+1].isspace(): j += 1
    vals.append(d[i:j]); i = j
i += 1
w, h = int(vals[1]), int(vals[2]); px = d[i:]
raw = b"".join(b"\0" + px[y*w*3:(y+1)*w*3] for y in range(h))
def ch(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(raw, 6)) + ch(b"IEND", b""))
