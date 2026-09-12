#!/usr/bin/env python3
"""Chargement de `control-center.toml` — la ligne de partage de la portabilité.

Ce module existe pour une raison précise : **le fichier de configuration existait
sans que personne ne le lise**. Il était commenté, versionné, et entièrement
décoratif. C'est la pire forme de dette — une configuration qui a l'air d'être en
service alors qu'elle ne pilote rien.

## Ce qui appartient à la configuration, et ce qui n'y appartient pas

Le principe est celui du projet : **tout ce qui peut être découvert l'est**. La
liste des agents vient du système de fichiers, la liste des specs aussi, les
fichiers modifiés viennent de git. La configuration ne porte QUE ce qu'aucune
inspection ne donne :

- l'ORDRE des colonnes et leur LIBELLÉ — « doing » ne dit pas si le travail
  avance ou s'il est bloqué ;
- le VOCABULAIRE des statuts — il est en français ici, il ne le sera pas ailleurs ;
- les COMMANDES lançables, en liste fermée ;
- les bornes de sûreté.

## Pourquoi tout doit avoir un défaut

Un projet instancié depuis le template n'aura peut-être pas de
`control-center.toml`. L'outil doit alors fonctionner, pas refuser de démarrer :
chaque réglage a une valeur par défaut, et `charger()` sur un fichier absent rend
une configuration utilisable. L'absence de configuration n'est pas une erreur,
c'est un projet qui n'a rien de particulier à déclarer.
"""

import os

try:
    import tomllib
except ImportError:  # Python < 3.11
    tomllib = None

# Valeurs par défaut. Elles ne décrivent AUCUN projet en particulier : ce sont
# les conventions les plus répandues, pas celles de ce dépôt.
_DEFAUTS = {
    "projet": {"nom": "Projet sans nom"},
    "specs": {
        "racine": "specs/features",
        "colonnes": [
            {"repertoire": "todo", "libelle": "À faire"},
            {"repertoire": "doing", "libelle": "En cours"},
            {"repertoire": "done", "libelle": "Clos"},
        ],
        # Fichiers à ne jamais prendre pour des specs. Codés en dur dans
        # `specs.py` jusqu'ici — donc invisibles et non modifiables par un autre
        # projet, qui archive peut-être ses notes sous un autre nom.
        "exclure": ["README.md"],
        "prefixes_exclus": ["_"],
        # Au-delà de cette ligne, on cesse de chercher des métadonnées. Certaines
        # specs redéfinissent un second « Statut » dans leurs notes, qui
        # contredit le premier.
        "lignes_entete": 60,
        # Vocabulaire de classification, au SEUL usage de la détection de
        # divergence. Tout mot absent d'ici rend « indéterminé », et l'outil se
        # tait alors au lieu d'affirmer une contradiction.
        # Les valeurs sont des NOMS DE COLONNE — c'est-à-dire, par défaut, des
        # noms de répertoire. Il n'existe donc qu'un seul système de noms.
        # Une couche intermédiaire (« a_faire », « en_cours », « clos ») existait
        # dans le code : deux vocabulaires pour la même notion, sans traduction
        # de l'un vers l'autre. Elle est supprimée.
        "vocabulaire": {
            "todo": "todo", "draft": "todo", "enattente": "todo",
            "doing": "doing", "wip": "doing", "encours": "doing",
            "done": "done", "livre": "done", "livree": "done",
            "cloturee": "done", "cloture": "done",
            "termine": "done", "terminee": "done",
            "implemente": "done", "implementee": "done",
            "abandonne": "done", "abandonnee": "done",
            # ⚠️ « GELÉE » n'est PAS ici : ranger une spec gelée en « clos »
            # suppose que le dernier répertoire archive les décisions de NE PAS
            # FAIRE, ce qui est vrai de ce dépôt et pas de tout projet. Ce choix
            # appartient au `control-center.toml` du projet, pas aux défauts.
        },
    },
    "agents": {"racine": ".claude/agents"},
    "commandes": {},
    "serveur": {
        # Par défaut on n'écoute QUE en local. Ouvrir au réseau est une décision
        # explicite du projet, jamais un héritage silencieux du défaut.
        "hote": "127.0.0.1",
        "port": 8787,
        "fichier_jeton": "",
    },
    "garde_fous": {"budget_max_usd": 5.0, "ecrivains_simultanes": 1},
}


class Config(dict):
    """Configuration fusionnée. Accès par section, jamais de KeyError."""

    def section(self, nom):
        return self.get(nom, {})

    def colonnes(self):
        """[(repertoire, classe, libelle), …] dans l'ordre déclaré."""
        sorties = []
        for colonne in self.section("specs").get("colonnes", []):
            repertoire = colonne.get("repertoire")
            if not repertoire:
                continue
            sorties.append((
                repertoire,
                # Sans `classe` explicite, le répertoire EST la classe : c'est le
                # cas le plus courant et il évite de tout déclarer deux fois.
                colonne.get("classe") or repertoire,
                colonne.get("libelle") or repertoire,
            ))
        return sorties

    def commande(self, cle):
        """L'argv d'une commande autorisée, ou None.

        ⚠️ C'est ici que vit la LISTE FERMÉE. L'appelant fournit une CLÉ ; il ne
        fournit jamais une ligne de commande. Aucune chaîne venue du réseau ne
        peut donc atteindre un shell : au pire elle ne correspond à aucune clé.
        """
        argv = self.section("commandes").get(cle)
        if isinstance(argv, list) and argv and all(isinstance(x, str) for x in argv):
            return list(argv)
        return None

    def tetes_de_commande(self):
        """Les deux premiers jetons de chaque commande autorisée.

        Fait le pont entre le format `argv` de la configuration et le format
        « tête à deux jetons » du journal, qui était jusqu'ici deux notions sans
        traduction l'une vers l'autre.

        ⚠️ Passe par `commande()` et non par les valeurs brutes : sans cela, les
        deux fonctions ne s'accordaient pas sur ce qu'est une commande valide.
        Une entrée malformée (`["ls", 42]`) était refusée par `commande()` et
        pourtant traduite en tête `"ls 42"` ici. Une liste fermée dont deux
        lecteurs divergent n'est plus fermée — défaut trouvé par son test.
        """
        tetes = set()
        for cle in self.section("commandes"):
            argv = self.commande(cle)
            if argv:
                tetes.add(" ".join(argv[:2]))
        return tetes


def _fusionner(defaut, surcharge):
    """Fusion récursive : la surcharge complète le défaut, elle ne l'efface pas.

    Un projet qui déclare seulement `[serveur] port = 9000` garde tous les autres
    réglages. Sans cette fusion, déclarer une section vide effacerait tout ce
    qu'elle contenait par défaut — un piège classique des fichiers de config.
    """
    resultat = dict(defaut)
    for cle, valeur in surcharge.items():
        if isinstance(valeur, dict) and isinstance(resultat.get(cle), dict):
            resultat[cle] = _fusionner(resultat[cle], valeur)
        else:
            resultat[cle] = valeur
    return resultat


def charger(chemin=None, racine_projet=None):
    """Charge la configuration. Un fichier absent ou illisible rend les défauts.

    `racine_projet` est le répertoire auquel les chemins relatifs se rapportent ;
    par défaut, le parent de `control-center/`.
    """
    ici = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if chemin is None:
        chemin = os.path.join(ici, "control-center.toml")
    if racine_projet is None:
        racine_projet = os.path.dirname(ici)

    brut = {}
    if tomllib is not None and os.path.exists(chemin):
        try:
            with open(chemin, "rb") as fichier:
                brut = tomllib.load(fichier)
        except (OSError, ValueError):
            # Un TOML cassé ne doit pas empêcher l'outil de démarrer : il
            # démarre sur les défauts, et l'appelant verra `fichier_lu` à False.
            brut = {}

    config = Config(_fusionner(_DEFAUTS, brut if isinstance(brut, dict) else {}))
    config["_chemin"] = chemin
    config["_racine_projet"] = racine_projet
    config["_fichier_lu"] = bool(brut)
    return config


if __name__ == "__main__":
    import json

    config = charger()
    print(json.dumps({
        "fichier": config["_chemin"],
        "lu": config["_fichier_lu"],
        "racine_projet": config["_racine_projet"],
        "projet": config.section("projet").get("nom"),
        "colonnes": config.colonnes(),
        "commandes": sorted(config.section("commandes")),
        "tetes": sorted(config.tetes_de_commande()),
        "serveur": config.section("serveur"),
        "garde_fous": config.section("garde_fous"),
        "taille_vocabulaire": len(config.section("specs").get("vocabulaire", {})),
    }, ensure_ascii=False, indent=2))
