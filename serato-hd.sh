#!/bin/zsh
# Lance Serato DJ Pro avec mac/serato_hd.dylib : sur un CDJ-2000NXS au firmware modifié
# (voir README), la grande waveform du haut s'affiche en couleur en mode HID, avec
# la grille des temps de Serato, et la petite waveform est colorée.
#   ./serato-hd.sh
# Réglages (facultatifs) : HD_DECKS=1 (seule la platine détectée en premier reçoit les waveforms ;
# inutile avec une platine d'origine, qui les ignore), HD_GAINS, HD_GAMMA, HD_HMAX
# (couleurs et hauteurs, voir mac/hdcore.c). Journal : captures/hd-<date>.log ; tous les échanges
# HID de la session sont enregistrés dans captures/hd-<date>.rec (rejouables dans l'émulateur).
set -e
cd "${0:A:h}"
if pgrep -xq "Serato DJ Pro"; then
  echo "Serato est déjà ouvert : quitte-le d'abord (Cmd+Q)." >&2
  exit 1
fi
[ -f mac/serato_hd.dylib ] || mac/build.sh
mkdir -p captures
log="$PWD/captures/hd-$(date +%Y-%m-%dT%H-%M-%S).log"
echo "Journal : $log"
SERATO_HD_LOG="$log" SERATO_HD_REC="${log%.log}.rec" DYLD_INSERT_LIBRARIES="$PWD/mac/serato_hd.dylib" \
  "/Applications/Serato DJ Pro.app/Contents/MacOS/Serato DJ Pro" >/dev/null 2>&1
echo "Serato fermé."
grep -E "CHARGEMENT|ENVOI|sans waveform" "$log" | tail -20
