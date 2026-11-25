#!/bin/zsh
# Compile la partie Mac : serato_hd.dylib (universelle, macOS 11 ou plus récent comme Serato, injectée
# dans Serato) et hdtest (essais).
set -e
cd "${0:A:h}"
FW=(-framework AudioToolbox -framework CoreFoundation)
clang -O2 -Wall -Wextra -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -dynamiclib -o serato_hd.dylib \
  serato_hd.c hdcore.c $FW -framework IOKit
if [ -f hdtest.c ]; then   # outil d'essai, absent du paquet pour un autre Mac (paquet.sh)
  clang -O2 -Wall -Wextra -DHD_HARNESS -o hdtest hdtest.c serato_hd.c hdcore.c $FW
  echo "ok : serato_hd.dylib, hdtest"
else
  echo "ok : serato_hd.dylib"
fi
