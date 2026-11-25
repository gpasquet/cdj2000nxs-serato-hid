#!/usr/bin/env python3
"""Construit le programme principal patché (hdwave) et, en option, le fichier de mise à jour.

  python3 build.py                 → patch/app_hdwave.bin (programme décompressé, pour l'émulateur)
  python3 build.py --upd           → aussi patch/C2KNXS.UPD (version 9.99, affichage couleur compris, à tester d'abord dans
                                     l'émulateur ; ne jamais flasher sans validation complète)
"""
import os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
TC = os.path.join(FW, "..", "ext", "toolchain", "sh-elf", "bin")
BASE = 0x04000000
APP_END = 0x043E0000

# entrées de tables d'adresses à rediriger : (adresse, valeur d'origine, fonction du patch)
HOOKS = [(a, 0x04310aca, "_hd_send") for a in
         (0x04258444, 0x04258728, 0x04258980, 0x0425acc8, 0x0425af2c, 0x0425b248, 0x0425b514, 0x0425c9e4)]
HOOKS += [(0x04257b90, 0x04311216, "_hd_check"), (0x04355b94, 0x0435785e, "_hd_hid"),
          (0x04358d0c, 0x04353f80, "_hd_inrep")]


def run(*cmd):
    subprocess.run(cmd, check=True, cwd=HERE)


def apply(stock_app, code, syms, verbose=True):
    """Programme principal d'origine (décompressé) + code du patch → programme patché."""
    app = bytearray(stock_app)
    assert len(app) == APP_END - BASE, "programme d'origine inattendu"
    for addr, orig, sym in HOOKS:
        cur = struct.unpack_from("<I", app, addr - BASE)[0]
        assert cur == orig, f"{addr:#x} contient {cur:#x}, attendu {orig:#x} : mauvais firmware ?"
        struct.pack_into("<I", app, addr - BASE, syms[sym])
        if verbose: print(f"  {addr:#010x} : {orig:#010x} → {syms[sym]:#010x} ({sym[1:]})")
    return bytes(app + code)


def main():
    gcc, ld, objcopy, nm = (os.path.join(TC, "sh-elf-" + t) for t in ("gcc", "ld", "objcopy", "nm"))
    run(gcc, "-m4-single", "-ml", "-Os", "-ffreestanding", "-fno-builtin", "-nostdlib",
        "-Wall", "-Wextra", "-c", "hdwave.c", "-o", "hdwave.o")
    run(gcc, "-m4-single", "-ml", "-nostdlib", "-T", "hdwave.ld", "-o", "hdwave.elf", "hdwave.o", "-lgcc")
    run(objcopy, "-O", "binary", "-j", ".text", "-j", ".rodata", "-j", ".data", "-j", ".bss",
        "hdwave.elf", "hdwave.bin")
    syms = {}
    for line in subprocess.run([nm, "hdwave.elf"], capture_output=True, text=True, check=True).stdout.split("\n"):
        p = line.split()
        if len(p) == 3: syms[p[2]] = int(p[0], 16)
    code = open(os.path.join(HERE, "hdwave.bin"), "rb").read()
    print(f"code du patch : {len(code)} octets à {APP_END:#x} ; tampons jusqu'à {syms['_hd_ram_end']:#x}")
    assert syms["_hd_ram_end"] <= 0x04500000, "les tampons débordent sur la zone de la liaison (0x04500000)"

    app = apply(open(os.path.join(FW, "stock", "split", "app.bin"), "rb").read(), code, syms)
    out = os.path.join(HERE, "app_hdwave.bin")
    open(out, "wb").write(app)
    print("écrit", out, len(app), "octets")

    if "--upd" in sys.argv:
        sys.path.insert(0, FW)
        import upd
        d = open(os.path.join(FW, "stock", "C2KNXS.UPD"), "rb").read()
        import build_gui
        res = upd.build(d, bytes(app), "9.99", gui=build_gui.build())
        open(os.path.join(HERE, "C2KNXS.UPD"), "wb").write(res)
        print("écrit", os.path.join(HERE, "C2KNXS.UPD"))


if __name__ == "__main__":
    main()
