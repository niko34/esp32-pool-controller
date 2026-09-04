#!/bin/bash
# ============================================================================
# Génération des PDF utilisateur (docs/user/) depuis les sources HTML.
#
# - Les captures d'écran sont REDIMENSIONNÉES à la résolution d'impression
#   (1200 px de large ≈ 180 dpi sur la largeur utile A4) dans un dossier
#   temporaire — les originaux de screenshots/ restent intacts.
# - La structure relative est reproduite dans le temporaire pour que les
#   chemins ../../../screenshots/ des HTML résolvent sans modification.
# - Sortie : docs/user/guide-*-v<version>.pdf
#
# Usage : ./tools/build_user_pdfs.sh [version]   (défaut : lit src/version.h)
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
LARGEUR_CIBLE=1200   # px — largeur utile A4 ≈ 172 mm : ~180 dpi, net à l'impression
QUALITE_JPEG=75      # normalFormatOptions sips (0-100)

VERSION="${1:-$(sed -n 's/.*FIRMWARE_VERSION "\([0-9]*\.[0-9]*\)\..*/\1/p' "$ROOT/src/version.h")}"
if [ -z "$VERSION" ]; then echo "❌ version introuvable"; exit 1; fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/docs/user/src" "$TMP/screenshots"

# 1. Sources HTML + style — la version affichée (couverture, pied de page,
#    colophon) est injectée depuis version.h. Motifs CIBLÉS uniquement :
#    les mentions historiques du contenu (« depuis la v2.32 ») sont préservées.
cp "$ROOT/docs/user/src/"*.css "$TMP/docs/user/src/"
for html in "$ROOT/docs/user/src/"*.html; do
  # ( |&nbsp;) : la typographie française met des espaces insécables
  sed -E "s/(firmware|Firmware|version)( |\&nbsp;)2\.[0-9]+(\.[0-9]+)?/\1\2${VERSION}/g" \
      "$html" > "$TMP/docs/user/src/$(basename "$html")"
done

# 2. Captures redimensionnées (jamais agrandies : sips -Z borne au max)
echo "→ Redimensionnement des captures (${LARGEUR_CIBLE}px, JPEG q${QUALITE_JPEG})…"
for img in "$ROOT/screenshots/"*.jpg; do
  base="$(basename "$img")"
  sips -Z "$LARGEUR_CIBLE" -s format jpeg -s formatOptions "$QUALITE_JPEG" \
       "$img" --out "$TMP/screenshots/$base" >/dev/null
done

# 3. Génération des PDF
for guide in guide-utilisation guide-regulation; do
  case "$guide" in
    guide-utilisation) out="guide-d-utilisation-v${VERSION}.pdf" ;;
    guide-regulation)  out="guide-regulation-v${VERSION}.pdf" ;;
  esac
  echo "→ $out"
  "$CHROME" --headless --no-pdf-header-footer \
    --print-to-pdf="$ROOT/docs/user/$out" \
    "$TMP/docs/user/src/$guide.html" 2>/dev/null
done

echo "✅ Terminé :"
ls -lh "$ROOT/docs/user/"*.pdf | awk '{print "   " $9 " — " $5}'
