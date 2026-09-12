#!/usr/bin/env python3
"""Tests du chargement de configuration.

    python3 control-center/backend/test_config.py

Deux propriétés comptent ici, et elles tirent dans des sens opposés :

1. **L'outil démarre toujours.** Fichier absent, TOML cassé, section vide : il
   part sur les défauts. Un projet instancié depuis le template n'a peut-être
   rien à déclarer, et ce n'est pas une erreur.

2. **La liste des commandes est FERMÉE.** C'est la seule chose qui empêchera une
   chaîne venue du réseau d'atteindre un shell. Elle doit refuser tout ce qui
   n'est pas exactement une liste d'arguments déclarée.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import config as C  # noqa: E402

_echecs = []
_tests = 0


def check(nom, condition, detail=""):
    global _tests
    _tests += 1
    if condition:
        print("  ok     %s" % nom)
    else:
        _echecs.append(nom)
        print("  ECHEC  %s%s" % (nom, (" : " + detail) if detail else ""))


def ecrire(dossier, contenu):
    chemin = os.path.join(dossier, "c.toml")
    with open(chemin, "w", encoding="utf-8") as fichier:
        fichier.write(contenu)
    return chemin


def main():
    with tempfile.TemporaryDirectory() as tmp:
        # --- démarrage sans configuration -----------------------------------
        c = C.charger(chemin=os.path.join(tmp, "absent.toml"), racine_projet=tmp)
        check("test_fichier_absent_donne_les_defauts",
              c["_fichier_lu"] is False and len(c.colonnes()) == 3)
        check("test_defaut_ecoute_uniquement_en_local",
              c.section("serveur")["hote"] == "127.0.0.1",
              "ouvrir au réseau doit être une décision explicite du projet, "
              "jamais un héritage silencieux du défaut")
        check("test_defaut_aucune_commande_autorisee",
              c.section("commandes") == {} and c.commande("tests") is None,
              "sans déclaration, RIEN n'est lançable")

        c = C.charger(chemin=ecrire(tmp, "ceci n'est pas du toml ["), racine_projet=tmp)
        check("test_toml_casse_ne_bloque_pas_le_demarrage",
              c["_fichier_lu"] is False and len(c.colonnes()) == 3,
              "un fichier cassé doit dégrader vers les défauts, pas empêcher de démarrer")

        # --- fusion : la surcharge COMPLÈTE, elle n'efface pas ---------------
        c = C.charger(chemin=ecrire(tmp, "[serveur]\nport = 9000\n"), racine_projet=tmp)
        check("test_surcharge_partielle_conserve_le_reste",
              c.section("serveur")["port"] == 9000
              and c.section("serveur")["hote"] == "127.0.0.1",
              "déclarer un seul réglage ne doit pas effacer les autres de sa section")

        c = C.charger(chemin=ecrire(tmp, '[specs.vocabulaire]\ngelee = "done"\n'),
                      racine_projet=tmp)
        voc = c.section("specs")["vocabulaire"]
        check("test_vocabulaire_ajoute_sans_effacer",
              voc.get("gelee") == "done" and voc.get("todo") == "todo",
              "un projet ajoute son vocabulaire propre, il ne repart pas de zéro")

        # --- liste FERMÉE des commandes -------------------------------------
        c = C.charger(chemin=ecrire(tmp, '''[commandes]
tests = ["pio", "test", "-e", "native"]
mauvais_type = "rm -rf /"
vide = []
melange = ["ls", 42]
'''), racine_projet=tmp)
        check("test_commande_declaree_rendue_en_argv",
              c.commande("tests") == ["pio", "test", "-e", "native"])
        check("test_commande_inconnue_refusee", c.commande("nimporte_quoi") is None,
              "une clé inconnue ne doit RIEN rendre : c'est ce qui ferme la liste")
        check("test_commande_en_chaine_refusee", c.commande("mauvais_type") is None,
              "une chaîne serait une ligne de commande — exactement ce qu'on interdit")
        check("test_commande_vide_refusee", c.commande("vide") is None)
        check("test_commande_aux_elements_non_textuels_refusee",
              c.commande("melange") is None)
        check("test_argv_rendu_est_une_copie",
              (lambda a: (a.append("rm"), c.commande("tests") == ["pio", "test", "-e", "native"])[1])(
                  c.commande("tests")),
              "modifier l'argv rendu ne doit pas contaminer la configuration")

        check("test_tetes_de_commande_font_le_pont_avec_le_journal",
              c.tetes_de_commande() == {"pio test"},
              "le journal ne connaît que des têtes à deux jetons ; la config des "
              "argv. Sans traduction, ce sont deux notions sans lien ; obtenu %r"
              % c.tetes_de_commande())

        # --- colonnes --------------------------------------------------------
        c = C.charger(chemin=ecrire(tmp, '''[[specs.colonnes]]
repertoire = "backlog"
libelle = "Idées"

[[specs.colonnes]]
repertoire = "wip"
'''), racine_projet=tmp)
        check("test_colonnes_du_projet_remplacent_les_defauts",
              [r for r, _, _ in c.colonnes()] == ["backlog", "wip"],
              "une LISTE se remplace, elle ne fusionne pas — sinon on hériterait "
              "de colonnes d'un autre projet")
        check("test_repertoire_sert_de_classe_par_defaut",
              [cl for _, cl, _ in c.colonnes()] == ["backlog", "wip"],
              "il n'existe qu'un seul système de noms : la colonne EST le répertoire")
        check("test_libelle_retombe_sur_le_repertoire",
              [l for _, _, l in c.colonnes()] == ["Idées", "wip"],
              "sans libellé, afficher le répertoire est vrai ; inventer serait faux")

        c = C.charger(chemin=ecrire(tmp, '[[specs.colonnes]]\nlibelle = "sans repertoire"\n'),
                      racine_projet=tmp)
        check("test_colonne_sans_repertoire_ignoree", c.colonnes() == [])

    print("\n%d cas, %d echec(s)" % (_tests, len(_echecs)))
    for echec in _echecs:
        print("  - %s" % echec)
    return 1 if _echecs else 0


if __name__ == "__main__":
    # ⚠️ `check()` évalue sa condition AVANT l'appel : une exception levée par le
    # code testé tue le script au lieu de compter un échec, et le résumé ne
    # s'imprime jamais. Un compte qui s'interrompt sans le dire est un compte
    # faux — constaté en validant par mutation, où trois mutants sur six ne
    # produisaient plus aucune sortie du tout.
    #
    # Le filet ci-dessous garantit qu'un résumé sort toujours et que le code de
    # retour reste non nul. Il ne remplace pas les tests : il empêche un échec
    # de se déguiser en silence.
    # ⚠️ `sys.exit()` est APPELÉ HORS du try : il lève SystemExit, que le filet
    # attraperait sinon — une suite verte affichait alors « INTERROMPU » et
    # sortait en code 1. Le filet transformait un succès en échec apparent.
    try:
        _code = main()
    except Exception as erreur:
        print("\n%d cas atteints, INTERROMPU par %s : %s"
              % (_tests, type(erreur).__name__, erreur))
        print("  (le compte ci-dessus est un MINORANT : les cas suivants "
              "n'ont pas été joués)")
        _code = 1
    sys.exit(_code)
