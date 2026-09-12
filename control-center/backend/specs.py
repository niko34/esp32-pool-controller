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

# SEULE constante de classe qui reste dans le code : la sentinelle d'ignorance.
# Ce n'est pas une colonne, c'est l'aveu qu'on ne sait pas lire ce statut — et à
# ce titre elle ne peut pas venir de la configuration d'un projet.
INDETERMINE = "indetermine"

# Les colonnes, le vocabulaire, la fenêtre d'en-tête et les exclusions viennent
# désormais de `control-center.toml` (voir `config.py`). Ils étaient codés ici :
# vocabulaire français, `README.md`, 60 lignes. Une revue l'a relevé — le module
# promettait la portabilité et ne la tenait pas, ce qui ne se serait vu qu'au
# moment du déménagement vers le template.

_ACCENTS = str.maketrans("àâäéèêëîïôöùûüç", "aaaeeeeiioouuuc")


def _normaliser(mot):
    """Minuscules, sans accent, sans ponctuation ni gras Markdown."""
    mot = mot.strip().lower().translate(_ACCENTS)
    return re.sub(r"[^a-z0-9]", "", mot)


def classifier_statut(valeur, vocabulaire):
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
        classe = vocabulaire.get(_normaliser(mots[0]) + _normaliser(mots[1]))
        if classe:
            return classe
    return vocabulaire.get(_normaliser(mots[0]), INDETERMINE)


# --- extraction des métadonnées, trois formats -------------------------------

# (a) format template : `- **ID** : valeur`
_PUCE = re.compile(r"^\s*[-*]\s+\*\*(?P<champ>[^*]+?)\*\*\s*:\s*(?P<valeur>.*)$")
# (c) format minimal historique : `- **Statut** : valeur` — même forme que (a),
#     mais sans en-tête `## Métadonnées`. Rien à distinguer à la lecture.
# (b) format tableau : `| Statut | valeur |`
_LIGNE_TABLEAU = re.compile(r"^\s*\|\s*(?P<champ>[^|]+?)\s*\|\s*(?P<valeur>.*?)\s*\|\s*$")
_SEPARATEUR_TABLEAU = re.compile(r"^\s*\|[\s:|-]+\|\s*$")

_TITRE_H1 = re.compile(r"^#\s+(?P<titre>.+?)\s*$")

def _extraire_champs(lignes, lignes_entete):
    """Champs de tête, quel que soit le format. Le PREMIER gagne.

    `lignes_entete` borne la recherche : au-delà, on cesse de chercher. Certaines
    specs redéfinissent un second champ « Statut » dans leurs notes
    d'implémentation (constaté sur `bug-ha-eclairage-select`), qui contredit
    partiellement le premier. Le premier fait foi.
    """
    champs = {}
    for ligne in lignes[:lignes_entete]:
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


def lire_spec(chemin, colonne, vocabulaire=None, lignes_entete=60):
    """Lit une spec. Ne lève jamais : un fichier illisible est rendu tel quel.

    `vocabulaire` et `lignes_entete` viennent de la configuration du projet. Leurs
    défauts ne servent qu'aux appels directs : le serveur passe toujours ceux du
    projet.
    """
    if vocabulaire is None:
        vocabulaire = {}
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

    champs = _extraire_champs(lignes, lignes_entete)
    statut = champs.get("statut")
    classe = classifier_statut(statut, vocabulaire)

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


def lister_specs(racine, colonnes, vocabulaire=None, lignes_entete=60,
                 exclure=(), prefixes_exclus=("_",)):
    """Toutes les specs, par colonne. `colonnes` = [(repertoire, classe), …].

    Tout ce qui varie d'un projet à l'autre est un PARAMÈTRE : les noms de
    répertoire, le vocabulaire des statuts, les fichiers à ignorer. Rien de tout
    cela n'est écrit dans ce module.
    """
    exclus = set(exclure)
    specs = []
    for repertoire, classe in colonnes:
        dossier = os.path.join(racine, repertoire)
        if not os.path.isdir(dossier):
            continue
        for nom in sorted(os.listdir(dossier)):
            if not nom.endswith(".md") or nom in exclus:
                continue
            if any(nom.startswith(p) for p in prefixes_exclus if p):
                continue
            specs.append(lire_spec(os.path.join(dossier, nom), classe,
                                   vocabulaire, lignes_entete))
    return specs


def lister_depuis_config(config):
    """Raccourci : lit les specs d'après une configuration chargée."""
    reglages = config.section("specs")
    racine = os.path.join(config["_racine_projet"], reglages.get("racine", "specs/features"))
    colonnes = [(repertoire, classe) for repertoire, classe, _ in config.colonnes()]
    return lister_specs(
        racine, colonnes,
        vocabulaire=reglages.get("vocabulaire", {}),
        lignes_entete=reglages.get("lignes_entete", 60),
        exclure=reglages.get("exclure", ()),
        prefixes_exclus=reglages.get("prefixes_exclus", ("_",)),
    )


if __name__ == "__main__":
    import json

    import config as C

    configuration = C.charger()
    specs = lister_depuis_config(configuration)
    libelles = {r: l for r, _, l in configuration.colonnes()}
    divergentes = [s for s in specs if s["divergence"]]
    print(json.dumps({
        "projet": configuration.section("projet").get("nom"),
        "config_lue": configuration["_fichier_lu"],
        "total": len(specs),
        "par_colonne": {libelles.get(c, c): sum(1 for s in specs if s["colonne"] == c)
                        for c, _, _ in [(r, x, y) for r, x, y in configuration.colonnes()]},
        "divergences": len(divergentes),
        "statut_illisible": sum(1 for s in specs if s["statut_classe"] == INDETERMINE),
        "sans_statut": sum(1 for s in specs if not s["statut_texte"]),
    }, ensure_ascii=False, indent=2))
    for spec in divergentes:
        print("  %-58s %-6s ≠ %s" % (spec["id"][:58], spec["colonne"],
                                     (spec["statut_texte"] or "")[:58]))
