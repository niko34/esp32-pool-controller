#!/usr/bin/env python3
"""Reconstruction des workflows depuis le journal d'événements.

Ce module répond à la question « que s'est-il RÉELLEMENT passé ? », par
opposition au workflow théorique que décrit `CLAUDE.md`. Il ne lit que le
journal produit par `hooks/record-event.py` ; il n'interroge pas Claude Code, ne
lance rien, et ne connaît aucun nom d'agent.

## Pourquoi c'est un tri, et non un arbre

Les hooks `SubagentStart`/`SubagentStop` portent `agent_id`, `agent_type` et
`session_id`, mais **aucun identifiant d'appel parent**. Ce serait bloquant s'il
y avait de l'imbrication.

Or aucun agent de ce dépôt n'a l'outil `Task` : aucun ne peut déléguer. Un
workflow est donc une session et une suite de sous-agents partageant un
`session_id`, et le reconstruire revient à trier par horodatage.

⚠️ Si un jour un agent reçoit `Task`, cette hypothèse tombe et l'imbrication
devient invisible ici — les passages d'agent s'afficheraient à plat, dans le
désordre apparent. Le module le SIGNALE (`imbrication_suspectee`) plutôt que de
produire un récit faux : deux ouvertures sans fermeture intercalée en sont la
trace observable.

## Ce qu'on n'invente pas

Un passage d'agent sans fermeture n'est pas « terminé » : il est **en cours**, ou
sa fermeture a été perdue. On ne lui fabrique ni durée ni verdict. De même, une
session sans `SessionEnd` n'est pas « échouée » — elle tourne peut-être encore,
et c'est le cas le plus fréquent puisque la session interactive de l'utilisateur
ne se termine pas.
"""

import json
import os

# Événements structurants. Tout le reste est rattaché au passage d'agent courant.
DEBUT_SESSION = "SessionStart"
FIN_SESSION = "SessionEnd"
DEBUT_AGENT = "SubagentStart"
FIN_AGENT = "SubagentStop"
ECHEC_OUTIL = "PostToolUseFailure"

# Nom de l'acteur quand aucun `agent_id` n'est présent. L'ABSENCE d'agent_id
# identifie l'agent principal — elle n'est jamais comblée par une valeur du
# domaine, mais il faut bien l'afficher (ADR-0072 : la sentinelle est explicite).
AGENT_PRINCIPAL = "(agent principal)"


def lire_journal(chemin):
    """Lit le journal. Une ligne illisible est IGNORÉE, jamais fatale.

    Le journal est écrit en ajout par plusieurs sessions ; une ligne tronquée
    par un arrêt brutal ne doit pas rendre tout l'historique inexploitable.
    """
    evenements = []
    if not os.path.exists(chemin):
        return evenements
    try:
        with open(chemin, encoding="utf-8") as fichier:
            for ligne in fichier:
                ligne = ligne.strip()
                if not ligne:
                    continue
                try:
                    evenement = json.loads(ligne)
                except ValueError:
                    continue
                if isinstance(evenement, dict) and evenement.get("ts"):
                    evenements.append(evenement)
    except OSError:
        return evenements
    # Les horodatages sont ISO 8601 à la milliseconde, en UTC, de longueur
    # constante : l'ordre lexicographique EST l'ordre chronologique.
    evenements.sort(key=lambda e: e["ts"])
    return evenements


def _duree_ms(debut, fin):
    """Durée entre deux horodatages ISO, ou None si l'un manque.

    None signifie « pas mesurable », jamais 0 : un passage d'agent sans
    fermeture n'a pas duré zéro milliseconde, on ignore sa durée.
    """
    if not debut or not fin:
        return None
    import datetime
    try:
        d = datetime.datetime.strptime(debut, "%Y-%m-%dT%H:%M:%S.%fZ")
        f = datetime.datetime.strptime(fin, "%Y-%m-%dT%H:%M:%S.%fZ")
    except ValueError:
        return None
    millisecondes = int((f - d).total_seconds() * 1000)
    # Une durée négative signifie que l'horloge a reculé entre les deux
    # événements. Ce n'est pas une durée : c'est une mesure invalide, et on rend
    # None plutôt qu'un nombre qui s'afficherait comme un fait.
    return millisecondes if millisecondes >= 0 else None


def reconstruire(evenements, commandes_suivies=()):
    """Groupe les événements en workflows, un par `session_id`.

    `commandes_suivies` est la liste FERMÉE des débuts de commande à compter à
    part (« pio test », « pio run »…). Elle vient de la configuration du projet :
    ce module ne connaît aucune commande.

    ⚠️ CONTRAT D'ORDRE : cette fonction NE TRIE PAS. Elle consomme les événements
    dans l'ordre reçu, et c'est `lire_journal()` qui garantit l'ordre
    chronologique. Un appelant qui passe une liste non triée obtient un récit
    dans l'ordre qu'il a fourni — ce qui est voulu (on peut vouloir rejouer une
    capture telle quelle), mais qui doit être su.

    C'est la raison d'être du garde-fou sur les durées négatives : une fermeture
    antérieure à son ouverture est alors possible, et `_duree_ms()` rend `None`
    plutôt qu'un nombre négatif qui s'afficherait comme un fait.
    """
    suivies = set(commandes_suivies)
    workflows = {}

    for evenement in evenements:
        session = evenement.get("session_id")
        if not session:
            continue
        workflow = workflows.setdefault(session, {
            "session_id": session,
            "debut": evenement["ts"],
            "fin": None,
            "cwd": evenement.get("cwd"),
            "runtime": evenement.get("runtime", "inconnu"),
            "passages": [],
            "fichiers": [],
            "en_cours": None,
            "echecs": 0,
            "commandes": {},
            "imbrication_suspectee": False,
            "fermetures_orphelines": 0,
        })
        workflow["fin_observee"] = evenement["ts"]
        if evenement.get("cwd") and not workflow["cwd"]:
            workflow["cwd"] = evenement["cwd"]

        nom = evenement.get("hook_event_name")

        if nom == DEBUT_AGENT:
            if workflow["en_cours"] is not None:
                # Deux ouvertures sans fermeture intercalée : soit une fermeture
                # a été perdue, soit un agent en a invoqué un autre. Dans les
                # deux cas le récit devient incertain, et on le DIT.
                workflow["imbrication_suspectee"] = True
            passage = {
                "agent": evenement.get("agent_type") or AGENT_PRINCIPAL,
                "agent_id": evenement.get("agent_id"),
                "debut": evenement["ts"],
                "fin": None,
                "duree_ms": None,
                "echecs": 0,
                "fichiers": [],
                "commandes": [],
                "termine": False,
            }
            workflow["passages"].append(passage)
            workflow["en_cours"] = passage
            continue

        if nom == FIN_AGENT:
            # ⚠️ OBSERVÉ SUR LE TERRAIN, ET NON DOCUMENTÉ : `SubagentStop` arrive
            # parfois SANS `SubagentStart` correspondant. Relevé le 2026-09-12 :
            # chaque `Stop` de l'agent principal (fin de tour) est suivi ~2 s plus
            # tard d'un `SubagentStop` portant un `agent_id` neuf et un
            # `agent_type` VIDE. Ce ne sont pas des sous-agents du workflow.
            #
            # Deuxième surprise du même relevé : `SubagentStop` ne porte PAS le
            # nom de l'agent (`agent_type` est vide) là où `SubagentStart` le
            # porte. L'appariement DOIT donc se faire par `agent_id`, jamais par
            # nom — et jamais « le dernier ouvert », qui attribuerait la
            # fermeture d'un intrus au vrai passage en cours.
            identifiant = evenement.get("agent_id")
            passage = None
            if identifiant:
                for candidat in reversed(workflow["passages"]):
                    if candidat["agent_id"] == identifiant and not candidat["termine"]:
                        passage = candidat
                        break
            if passage is None:
                # On la COMPTE plutôt que de l'avaler : un événement inexpliqué
                # passé sous silence deviendrait un écart invisible entre le
                # journal et le récit qu'on en tire.
                workflow["fermetures_orphelines"] += 1
                continue
            passage["fin"] = evenement["ts"]
            passage["duree_ms"] = _duree_ms(passage["debut"], passage["fin"])
            passage["termine"] = True
            if workflow["en_cours"] is passage:
                workflow["en_cours"] = None
            continue

        if nom == FIN_SESSION:
            workflow["fin"] = evenement["ts"]
            continue

        # --- événements rattachés au passage courant -------------------------
        # ⚠️ Hors passage, l'événement appartient à l'AGENT PRINCIPAL, pas à
        # personne. Les rattacher au seul passage courant faisait disparaître
        # tout son travail : sur le journal réel, 33 appels d'outils ne
        # rapportaient qu'un seul fichier. Défaut relevé en revue.
        passage = workflow["en_cours"]

        if nom == ECHEC_OUTIL:
            workflow["echecs"] += 1
            if passage is not None:
                passage["echecs"] += 1

        fichier = evenement.get("file")
        if fichier:
            if fichier not in workflow["fichiers"]:
                workflow["fichiers"].append(fichier)
            if passage is not None and fichier not in passage["fichiers"]:
                passage["fichiers"].append(fichier)

        tete = evenement.get("command_head")
        if tete and tete in suivies:
            workflow["commandes"][tete] = workflow["commandes"].get(tete, 0) + 1
            if passage is not None:
                passage["commandes"].append(tete)

    for workflow in workflows.values():
        workflow["reprises"] = _reprises(workflow["passages"])
        # ⚠️ « sans fin observée » et NON « actif ». Une session abandonnée n'émet
        # pas de `SessionEnd` : la déclarer active la laisserait éternellement en
        # cours. Le module promet en en-tête de ne pas trancher — il ne tranche
        # donc pas, il rapporte ce qu'il a vu et donne la date de la dernière
        # activité pour que l'appelant décide.
        workflow["fin_observee_absente"] = workflow["fin"] is None
        del workflow["en_cours"]

    return sorted(workflows.values(), key=lambda w: w["debut"])


def _reprises(passages):
    """Agents appelés PLUSIEURS fois — la trace observable d'une boucle.

    C'est ce qui distingue un cycle nominal d'un cycle qui a rebouclé :
    `test-engineer` appelé deux fois avec un `debugger` entre les deux, c'est une
    correction. On rapporte le COMPTE, on ne qualifie pas la cause.
    """
    compte = {}
    for passage in passages:
        compte[passage["agent"]] = compte.get(passage["agent"], 0) + 1
    return {agent: n for agent, n in compte.items() if n > 1}


if __name__ == "__main__":
    import sys

    ici = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    chemin = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ici, "var", "agent-events.jsonl")
    evenements = lire_journal(chemin)
    print("%d événement(s) dans %s\n" % (len(evenements), chemin))
    for workflow in reconstruire(evenements, ("pio test", "pio run")):
        marque = "● sans fin observée" if workflow["fin_observee_absente"] else "✓ clos"
        print("%s  session %s  (%s)" % (marque, workflow["session_id"][:8], workflow["runtime"]))
        print("   début %s · dernière activité %s · %d fichier(s) au total"
              % (workflow["debut"], workflow["fin_observee"], len(workflow["fichiers"])))
        if workflow["imbrication_suspectee"]:
            print("   ⚠️  imbrication suspectée — le récit peut être incomplet")
        if workflow["fermetures_orphelines"]:
            print("   %d fermeture(s) sans ouverture — attendu : le runtime en émet "
                  "une par fin de tour" % workflow["fermetures_orphelines"])
        for passage in workflow["passages"]:
            # `is not None` et non la vérité de la valeur : une durée de 0 ms
            # est une durée, pas une absence de mesure.
            duree = ("%.1f s" % (passage["duree_ms"] / 1000.0)
                     if passage["duree_ms"] is not None
                     else ("en cours" if not passage["termine"] else "durée invalide"))
            print("     %-22s %-10s %d fichier(s)  %d échec(s)"
                  % (passage["agent"], duree, len(passage["fichiers"]), passage["echecs"]))
        if workflow["reprises"]:
            print("   reprises : %s" % workflow["reprises"])
        if workflow["commandes"]:
            print("   commandes suivies : %s" % workflow["commandes"])
        print()
