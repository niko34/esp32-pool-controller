#!/usr/bin/env bash
# =============================================================================
# check_ph_card_permanent_vectors.sh — la carte « Correction manuelle de la
# mesure » est permanente, et sa friction tient (bug-201)
# =============================================================================
# Patron `check_ph_tune_remove_vectors.sh` : une sentinelle que personne ne lance
# ne protège de rien.
#
# CE QUE test/ph_card_permanent_vectors.js VÉRIFIE (code VIVANT : data/app.js est
# extrait par ancre, data/index.html est lu tel quel — jamais une recopie) :
#   - bug-201 (C40) : #ph-card-tune sans `hidden`, absente de cardIds, MAIS gardant
#     la classe .flow-maintenance (sinon le flux d'étalonnage la masquerait) ;
#   - bug-201 (C49/C48) : #ph_tune_stale_cancel_btn n'existe plus, un seul contrôle
#     destructif sur la carte ;
#   - bug-201 (C41/C45/C46/C47) : seuil LU sur la clé servie, deux variantes de texte
#     selon le signe, phValueRaw et ph (jamais phRaw), repli, focus, double appui,
#     sorties (Échap, clic extérieur, page, retour de /get-config) ;
#   - bug-201 (C50/C51) : les deux phrases gelées ; refus et avertissement
#     feature-168 (C8) sur DEUX rendus distincts ;
#   - bug-201 (a11y) : aucune écriture DOM si le texte n'a pas changé.
#
# Usage : ./tools/check_ph_card_permanent_vectors.sh
# Sortie : 0 si conforme, 1 sinon.
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/ph_card_permanent_vectors.js" >&2
  exit 1
fi

node test/ph_card_permanent_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR bug-201 : la carte de correction pH ne correspond plus aux vecteurs" >&2
  echo "(data/index.html, data/app.js) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
