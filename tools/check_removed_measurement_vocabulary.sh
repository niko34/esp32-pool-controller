#!/usr/bin/env bash
# =============================================================================
# check_removed_measurement_vocabulary.sh — feature-152 (D6/D7) : garde
# anti-retour du vocabulaire de l'ancienne chaîne de mesure (14 étages, verrous
# collants, spikeReject/rearm/latch/dispersion)
# =============================================================================
# feature-152 a retiré des MÉCANISMES, pas juste des mots : « filtre non
# prêt », « capteur instable », « stabilisation du filtre », les alertes MQTT
# ph_filter_rearm_denied/orp_filter_rearm_denied/ph_filter_not_ready/
# orp_filter_not_ready. Sans garde, le premier correctif pressé (quelqu'un qui
# recopie un vieux message depuis un commit antérieur, ou réintroduit une
# qualification de garde par facilité) les fait REVENIR en silence — et avec
# eux le vocabulaire d'un fonctionnement abandonné, dans le code comme dans la
# tête (spec feature-152, section « Nettoyage du vocabulaire »).
#
# ⚠️ PÉRIMÈTRE PRÉCIS (condition D7, explicite) — PAS un grep global :
#   - data/*.js, data/*.html, data/*.css (hors libs tierces vendues telles
#     quelles : uPlot.*) : texte BRUT, commentaires compris — tout ce qui est
#     dans data/ est livré au navigateur, un commentaire y est aussi visible
#     qu'une chaîne affichée pour qui inspecte la source.
#   - src/*.cpp, src/*.h : UNIQUEMENT les LIBELLÉS UTILISATEUR, c'est-à-dire le
#     contenu de littéraux `"..."` — jamais les commentaires. « rearm » et
#     « latch » existent LÉGITIMEMENT comme noms de variables/fonctions dans
#     oom_guard.{h,cpp}, oom_guard_logic.h, net_health_logic.{h,cpp},
#     heap_metrics.{h,cpp}, ph_feedforward_logic.h, ph_session_logic.h,
#     ph_integral_logic.h (mécanismes INDÉPENDANTS, non retirés) : ces fichiers
#     sont EXCLUS entièrement. Un grep global sur ces mots crierait au loup sur
#     des dizaines de commentaires légitimes (historique, renvois vers
#     l'ADR...) et serait désactivé au premier correctif pressé.
#
# MOTIFS RECHERCHÉS (phrases COMPOSÉES, pas des mots nus pour « latch »/
# « rearm » seuls — trop ambigus face aux mécanismes légitimes ci-dessus listés
# : le latch du feedforward pH, par exemple, est un garde-fou qui RESTE) :
#   - "filtre non prêt" / "Filtre de mesure non prêt"
#   - "capteur instable"
#   - "stabilisation du filtre"
#   - "latch EMI"
#   - "réarmage" / "re-armage" / "ré-armage" (bare — zéro occurrence légitime
#     connue à ce jour, terme assez distinctif pour ne pas crier au loup)
#   - ph_filter_rearm_denied / orp_filter_rearm_denied (alertes MQTT retirées)
#   - ph_filter_not_ready / orp_filter_not_ready (alertes MQTT retirées)
#
# EXEMPTION — UN SEUL MÉCANISME, EXPLICITE : le marqueur `vocab-ok` posé sur la
# ligne elle-même. Qui veut écrire une de ces phrases doit le DÉCLARER ; le
# marqueur est visible en revue de diff, et il ne peut pas se produire par
# accident.
#
# ⚠️ CORRIGÉ EN REVUE (feature-152, code-reviewer, 2026-10-11). La première
# version exemptait toute ligne contenant « sans », « aucun », « feedforward »,
# « ff » ou « firmware ». Deux défauts, le second prouvé par mutation :
#   1. « sans » et « aucun » sont deux des négations les plus COURANTES du
#      français. « Filtre non prêt : aucune mesure acceptée » et « Capteur
#      instable (…) sans dosage » passaient le contrôle (exit 0) — soit très
#      exactement les réintroductions que cette garde existe pour attraper.
#   2. Ces exemptions n'exemptaient RIEN : POOL_VOCAB_LIST=1 rend ZÉRO
#      occurrence brute. Elles étaient donc de la surface d'attaque pure,
#      justifiées par des lignes (app.js:7769/7809, :4013) qui ne matchent même
#      pas le motif. Une exemption qui ne protège aucune ligne réelle ne doit
#      pas exister.
# Les fichiers où `latch`/`rearm` sont des mécanismes légitimes INDÉPENDANTS
# (feedforward, oom_guard, net_health, heap, session/intégrale pH) restent
# exclus PAR FICHIER, ci-dessous (EXEMPT_FILES) : c'est une liste close, pas un
# mot-clé devinable.
#
# ⛔ CE QUE CE SCRIPT NE PEUT PAS FERMER (humilité assumée, patron
# check_memory_discipline.sh) :
#   1. grep ligne à ligne : un littéral JS/C++ éclaté sur plusieurs lignes
#      échappe au motif de détection de chaîne (src/).
#   2. Le marqueur `vocab-ok` est, par construction, contournable par qui le
#      pose sans réfléchir. Il rend la dérogation VISIBLE, il ne l'empêche pas.
#
# MODE AUDIT : POOL_VOCAB_LIST=1 ./tools/check_removed_measurement_vocabulary.sh
# liste les occurrences BRUTES, SANS filtrage du marqueur. Référence au
# 2026-10-11 : ZÉRO occurrence des deux côtés.
#
# Usage : ./tools/check_removed_measurement_vocabulary.sh
# Sortie : 0 si conforme, 1 sinon (liste des sites fautifs sur stderr).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

# Phrases composées (étendu, regex étendue POSIX, insensible à la casse via -i).
PATTERN='filtre non pr(e|é|ê)t|capteur instable|stabilisation du filtre|latch emi|r(e|é)-?armage|ph_filter_rearm_denied|orp_filter_rearm_denied|ph_filter_not_ready|orp_filter_not_ready'

# Marqueur de dérogation explicite, posé sur la ligne elle-même (cf. en-tête).
EXEMPT='vocab-ok'

# --- data/ : texte BRUT (commentaires compris), hors libs tierces vendues ---
DATA_FILES=$(find data -maxdepth 1 -type f \( -name '*.js' -o -name '*.html' -o -name '*.css' \) \
             | grep -v '^data/uPlot' | sort)

raw_data=""
if [ -n "$DATA_FILES" ]; then
  hits=$(printf '%s\n' "$DATA_FILES" | xargs -r grep -HnoiE "$PATTERN" 2>/dev/null || true)
  # grep -o ne rend que le motif : on relit la ligne ENTIÈRE pour y chercher le
  # marqueur `vocab-ok`, qui peut être n'importe où sur la ligne (commentaire
  # de fin de ligne, typiquement).
  while IFS= read -r hit; do
    [ -z "$hit" ] && continue
    file="${hit%%:*}"
    rest="${hit#*:}"
    lineno="${rest%%:*}"
    content=$(sed -n "${lineno}p" "$file" 2>/dev/null)
    if printf '%s' "$content" | grep -qiE "$EXEMPT"; then
      continue
    fi
    raw_data="${raw_data}${file}:${lineno}: ${content}
"
  done <<< "$hits"
fi

# --- src/ : UNIQUEMENT les littéraux "..." sur la ligne, hors fichiers où
# rearm/latch sont des mécanismes légitimes INDÉPENDANTS (D7).
EXEMPT_FILES='src/oom_guard\.h|src/oom_guard\.cpp|src/oom_guard_logic\.h|src/net_health_logic\.h|src/net_health_logic\.cpp|src/heap_metrics\.h|src/heap_metrics\.cpp|src/ph_feedforward_logic\.h|src/ph_session_logic\.h|src/ph_integral_logic\.h'

SRC_FILES=$(find src -type f \( -name '*.cpp' -o -name '*.h' \) \
            | grep -vE "^(${EXEMPT_FILES})$" | sort)

# Motif : le fragment recherché DOIT apparaître À L'INTÉRIEUR d'une paire de
# guillemets doubles sur la MÊME ligne physique (limite assumée : un littéral
# éclaté sur plusieurs lignes échappe à ce motif, cf. en-tête).
STR_PATTERN="\"[^\"]*(${PATTERN})[^\"]*\""

raw_src=""
if [ -n "$SRC_FILES" ]; then
  hits=$(printf '%s\n' "$SRC_FILES" | xargs -r grep -HnoiE "$STR_PATTERN" 2>/dev/null || true)
  while IFS= read -r hit; do
    [ -z "$hit" ] && continue
    file="${hit%%:*}"
    rest="${hit#*:}"
    lineno="${rest%%:*}"
    content=$(sed -n "${lineno}p" "$file" 2>/dev/null)
    if printf '%s' "$content" | grep -qiE "$EXEMPT"; then
      continue
    fi
    raw_src="${raw_src}${file}:${lineno}: ${content}
"
  done <<< "$hits"
fi

if [ "${POOL_VOCAB_LIST:-0}" = "1" ]; then
  echo "--- data/ (BRUT, motif seul, sans filtrage exemption) ---"
  [ -n "$DATA_FILES" ] && printf '%s\n' "$DATA_FILES" | xargs -r grep -HnoiE "$PATTERN" 2>/dev/null
  echo "--- src/ (BRUT, littéraux de chaîne, sans filtrage exemption) ---"
  [ -n "$SRC_FILES" ] && printf '%s\n' "$SRC_FILES" | xargs -r grep -HnoiE "$STR_PATTERN" 2>/dev/null
  exit 0
fi

found="${raw_data}${raw_src}"
found=$(printf '%s\n' "$found" | grep -v '^$' || true)

if [ -n "$found" ]; then
  {
    echo "ERREUR feature-152 (D6/D7) : vocabulaire de l'ancienne chaîne de mesure détecté."
    echo
    echo "La chaîne pH/ORP est passée de 14 étages et 13 états bloquants à 5 étages"
    echo "sans aucun verrou (feature-152). Un message, chip, badge, champ, code de"
    echo "blocage, alerte ou topic décrivant un mécanisme RETIRÉ (filtre non prêt,"
    echo "capteur instable, stabilisation du filtre, ré-armage, les alertes MQTT"
    echo "*_filter_rearm_denied/*_filter_not_ready) fait chercher à l'utilisateur une"
    echo "cause qui n'existe plus, et entretient le vocabulaire d'un fonctionnement"
    echo "abandonné."
    echo
    echo "Si l'occurrence ci-dessous est légitime (mécanisme DIFFÉRENT, toujours"
    echo "actif — feedforward, oom_guard, net_health, heap, session pH, intégrale"
    echo "pH) : reformuler pour lever l'ambiguïté avec le vocabulaire retiré,"
    echo "déplacer dans un des fichiers déjà exemptés de ce contrôle (voir en-tête,"
    echo "liste D7), ou — en dernier recours et de façon VISIBLE en revue —"
    echo "apposer le marqueur vocab-ok sur la ligne."
    echo
    echo "$found"
  } >&2
  exit 1
fi

echo "OK — aucun vocabulaire de l'ancienne chaîne de mesure (feature-152) détecté."
