#!/bin/bash
# ============================================================================
# Génère les PDF de documentation utilisateur depuis les sources HTML.
#
#   docs/user/src/guide-utilisation.html  ->  docs/user/guide-d-utilisation-vX.Y.pdf
#   docs/user/src/guide-regulation.html   ->  docs/user/guide-regulation-vX.Y.pdf
#
# Ce script est un POINT D'ENTRÉE STABLE pour /release. Le travail réel est fait
# par tools/build_user_pdfs.sh, qui ne se contente pas d'imprimer le HTML :
#   - il redimensionne les captures à la résolution d'impression (sans quoi les
#     PDF pèsent 26 Mo au lieu de 4) ;
#   - il injecte la version lue dans src/version.h dans les pages ;
#   - il reconstruit une arborescence temporaire pour que les chemins relatifs
#     des captures résolvent sans toucher aux sources.
# Ne pas court-circuiter tools/build_user_pdfs.sh : on perdrait ces trois points.
#
# Ce qu'ajoute ce script : la DÉTECTION du moteur de rendu, pour ne pas dépendre
# d'un chemin macOS codé en dur.
#
# Usage : ./scripts/build-docs.sh [version]   (défaut : lue dans src/version.h)
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# ---------------------------------------------------------------------------
# 1. Détection du moteur : weasyprint d'abord, puis Chrome/Chromium.
# ---------------------------------------------------------------------------
# ⚠️ weasyprint est détecté mais PAS encore utilisé : il ne sait pas exécuter le
# JavaScript des pages ni honorer les sauts de page pilotés par le CSS
# d'impression de docs/user/src/. Le basculer exigerait de revoir ces feuilles.
# On le signale plutôt que de produire silencieusement un PDF dégradé.
MOTEUR=""
if command -v weasyprint >/dev/null 2>&1; then
  MOTEUR="weasyprint"
fi

CHROME_DETECTE=""
for candidat in \
  "${CHROME:-}" \
  "$(command -v chromium 2>/dev/null || true)" \
  "$(command -v chromium-browser 2>/dev/null || true)" \
  "$(command -v google-chrome 2>/dev/null || true)" \
  "$(command -v chrome 2>/dev/null || true)" \
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  "/Applications/Chromium.app/Contents/MacOS/Chromium" \
  "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge" \
  "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser"
do
  if [ -n "$candidat" ] && [ -x "$candidat" ]; then
    CHROME_DETECTE="$candidat"
    break
  fi
done

if [ -z "$CHROME_DETECTE" ]; then
  echo "❌ Aucun moteur de rendu PDF utilisable." >&2
  echo "" >&2
  if [ -n "$MOTEUR" ]; then
    echo "   weasyprint est installé, mais il ne convient pas ici : les guides" >&2
    echo "   utilisent du CSS d'impression et des sauts de page que seul un" >&2
    echo "   moteur Chromium rend correctement." >&2
    echo "" >&2
  fi
  echo "   Installez l'un de ces navigateurs :" >&2
  echo "     macOS  : brew install --cask google-chrome" >&2
  echo "              (ou Chromium, Brave, Microsoft Edge)" >&2
  echo "     Debian : sudo apt install chromium" >&2
  echo "" >&2
  echo "   Ou pointez explicitement un binaire :" >&2
  echo "     CHROME=/chemin/vers/chrome ./scripts/build-docs.sh" >&2
  exit 1
fi

echo "→ Moteur de rendu : $CHROME_DETECTE"

# ---------------------------------------------------------------------------
# 2. Vérification des sources
# ---------------------------------------------------------------------------
for src in docs/user/src/guide-utilisation.html docs/user/src/guide-regulation.html; do
  if [ ! -f "$src" ]; then
    echo "❌ Source manquante : $src" >&2
    echo "   Les HTML de docs/user/src/ sont la source de vérité de la" >&2
    echo "   documentation utilisateur ; les PDF n'en sont qu'un rendu." >&2
    exit 1
  fi
done

# ---------------------------------------------------------------------------
# 3. Génération
# ---------------------------------------------------------------------------
CHROME="$CHROME_DETECTE" ./tools/build_user_pdfs.sh "$@"
