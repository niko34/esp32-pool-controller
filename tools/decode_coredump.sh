#!/bin/bash
# Décode un coredump ESP32 récupéré via GET /coredump/download
# Usage: ./tools/decode_coredump.sh [coredump.bin] [firmware.elf]
#
# Si le firmware.elf courant ne correspond pas au coredump (SHA mismatch),
# les archives disponibles dans builds/ sont listées automatiquement.
#
# Téléchargement du coredump (authentification requise) :
#   curl -H "X-Auth-Token: <token>" http://<ip>/coredump/download -o coredump.bin

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CORE="${1:-coredump.bin}"
ELF="${2:-$PROJECT_DIR/.pio/build/esp32dev/firmware.elf}"

if [ ! -f "$CORE" ]; then
  echo "ERREUR: fichier coredump '$CORE' introuvable"
  echo "Usage: $0 [coredump.bin] [firmware.elf]"
  echo ""
  echo "Téléchargement depuis l'ESP32 :"
  echo "  curl -H 'X-Auth-Token: <token>' http://<ip>/coredump/download -o coredump.bin"
  exit 1
fi

if [ ! -f "$ELF" ]; then
  echo "ERREUR: firmware.elf introuvable : $ELF"
  echo "  Lancer 'pio run' ou spécifier un ELF archivé en 2e argument"
  exit 1
fi

PENV_PYTHON="$HOME/.platformio/penv/bin/python3"
if [ ! -f "$PENV_PYTHON" ]; then
  PENV_PYTHON="python3"
fi

GDB="$HOME/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-gdb"

echo "=== Décodage coredump ESP32 ==="
echo "ELF  : $ELF"
echo "Core : $CORE"
echo ""

# Lancer le décodage — capturer la sortie pour détecter un SHA mismatch
OUTPUT=$("$PENV_PYTHON" -m esp_coredump --chip esp32 info_corefile \
  --core "$CORE" \
  --core-format raw \
  ${GDB:+--gdb "$GDB"} \
  "$ELF" 2>&1) || true

if echo "$OUTPUT" | grep -q "SHA256.*!="; then
  # Extraire le SHA du coredump depuis le message d'erreur
  CORE_SHA=$(echo "$OUTPUT" | grep -oE 'coredump SHA256\([0-9a-f]+\)' | grep -oE '[0-9a-f]{8,}')
  echo "$OUTPUT"
  echo ""
  echo "─────────────────────────────────────────────────────"
  echo "Le coredump a été capturé avec un firmware différent."
  if [ -n "$CORE_SHA" ]; then
    echo "SHA du coredump : $CORE_SHA"
  fi
  echo ""
  BUILDS_DIR="$PROJECT_DIR/builds"
  if [ -d "$BUILDS_DIR" ] && ls "$BUILDS_DIR"/firmware_*.elf &>/dev/null; then
    echo "ELF archivés disponibles (du plus récent au plus ancien) :"
    ls -t "$BUILDS_DIR"/firmware_*.elf | while read -r f; do
      echo "  $f"
    done
    echo ""
    echo "Relancer avec l'ELF correspondant :"
    echo "  $0 $CORE <chemin/vers/firmware_YYYYMMDD_HHMMSS.elf>"
  else
    echo "Aucun ELF archivé trouvé dans builds/."
    echo "Les archives sont créées automatiquement par deploy.sh à partir de maintenant."
  fi
  echo "─────────────────────────────────────────────────────"
  exit 1
fi

echo "$OUTPUT"
