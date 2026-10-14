#!/usr/bin/env bash
# =============================================================================
# check_resolve_selector_mode_vectors.sh — les vecteurs de « quel mode
# AFFICHER sur le sélecteur après une tentative de bascule ? » tournent
# vraiment (feature-161, correctif bloquant du code-reviewer)
# =============================================================================
# Patron `check_cal_points_acquired_vectors.sh` (feature-159) / `check_ph_
# health_vectors.sh` (feature-154) : une sentinelle que personne ne lance ne
# protège de rien.
#
# CE QUE test/resolve_selector_mode_vectors.js VÉRIFIE : la fonction PURE
# `resolveSelectorMode(requested, lastConfirmed, outcome)` de `data/app.js`,
# qui décide quel mode le sélecteur pH/ORP doit AFFICHER après une tentative
# de bascule — succès serveur, refus (409/400), panne réseau, ou annulation
# de la confirmation C4 (« Interrompre la chloration de secours ? »). Son
# absence a directement causé un bloquant de revue : annuler cette
# confirmation laissait le sélecteur ORP affiché sur « Arrêt » alors que la
# régulation continuait de tourner, jusqu'au prochain rafraîchissement — un
# mutant qui supprimait le retour à l'état réel ne mourait pas, faute de
# décision extraite et testable.
#
# Le script EXTRAIT le texte réel de cette fonction (ancre + comptage de
# profondeur via `test/js_extract.js`, ADR-0099) au lieu d'en tester une
# copie, puis l'évalue contre la matrice complète : succès (mode demandé),
# refus serveur ET panne réseau (même traitement que sendConfigDetailed,
# dernier mode confirmé), annulation (dernier mode confirmé), et le cas où
# le dernier mode confirmé est inconnu (undefined/null).
#
# NODE : ubuntu-latest (runners GitHub-hébergés) embarque Node.js par défaut —
# aucun actions/setup-node nécessaire. En local, `node` doit être sur le PATH.
#
# Usage : ./tools/check_resolve_selector_mode_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/resolve_selector_mode_vectors.js" >&2
  exit 1
fi

node test/resolve_selector_mode_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR feature-161 : les vecteurs de resolveSelectorMode ne correspondent plus au code livré" >&2
  echo "(data/app.js, resolveSelectorMode) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
