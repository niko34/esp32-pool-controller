#!/usr/bin/env python3
"""Découverte des agents — le répertoire fait foi, jamais une liste écrite.

Les agents sont des fichiers Markdown à frontmatter YAML. On ne lit QUE le
frontmatter : le corps est le prompt système de l'agent, il peut faire des
centaines de lignes et n'a rien à faire dans une réponse HTTP.

⚠️ On n'analyse pas du YAML général. Le frontmatter d'un agent tient en quelques
paires `clé: valeur` sur une ligne, et c'est tout ce qu'on lit. Importer un
analyseur YAML pour cela ajouterait une dépendance à porter dans chaque projet
issu du template, pour gagner des cas qui n'existent pas ici.

Une clé inconnue est CONSERVÉE telle quelle : un champ neuf apparu dans une
version future de Claude Code doit remonter à l'interface, pas disparaître parce
que ce module ne le connaissait pas.
"""

import os

# Un frontmatter d'agent dépasse rarement dix lignes. Au-delà, on cesse de lire :
# le corps du fichier est un prompt, parfois très long.
_MAX_LIGNES_FRONTMATTER = 60


def _decouper_liste(valeur):
    """« Read, Grep, Glob » → ['Read', 'Grep', 'Glob']. Gère aussi la forme [a, b]."""
    valeur = valeur.strip()
    if valeur.startswith("[") and valeur.endswith("]"):
        valeur = valeur[1:-1]
    elements = [element.strip().strip("'\"") for element in valeur.split(",")]
    return [element for element in elements if element]


def lire_agent(chemin):
    """Frontmatter d'un fichier d'agent. Ne lève jamais."""
    identifiant = os.path.splitext(os.path.basename(chemin))[0]
    agent = {"id": identifiant, "nom": identifiant, "modele": None,
             "outils": [], "description": "", "peut_ecrire": False,
             "peut_deleguer": False, "illisible": False}
    try:
        with open(chemin, encoding="utf-8") as fichier:
            lignes = fichier.read().splitlines()
    except (OSError, UnicodeDecodeError):
        agent["illisible"] = True
        return agent

    if not lignes or lignes[0].strip() != "---":
        # Pas de frontmatter : ce n'est pas une erreur, c'est un fichier qui n'a
        # rien à déclarer. On rend ce qu'on sait — son nom de fichier.
        return agent

    for ligne in lignes[1:_MAX_LIGNES_FRONTMATTER]:
        if ligne.strip() == "---":
            break
        if ":" not in ligne or ligne.startswith((" ", "\t", "#")):
            continue
        cle, _, valeur = ligne.partition(":")
        cle = cle.strip().lower()
        valeur = valeur.strip()
        if not cle or not valeur:
            continue
        if cle == "name":
            agent["nom"] = valeur
        elif cle == "model":
            agent["modele"] = valeur
        elif cle == "description":
            agent["description"] = valeur
        elif cle == "tools":
            agent["outils"] = _decouper_liste(valeur)
        else:
            # Clé inconnue conservée : un champ neuf doit remonter à l'interface.
            agent.setdefault("autres", {})[cle] = valeur

    outils = set(agent["outils"])
    # Deux propriétés que l'interface affiche et qui se DÉDUISENT des outils,
    # plutôt que de se redéclarer quelque part où elles pourraient mentir.
    agent["peut_ecrire"] = bool(outils & {"Write", "Edit", "NotebookEdit"})
    # ⚠️ `Task` absent ⇒ l'agent ne peut pas déléguer. C'est CE fait, et non une
    # limitation du runtime, qui rend le graphe d'exécution plat et permet de
    # reconstruire un workflow par simple tri chronologique (cf. events.py).
    agent["peut_deleguer"] = "Task" in outils
    return agent


def lister_agents(racine):
    """Tous les agents d'un répertoire, triés par nom. Répertoire absent → []."""
    if not os.path.isdir(racine):
        return []
    agents = []
    for nom in sorted(os.listdir(racine)):
        if nom.endswith(".md") and not nom.startswith("_"):
            agents.append(lire_agent(os.path.join(racine, nom)))
    return sorted(agents, key=lambda a: a["nom"])


def lister_depuis_config(config):
    racine = os.path.join(config["_racine_projet"],
                          config.section("agents").get("racine", ".claude/agents"))
    return lister_agents(racine)


if __name__ == "__main__":
    import sys

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import config as C

    for agent in lister_depuis_config(C.charger()):
        print("%-22s %-8s %-7s %s" % (
            agent["nom"], agent["modele"] or "—",
            "écrit" if agent["peut_ecrire"] else "lecture",
            ", ".join(agent["outils"]) or "—"))
