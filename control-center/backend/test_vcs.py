#!/usr/bin/env python3
"""Tests de la lecture de git.

    python3 control-center/backend/test_vcs.py

Sans dépendance. Crée un vrai dépôt git jetable — les cas qui comptent ici sont
des cas de VRAI git : un nom de fichier avec une espace, un renommage qui occupe
deux entrées, un fichier dont le nom ressemble à une option.

Ce qui est testé en priorité, c'est ce qui deviendra exploitable quand le serveur
HTTP existera : un chemin venu du navigateur ne doit jamais sortir du dépôt, et
une référence de commit ne doit jamais devenir une option de git.
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vcs as V  # noqa: E402

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


def git(racine, *arguments):
    subprocess.run(["git", "-C", racine] + list(arguments),
                   capture_output=True, text=True, check=False)


def depot(racine):
    git(racine, "init", "-q", "-b", "main")
    git(racine, "config", "user.email", "t@t")
    git(racine, "config", "user.name", "Test")


def ecrire(racine, nom, contenu="x\n"):
    chemin = os.path.join(racine, nom)
    os.makedirs(os.path.dirname(chemin), exist_ok=True)
    with open(chemin, "w", encoding="utf-8") as fichier:
        fichier.write(contenu)
    return chemin


def main():
    with tempfile.TemporaryDirectory() as parent:
        racine = os.path.join(parent, "depot")
        dehors = os.path.join(parent, "dehors")
        os.makedirs(racine)
        os.makedirs(dehors)
        ecrire(dehors, "secret.txt", "MOT DE PASSE\n")
        depot(racine)

        # --- CONFINEMENT : le point qui deviendra exploitable ----------------
        ecrire(racine, "src/a.py")
        check("test_chemin_interne_accepte", V.chemin_sur(racine, "src/a.py") == "src/a.py")
        check("test_chemin_absolu_interne_accepte",
              V.chemin_sur(racine, os.path.join(racine, "src/a.py")) == "src/a.py")
        check("test_remontee_refusee", V.chemin_sur(racine, "../dehors/secret.txt") is None,
              "`..` ne doit jamais sortir du dépôt")
        check("test_chemin_absolu_externe_refuse",
              V.chemin_sur(racine, "/etc/passwd") is None)
        check("test_racine_elle_meme_refusee", V.chemin_sur(racine, ".") is None,
              "la racine n'est pas un fichier à servir")
        check("test_octet_nul_refuse", V.chemin_sur(racine, "src/a\x00.py") is None)
        check("test_chemin_vide_refuse", V.chemin_sur(racine, "") is None)

        os.symlink(os.path.join(dehors, "secret.txt"), os.path.join(racine, "evasion"))
        check("test_lien_symbolique_sortant_refuse",
              V.chemin_sur(racine, "evasion") is None,
              "un lien qui pointe hors du dépôt est refusé, jamais suivi")

        # --- noms de fichiers hostiles ---------------------------------------
        ecrire(racine, "un fichier avec espaces.md")
        ecrire(racine, "--stat")
        git(racine, "add", "-A")
        git(racine, "commit", "-q", "-m", "premier")

        with open(os.path.join(racine, "--stat"), "a", encoding="utf-8") as f:
            f.write("modif\n")
        texte, _ = V.diff(racine, "--stat")
        check("test_fichier_nomme_comme_une_option_reste_un_fichier",
              texte is not None and "--stat" in texte and "diff --git" in texte,
              "sans le séparateur `--`, git aurait lu le nom comme une option ; "
              "obtenu %r" % (texte or "")[:60])

        with open(os.path.join(racine, "un fichier avec espaces.md"), "a",
                  encoding="utf-8") as f:
            f.write("modif\n")
        chemins = [e["chemin"] for e in V.etat(racine)]
        check("test_nom_avec_espaces_rendu_intact",
              "un fichier avec espaces.md" in chemins,
              "le format -z évite l'échappement et les guillemets ; obtenu %r" % chemins)

        # --- renommage : DEUX entrées, l'ancienne ne doit pas être prise ------
        git(racine, "mv", "src/a.py", "src/b.py")
        entrees = {e["chemin"]: e for e in V.etat(racine)}
        check("test_renommage_rend_le_nouveau_chemin", "src/b.py" in entrees,
              "obtenu %r" % sorted(entrees))
        check("test_renommage_conserve_l_ancien_nom",
              entrees.get("src/b.py", {}).get("ancien") == "src/a.py",
              "obtenu %r" % entrees.get("src/b.py", {}).get("ancien"))

        # --- fichier non suivi ------------------------------------------------
        ecrire(racine, "neuf.txt")
        entrees = {e["chemin"]: e for e in V.etat(racine)}
        check("test_fichier_non_suivi_repere",
              entrees.get("neuf.txt", {}).get("libelle") == "non suivi")

        # --- diff tronqué : l'appelant DOIT pouvoir le dire -------------------
        ecrire(racine, "gros.txt", "ligne\n" * 5000)
        git(racine, "add", "-A")
        git(racine, "commit", "-q", "-m", "gros")
        with open(os.path.join(racine, "gros.txt"), "a", encoding="utf-8") as f:
            f.write("encore\n" * 5000)
        texte, tronque = V.diff(racine, "gros.txt", taille_max=500)
        check("test_diff_tronque_est_annonce", tronque is True and len(texte) == 500,
              "un diff coupé en silence se lit comme un diff complet")
        texte, tronque = V.diff(racine, "gros.txt")
        check("test_diff_entier_n_est_pas_annonce_tronque", tronque is False)

        check("test_diff_d_un_chemin_hors_depot_refuse",
              V.diff(racine, "../dehors/secret.txt") == (None, False),
              "le confinement s'applique AUSSI au diff, pas seulement à la lecture")

        # --- référence de commit : ne doit jamais devenir une option ----------
        for mauvaise in ("--help", "-n", "../autre", "a/b", "..", "; rm -rf /",
                         "$(whoami)", ""):
            if V.fichiers_du_commit(racine, mauvaise) != []:
                check("test_reference_refusee_%r" % mauvaise, False,
                      "une référence hostile a été acceptée")
                break
        else:
            check("test_references_hostiles_toutes_refusees", True)

        commits = V.journal(racine, 5)
        check("test_journal_rend_les_commits",
              len(commits) == 2 and commits[0]["sujet"] == "gros",
              "obtenu %r" % [c.get("sujet") for c in commits])
        check("test_journal_sujet_avec_separateur_impossible",
              all("\x1f" not in c["sujet"] for c in commits))

        fichiers = V.fichiers_du_commit(racine, commits[0]["hash"])
        check("test_fichiers_du_commit",
              any(f["chemin"] == "gros.txt" for f in fichiers),
              "obtenu %r" % fichiers)

        # La borne est exercée en l'abaissant : sans cela il aurait fallu un
        # dépôt de plus de 200 commits, et le mutant qui la supprimait survivait.
        ancienne = V._MAX_COMMITS
        V._MAX_COMMITS = 1
        try:
            check("test_limite_du_journal_bornee", len(V.journal(racine, 10_000)) == 1,
                  "une demande absurde venue du navigateur doit être plafonnée")
        finally:
            V._MAX_COMMITS = ancienne
        check("test_limite_demandee_respectee", len(V.journal(racine, 1)) == 1)
        check("test_limite_absurde_retombe_sur_un_defaut",
              isinstance(V.journal(racine, -5), list))

        # --- robustesse : pas de dépôt ---------------------------------------
        check("test_repertoire_sans_depot_ne_leve_pas",
              V.est_un_depot(dehors) is False and V.etat(dehors) == []
              and V.journal(dehors) == [] and V.diff(dehors) == (None, False))
        check("test_repertoire_inexistant_ne_leve_pas",
              V.est_un_depot("/nexiste/pas") is False
              and V.branche("/nexiste/pas") is None)

        check("test_branche_lue", V.branche(racine) == "main")

    print("\n%d cas, %d echec(s)" % (_tests, len(_echecs)))
    for echec in _echecs:
        print("  - %s" % echec)
    return 1 if _echecs else 0


if __name__ == "__main__":
    # Même filet que les autres suites : une exception ne doit pas faire
    # disparaître le résumé. `sys.exit()` reste HORS du try — l'y mettre
    # attrapait son propre SystemExit et transformait un succès en échec.
    try:
        _code = main()
    except Exception as erreur:
        print("\n%d cas atteints, INTERROMPU par %s : %s"
              % (_tests, type(erreur).__name__, erreur))
        print("  (le compte ci-dessus est un MINORANT)")
        _code = 1
    sys.exit(_code)
