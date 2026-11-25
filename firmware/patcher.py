#!/usr/bin/env python3
"""Fabrique les fichiers de mise à jour du CDJ-2000NXS à partir du firmware officiel 1.44.

  python3 firmware/patcher.py <CDJ2000NXS_v144.zip ou C2KNXS.UPD> [dossier de sortie]

À TES PROPRES RISQUES / USE AT YOUR OWN RISK : flasher un firmware modifié peut rendre la platine
inutilisable et fait perdre la garantie ; logiciel fourni « tel quel », sans aucune garantie.

Le firmware officiel se télécharge sur le site de Pioneer DJ (page d'assistance du CDJ-2000NXS,
version 1.44). Ce dépôt ne contient aucun octet du firmware de Pioneer : seulement le code ajouté
(binaires/, compilé depuis patch/) et la liste des endroits où il est branché (patch/build.py,
patch/build_gui.py).

Écrit dans le dossier de sortie (par défaut : ./sortie) :
  PATCH/C2KNXS.UPD         firmware 1.44 + waveform couleur en mode HID, numéroté 9.99
  RESTAURATION/C2KNXS.UPD  firmware 1.44 d'origine, numéroté 9.99 (pour revenir en arrière)
Les deux résultats sont comparés aux empreintes attendues (binaires/manifeste.json) : en cas de
différence, rien n'est écrit.
"""
import hashlib, json, os, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "patch"))
import upd, build, build_gui  # noqa: E402

OFFICIEL = "b17c0f715fb8dd4187f4857409a99c40a3708b383cc0d360a983d291ded81097"   # C2KNXS.UPD 1.44


def sha(b):
    return hashlib.sha256(b).hexdigest()


def lire_officiel(chemin):
    if zipfile.is_zipfile(chemin):
        with zipfile.ZipFile(chemin) as z:
            noms = [n for n in z.namelist() if os.path.basename(n).upper() == "C2KNXS.UPD"]
            if not noms:
                sys.exit("Ce zip ne contient pas de C2KNXS.UPD.")
            return z.read(noms[0])
    return open(chemin, "rb").read()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = lire_officiel(sys.argv[1])
    if sha(d) != OFFICIEL:
        sys.exit("Ce n'est pas le firmware officiel 1.44 du CDJ-2000NXS (empreinte SHA-256 "
                 f"{sha(d)}, attendue {OFFICIEL}). Rien n'a été écrit.")
    b = os.path.join(HERE, "binaires")
    m = json.load(open(os.path.join(b, "manifeste.json")))
    hd = open(os.path.join(b, "hdwave.bin"), "rb").read()
    rgb = open(os.path.join(b, "rgbwave.bin"), "rb").read()
    print("Firmware officiel 1.44 reconnu. Construction du patch", m["version"], "…")
    app = build.apply(upd.extract_app(upd.main_image(upd.parse(d))), hd, m["hdwave"], verbose=False)
    gui = build_gui.apply(d, rgb, m["rgbwave"], verbose=False)
    patch = upd.build(d, app, "9.99", gui=gui)
    rest = upd.build(d, None, "9.99")
    for nom, res, att in (("PATCH", patch, m["sha256_patch"]), ("RESTAURATION", rest, m["sha256_restauration"])):
        if sha(res) != att:
            sys.exit(f"{nom} : résultat inattendu ({sha(res)}, attendu {att}). Rien n'a été écrit.")
    out = sys.argv[2] if len(sys.argv) > 2 else "sortie"
    for nom, res in (("PATCH", patch), ("RESTAURATION", rest)):
        os.makedirs(os.path.join(out, nom), exist_ok=True)
        p = os.path.join(out, nom, "C2KNXS.UPD")
        open(p, "wb").write(res)
        print(f"écrit {p}\n  SHA-256 {sha(res)}")
    print("Copy each file to the root of a FAT32 USB drive, without renaming it (see README).")
    print("Copie chaque fichier à la racine d'une clé USB en FAT32, sans le renommer (voir README).")
    print()
    print("USE AT YOUR OWN RISK: flashing modified firmware can permanently damage the player and")
    print("voids its warranty. Never cut the power during the update.")
    print("À TES PROPRES RISQUES : flasher un firmware modifié peut rendre la platine inutilisable et")
    print("fait perdre la garantie. Ne coupe jamais l'alimentation pendant la mise à jour.")


if __name__ == "__main__":
    main()
