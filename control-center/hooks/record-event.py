#!/usr/bin/env python3
"""Collecteur d'événements du centre de contrôle — un hook Claude Code.

Lit le JSON du hook sur stdin, écrit UNE ligne JSON dans le journal en ajout
seul, et sort. Rien d'autre.

⚠️ CONTRAINTES NON NÉGOCIABLES — ce script s'exécute DANS le chemin critique de
   chaque session Claude Code, y compris la session interactive de l'utilisateur.

1. **Il ne doit jamais bloquer.** Un hook lent retarde la session ; un hook qui
   sort en code 2 ANNULE l'action observée. Ce script sort donc TOUJOURS en 0,
   quoi qu'il arrive — un journal incomplet est un désagrément, une session
   cassée est une panne.

2. **Il ne doit jamais recopier de contenu volumineux.** Ni `tool_result`, ni
   diff, ni sortie de test. Les gros volumes sont référencés par chemin, jamais
   inclus (cf. le point 15 du cahier des charges).

3. **Il ne doit jamais recopier de secret.** Une commande shell peut contenir un
   mot de passe (`curl -u admin:…`, `POOL_PASSWORD=…`). On ne conserve donc que
   les DEUX PREMIERS jetons d'une commande — assez pour distinguer `pio test` de
   `git commit`, trop peu pour porter un identifiant. Voir `_command_head()`.

4. **Il ne connaît rien du projet.** Aucun nom d'agent, aucun chemin de spec,
   aucune commande de test en dur : ce fichier doit survivre au déménagement vers
   le template, où les agents porteront d'autres noms.
"""

import json
import os
import sys
import time

# Le journal vit à côté du script, jamais à un chemin absolu codé en dur : le
# répertoire `control-center/` doit pouvoir être déplacé d'un bloc.
_HOOKS_DIR = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HOOKS_DIR)
_JOURNAL = os.path.join(_ROOT, "var", "agent-events.jsonl")

# Champs communs à tous les hooks, repris tels quels. `agent_id`/`agent_type`
# ne sont présents que pour les sous-agents : leur ABSENCE identifie l'agent
# principal, elle n'est donc jamais remplacée par une valeur par défaut.
_PASSTHROUGH = (
    "session_id",
    "hook_event_name",
    "agent_id",
    "agent_type",
    "cwd",
    "transcript_path",
    "permission_mode",
    "prompt_id",
)

_MAX_DESCRIPTION = 120


_MAX_TOKEN = 24


def _command_head(command):
    """Les deux premiers jetons SÛRS d'une commande, et rien de plus.

    `pio test -e native` → `pio test` ; `git commit -F …` → `git commit`.

    ⚠️ Tout jeton contenant `=` ou `:` est ÉCARTÉ, jamais tronqué. Deux idiomes
    du shell portent couramment un secret dans les tout premiers jetons :

        POOL_PASSWORD=secret ./ota_update.sh firmware     ← affectation en tête
        curl -u admin:secret http://…                     ← identifiant inline

    Le premier est documenté par `ota_update.sh` lui-même comme un usage
    normal. Une règle « les deux premiers jetons » l'aurait recopié tel quel
    dans le journal — défaut constaté sur le PREMIER événement réel produit par
    ce collecteur, avant toute mise en service.

    Écarter plutôt que tronquer : `POOL_PASSWORD=sec` reste un préfixe de
    secret. Un jeton suspect disparaît, on passe au suivant.
    """
    if not isinstance(command, str):
        return None
    safe = []
    for token in command.split():
        if "=" in token or ":" in token:
            continue
        if len(token) > _MAX_TOKEN:
            # Un jeton anormalement long est un chemin ou une donnée, pas un nom
            # de programme : il n'apporte rien et peut porter n'importe quoi.
            continue
        safe.append(token)
        if len(safe) == 2:
            break
    return " ".join(safe) if safe else None


def _relative_path(path, cwd):
    """Chemin relatif au dépôt, ou None s'il en sort.

    Un chemin hors du répertoire de travail n'a rien à faire dans le journal :
    il désignerait un fichier que l'interface n'a pas le droit d'afficher.
    """
    if not isinstance(path, str) or not isinstance(cwd, str):
        return None
    try:
        absolute = os.path.realpath(path)
        base = os.path.realpath(cwd)
        if absolute == base or absolute.startswith(base + os.sep):
            return os.path.relpath(absolute, base)
    except (OSError, ValueError):
        pass
    return None


def _tool_details(payload):
    """Ce qu'on retient d'un appel d'outil : de quoi identifier, pas de quoi rejouer."""
    details = {}
    tool_name = payload.get("tool_name")
    if tool_name:
        details["tool"] = tool_name

    tool_input = payload.get("tool_input")
    if not isinstance(tool_input, dict):
        return details

    head = _command_head(tool_input.get("command"))
    if head:
        details["command_head"] = head

    description = tool_input.get("description")
    if isinstance(description, str) and description:
        details["description"] = description[:_MAX_DESCRIPTION]

    relative = _relative_path(tool_input.get("file_path"), payload.get("cwd"))
    if relative:
        details["file"] = relative

    return details


def main():
    try:
        raw = sys.stdin.read()
    except Exception:
        return
    if not raw.strip():
        return

    try:
        payload = json.loads(raw)
    except (ValueError, TypeError):
        return
    if not isinstance(payload, dict):
        return

    # Horodatage à la MILLISECONDE, et non à la seconde : plusieurs événements
    # tombent couramment dans la même seconde (un sous-agent qui s'achève et le
    # suivant qui démarre), et c'est leur ORDRE qui distingue une boucle de
    # correction d'un enchaînement normal. Une résolution à la seconde rendait
    # cet ordre indécidable — constaté au premier essai du collecteur.
    now = time.time()
    event = {
        "ts": "%s.%03dZ" % (
            time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(now)),
            int((now % 1) * 1000),
        ),
        # Le runtime est inscrit dès le premier événement : l'ajout d'OpenCode ne
        # doit pas exiger de relire un journal écrit sans lui (point 23).
        "runtime": "claude-code",
    }
    for key in _PASSTHROUGH:
        value = payload.get(key)
        if value is not None:
            event[key] = value

    details = _tool_details(payload)
    if details:
        event.update(details)

    # Un échec d'outil est la moitié manquante des boucles de correction : sans
    # lui, un retour `debugger → developer` ressemble à un enchaînement normal.
    if payload.get("hook_event_name") == "PostToolUseFailure":
        event["failed"] = True

    try:
        os.makedirs(os.path.dirname(_JOURNAL), exist_ok=True)
        # Ouverture en ajout : sur POSIX, une écriture unique sous la taille d'un
        # tampon de pipe est atomique vis-à-vis des autres écrivains en mode "a".
        # Plusieurs sessions Claude Code peuvent donc écrire dans le même journal
        # sans verrou ni entrelacement de lignes.
        with open(_JOURNAL, "a", encoding="utf-8") as handle:
            handle.write(json.dumps(event, ensure_ascii=False) + "\n")
    except OSError:
        return


if __name__ == "__main__":
    try:
        main()
    except Exception:
        # Filet de dernier recours. Aucune exception ne doit remonter : elle
        # ferait sortir le script en code non nul et polluerait la session
        # observée d'un avertissement, à chaque événement.
        pass
    # TOUJOURS 0. Voir la contrainte 1 de l'en-tête.
    sys.exit(0)
