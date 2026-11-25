#!/usr/bin/env python3
"""Flux de démarrage LDR du Blackfin BF531 (section GUI du C2KNXS.UPD).

Bloc = en-tête 10 octets (adresse u32, nombre u32, drapeaux u16) + données (sauf bloc de remplissage).
Drapeaux utiles : 0x0001 remplissage à zéro (pas de données), 0x0010 ignoré, 0x8000 dernier bloc.
Après le dernier bloc viennent les ressources (images, polices…), laissées telles quelles.
"""
import struct

ZEROFILL, IGNORE, FINAL = 0x0001, 0x0010, 0x8000


class Ldr:
    def __init__(self, data):
        self.blocks = []            # [adresse, drapeaux, données (bytearray) ou nombre (remplissage)]
        i = 0
        while True:
            a, n, f = struct.unpack_from("<IIH", data, i)
            i += 10
            if f & ZEROFILL:
                self.blocks.append([a, f, n])
            else:
                self.blocks.append([a, f, bytearray(data[i:i + n])])
                i += n
            if f & FINAL:
                break
        self.stream_len = i
        self.tail = bytes(data[i:])

    def find(self, addr, n=1):
        for a, f, d in self.blocks:
            if not f & (ZEROFILL | IGNORE) and a <= addr and addr + n <= a + len(d):
                return d, addr - a
        raise KeyError(f"{addr:#x} n'est dans aucun bloc de données")

    def read(self, addr, n):
        d, o = self.find(addr, n)
        return bytes(d[o:o + n])

    def write(self, addr, data):
        d, o = self.find(addr, len(data))
        d[o:o + len(data)] = data

    def occupied(self, lo, hi):
        """Vrai si un bloc (données ou remplissage) touche [lo, hi[."""
        for a, f, d in self.blocks:
            n = d if f & ZEROFILL else len(d)
            if a < hi and lo < a + n and not f & IGNORE:
                return True
        return False

    def insert_before_final(self, addr, data):
        """Ajoute un bloc de données juste avant le dernier bloc (qui doit rester le dernier)."""
        assert self.blocks[-1][1] & FINAL
        self.blocks.insert(len(self.blocks) - 1, [addr, 0, bytearray(data)])

    def build(self):
        """Les ressources commencent à une position fixe de la section (0xF4240) : le flux grandit
        dans les zéros de remplissage qui le suivent, la taille totale et les ressources ne bougent pas."""
        out = bytearray()
        for a, f, d in self.blocks:
            if f & ZEROFILL:
                out += struct.pack("<IIH", a, d, f)
            else:
                out += struct.pack("<IIH", a, len(d), f) + d
        grow = len(out) - self.stream_len
        if grow > 0:
            assert not any(self.tail[:grow]), "plus assez de place libre avant les ressources"
            tail = self.tail[grow:]
        else:
            tail = bytes(-grow) + self.tail
        return bytes(out + tail)


def call(pc, target):
    """Instruction CALL (32 bits) du Blackfin, de pc vers target."""
    off = (target - pc) // 2
    assert (target - pc) % 2 == 0 and -(1 << 23) <= off < (1 << 23), "CALL hors de portée"
    off &= 0xFFFFFF
    return struct.pack("<HH", 0xE300 | off >> 16, off & 0xFFFF)


def call_target(pc, raw):
    w0, w1 = struct.unpack("<HH", raw)
    assert w0 & 0xFF00 == 0xE300, "pas un CALL"
    off = (w0 & 0xFF) << 16 | w1
    if off & 0x800000:
        off -= 1 << 24
    return pc + 2 * off
