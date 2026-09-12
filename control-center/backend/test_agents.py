#!/usr/bin/env python3
"""Tests de la découverte des agents.

    python3 control-center/backend/test_agents.py

Sans dépendance. Le module lu ne parse QUE le frontmatter YAML d'un fichier
d'agent — quelques paires `clé: valeur` avant le `---` de fermeture — jamais
le corps, qui est le prompt système et peut faire des centaines de lignes.

Ce qui compte ici :
- les trois champs simples (`name`, `model`, `description`) et `tools` sous
  ses deux formes ;
- une clé inconnue est CONSERVÉE dans `autres`, pas jetée ;
- `peut_ecrire` et `peut_deleguer` se DÉDUISENT de la liste d'outils, jamais
  d'un champ déclaré — `peut_deleguer` en particulier, puisque son absence
  aplatit le graphe d'exécution que `events.py` reconstruit par tri
  chronologique ;
- l'absence de frontmatter, un fichier illisible, un répertoire absent : rien
  de tout cela ne doit lever.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agents as A  # noqa: E402

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


def ecrire(dossier, nom, contenu):
    chemin = os.path.join(dossier, nom)
    with open(chemin, "w", encoding="utf-8") as fichier:
        fichier.write(contenu)
    return chemin


def main():
    with tempfile.TemporaryDirectory() as tmp:
        # --- les trois champs simples + tools en forme "a, b, c" -------------
        chemin = ecrire(tmp, "architecte.md", """---
name: architecte
model: opus
description: Conçoit le plan avant codage
tools: Read, Grep, Glob
---
# Prompt système, jamais lu par ce module.
model: sonnet
""")
        agent = A.lire_agent(chemin)
        check("test_name_lu", agent["nom"] == "architecte")
        check("test_model_lu", agent["modele"] == "opus")
        check("test_description_lue",
              agent["description"] == "Conçoit le plan avant codage")
        check("test_tools_forme_virgule",
              agent["outils"] == ["Read", "Grep", "Glob"])

        # --- tools en forme liste "[a, b]" ------------------------------------
        chemin = ecrire(tmp, "reviewer.md", """---
name: reviewer
tools: [Read, Grep]
---
corps
""")
        agent = A.lire_agent(chemin)
        check("test_tools_forme_crochets", agent["outils"] == ["Read", "Grep"])

        # --- clé inconnue conservée, pas jetée --------------------------------
        chemin = ecrire(tmp, "futuriste.md", """---
name: futuriste
champ_inconnu: valeur_a_conserver
---
corps
""")
        agent = A.lire_agent(chemin)
        check("test_cle_inconnue_conservee",
              agent.get("autres", {}).get("champ_inconnu") == "valeur_a_conserver",
              "un champ neuf d'une future version de Claude Code doit remonter "
              "à l'interface, pas disparaître ; obtenu %r" % agent.get("autres"))

        # --- peut_ecrire : DÉDUIT des outils, jamais d'un champ déclaré -------
        chemin = ecrire(tmp, "ecrivain.md", """---
name: ecrivain
tools: Read, Write
peut_ecrire: false
---
corps
""")
        agent = A.lire_agent(chemin)
        check("test_peut_ecrire_deduit_de_write",
              agent["peut_ecrire"] is True,
              "un champ `peut_ecrire: false` déclaré à la main ne doit pas "
              "l'emporter sur la présence réelle de Write dans tools")

        for outil in ("Write", "Edit", "NotebookEdit"):
            chemin = ecrire(tmp, "x.md", "---\nname: x\ntools: Read, %s\n---\n" % outil)
            agent = A.lire_agent(chemin)
            check("test_peut_ecrire_avec_%s" % outil, agent["peut_ecrire"] is True)

        chemin = ecrire(tmp, "lecteur.md", "---\nname: lecteur\ntools: Read, Grep\n---\n")
        agent = A.lire_agent(chemin)
        check("test_peut_ecrire_faux_sans_outil_d_ecriture",
              agent["peut_ecrire"] is False)

        # --- peut_deleguer : DÉDUIT de la présence de Task --------------------
        chemin = ecrire(tmp, "orchestrateur.md", "---\nname: orchestrateur\ntools: Read, Task\n---\n")
        agent = A.lire_agent(chemin)
        check("test_peut_deleguer_vrai_avec_task", agent["peut_deleguer"] is True)

        chemin = ecrire(tmp, "sans_task.md", "---\nname: sans_task\ntools: Read, Write\n---\n")
        agent = A.lire_agent(chemin)
        check("test_peut_deleguer_faux_sans_task", agent["peut_deleguer"] is False,
              "l'absence de Task doit rendre le graphe d'exécution plat")

        # --- pas de frontmatter : pas une erreur -------------------------------
        chemin = ecrire(tmp, "brut.md", "Aucun frontmatter ici, juste du texte.\n")
        agent = A.lire_agent(chemin)
        check("test_sans_frontmatter_pas_d_erreur", agent["illisible"] is False)
        check("test_sans_frontmatter_nom_de_fichier",
              agent["nom"] == "brut" and agent["id"] == "brut",
              "sans frontmatter, on rend ce qu'on sait : le nom du fichier")
        check("test_sans_frontmatter_pas_de_capacites",
              agent["outils"] == [] and agent["peut_ecrire"] is False
              and agent["peut_deleguer"] is False)

        # --- fichier illisible (octets non UTF-8) : pas d'exception -----------
        chemin = os.path.join(tmp, "corrompu.md")
        with open(chemin, "wb") as fichier:
            fichier.write(b"---\nname: corrompu\n---\n\xff\xfe\x00broken")
        agent = A.lire_agent(chemin)
        check("test_fichier_illisible_signale", agent["illisible"] is True)
        check("test_fichier_illisible_pas_d_exception_levee", True,
              "l'appel ci-dessus n'a pas levé, sinon la suite se serait arrêtée")

        # --- le corps n'est pas lu : rien après le --- de fermeture -----------
        chemin = ecrire(tmp, "corps_piege.md", """---
name: corps_piege
model: haiku
---
Ceci est le prompt système.
model: opus
tools: Read, Write, Task
""")
        agent = A.lire_agent(chemin)
        check("test_corps_ignore_apres_fermeture",
              agent["modele"] == "haiku" and agent["outils"] == []
              and agent["peut_ecrire"] is False and agent["peut_deleguer"] is False,
              "une ligne `model: opus` après le --- de fermeture ne doit pas "
              "écraser le frontmatter réel ; obtenu %r" % agent)

        # --- fenêtre de lecture du frontmatter : au-delà, on cesse de lire ----
        lignes_de_bourrage = "\n".join("# ligne %d" % i for i in range(100))
        chemin = ecrire(tmp, "long.md", "---\n%s\nname: tardif\n---\n" % lignes_de_bourrage)
        agent = A.lire_agent(chemin)
        check("test_champ_au_dela_de_la_fenetre_ignore",
              agent["nom"] == "long",
              "un champ situé au-delà de _MAX_LIGNES_FRONTMATTER ne doit pas "
              "être lu ; obtenu nom=%r" % agent["nom"])

        # --- répertoire absent : liste vide, pas d'exception -------------------
        check("test_repertoire_absent_liste_vide",
              A.lister_agents(os.path.join(tmp, "nexiste_pas")) == [])

        # --- tri par nom, et les fichiers `_` ignorés --------------------------
        sous_dossier = os.path.join(tmp, "liste")
        os.makedirs(sous_dossier)
        ecrire(sous_dossier, "zebre.md", "---\nname: zebre\n---\n")
        ecrire(sous_dossier, "alpha.md", "---\nname: alpha\n---\n")
        ecrire(sous_dossier, "_template.md", "---\nname: gabarit\n---\n")
        ecrire(sous_dossier, "pasunmd.txt", "ignoré")
        agents = A.lister_agents(sous_dossier)
        check("test_tri_par_nom",
              [a["nom"] for a in agents] == ["alpha", "zebre"],
              "obtenu %r" % [a["nom"] for a in agents])
        check("test_prefixe_souligne_ignore",
              all(a["nom"] != "gabarit" for a in agents),
              "un gabarit préfixé `_` ne doit pas apparaître comme un agent réel")
        check("test_fichier_non_md_ignore",
              all(a["nom"] != "pasunmd" for a in agents))

    print("\n%d cas, %d echec(s)" % (_tests, len(_echecs)))
    for echec in _echecs:
        print("  - %s" % echec)
    return 1 if _echecs else 0


if __name__ == "__main__":
    # Même filet que les autres suites : une exception levée par le code testé
    # ne doit pas faire disparaître le résumé. `sys.exit()` reste HORS du try —
    # l'y mettre attrape son propre SystemExit et transforme un succès en échec
    # apparent (piège déjà rencontré dans ce projet).
    try:
        _code = main()
    except Exception as erreur:
        print("\n%d cas atteints, INTERROMPU par %s : %s"
              % (_tests, type(erreur).__name__, erreur))
        print("  (le compte ci-dessus est un MINORANT)")
        _code = 1
    sys.exit(_code)
