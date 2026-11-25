#!/usr/bin/env python3
"""Construit le programme d'affichage (section GUI, flux LDR Blackfin) avec la waveform couleur.

  python3 build_gui.py      → patch/gui_rgb.ldr (contenu de la section GUI)
Utilisé par build.py --upd pour produire un seul C2KNXS.UPD (affichage + processeur principal).
"""
import os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
sys.path.insert(0, FW)
import ldr, upd

TC = os.path.join(FW, "..", "ext", "toolchain", "bfin-elf", "bin")
BASE = 0x00FD6000
# appels redirigés : (adresse du CALL, cible d'origine, fonction du patch)
CALLS = [(0x00D0F538, 0x00D2F51C, "_rgb_decode"),
         (0x00D2F3B6, 0x00D2E230, "_rgb_column"),
         (0x00D3058A, 0x00D2F7C6, "_rgb_overview"),
         (0x00CF5D4E, 0x00D17314, "_rgb_fshow"),
         (0x00CF6C72, 0x00D306D4, "_rgb_time"), (0x00CF72B0, 0x00D306D4, "_rgb_time"),
         (0x00CF7292, 0x00D306D4, "_rgb_time"),
         (0x00D2D822, 0x00D2CDC8, "_rgb_phase"),
         (0x00D2F4DC, 0x00D2F0BE, "_rgb_wframe"),
         (0x00D0B776, 0x00D0B58C, "_rgb_blit0"), (0x00D0B78A, 0x00D0ADEC, "_rgb_blit1"),
         (0x00D0B79E, 0x00D0AF2C, "_rgb_blit2"), (0x00D0B7B2, 0x00D0B42C, "_rgb_blit3"),
         (0x00D0B7C6, 0x00D0A9E4, "_rgb_blit4")]
# instructions de 4 octets remplacées par un CALL : (adresse, octets d'origine, fonction du patch)
INSNS = [(0x00D30A46, bytes.fromhex("88c20120"), "_rgb_frac_asm"),
         (0x00D30ABA, bytes.fromhex("98e60200"), "_rgb_dec_asm")]
# pointeurs redirigés vers une variable du patch : (adresse de « Px.L = imm », octets d'origine
# des deux instructions « Px.L = » et « Px.H = » consécutives, variable). V37 : le dessin de la
# grande waveform (0xD2F0BE) lit l'état reçu dans une copie privée (rgb_posh), pas dans 0x6003C0.
PTRS = [(0x00D2F0C6, bytes.fromhex("0de1c0034de16000"), "_rgb_posh")]
# sauts posés en tête de fonction : (adresse, octets d'origine attendus, fonction du patch)
JUMPS = [(0x00D49394, bytes.fromhex("020c2718"), "_rgb_memcpy")]
# octets remplacés (adresse, d'origine, nouveaux). Table des points de la grande waveform
# (12 octets par point) déplacée de 0x01011940 à 0x01015180, juste après une image de 480 × 90
# (86 400 octets au lieu de 72 000 pour 400 × 90) : les onze adresses de la table, sa taille
# effacée au chargement (0xA4CDD8 → 0xA49598) et sa capacité en points (900 050 → 898 850). La fin
# de la table ne bouge pas. Invisible en mode USB (même code, table 14 400 octets plus loin).
BYTES = [(0x00D2BF8C, "09e14319", "09e18351"), (0x00D2BF9A, "08e14419", "08e18451"),
         (0x00D2C0FE, "22e1d8cd", "22e19895"), (0x00D2C106, "00e14019", "00e18051"),
         (0x00D2C176, "0ce14019", "0ce18051"), (0x00D2E186, "0ae14019", "0ae18051"),
         (0x00D2E1FE, "0ae14019", "0ae18051"), (0x00D2E29E, "08e14019", "08e18051"),
         (0x00D2E312, "0ae14219", "0ae18251"), (0x00D2E592, "0be14019", "0be18051"),
         (0x00D2EEDA, "08e14319", "08e18351"), (0x00D2F566, "08e14019", "08e18051"),
         (0x00CBC23C, "d2bb0d00", "22b70d00")]


def run(*cmd):
    subprocess.run(cmd, check=True, cwd=HERE)


def apply(stock_upd, code, syms, verbose=True):
    """Fichier de mise à jour d'origine + code couleur → nouvelle section GUI (flux LDR)."""
    secs = upd.parse(stock_upd)
    l = ldr.Ldr(secs[0][32:-4])
    assert not l.occupied(BASE, BASE + len(code)), "zone du code couleur déjà utilisée par le flux d'origine"
    for pc, orig, sym in CALLS:
        cur = l.read(pc, 4)
        assert ldr.call_target(pc, cur) == orig, f"{pc:#x} : appel inattendu, mauvais firmware ?"
        l.write(pc, ldr.call(pc, syms[sym]))
        if verbose: print(f"  CALL {pc:#x} : {orig:#x} → {syms[sym]:#x} ({sym[1:]})")
    for pc, orig, sym in JUMPS:
        assert l.read(pc, 4) == orig, f"{pc:#x} : octets inattendus, mauvais firmware ?"
        j = bytearray(ldr.call(pc, syms[sym])); j[1] = 0xe2          # CALL (e3) → JUMP.L (e2)
        l.write(pc, bytes(j))
        if verbose: print(f"  JUMP {pc:#x} → {syms[sym]:#x} ({sym[1:]})")
    for pc, orig, sym in PTRS:
        assert l.read(pc, 8) == orig, f"{pc:#x} : octets inattendus, mauvais firmware ?"
        a = syms[sym]
        l.write(pc, orig[:2] + struct.pack("<H", a & 0xFFFF) + orig[4:6] + struct.pack("<H", a >> 16))
        if verbose: print(f"  POINTEUR {pc:#x} → {a:#x} ({sym[1:]})")
    for pc, orig, new in BYTES:
        o, n = bytes.fromhex(orig), bytes.fromhex(new)
        assert l.read(pc, len(o)) == o, f"{pc:#x} : octets inattendus, mauvais firmware ?"
        l.write(pc, n)
    if verbose: print(f"  {len(BYTES)} remplacements d'octets (table des points déplacée)")
    for pc, orig, sym in INSNS:
        assert l.read(pc, 4) == orig, f"{pc:#x} : octets inattendus, mauvais firmware ?"
        l.write(pc, ldr.call(pc, syms[sym]))
        if verbose: print(f"  CALL {pc:#x} (instruction remplacée) → {syms[sym]:#x} ({sym[1:]})")
    l.insert_before_final(BASE, code)
    gui = l.build()
    # contrôle : relecture
    r = ldr.Ldr(gui)
    orig = secs[0][32:-4]
    assert r.read(BASE, len(code)) == code and len(gui) == len(orig)
    assert gui[0xF4240:] == orig[0xF4240:], "les ressources ont bougé"
    return gui


def build():
    gcc, objcopy, nm = (os.path.join(TC, "bfin-elf-" + t) for t in ("gcc", "objcopy", "nm"))
    run(gcc, "-mcpu=bf531", "-O2", "-fno-delete-null-pointer-checks", "-fno-branch-count-reg", "-ffreestanding", "-fno-builtin", "-nostdlib", "-Wall", "-Wextra",
        "-c", "rgbwave.c", "-o", "rgbwave.o")
    run(gcc, "-mcpu=bf531", "-nostdlib", "-T", "rgbwave.ld", "-o", "rgbwave.elf", "rgbwave.o", "-lgcc")
    run(objcopy, "-O", "binary", "-j", ".text", "-j", ".rodata", "-j", ".data", "-j", ".bss",
        "rgbwave.elf", "rgbwave.bin")
    syms = {}
    for line in subprocess.run([nm, "rgbwave.elf"], capture_output=True, text=True, check=True).stdout.split("\n"):
        p = line.split()
        if len(p) == 3: syms[p[2]] = int(p[0], 16)
    code = open(os.path.join(HERE, "rgbwave.bin"), "rb").read()
    print(f"code couleur : {len(code)} octets à {BASE:#x}")
    assert BASE + len(code) <= 0x00FE0000, "le code couleur déborde sur la copie de l'image (0xFE0000)"

    gui = apply(open(os.path.join(FW, "stock", "C2KNXS.UPD"), "rb").read(), code, syms)
    open(os.path.join(HERE, "gui_rgb.ldr"), "wb").write(gui)
    print("écrit", os.path.join(HERE, "gui_rgb.ldr"), len(gui), "octets")
    return gui


if __name__ == "__main__":
    build()
