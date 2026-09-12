#!/usr/bin/env python3
"""Tests du lecteur de spécifications.

    python3 control-center/backend/test_specs.py

Sans dépendance. Les cas sont tirés de formes RÉELLEMENT rencontrées dans les
167 specs du dépôt, pas inventées : trois formats de métadonnées coexistent, le
champ « Statut » est en texte libre, et certaines specs en redéfinissent un
second dans leurs notes d'implémentation.

VALIDATION PAR MUTATION (CLAUDE.md, étape 6) — chaque garde doit avoir son
mutant tueur. Voir le bas du fichier pour la table.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import config as C  # noqa: E402
import specs as S  # noqa: E402

# Vocabulaire et colonnes viennent de la CONFIGURATION, plus du code : les tests
# les fournissent donc explicitement, comme le fera le serveur. `gelee` n'y est
# pas — c'est un choix du projet, pas un défaut.
VOC = C.charger(chemin="/introuvable.toml").section("specs")["vocabulaire"]
TODO, DOING, DONE = "todo", "doing", "done"
COLONNES = [(TODO, TODO), (DOING, DOING), (DONE, DONE)]


def lire(chemin, colonne, lignes_entete=60):
    return S.lire_spec(chemin, colonne, VOC, lignes_entete)

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


# --- formes réelles du dépôt -------------------------------------------------

FORMAT_PUCES = """# Bug — quelque chose

## Métadonnées
- **ID** : bug-121
- **Type** : bug — republication redondante
- **Statut** : todo
- **Priorité** : **normale** — abaissée le 08/09
- **Dernière mise à jour** : 2026-09-08
"""

FORMAT_TABLEAU = """# Bug — redémarrage panic

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | **bug critique** — crash firmware en production |
| Statut | doing — **lot A livré (v2.33.0, 02/09)** |
| Priorité | **Haute** — un crash non expliqué |
"""

# Format « minimal » historique : pas d'en-tête `## Métadonnées`, pas de H1 non
# plus dans certains cas. 78 specs sur 167 n'ont aucun titre H1.
FORMAT_MINIMAL = """## Métadonnées
- **Type** : bug simple (format minimal `specs/modele.md`)
- **Statut** : done (2026-07-05 — reste validation on-target)
- **Créée** : 2026-07-05
"""

DEUX_MOTS = """# Bug

- **Statut** : **en cours** — phase 1 livrée en v2.19.0 ; phase 2 en attente
"""

STATUT_INCONNU = """# Bug

- **Statut** : callout bizarre jamais vu ailleurs
"""

# `bug-ha-eclairage-select` redéfinit un second « Statut » dans ses notes, qui
# contredit partiellement le premier. Le premier doit gagner.
#
# Ce cas-ci place le second DANS la fenêtre d'en-tête : c'est la garde
# « le premier champ gagne » qui doit le repousser, et elle seule.
SECOND_STATUT = """# Bug

- **Statut** : todo

## Notes d'implémentation
- **Statut** : livré — build OK, revue OK
"""

# Celui-ci place un champ ABSENT de l'en-tête très bas dans le fichier. Ici la
# garde « premier gagné » ne protège rien — le champ n'a jamais été vu. Seule la
# fenêtre d'en-tête empêche d'aller le chercher au fond des notes.
#
# Les deux gardes sont donc bien distinctes. Un seul cas de test les rendait
# mutuellement redondantes : casser l'une ou l'autre ne tuait alors rien
# (constaté par mutation).
CHAMP_TARDIF = """# Bug

- **Statut** : todo

## Notes d'implémentation
""" + "\n" * 70 + """
- **Priorité** : critique (mentionnée seulement dans les notes)
"""


def ecrire(dossier, nom, contenu):
    chemin = os.path.join(dossier, nom)
    with open(chemin, "w", encoding="utf-8") as fichier:
        fichier.write(contenu)
    return chemin


def main():
    with tempfile.TemporaryDirectory() as tmp:
        racine = os.path.join(tmp, "features")
        for colonne in ("todo", "doing", "done"):
            os.makedirs(os.path.join(racine, colonne))

        t = os.path.join(racine, "todo")
        d = os.path.join(racine, "done")

        # --- les trois formats de métadonnées ------------------------------
        s = lire(ecrire(t, "bug-121-x.md", FORMAT_PUCES), TODO)
        check("test_format_puces", s["statut_texte"] == "todo" and s["type"].startswith("bug"),
              "statut=%r" % s["statut_texte"])
        check("test_priorite_avec_justification_conservee_telle_quelle",
              s["priorite"] == "**normale** — abaissée le 08/09",
              "obtenu %r" % s["priorite"])

        s = lire(ecrire(t, "bug-080-y.md", FORMAT_TABLEAU), TODO)
        check("test_format_tableau", s["statut_classe"] == DOING,
              "classe=%r sur %r" % (s["statut_classe"], s["statut_texte"]))

        s = lire(ecrire(d, "bug-minimal.md", FORMAT_MINIMAL), DONE)
        check("test_format_minimal_sans_titre_h1", s["statut_classe"] == DONE)
        check("test_titre_retombe_sur_l_identifiant", s["titre"] == "bug-minimal",
              "un titre inventé serait pire qu'un identifiant vrai ; obtenu %r" % s["titre"])

        # --- classification --------------------------------------------------
        s = lire(ecrire(d, "bug-deux-mots.md", DEUX_MOTS), DONE)
        check("test_statut_en_deux_mots", s["statut_classe"] == DOING,
              "« en cours » doit être lu ; obtenu %r" % s["statut_classe"])
        check("test_statut_en_deux_mots_produit_la_divergence", s["divergence"] is True)

        s = lire(ecrire(d, "bug-inconnu.md", STATUT_INCONNU), DONE)
        check("test_statut_inconnu_reste_indetermine", s["statut_classe"] == S.INDETERMINE)
        check("test_statut_inconnu_n_affirme_aucune_divergence", s["divergence"] is False,
              "« je ne sais pas lire ce statut » n'est PAS « ils se contredisent »")
        check("test_statut_inconnu_reste_affiche",
              s["statut_texte"] == "callout bizarre jamais vu ailleurs",
              "le texte brut doit survivre à l'échec de classification")

        s = lire(ecrire(d, "bug-second.md", SECOND_STATUT), DONE)
        check("test_premier_statut_gagne", s["statut_texte"] == "todo",
              "un second « Statut » dans les notes ne doit pas écraser celui de tête ; obtenu %r"
              % s["statut_texte"])

        s = lire(ecrire(d, "bug-tardif.md", CHAMP_TARDIF), DONE)
        check("test_champ_hors_entete_ignore", s["priorite"] is None,
              "un champ trouvé au fond des notes n'est pas une métadonnée d'en-tête ; obtenu %r"
              % s["priorite"])

        # --- robustesse ------------------------------------------------------
        chemin = ecrire(d, "bug-binaire.md", "")
        with open(chemin, "wb") as fichier:
            fichier.write(b"\xff\xfe\x00 pas de l'utf-8")
        s = lire(chemin, DONE)
        check("test_fichier_illisible_ne_leve_pas", s["illisible"] is True)
        check("test_fichier_illisible_n_affirme_aucune_divergence", s["divergence"] is False)

        # --- inventaire ------------------------------------------------------
        ecrire(racine, "_template.md", FORMAT_PUCES)
        ecrire(d, "README.md", "# archive")
        listees = S.lister_specs(racine, COLONNES, VOC, 60,
                                 exclure=["README.md"], prefixes_exclus=["_"])
        noms = {x["id"] for x in listees}
        check("test_template_et_readme_exclus",
              "README" not in noms and "_template" not in noms,
              "listés : %r" % sorted(noms))
        check("test_toutes_les_autres_sont_listees", len(listees) == 8,
              "attendu 8, obtenu %d : %r" % (len(listees), sorted(noms)))

    print("\n%d cas, %d echec(s)" % (_tests, len(_echecs)))
    for echec in _echecs:
        print("  - %s" % echec)
    return 1 if _echecs else 0


if __name__ == "__main__":
    sys.exit(main())
