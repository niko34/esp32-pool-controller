#!/usr/bin/env python3
"""Lecture des spécifications — le dépôt est la source de vérité.

Ce module ne fait que LIRE. Il ne déplace aucune spec, n'en écrit aucune, et ne
corrige rien : le cycle de vie appartient aux agents et à l'humain.

## Le parti pris central

**La colonne d'une spec vient de son RÉPERTOIRE, jamais de son champ « Statut ».**

Ce n'est pas de la facilité. Mesuré sur les 167 specs de ce dépôt :

- le champ « Statut » existe dans 165 fichiers, mais en **texte libre** — plus de
  vingt formes distinctes (`done`, `livré`, `clôturée`, `GELÉE`, `doing —
  itération 6 en préparation`…), parfois étalées sur plusieurs lignes ;
- il **contredit le répertoire 23 fois**, toutes dans `done/` ;
- trois formats de métadonnées coexistent (liste à puces, tableau Markdown, et un
  format « minimal » historique sans en-tête `## Métadonnées`).

`tools/check_specs.sh` avait déjà tranché la question, en toutes lettres :
« La cohérence du champ Statut avec le dossier a été écartée — ce champ est en
texte libre dans le dépôt et son analyse produirait des faux positifs. »

Le répertoire, lui, est le **seul invariant vérifié mécaniquement** : un nom de
spec ne vit que dans un dossier, et la CI le refuse autrement.

## Ce qu'on fait quand même du champ « Statut »

On l'affiche **tel quel**. Et on tente une classification GROSSIÈRE dans le seul
but de signaler les divergences — jamais pour ranger une carte. Quand le premier
mot n'appartient pas au vocabulaire connu, la classe est `INDETERMINE` et aucune
divergence n'est affirmée : une lecture incertaine ne doit pas produire une
alerte fausse (ADR-0072 du dépôt — l'absence n'est jamais une valeur du domaine).
"""

import os
import re

# --- classification grossière, au seul usage de la détection de divergence ----

A_FAIRE = "a_faire"
EN_COURS = "en_cours"
CLOS = "clos"
INDETERMINE = "indetermine"

# Premier mot du champ « Statut », normalisé, → classe.
# Volontairement COURT : tout ce qui n'est pas ici devient INDETERMINE, ce qui
# est un aveu d'ignorance, pas une erreur. Élargir cette table est un choix
# éditorial qui doit rester visible.
_VOCABULAIRE = {
    "todo": A_FAIRE,
    "draft": A_FAIRE,
    "doing": EN_COURS,
    "wip": EN_COURS,
    "done": CLOS,
    "livre": CLOS,
    "livree": CLOS,
    "cloturee": CLOS,
    "cloture": CLOS,
    "termine": CLOS,
    "terminee": CLOS,
    "implemente": CLOS,
    "implementee": CLOS,
    "abandonne": CLOS,
    "abandonnee": CLOS,
    # Formes en DEUX mots. Sans elles, `**en cours** — phase 1 livrée…` tombait
    # en INDETERMINE et sa divergence passait inaperçue (constaté en confrontant
    # ce module aux 167 specs réelles : 21 divergences trouvées contre 22).
    "encours": EN_COURS,
    "enattente": A_FAIRE,
    # ⚠️ CHOIX ÉDITORIAL, et non une lecture évidente. `done/` archive aussi les
    # décisions de NE PAS FAIRE : `feature-123` y porte `GELÉE (2026-09-09) — pas
    # abandonnée`. La ranger en CLOS supprime une divergence qu'un décompte
    # littéral signalerait. C'est délibéré — une spec gelée EST de la matière
    # close — mais c'est une convention, pas un fait. Retirer ces deux lignes
    # fait réapparaître la divergence, si l'on préfère la voir.
    "gelee": CLOS,
    "gele": CLOS,
}

_ACCENTS = str.maketrans("àâäéèêëîïôöùûüç", "aaaeeeeiioouuuc")


def _normaliser(mot):
    """Minuscules, sans accent, sans ponctuation ni gras Markdown."""
    mot = mot.strip().lower().translate(_ACCENTS)
    return re.sub(r"[^a-z0-9]", "", mot)


def classifier_statut(valeur):
    """Classe grossière d'un champ « Statut » libre, ou INDETERMINE.

    On ne lit QUE le premier mot : le reste est une justification en prose
    (`doing — itération 6 en préparation`, `livré non déployé (v2.36.0 …)`), et
    l'interpréter reviendrait à parser du français.
    """
    if not valeur:
        return INDETERMINE
    mots = valeur.strip().split()
    if not mots:
        return INDETERMINE
    # Les DEUX premiers mots d'abord (`en cours`, `en attente`), le premier
    # ensuite. Sans cet essai, toute forme en deux mots devenait INDETERMINE.
    if len(mots) >= 2:
        classe = _VOCABULAIRE.get(_normaliser(mots[0]) + _normaliser(mots[1]))
        if classe:
            return classe
    return _VOCABULAIRE.get(_normaliser(mots[0]), INDETERMINE)


# --- extraction des métadonnées, trois formats -------------------------------

# (a) format template : `- **ID** : valeur`
_PUCE = re.compile(r"^\s*[-*]\s+\*\*(?P<champ>[^*]+?)\*\*\s*:\s*(?P<valeur>.*)$")
# (c) format minimal historique : `- **Statut** : valeur` — même forme que (a),
#     mais sans en-tête `## Métadonnées`. Rien à distinguer à la lecture.
# (b) format tableau : `| Statut | valeur |`
_LIGNE_TABLEAU = re.compile(r"^\s*\|\s*(?P<champ>[^|]+?)\s*\|\s*(?P<valeur>.*?)\s*\|\s*$")
_SEPARATEUR_TABLEAU = re.compile(r"^\s*\|[\s:|-]+\|\s*$")

_TITRE_H1 = re.compile(r"^#\s+(?P<titre>.+?)\s*$")

# Au-delà de cette ligne, on cesse de chercher des métadonnées : elles vivent en
# tête de fichier, et certaines specs redéfinissent un second champ « Statut »
# dans leurs notes d'implémentation (constaté sur `bug-ha-eclairage-select`),
# qui contredit partiellement le premier. Le premier fait foi.
_LIGNES_ENTETE = 60


def _extraire_champs(lignes):
    """Champs de tête, quel que soit le format. Le PREMIER gagne."""
    champs = {}
    for ligne in lignes[:_LIGNES_ENTETE]:
        if _SEPARATEUR_TABLEAU.match(ligne):
            continue
        correspondance = _PUCE.match(ligne) or _LIGNE_TABLEAU.match(ligne)
        if not correspondance:
            continue
        champ = _normaliser(correspondance.group("champ"))
        valeur = correspondance.group("valeur").strip()
        if champ and valeur and champ not in champs:
            champs[champ] = valeur
    return champs


def _titre(lignes, identifiant):
    for ligne in lignes[:5]:
        correspondance = _TITRE_H1.match(ligne)
        if correspondance:
            return correspondance.group("titre")
    # 78 specs sur 167 n'ont pas de titre H1 : elles commencent directement par
    # `## Métadonnées`. On ne fabrique pas de titre — on rend l'identifiant, qui
    # est vrai, plutôt qu'une invention plausible.
    return identifiant


def lire_spec(chemin, colonne):
    """Lit une spec. Ne lève jamais : un fichier illisible est rendu tel quel."""
    identifiant = os.path.splitext(os.path.basename(chemin))[0]
    try:
        with open(chemin, encoding="utf-8") as fichier:
            lignes = fichier.read().splitlines()
    except (OSError, UnicodeDecodeError):
        return {
            "id": identifiant, "titre": identifiant, "colonne": colonne,
            "statut_texte": None, "statut_classe": INDETERMINE,
            "priorite": None, "type": None, "maj": None,
            "divergence": False, "illisible": True,
        }

    champs = _extraire_champs(lignes)
    statut = champs.get("statut")
    classe = classifier_statut(statut)

    return {
        "id": identifiant,
        "titre": _titre(lignes, identifiant),
        "colonne": colonne,
        # Affiché TEL QUEL. Jamais réécrit, jamais normalisé pour l'affichage.
        "statut_texte": statut,
        "statut_classe": classe,
        "priorite": champs.get("priorite"),
        "type": champs.get("type"),
        "maj": champs.get("dernieremiseajour") or champs.get("creee"),
        # Une divergence n'est AFFIRMÉE que si la classe est connue. Sinon on se
        # tait : « je ne sais pas lire ce statut » n'est pas « ils se contredisent ».
        "divergence": classe != INDETERMINE and classe != colonne,
        "illisible": False,
    }


def lister_specs(racine, colonnes):
    """Toutes les specs, par colonne. `colonnes` = [(repertoire, classe), …].

    La classe d'une colonne est celle à laquelle son répertoire correspond ; elle
    vient de la configuration du projet, pas d'une convention codée ici : un
    autre dépôt nommera ses répertoires autrement.
    """
    specs = []
    for repertoire, classe in colonnes:
        dossier = os.path.join(racine, repertoire)
        if not os.path.isdir(dossier):
            continue
        for nom in sorted(os.listdir(dossier)):
            if not nom.endswith(".md") or nom.startswith("_"):
                continue
            if nom == "README.md":
                continue
            specs.append(lire_spec(os.path.join(dossier, nom), classe))
    return specs


if __name__ == "__main__":
    import json
    import sys

    racine = sys.argv[1] if len(sys.argv) > 1 else "specs/features"
    specs = lister_specs(racine, [("todo", A_FAIRE), ("doing", EN_COURS), ("done", CLOS)])
    divergentes = [s for s in specs if s["divergence"]]
    indeterminees = [s for s in specs if s["statut_classe"] == INDETERMINE]
    print(json.dumps({
        "total": len(specs),
        "par_colonne": {c: sum(1 for s in specs if s["colonne"] == c)
                        for c in (A_FAIRE, EN_COURS, CLOS)},
        "divergences": len(divergentes),
        "statut_illisible": len(indeterminees),
        "sans_statut": sum(1 for s in specs if not s["statut_texte"]),
    }, ensure_ascii=False, indent=2))
    for spec in divergentes:
        print("  %-58s %-8s ≠ %s" % (spec["id"][:58], spec["colonne"], spec["statut_texte"][:60]))
