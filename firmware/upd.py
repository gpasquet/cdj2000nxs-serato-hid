#!/usr/bin/env python3
"""Lecture / reconstruction du firmware CDJ-2000NXS (C2KNXS.UPD, version 1.44).

Format (déduit et vérifié sur le fichier officiel et deux fichiers LNNRT) :
  4 lignes décimales terminées par CRLF = tailles des 4 sections, puis les sections bout à bout.
  Section = libellé 32 octets | contenu | contrôle
    GUI  : contenu binaire ; contrôle = 00 00 + CRC16-XModem(libellé..fin-4) en big-endian
    DRIV : S-records (G11.MOT)    ; contrôle = CRC16-XModem(libellé+contenu) en little-endian
    MAIN : S-records (romobj.mot) ; idem
    PANL : S-records              ; idem
  MAIN, une fois converti en image (adresses 0..), contient :
    0x00000-0x3FFFF : chargeur de démarrage (non compressé) + image de secours à 0x10000
    0x40000-…       : bloc LZSS = u32 L | L octets | u16 somme des L+4 premiers octets
                      (décompressé en RAM à 0xA4000000, entrée 0xA4000800)
  S-records MAIN : S2, 32 octets de données par ligne alignés sur 32 ; les lignes entièrement
  à 0xFF sont omises ; en-tête S0 « romobj  mot », fin S705A00000005A.

Utilisation :
  python3 upd.py verifier C2KNXS.UPD
  python3 upd.py extraire C2KNXS.UPD app.bin                 (programme principal décompressé)
  python3 upd.py construire C2KNXS.UPD sortie.UPD [--app app.bin] [--version 9.99]
"""
import binascii, struct, sys

import lzss

NAMES = ["GUI", "DRIV", "MAIN", "PANL"]


def parse(d):
    lines = d.split(b"\r\n", 4)
    sizes = [int(x) for x in lines[:4]]
    off = sum(len(x) + 2 for x in lines[:4])
    secs = []
    for s in sizes:
        secs.append(d[off:off + s])
        off += s
    assert off == len(d), "taille totale incohérente"
    return secs


def crc_ok(name, sec):
    if name == "GUI":
        return sec[-4:] == b"\0\0" + struct.pack(">H", binascii.crc_hqx(sec[:-4], 0))
    return sec[-2:] == struct.pack("<H", binascii.crc_hqx(sec[:-2], 0))


def seal(name, sec_without_check):
    if name == "GUI":
        return sec_without_check + b"\0\0" + struct.pack(">H", binascii.crc_hqx(sec_without_check, 0))
    return sec_without_check + struct.pack("<H", binascii.crc_hqx(sec_without_check, 0))


def srec_to_image(body):
    mem, hi = {}, 0
    for l in body.split(b"\r\n"):
        if l[:2] != b"S2":
            continue
        rec = bytes.fromhex(l[2:].decode())
        assert sum(rec) & 0xFF == 0xFF, "somme S-record"
        a = int.from_bytes(rec[1:4], "big")
        mem[a] = rec[4:-1]
        hi = max(hi, a + len(rec) - 5)
    img = bytearray(b"\xff" * hi)
    for a, v in mem.items():
        img[a:a + len(v)] = v
    return bytes(img)


def image_to_srec(img, s0=b"S00E0000726F6D6F626A20206D6F74D8", end=b"S705A00000005A"):
    out = [s0]
    for a in range(0, len(img), 32):
        chunk = img[a:a + 32]
        if chunk == b"\xff" * len(chunk):
            continue
        chunk = chunk.ljust(32, b"\xff")
        rec = bytes([len(chunk) + 4]) + a.to_bytes(3, "big") + chunk
        rec += bytes([0xFF - (sum(rec) & 0xFF)])
        out.append(b"S2" + rec.hex().upper().encode())
    out.append(end)
    return b"\r\n".join(out) + b"\r\n"


def main_image(secs):
    return srec_to_image(secs[2][32:-2])


def extract_app(img):
    L = struct.unpack_from("<I", img, 0x40000)[0]
    blk = img[0x40000:0x40000 + 4 + L + 2]
    assert struct.unpack_from("<H", blk, 4 + L)[0] == sum(blk[:4 + L]) & 0xFFFF, "somme du bloc LZSS"
    return lzss.decompress(blk[4:4 + L])


def build(d, app=None, version=None, gui=None):
    """gui : nouveau contenu (flux LDR) de la section GUI, sans libellé ni contrôle."""
    secs = parse(d)
    new = []
    for name, sec in zip(NAMES, secs):
        label = sec[:32]
        if name == "GUI":
            body = sec[32:-4] if gui is None else gui
            if version:
                label = label.replace(b"Ver1.200", b"Ver" + version_gui(version))
        else:
            body = sec[32:-2]
            if name == "MAIN":
                if version:
                    label = label.replace(b"Ver1.44", b"Ver" + version.encode())
                if app is not None:
                    img = bytearray(srec_to_image(body))
                    c = lzss.compress(app)
                    blk = struct.pack("<I", len(c)) + c
                    blk += struct.pack("<H", sum(blk) & 0xFFFF)
                    capacity = 3801088  # place disponible pour le bloc (d'après LNNRT)
                    assert len(blk) <= capacity, f"programme trop gros : {len(blk)} > {capacity}"
                    img = bytes(img[:0x40000]) + blk
                    body = image_to_srec(img)
        new.append(seal(name, label + body))
    head = b"".join(str(len(s)).encode() + b"\r\n" for s in new)
    return head + b"".join(new)


def version_gui(v):
    # « 9.99 » → « 9.990 » (le libellé GUI a 3 décimales)
    return (v + "0").encode() if len(v.split(".")[1]) == 2 else v.encode()


if __name__ == "__main__":
    cmd = sys.argv[1]
    d = open(sys.argv[2], "rb").read()
    if cmd == "verifier":
        secs = parse(d)
        for n, s in zip(NAMES, secs):
            print(f"{n:5s} {len(s):9d} octets  {s[:32]!r}  CRC {'OK' if crc_ok(n, s) else 'FAUX'}")
        app = extract_app(main_image(secs))
        print(f"programme principal : {len(app)} octets décompressés, somme LZSS OK")
    elif cmd == "extraire":
        open(sys.argv[3], "wb").write(extract_app(main_image(parse(d))))
    elif cmd == "construire":
        out = sys.argv[3]
        args = sys.argv[4:]
        app = open(args[args.index("--app") + 1], "rb").read() if "--app" in args else None
        ver = args[args.index("--version") + 1] if "--version" in args else None
        open(out, "wb").write(build(d, app, ver))
        print(f"écrit {out}")
