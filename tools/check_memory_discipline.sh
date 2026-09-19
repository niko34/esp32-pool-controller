#!/usr/bin/env bash
# =============================================================================
# check_memory_discipline.sh — la discipline mémoire de feature-129 reste vraie
# =============================================================================
# ADR-0082 : le journal fractionnait le plus gros bloc libre du tas 8 bits en
# y allouant des `String` à chaque ligne. Chiffre reproductible (quatre
# démarrages, effondrement initial — pas la dérive lente de 18 h) : **168
# octets — les deux `String` d'une seule ligne de journal — ont détruit
# 23 552 octets de contiguïté**. Ce n'est pas une pénurie d'octets, c'est une
# pénurie de POSITION : le plus gros bloc contigu finit par tomber sous
# `kStaticFileMinAllocBytes`, et le serveur de fichiers statiques répond 503.
# Voir docs/adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md.
#
# Ce script rend cette discipline VÉRIFIABLE, pas seulement écrite en prose
# (CLAUDE.md, section « Mémoire ») : sans lui, un `std::vector<String>`
# réintroduit demain dans `logger.h` compile, passe `pio test -e native`, et
# ne rencontre AUCUN contrôle — exactement le défaut que feature-129 a corrigé.
#
# PREUVE QUE CE N'EST PAS DÉCORATIF : rejouée sur HEAD~3 (avant le commit
# c68b276 qui a livré l'arène circulaire), cette sentinelle refuse
# `_persistBuffer` (`std::vector<String> _persistBuffer;`, logger.h) ET
# `toWrite` (`std::vector<String> toWrite;`, logger.cpp) — exactement les deux
# structures qu'ADR-0082 a supprimées. Elle aurait attrapé le défaut réel, pas
# une reformulation a posteriori de son diagnostic.
#
# ⛔ CE QUE CE SCRIPT NE PEUT PAS FERMER — à savoir plutôt qu'à croire couvert :
#   1. grep travaille LIGNE À LIGNE : une déclaration éclatée sur plusieurs
#      lignes (formatage automatique d'un type long) échappe aux quatre
#      motifs. Aucun motif régulier mono-ligne ne peut y remédier.
#   2. Il ne prouve AUCUNE absence d'allocation dynamique en général — un
#      `char* p = (char*)heap_caps_malloc(...)` légitime (hors chemin de
#      journalisation) n'est pas son objet. Il ferme quatre motifs PRÉCIS
#      (feature-129/feature-130), pas la discipline mémoire entière.
#   3. L'échappatoire MEM-EXEMPT protège de l'INADVERTANCE, pas d'un
#      contournement délibéré : quiconque peut committer peut poser un motif
#      de complaisance. Il reste examiné par `code-reviewer`.
#
# MOTIFS RECHERCHÉS :
#   M1  — `String` MEMBRE D'UN TABLEAU (le motif exact de `_lastFilterPub`,
#         feature-025/feature-130) : `String foo[N]`, sur TOUT src/.
#   M2  — conteneur STL paramétré par `String` (le motif exact de
#         `_persistBuffer`/`toWrite`, retirés par ADR-0082) : `vector<String>`,
#         `map<String, ...>`, etc., sur TOUT src/.
#   M3A — toute déclaration `String` (variable, retour de fonction) DANS le
#         chemin de journalisation lui-même (`logger.{h,cpp}`,
#         `log_arena_logic.{h,cpp}`) : plus stricte que M1/M2 parce que ces
#         quatre fichiers sont PRÉCISÉMENT l'acquis à protéger — AUCUN
#         `String` n'y est légitime, même hors tableau/conteneur.
#   M3B — toute allocation dynamique explicite DANS ces mêmes quatre
#         fichiers : `new`, `malloc/calloc/realloc/strdup`, `std::vector`,
#         `std::string`, `JsonDocument`.
#
# Sont exclues les lignes de commentaire (`//`, `*`, `/*` en tête, NOCOM).
#
# `grep -H` est OBLIGATOIRE (même piège que check_fopen_guard.sh) : sans lui,
# le préfixe `fichier:` disparaît dès qu'un lot ne contient qu'un seul
# fichier, et les deux filtres qui s'y ancrent (NOCOM, exemption) deviennent
# inopérants en silence.
#
# ÉCHAPPATOIRE : `// MEM-EXEMPT(ADR-0082) : <motif> [feature-131]`, sur la
# ligne du hit ou celle JUSTE AU-DESSUS. Le motif est OBLIGATOIRE — un
# marqueur nu ne désarme rien : la ligne qui suit le marqueur (après le `:`)
# doit compter AU MOINS 20 caractères NON BLANCS. Pas d'exemption par nom de
# fichier entier (contrairement à check_fopen_guard.sh, qui exempte
# `fs_guard.cpp` intégralement — ici l'inverse est vrai : exempter
# `mqtt_manager.h` par son nom aveuglerait ~2 000 lignes qui n'ont rien à voir
# avec le motif ponctuel qui le justifie).
#
# MODE AUDIT : `POOL_MEM_LIST=1 ./tools/check_memory_discipline.sh` liste les
# occurrences BRUTES, SANS filtrage des exemptions — indispensable pour
# valider ce script sur l'historique (`git show HEADn:...`), où les
# commentaires d'exemption n'existent évidemment pas.
#
# Usage : ./tools/check_memory_discipline.sh
# Sortie : 0 si conforme, 1 sinon (liste des sites fautifs sur stderr).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

# Ce script vit sous tools/, hors du périmètre scanné (`src/` uniquement,
# voir ci-dessous) — mais il CITE littéralement `std::vector<String>` dans son
# propre en-tête (ci-dessus). Auto-exclusion défensive, même piège que
# check_no_peek_flag.sh:49 : si le périmètre venait un jour à s'élargir à
# tools/, ce script ne doit jamais se signaler lui-même.
SELF="tools/check_memory_discipline.sh"

M1='(^|[^:[:alnum:]_])String[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\['
M2='(vector|array|deque|list|set|map)[[:space:]]*<[^;]*\bString\b'
M3A='^[[:space:]]*(static[[:space:]]+|inline[[:space:]]+|virtual[[:space:]]+)*String[[:space:]]+[A-Za-z_]'
M3B='(\bnew\b[[:space:]]|\b(malloc|calloc|realloc|strdup)[[:space:]]*\(|std::vector|std::string|JsonDocument)'
NOCOM='^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*)'

LOG_PATH_FILES="src/logger.h src/logger.cpp src/log_arena_logic.h src/log_arena_logic.cpp"

# --- Collecte des occurrences BRUTES (avant filtrage des exemptions) -------

raw=""

# M1 + M2 : TOUT src/ (.cpp et .h), défensivement hors SELF.
all_src=$(find src -type f \( -name '*.cpp' -o -name '*.h' \) | sort)
for pat in "$M1" "$M2"; do
  hits=$(printf '%s\n' "$all_src" \
         | grep -v "^${SELF}$" \
         | xargs -r grep -HnE "$pat" 2>/dev/null \
         | grep -vE "$NOCOM" || true)
  [ -n "$hits" ] && raw="${raw}${hits}
"
done

# M3A + M3B : UNIQUEMENT le chemin de journalisation lui-même.
for pat in "$M3A" "$M3B"; do
  hits=$(grep -HnE "$pat" $LOG_PATH_FILES 2>/dev/null | grep -vE "$NOCOM" || true)
  [ -n "$hits" ] && raw="${raw}${hits}
"
done

# Une même ligne peut matcher plusieurs motifs (ex. M1 et M3A sur la même
# déclaration dans logger.h) : dédupliquer sur la ligne ENTIÈRE (fichier +
# numéro + contenu), jamais sur le seul contenu (deux fichiers peuvent
# partager une ligne identique).
raw=$(printf '%s\n' "$raw" | grep -v '^$' | sort -u)

if [ "${POOL_MEM_LIST:-0}" = "1" ]; then
  # Mode audit : validation historique (HEADn) — les commentaires
  # d'exemption n'existent pas dans les commits passés, le filtrage n'aurait
  # aucun sens.
  printf '%s\n' "$raw"
  exit 0
fi

# --- Filtrage des exemptions -------------------------------------------------
# Le motif est OBLIGATOIRE : on relit la ligne du hit ET celle juste
# au-dessus, on n'accepte le marqueur que s'il est suivi d'au moins 20
# caractères NON BLANCS après le ':'. Un marqueur nu (ou suivi de trois mots)
# ne désarme rien.

is_exempt() {
  local file="$1" line="$2" content motif nonblank prev
  prev=$((line - 1))
  for l in "$line" "$prev"; do
    [ "$l" -lt 1 ] && continue
    content=$(sed -n "${l}p" "$file" 2>/dev/null) || continue
    case "$content" in
      *MEM-EXEMPT\(ADR-0082\)*)
        motif=$(printf '%s\n' "$content" | sed -E 's/.*MEM-EXEMPT\(ADR-0082\)[[:space:]]*:?[[:space:]]*//')
        # L'etiquette de suivi [feature-NNN] est OBLIGATOIRE mais ne compte PAS
        # comme justification : sans ce retrait elle apportait a elle seule 13
        # des 20 caracteres exiges, et un motif de sept lettres suffisait.
        motif=$(printf '%s\n' "$motif" | sed -E 's/\[[a-z]+-[0-9]+\]//g')
        # On retire aussi la PONCTUATION, pas seulement les blancs : « ------- »
        # (27 tirets) satisfaisait le seuil et desarmait l'exigence de motif.
        # Verifie par mutation le 19/09/2026 — les deux cas passaient.
        nonblank=$(printf '%s' "$motif" | tr -d '[:space:][:punct:]' | wc -c)
        if [ "$nonblank" -ge 20 ]; then
          return 0
        fi
        ;;
    esac
  done
  return 1
}

found=""
while IFS= read -r hit; do
  [ -z "$hit" ] && continue
  file="${hit%%:*}"
  rest="${hit#*:}"
  lineno="${rest%%:*}"
  if is_exempt "$file" "$lineno"; then
    continue
  fi
  found="${found}${hit}
"
done <<< "$raw"
found=$(printf '%s\n' "$found" | grep -v '^$' || true)

if [ -n "$found" ]; then
  {
    echo "ERREUR ADR-0082 : discipline mémoire du journal (feature-129) contournée."
    echo
    echo "Le journal a déjà détruit 23 552 octets de contiguïté du tas 8 bits avec"
    echo "168 octets alloués (deux \`String\` d'une seule ligne de journal, effondrement"
    echo "initial reproductible sur quatre démarrages) — voir"
    echo "docs/adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md."
    echo "Une \`String\` par entrée d'un tableau/conteneur, ou toute allocation"
    echo "dynamique dans logger.{h,cpp}/log_arena_logic.{h,cpp}, réintroduit"
    echo "EXACTEMENT ce risque : l'amplification, pas la fuite."
    echo
    echo "Si l'occurrence ci-dessous est légitime (hors chemin de journalisation,"
    echo "borné, purgé périodiquement) : ajouter, sur la ligne du hit ou celle"
    echo "juste au-dessus, un commentaire"
    echo "  // MEM-EXEMPT(ADR-0082) : <motif d'au moins 20 caractères> [feature-131]"
    echo "Un marqueur sans motif ne désarme rien (vérifié par ce script)."
    echo
    echo "$found"
  } >&2
  exit 1
fi

echo "OK — aucune discipline mémoire du journal (ADR-0082) contournée."
