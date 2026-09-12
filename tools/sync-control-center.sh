#!/usr/bin/env bash
#
# Récupère la dernière version du centre de contrôle depuis le template
# `agent-kit`, sans jamais toucher à ce qui appartient à CE projet.
#
#     ./tools/sync-control-center.sh              # remplace le code
#     ./tools/sync-control-center.sh --verifier   # dit ce qui changerait, sans rien écrire
#
# ── CE QUI NE SERA JAMAIS ÉCRASÉ ─────────────────────────────────────────────
#
#   control-center/control-center.toml   la configuration DE CE PROJET
#   control-center/var/                  le journal d'événements
#
# Le template n'en porte pas : il livre `control-center.example.toml`, un modèle
# à copier une fois. Ces deux chemins sont protégés explicitement plus bas, et
# un test de fin vérifie qu'ils n'ont pas bougé.
#
# ── POURQUOI HTTPS ET NON SSH ────────────────────────────────────────────────
#
# La clé SSH de ce conteneur est une clé de DÉPLOIEMENT, liée au seul dépôt
# `esp32-pool-controller` : `ssh -T git@github.com` répond
# « Hi niko34/esp32-pool-controller ». Elle ne peut donc pas lire `agent-kit`.
# On passe par HTTPS avec le jeton de `gh`, qui est un jeton de compte.
#
# ── CE QUE CE SCRIPT NE FAIT PAS ─────────────────────────────────────────────
#
# Il ne commite pas, ne pousse pas, ne lance aucun `git checkout` sur ce dépôt.
# Il remplace des fichiers ; c'est à l'humain de relire le diff et de décider.

set -euo pipefail

DEPOT_TEMPLATE="${CONTROL_CENTER_TEMPLATE:-niko34/agent-kit}"
BRANCHE="${CONTROL_CENTER_BRANCHE:-main}"
SOUS_DOSSIER="control-center"

# Chemins protégés, RELATIFS à $SOUS_DOSSIER.
PROTEGES=("control-center.toml" "var")

RACINE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CIBLE="$RACINE/$SOUS_DOSSIER"

VERIFIER=0
if [ "${1:-}" = "--verifier" ] || [ "${1:-}" = "--check" ]; then
  VERIFIER=1
fi

rouge() { printf '\033[0;31m%s\033[0m\n' "$*"; }
vert()  { printf '\033[0;32m%s\033[0m\n' "$*"; }
gris()  { printf '\033[0;90m%s\033[0m\n' "$*"; }

# ── outillage ────────────────────────────────────────────────────────────────
command -v git >/dev/null 2>&1 || { rouge "git est introuvable."; exit 1; }

JETON=""
if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
  JETON="$(gh auth token 2>/dev/null || true)"
fi

TEMPO="$(mktemp -d)" || { rouge "mktemp a échoué."; exit 1; }
nettoyer() { rm -rf "$TEMPO"; }
trap nettoyer EXIT

# ── récupération ─────────────────────────────────────────────────────────────
gris "Template : $DEPOT_TEMPLATE ($BRANCHE)"
if [ -n "$JETON" ]; then
  URL="https://x-access-token:${JETON}@github.com/${DEPOT_TEMPLATE}.git"
  AFFICHE="https://github.com/${DEPOT_TEMPLATE}.git (jeton gh)"
else
  # Sans `gh`, on laisse git demander ses identifiants comme d'habitude.
  URL="https://github.com/${DEPOT_TEMPLATE}.git"
  AFFICHE="$URL (identifiants git)"
  gris "gh indisponible — git demandera les identifiants."
fi

# `--depth 1` : on veut l'état courant, pas l'histoire du template.
if ! git clone --quiet --depth 1 --branch "$BRANCHE" "$URL" "$TEMPO/src" 2>"$TEMPO/err"; then
  rouge "Clonage impossible depuis $AFFICHE"
  # On n'affiche jamais l'URL brute : elle contient le jeton.
  sed "s|$JETON|<JETON>|g" "$TEMPO/err" >&2 || true
  exit 1
fi

SOURCE="$TEMPO/src/$SOUS_DOSSIER"
[ -d "$SOURCE" ] || { rouge "Le template ne contient pas de dossier $SOUS_DOSSIER/."; exit 1; }

# Garde-fou : on refuse de remplacer quoi que ce soit si la source ne ressemble
# pas à un centre de contrôle. Une branche vide ou un dépôt réorganisé ne doit
# pas effacer une installation qui fonctionne.
for exige in "backend/server.py" "hooks/record-event.py" "web/index.html"; do
  [ -f "$SOURCE/$exige" ] || { rouge "Source incomplète : $exige manquant. Rien n'a été touché."; exit 1; }
done

COMMIT="$(git -C "$TEMPO/src" rev-parse --short HEAD)"
DATE_COMMIT="$(git -C "$TEMPO/src" log -1 --format=%cI)"
SUJET="$(git -C "$TEMPO/src" log -1 --format=%s)"
gris "Version   : $COMMIT ($DATE_COMMIT)"
gris "            $SUJET"

# ── comparaison ──────────────────────────────────────────────────────────────
# On liste les fichiers du template, en écartant les chemins protégés.
liste_source() {
  ( cd "$SOURCE" && find . -type f -not -path './var/*' -not -name 'control-center.toml' \
      -not -path '*/__pycache__/*' | sed 's|^\./||' | sort )
}

NOUVEAUX=0; MODIFIES=0; IDENTIQUES=0
CHANGEMENTS="$TEMPO/changements"
: > "$CHANGEMENTS"
while IFS= read -r fichier; do
  if [ ! -f "$CIBLE/$fichier" ]; then
    NOUVEAUX=$((NOUVEAUX + 1)); echo "  + $fichier" >> "$CHANGEMENTS"
  elif ! cmp -s "$SOURCE/$fichier" "$CIBLE/$fichier"; then
    MODIFIES=$((MODIFIES + 1)); echo "  ~ $fichier" >> "$CHANGEMENTS"
  else
    IDENTIQUES=$((IDENTIQUES + 1))
  fi
done < <(liste_source)

# Fichiers présents ici mais absents du template : SIGNALÉS, jamais supprimés.
# Ce sont peut-être des ajouts locaux, et les effacer serait une perte silencieuse.
ORPHELINS="$TEMPO/orphelins"
: > "$ORPHELINS"
if [ -d "$CIBLE" ]; then
  while IFS= read -r fichier; do
    [ -f "$SOURCE/$fichier" ] || echo "  ? $fichier" >> "$ORPHELINS"
  done < <( cd "$CIBLE" && find . -type f -not -path './var/*' -not -name 'control-center.toml' \
              -not -path '*/__pycache__/*' -not -name '.source' | sed 's|^\./||' | sort )
fi

echo
if [ -s "$CHANGEMENTS" ]; then cat "$CHANGEMENTS"; fi
if [ -s "$ORPHELINS" ]; then
  echo
  gris "Présents ici, absents du template — CONSERVÉS, jamais supprimés :"
  cat "$ORPHELINS"
fi
echo
gris "$NOUVEAUX nouveau(x) · $MODIFIES modifié(s) · $IDENTIQUES identique(s)"

if [ "$VERIFIER" -eq 1 ]; then
  echo
  if [ $((NOUVEAUX + MODIFIES)) -eq 0 ]; then
    vert "À jour. Rien à faire."
  else
    gris "Vérification seule : aucun fichier n'a été écrit."
  fi
  exit 0
fi

if [ $((NOUVEAUX + MODIFIES)) -eq 0 ]; then
  vert "Déjà à jour ($COMMIT)."
  exit 0
fi

# ── remplacement ─────────────────────────────────────────────────────────────
# Empreintes AVANT, pour prouver ensuite que les chemins protégés n'ont pas bougé.
empreinte_protegee() {
  for p in "${PROTEGES[@]}"; do
    if [ -e "$CIBLE/$p" ]; then
      find "$CIBLE/$p" -type f -exec cksum {} \; 2>/dev/null | sort
    fi
  done
}
AVANT="$(empreinte_protegee)"

mkdir -p "$CIBLE"
while IFS= read -r fichier; do
  mkdir -p "$CIBLE/$(dirname "$fichier")"
  cp "$SOURCE/$fichier" "$CIBLE/$fichier"
done < <(liste_source)

# Trace de provenance : sans elle, impossible de savoir quelle version tourne.
cat > "$CIBLE/.source" <<FIN
# Provenance du centre de contrôle — écrit par tools/sync-control-center.sh.
# Ne pas modifier à la main : la prochaine synchronisation l'écrase.
depot   = $DEPOT_TEMPLATE
branche = $BRANCHE
commit  = $COMMIT
date    = $DATE_COMMIT
sujet   = $SUJET
recupere_le = $(date -u +%Y-%m-%dT%H:%M:%SZ)
FIN

APRES="$(empreinte_protegee)"
if [ "$AVANT" != "$APRES" ]; then
  rouge "ANOMALIE : un chemin protégé a changé pendant la synchronisation."
  rouge "Vérifiez control-center/control-center.toml et control-center/var/."
  exit 1
fi

echo
vert "Centre de contrôle mis à jour depuis $DEPOT_TEMPLATE ($COMMIT)."
gris "Configuration et journal de ce projet : intacts (vérifié)."
echo
gris "Étapes suivantes :"
gris "  git diff --stat control-center/     relire ce qui a changé"
gris "  python3 control-center/backend/test_server.py   éprouver avant de committer"
