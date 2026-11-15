#!/usr/bin/env bash
# =============================================================================
# check_recovery_page_generated.sh — src/ota_recovery_page.cpp est À JOUR (D3-b1, S8)
# =============================================================================
# Le .cpp de la page de secours est GÉNÉRÉ (tools/gen_recovery_page.js) puis
# VERSIONNÉ (ADR-0124 : `pio run` ne dépend pas de node). Ce contrôle re-génère
# dans un fichier temporaire et compare : il rougit si le .cpp a été édité à la
# main, OU si data/app.js (SHA-256, parseur de .sig) ou le gabarit a changé sans
# régénération. Il rougit aussi si une ancre d'extraction disparaît (le
# générateur échoue fort).
#
# ⚠️ La CI n'est PAS bloquante pour une release (elle est restée rouge trois
# commits durant) : ce contrôle est donc AUSSI lancé par
# tools/check_release_artefacts.sh, que /release exécute et qui l'arrête (S8).
#
# Usage : ./tools/check_recovery_page_generated.sh
# Sortie : 0 si à jour, 1 sinon.
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH" >&2
  exit 1
fi

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

if ! node tools/gen_recovery_page.js --out "$tmp" >/dev/null; then
  echo "ERREUR : le générateur a échoué (ancre ou motif interdit — voir ci-dessus)" >&2
  exit 1
fi

if ! diff -u src/ota_recovery_page.cpp "$tmp" >&2; then
  echo >&2
  echo "ERREUR D3-b1 : src/ota_recovery_page.cpp ne correspond pas à sa source." >&2
  echo "Ne JAMAIS l'éditer à la main. Régénérer : node tools/gen_recovery_page.js" >&2
  exit 1
fi

# Syntaxe du JS GÉNÉRÉ : un dedent() qui la casserait passerait le diff.
chk=$(mktemp --suffix=.js)
trap 'rm -f "$tmp" "$chk"' EXIT
node -e '
const s = require("fs").readFileSync(process.argv[1], "utf8");
const m = s.match(/<script>([\s\S]*)<\/script>/);
if (!m) { console.error("bloc <script> introuvable"); process.exit(1); }
require("fs").writeFileSync(process.argv[2], m[1]);
' src/ota_recovery_page.cpp "$chk" || exit 1
if ! node --check "$chk"; then
  echo "ERREUR D3-b1 : le JavaScript de la page générée a une erreur de syntaxe." >&2
  exit 1
fi

echo "OK — src/ota_recovery_page.cpp est à jour (JS syntaxiquement valide)."
