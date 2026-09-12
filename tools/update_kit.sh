#!/usr/bin/env bash
# ============================================================================
# Récupère, dans un projet, les évolutions du kit d'agents — CHIRURGICALEMENT.
#
# POURQUOI CE SCRIPT : un projet créé depuis le kit n'a AUCUN lien git avec lui.
# Ni template GitHub, ni copie manuelle ne propagent quoi que ce soit : les deux
# dépôts sont étrangers dès la seconde qui suit la création. Sans outil, une
# amélioration d'agent faite trois mois plus tard n'atteint jamais les projets.
#
# POURQUOI « CHIRURGICALEMENT » : une synchronisation aveugle écraserait
# CLAUDE.md — le fichier le plus personnalisé du projet est aussi celui que le
# kit fournit. D'où deux catégories, à ne jamais confondre :
#
#   GELÉS      — fournis par le kit, jamais modifiés sur place. Une amélioration
#                se fait DANS le kit, puis redescend ici. Ce sont eux, et eux
#                seuls, que ce script récupère.
#   PERSONNALISÉS — CLAUDE.md, skill dev-workflow, settings.json, CHANGELOG,
#                mémoire, fichiers de CI, agents métier du projet. JAMAIS touchés
#                par ce script.
#
# Le sens inverse (une leçon apprise ici qui vaut pour tous les projets) reste
# MANUEL et délibéré : on copie le fichier vers le kit et on l'inscrit à son
# CHANGELOG avec le défaut qui l'a motivée.
#
# USAGE :
#     ./tools/update_kit.sh [--dry-run]
#
# Prérequis : un distant nommé `kit` pointant sur le dépôt du kit.
#     git remote add kit https://github.com/<compte>/agent-kit.git
#
# Portable bash 3.2 (macOS) : ni `declare -A`, ni `mapfile`.
# ============================================================================
set -uo pipefail

DISTANT="kit"
BRANCHE="main"

# Chemins GELÉS, toujours récupérés. Un chemin absent du kit est ignoré sans erreur.
CHEMINS="
.claude/agents
.claude/skills/spec-workflow
.claude/skills/security-checklist
.claude/skills/db-migrations
tools/check_specs.sh
tools/check_doc_links.sh
tools/sync_copilot_agents.py
tests/test_sync_copilot_agents.py
tools/update_kit.sh
specs/features/_template.md
specs/modele.md
specs/modele_iteration.md
GUIDE.md
"

# Chemins récupérés UNIQUEMENT s'ils existent déjà dans le projet.
# Le kit fournit les deux variantes de pipeline d'hygiène ; un projet n'en garde
# qu'une. Les rapatrier sans condition réinstallerait `.github/` dans un projet
# GitLab, et inversement — en ramenant un pipeline mort que personne ne regarde.
# Leur contenu, lui, n'est jamais personnalisé : ils se contentent d'appeler
# tools/, donc on peut les écraser sans risque quand ils sont là.
CHEMINS_SI_PRESENTS="
.github/workflows/hygiene-specs.yml
.gitlab/ci/hygiene.yml
control-center
"

# ⚠️ `control-center` est CONDITIONNEL, et non gelé, pour deux raisons.
#
# 1. C'est un composant OPTIONNEL. Le mettre dans les chemins gelés
#    installerait un serveur web et une vingtaine de fichiers dans tout projet
#    qui synchronise, qu'il en veuille ou non.
# 2. Il arriverait INERTE. Ses hooks vivent dans `.claude/settings.json`, qui est
#    personnalisé et que ce script ne touche jamais : le code atterrirait sans
#    son instrumentation, et le tableau de bord afficherait un journal vide pour
#    toujours. Un outil livré cassé est pire qu'un outil absent.
#
# Un projet l'installe donc UNE FOIS — `control-center/installer.py` du kit
# l'assiste — puis reçoit ensuite toutes les évolutions par ce script.
#
# Sa configuration (`control-center.toml`) et son journal (`var/`) ne risquent
# rien : ils sont absents de l'arbre du kit, et `git checkout` ne restaure que ce
# qu'il connaît.

# Compatibilité Copilot : on l'ajoute aux projets qui ont conservé .github/
# (GitHub, ou GitLab avec Copilot dans l'IDE), sans l'imposer aux autres.
CHEMINS_GITHUB="
.github/copilot-instructions.md
"

# ── SURCHARGE PAR PROJET ─────────────────────────────────────────────────────
# Un fichier `.kit-paths` à la racine du projet REMPLACE la liste des chemins
# gelés ci-dessus. Un chemin par ligne ; `#` commente ; les lignes vides sont
# ignorées.
#
# POURQUOI CETTE SOUPAPE. Tous les projets ne descendent pas du kit. Certains le
# précèdent, et ont spécialisé leurs agents pendant des mois : leur appliquer la
# liste gelée écraserait ce travail d'un coup, sans retour possible.
#
# Le cas réel qui l'a motivée : un projet firmware dont les agents avaient
# divergé de 50 à 88 % du kit — un agent métier propre au domaine, un
# test-engineer portant toute une doctrine de validation. La synchronisation
# aveugle les aurait remplacés par les versions génériques.
#
# ⚠️ DÉCLARER UN CHEMIN ICI, C'EST L'ADOPTER. Un chemin absent de `.kit-paths`
# n'est plus jamais mis à jour depuis le kit : le projet en devient
# propriétaire. C'est le but — mais c'est définitif tant que le fichier n'est pas
# modifié.
#
# La liste INCLUSIVE est délibérée, là où une liste d'exclusions serait
# fragile : si le kit ajoute demain un chemin gelé, un projet qui a forké n'en
# veut probablement pas, et une exclusion oubliée le lui imposerait en silence.
#
# Les chemins CONDITIONNELS (fichiers de CI, control-center) continuent de
# s'appliquer : ils ne sont récupérés que s'ils existent déjà dans le projet.
#
# ⚠️ PENSER À Y METTRE `tools/update_kit.sh` LUI-MÊME. Omis, le script cesse de
# se mettre à jour et se fossilise à la version du jour de son installation —
# les corrections apportées ici n'atteindraient jamais le projet.
RACINE_PROJET="$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
if [ -f "$RACINE_PROJET/.kit-paths" ]; then
  CHEMINS="$(sed 's/#.*//' "$RACINE_PROJET/.kit-paths" | grep -v '^[[:space:]]*$' || true)"
  # ⚠️ `.kit-paths` gouverne TOUT, conditionnels compris. Sans cela, les chemins
  # conditionnels lui échappaient et s'appliquaient quand même.
  #
  # Le défaut a été trouvé au premier essai réel : un projet avait déclaré ne
  # prendre que `control-center`, et la prévisualisation annonçait le
  # remplacement de son fichier de CI — qui portait un contrôle de sécurité
  # absent du kit. Un projet qui déclare ce qu'il prend ne s'attend pas à
  # recevoir autre chose.
  CHEMINS_SI_PRESENTS=""
  CHEMINS_GITHUB=""
  echo "ℹ️  .kit-paths trouvé — il définit SEUL ce que ce projet prend du kit."
fi

SIMULATION=0
[ "${1:-}" = "--dry-run" ] && SIMULATION=1

cd "$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "❌ Ce script doit tourner dans un dépôt git." >&2
  exit 1
}

if ! git remote get-url "$DISTANT" >/dev/null 2>&1; then
  echo "❌ Aucun distant nommé « $DISTANT »." >&2
  echo "   Déclare-le une fois pour toutes :" >&2
  echo "     git remote add $DISTANT https://github.com/<compte>/agent-kit.git" >&2
  exit 1
fi

# Arbre propre EXIGÉ : ce script écrase des fichiers suivis. Sans cette garde, une
# modification en cours sur un agent disparaîtrait sans trace ni possibilité de
# comparaison. C'est à l'humain de décider quoi faire de son travail en cours.
# ⚠️ La garde ne s'applique PAS en simulation : `--dry-run` n'écrit rien, et
# refuser de PRÉVISUALISER sur un arbre sale est exactement l'inverse de ce qu'on
# veut — c'est précisément quand on a du travail en cours qu'on aimerait savoir
# ce qu'une synchronisation ferait, avant de décider quoi en faire.
#
# Constaté en installant le script dans un projet : un seul fichier modifié,
# appartenant à l'utilisateur, empêchait toute prévisualisation.
if [ "$SIMULATION" -eq 0 ] && [ -n "$(git status --porcelain)" ]; then
  echo "❌ Arbre de travail sale — ce script écrase des fichiers suivis." >&2
  echo "   Committe ou remise tes modifications d'abord :" >&2
  git status --short >&2
  echo "" >&2
  echo "   (./tools/update_kit.sh --dry-run fonctionne, lui, sur un arbre sale)" >&2
  exit 1
fi

echo "→ Récupération de $DISTANT/$BRANCHE…"
git fetch "$DISTANT" "$BRANCHE" --quiet || {
  echo "❌ Échec du fetch. Le dépôt du kit est-il joignable depuis ce réseau ?" >&2
  exit 1
}

# Ne garder que les chemins réellement présents côté kit.
presents=""
for chemin in $CHEMINS; do
  if git cat-file -e "$DISTANT/$BRANCHE:$chemin" 2>/dev/null; then
    presents="$presents $chemin"
  fi
done

# Conditionnels : présents côté kit ET déjà présents dans le projet.
for chemin in $CHEMINS_SI_PRESENTS; do
  if [ -e "$chemin" ] && git cat-file -e "$DISTANT/$BRANCHE:$chemin" 2>/dev/null; then
    presents="$presents $chemin"
  fi
done

if [ -d ".github" ]; then
  for chemin in $CHEMINS_GITHUB; do
    if git cat-file -e "$DISTANT/$BRANCHE:$chemin" 2>/dev/null; then
      presents="$presents $chemin"
    fi
  done
fi

# Dédoublonnage : `control-center` peut figurer à la fois dans `.kit-paths` et
# dans les chemins conditionnels. `git checkout` s'en accommoderait, mais le
# récapitulatif afficherait deux fois la même chose.
presents="$(echo $presents | tr ' ' '\n' | grep . | sort -u | tr '\n' ' ')"

if [ -z "$presents" ]; then
  echo "❌ Aucun des chemins gelés n'existe dans $DISTANT/$BRANCHE." >&2
  echo "   Le distant « $DISTANT » pointe-t-il bien sur le kit ?" >&2
  exit 1
fi

# L'aperçu ne montre QUE ce qui sera réellement écrit, c'est-à-dire les fichiers
# présents DANS LE KIT. Un `git diff` sur les chemins entiers listait aussi les
# fichiers propres au projet — la configuration du centre de contrôle y
# apparaissait comme sur le point d'être modifiée, alors que `git checkout` ne
# peut pas la toucher : elle n'existe pas dans l'arbre du kit.
#
# Un aperçu auquel on ne peut pas se fier ne sert à rien : c'est sur lui qu'on
# décide de lancer la synchronisation ou non.
# shellcheck disable=SC2086
fichiers_du_kit="$(git ls-tree -r --name-only "$DISTANT/$BRANCHE" -- $presents 2>/dev/null)"

echo "→ Écarts sur les fichiers gelés :"
echo ""
# shellcheck disable=SC2086
if [ -n "$fichiers_du_kit" ] && git diff --stat "$DISTANT/$BRANCHE" -- $fichiers_du_kit | grep -q .; then
  # shellcheck disable=SC2086
  git diff --stat "$DISTANT/$BRANCHE" -- $fichiers_du_kit
else
  echo "   ✅ Rien à récupérer : le projet est déjà aligné sur le kit."
  exit 0
fi
echo ""

if [ "$SIMULATION" -eq 1 ]; then
  echo "ℹ️  --dry-run : aucune modification appliquée."
  echo "   Relance sans --dry-run pour récupérer ces fichiers."
  exit 0
fi

# shellcheck disable=SC2086
git checkout "$DISTANT/$BRANCHE" -- $presents || {
  echo "❌ Échec de la récupération." >&2
  exit 1
}

# `git checkout` n'EFFACE jamais : un fichier retiré du kit survit indéfiniment
# dans les projets, et personne ne s'en aperçoit. On le SIGNALE, sans jamais le
# supprimer — le script ne peut pas distinguer un fichier retiré du kit d'un
# ajout local délibéré, et effacer le second serait une perte silencieuse.
orphelins=""
for chemin in $presents; do
  # Seuls les répertoires sont concernés : un fichier listé nommément et absent
  # du kit n'aurait pas été retenu plus haut.
  [ -d "$chemin" ] || continue
  au_kit="$(git ls-tree -r --name-only "$DISTANT/$BRANCHE" -- "$chemin" 2>/dev/null | sort)"
  ici="$(git ls-files -- "$chemin" | sort)"
  manquants="$(comm -23 <(echo "$ici") <(echo "$au_kit"))"
  [ -n "$manquants" ] && orphelins="$orphelins$manquants
"
done

if [ -n "$(echo "$orphelins" | tr -d '[:space:]')" ]; then
  echo ""
  echo "ℹ️  Présents ici, absents du kit — CONSERVÉS, jamais supprimés :"
  echo "$orphelins" | grep . | sed 's/^/     /'
  echo "     (soit des ajouts propres à ce projet, soit des fichiers retirés du"
  echo "      kit depuis la dernière synchronisation — à toi de trancher)"
fi

# Les profils Copilot ne sont pas copiés depuis le kit : ils sont régénérés à
# partir des agents Claude réellement présents dans le projet. Ainsi, les agents
# métier propres au projet restent inclus et un profil Copilot manuel n'est
# jamais écrasé silencieusement par un répertoire venu du kit.
# ⚠️ La condition porte sur le SCRIPT, pas seulement sur `.github/`. Un projet
# peut avoir un `.github/` — pour sa CI — sans consommer la passerelle Copilot :
# c'est le cas dès qu'un `.kit-paths` exclut `tools/sync_copilot_agents.py`.
#
# Constaté au premier lancement dans un projet réel : la synchronisation copiait
# correctement ses fichiers, puis échouait en fin de course sur un script absent,
# laissant croire à un échec global alors que tout était écrit.
if [ -d ".github" ] && [ -f "tools/sync_copilot_agents.py" ]; then
  python3 tools/sync_copilot_agents.py --write || {
    echo "❌ Échec de la régénération des profils GitHub Copilot." >&2
    exit 1
  }
  git add .github/agents
fi

echo "✅ Fichiers gelés mis à jour et indexés."
echo ""
echo "À FAIRE MAINTENANT :"
echo "  1. Relire le diff — un agent peut avoir changé de comportement :"
echo "       git diff --cached"
echo "  2. Vérifier que les agents MÉTIER du projet n'ont pas été écrasés"
echo "     (ils ne sont pas dans le kit, donc ils ne le sont pas — mais vérifie)"
echo "  3. Relancer les contrôles :"
echo "       ./tools/check_specs.sh && ./tools/check_doc_links.sh"
if [ -d ".github/agents" ] && [ -f "tools/sync_copilot_agents.py" ]; then
  echo "       python3 tools/sync_copilot_agents.py --check"
fi
echo "  4. Committer en citant la version du kit récupérée :"
echo "       git log -1 --format='%h %s' $DISTANT/$BRANCHE"
echo ""
echo "NON RÉCUPÉRÉ, à regarder à la main si le CHANGELOG du kit le signale :"
echo "  .claude/commands/release.md   (personnalisé : build et artefacts du projet)"
echo "  .claude/settings.json         (personnalisé : commandes du projet)"
echo "  CLAUDE.md, skill dev-workflow (personnalisés par nature)"
