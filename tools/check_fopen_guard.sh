#!/usr/bin/env bash
# =============================================================================
# check_fopen_guard.sh — le point d'admission unique reste unique (ADR-0070)
# =============================================================================
# « L'ordre se voit, la liste s'oublie. »
#
# Sur ESP32, fopen() fait allouer à newlib un verrou récursif de 92 o. Si ce
# malloc échoue, lock_init_generic appelle abort() : AUCUN code de retour à
# tester. Deux crashs de production (29/08/2026 et 05/09/2026) viennent de là.
# ADR-0070 rend le défaut inexprimable en imposant UN point d'admission :
# src/fs_guard.cpp. Ce contrôle vérifie que personne ne l'a contourné.
#
# ⚠️ exists() ET openNextFile() COMPTENT COMME DES OUVERTURES : ni
# VFSImpl::exists() ni VFSFileImpl::openNextFile() ne font de stat() — ils
# construisent un VFSFileImpl, donc un fopen complet, verrou newlib compris.
# `exists()` est le site que le recensement du cycle 1 a failli manquer ; c'est
# pourquoi CE SCRIPT LES CONTRÔLE TOUS LES TROIS.
#
# ⛔ CE QUE CE SCRIPT NE PEUT PAS FERMER — à savoir plutôt qu'à croire couvert :
#   1. grep travaille LIGNE À LIGNE. Une coupure de ligne après le `->` ou le
#      `.` (formatage automatique d'un appel long) échappe au motif. Aucun
#      motif régulier mono-ligne ne peut y remédier ; il faudrait un analyseur
#      syntaxique.
#   2. Toute API du framework qui construit un VFSFileImpl sous un AUTRE nom
#      passera aussi. Le recensement de docs/subsystems/acces-fichiers.md reste
#      donc tenu À LA MAIN — ce script ferme le chemin qu'un grep PEUT voir,
#      pas la discipline entière.
#
# Usage : ./tools/check_fopen_guard.sh
# Sortie : 0 si conforme, 1 sinon (liste des sites fautifs sur stderr).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

EXEMPT="src/fs_guard.cpp"

# Ce que l'on cherche :
#   - `.open(` / `->open(` .............. l'ouverture explicite ;
#   - `.exists(` / `->exists(` .......... l'ouverture DÉGUISÉE (cf. entête) ;
#   - `.openNextFile(` / `->openNextFile(` l'ouverture déguisée en itération ;
#   - avec ESPACES tolérés autour du `->` ou du `.` : `pfs -> open(` compile
#     aussi bien que `pfs->open(`, et passait avant la revue de bug-100.
# Périmètre : TOUT src/, .cpp ET .h, sous-répertoires compris — une ouverture
# inlinée dans un en-tête est une ouverture.
# Sont exclues les lignes de commentaire (`//` ou `*` en tête).
#
# `grep -H` est OBLIGATOIRE : xargs peut découper en lots, et un lot d'UN SEUL
# fichier ferait taire le préfixe `fichier:` — les deux filtres suivants, qui
# s'y ancrent, deviendraient alors inopérants. Latent à 114 fichiers, ce défaut
# se déclencherait tout seul un jour.
found=$(find src -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
        | xargs -0 grep -HnE '(->|\.)[[:space:]]*(open|openNextFile|exists)[[:space:]]*\(' \
        | grep -v "^${EXEMPT}:" \
        | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*)' \
        || true)

if [ -n "$found" ]; then
  {
    echo "ERREUR ADR-0070 : ouverture de fichier hors du point d'admission unique."
    echo "Tout fopen interne — y compris un exists(), qui OUVRE le fichier —"
    echo "doit passer par openIfHeapAllows()/existsIfHeapAllows()"
    echo "de src/fs_guard.h, et le site doit être classé (A ou B) dans"
    echo "docs/subsystems/acces-fichiers.md DANS LE MÊME COMMIT."
    echo
    echo "$found"
  } >&2
  exit 1
fi

echo "OK — aucune ouverture de fichier hors de ${EXEMPT}."
