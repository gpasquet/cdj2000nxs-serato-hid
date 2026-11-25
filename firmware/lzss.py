#!/usr/bin/env python3
"""LZSS d'Okumura tel qu'utilisé par le chargeur du CDJ-2000NXS (décompresseur à 0xABFFD000).
Bloc en flash (romobj à 0x40000) : u32 L | L octets compressés | u16 somme des L+4 premiers octets.
  python3 lzss.py unpack romobj.bin app.bin      (décompresse le bloc à 0x40000)
  python3 lzss.py pack   app.bin   bloc.bin      (recompresse, avec longueur et somme)
"""
import struct, sys

N, F, THRESH = 4096, 18, 2

def decompress(src):
    ring = bytearray(b" " * N); r = N - F
    out = bytearray(); i = 0; flags = 0
    while True:
        flags >>= 1
        if not flags & 0x100:
            if i >= len(src): break
            flags = src[i] | 0xFF00; i += 1
        if flags & 1:
            if i >= len(src): break
            c = src[i]; i += 1
            out.append(c); ring[r] = c; r = (r + 1) & (N - 1)
        else:
            if i + 1 >= len(src): break
            pos = src[i] | ((src[i + 1] & 0xF0) << 4); ln = (src[i + 1] & 0x0F) + THRESH + 1; i += 2
            for k in range(ln):
                c = ring[(pos + k) & (N - 1)]; out.append(c); ring[r] = c; r = (r + 1) & (N - 1)
    return bytes(out)

def compress(data):
    """Compresseur glouton compatible (recherche par table de hachage sur 3 octets)."""
    ring = bytearray(b" " * N); r = N - F
    out = bytearray(); i = 0; n = len(data)
    # position dans `data` → position dans l'anneau : ring_pos(j) = (N - F + j) & (N-1)
    heads = {}
    while i < n:
        flagpos = len(out); out.append(0); flags = 0
        for bit in range(8):
            if i >= n: break
            best_len, best_pos = 0, 0
            if i + 2 < n:
                key = data[i:i + 3]
                for j in reversed(heads.get(key, [])[-64:]):
                    if i - j >= N - F: continue          # hors de la fenêtre sûre
                    l = 0
                    while l < F and i + l < n and data[j + l] == data[i + l] and j + l < i + l:
                        l += 1
                    if l > best_len: best_len, best_pos = l, j
                    if l == F: break
            if best_len > THRESH:
                pos = (N - F + best_pos) & (N - 1)
                out += bytes([pos & 0xFF, ((pos >> 4) & 0xF0) | (best_len - THRESH - 1)])
                step = best_len
            else:
                flags |= 1 << bit; out.append(data[i]); step = 1
            for k in range(step):
                if i + k + 2 < n: heads.setdefault(data[i + k:i + k + 3], []).append(i + k)
            i += step
        out[flagpos] = flags
    return bytes(out)

def checksum(b):
    return sum(b) & 0xFFFF

if __name__ == "__main__":
    mode, a, b = sys.argv[1:4]
    if mode == "unpack":
        rom = open(a, "rb").read(); blk = rom[0x40000:]
        L = struct.unpack_from("<I", blk)[0]
        stored = struct.unpack_from("<H", blk, 4 + L)[0]
        calc = checksum(blk[:4 + L])
        app = decompress(blk[4:4 + L])
        open(b, "wb").write(app)
        print(f"L={L} somme stockée={stored:#06x} calculée={calc:#06x} {'OK' if stored == calc else 'DIFFÉRENTE'} → {len(app)} octets")
    elif mode == "pack":
        app = open(a, "rb").read(); c = compress(app)
        blk = struct.pack("<I", len(c)) + c
        blk += struct.pack("<H", checksum(blk))
        open(b, "wb").write(blk)
        print(f"{len(app)} → {len(c)} octets compressés, bloc {len(blk)} octets")
