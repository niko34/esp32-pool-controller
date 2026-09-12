#!/usr/bin/env python3
"""Lecture de git — fichiers touchés, diffs, historique.

Git est la source de vérité de l'état du code, comme les specs le sont de l'état
fonctionnel. Ce module ne fait que **lire** : aucun `add`, aucun `commit`, aucun
`checkout`. Le dépôt appartient à l'humain et aux agents.

## Deux invariants de sûreté, et pourquoi ils sont ici

**1. Aucun shell, jamais.** Toutes les commandes partent en `argv`, avec
`shell=False` (le défaut de `subprocess`). Un nom de fichier venu du navigateur
ne peut donc pas contenir `; rm -rf /` : il arrive à git comme un argument, pas
comme du texte à interpréter.

**2. Aucun chemin hors du dépôt.** `chemin_sur()` est le point de passage unique,
et il résout les liens symboliques des deux côtés avant de comparer. C'est la
même garde que celle du collecteur d'événements — une revue a fait remarquer
qu'elle vivait dans le collecteur et pas ici, alors que le serveur en aura besoin
pour répondre au navigateur.

⚠️ `--` sépare systématiquement les options des chemins dans les appels à git.
Sans lui, un fichier nommé `--output=…` serait lu comme une option.

## Ce qu'on ne fait pas

Pas de bibliothèque git. `subprocess` et le binaire suffisent, ils se comportent
identiquement sur GitHub et GitLab, et ils n'ajoutent aucune dépendance à porter
dans chaque projet issu du template.
"""

import os
import subprocess

# Un diff est fait pour être lu par un humain dans une page web. Au-delà, on
# tronque et on le DIT : mieux vaut un diff annoncé incomplet qu'une page de
# quatre mégaoctets ou, pire, un diff silencieusement coupé.
_MAX_DIFF = 200_000

# Une commande git qui ne rend pas la main est un serveur qui ne rend pas la main.
_TIMEOUT = 15

# Plafond du nombre de commits demandables. Constante nommée, et non littéral
# enfoui : sans cela, la borne n'était exerçable par aucun test — le dépôt de
# test aurait dû contenir plus de 200 commits. Le mutant qui la supprimait
# survivait donc, et la borne n'était protégée par rien.
_MAX_COMMITS = 200

# Codes de `git status --porcelain`, traduits en intention lisible. On garde le
# code brut à côté : traduire, c'est déjà interpréter.
_LIBELLES = {
    "M": "modifié", "A": "ajouté", "D": "supprimé", "R": "renommé",
    "C": "copié", "U": "conflit", "?": "non suivi", "!": "ignoré",
}


def _git(racine, *arguments, timeout=_TIMEOUT):
    """Exécute git en lecture. Rend (succès, sortie).

    Ne lève jamais : un dépôt absent, un git manquant ou une commande en échec
    rendent (False, message). Le serveur doit pouvoir afficher « git
    indisponible » au lieu de renvoyer une erreur 500.
    """
    if not os.path.isdir(racine):
        return False, "répertoire introuvable"
    try:
        resultat = subprocess.run(
            ["git", "-C", racine] + list(arguments),
            capture_output=True, text=True, timeout=timeout,
            # shell=False est le défaut et DOIT le rester : c'est ce qui rend un
            # nom de fichier inoffensif quoi qu'il contienne.
        )
    except (OSError, subprocess.SubprocessError) as erreur:
        return False, str(erreur)
    if resultat.returncode != 0:
        return False, (resultat.stderr or "").strip() or "git a échoué"
    return True, resultat.stdout


def chemin_sur(racine, chemin):
    """Chemin relatif au dépôt, ou None s'il en sort.

    C'est le SEUL point d'entrée autorisé pour un chemin venu de l'extérieur.
    Les liens symboliques sont résolus des deux côtés : un lien qui pointe hors
    du dépôt est refusé, pas suivi.
    """
    if not isinstance(chemin, str) or not chemin or not isinstance(racine, str):
        return None
    # Un octet nul dans un chemin fait lever OSError à `realpath` sur certaines
    # plateformes et passe sur d'autres : on le refuse explicitement.
    if "\x00" in chemin:
        return None
    try:
        if not os.path.isabs(chemin):
            chemin = os.path.join(racine, chemin)
        absolu = os.path.realpath(chemin)
        base = os.path.realpath(racine)
    except (OSError, ValueError):
        return None
    if absolu == base:
        return None  # la racine elle-même n'est pas un fichier à servir
    if not absolu.startswith(base + os.sep):
        return None
    return os.path.relpath(absolu, base)


def est_un_depot(racine):
    ok, sortie = _git(racine, "rev-parse", "--is-inside-work-tree")
    return ok and sortie.strip() == "true"


def branche(racine):
    ok, sortie = _git(racine, "rev-parse", "--abbrev-ref", "HEAD")
    return sortie.strip() if ok else None


def etat(racine):
    """Fichiers touchés dans l'arbre de travail.

    Utilise `-z` : les noms sont séparés par des octets nuls, donc un fichier
    dont le nom contient une espace, un guillemet ou un saut de ligne est rendu
    correctement. Le format texte, lui, échappe et cite — et se relit mal.
    """
    ok, sortie = _git(racine, "status", "--porcelain", "-z")
    if not ok:
        return []
    entrees = []
    morceaux = sortie.split("\0")
    i = 0
    while i < len(morceaux):
        morceau = morceaux[i]
        i += 1
        if len(morceau) < 4:
            continue
        index, arbre, chemin = morceau[0], morceau[1], morceau[3:]
        # Un renommage occupe DEUX entrées : la nouvelle puis l'ancienne.
        ancien = None
        if index in ("R", "C"):
            if i < len(morceaux):
                ancien = morceaux[i]
                i += 1
        code = index if index != " " else arbre
        entrees.append({
            "chemin": chemin,
            "ancien": ancien,
            "code": (index + arbre).strip(),
            "libelle": _LIBELLES.get(code, code),
            # Un fichier peut être à la fois indexé et modifié après coup : les
            # deux colonnes sont distinctes et on ne les écrase pas l'une par
            # l'autre.
            "indexe": index not in (" ", "?"),
            "modifie_hors_index": arbre not in (" ",),
        })
    return entrees


def diff(racine, chemin=None, indexe=False, taille_max=_MAX_DIFF):
    """Diff de l'arbre de travail, ou d'un fichier précis.

    Rend (texte, tronque). `tronque` est vrai quand le diff dépassait la borne —
    l'appelant DOIT le dire à l'utilisateur : un diff coupé en silence se lit
    comme un diff complet, et c'est un mensonge sur l'état du code.
    """
    arguments = ["diff", "--no-color"]
    if indexe:
        arguments.append("--cached")
    if chemin is not None:
        relatif = chemin_sur(racine, chemin)
        if relatif is None:
            return None, False
        # `--` ferme la liste des options : un fichier nommé `--stat` reste un
        # fichier.
        arguments += ["--", relatif]
    ok, sortie = _git(racine, *arguments)
    if not ok:
        return None, False
    if len(sortie) > taille_max:
        return sortie[:taille_max], True
    return sortie, False


def journal(racine, limite=20):
    """Derniers commits : hash court, date ISO, auteur, sujet.

    Séparateur `\\x1f` (unité) entre champs et `\\x1e` (enregistrement) entre
    commits : ces octets ne peuvent pas apparaître dans un sujet de commit, là
    où une tabulation ou un `|` le peuvent parfaitement.
    """
    if not isinstance(limite, int) or limite < 1:
        limite = 20
    limite = min(limite, _MAX_COMMITS)
    ok, sortie = _git(racine, "log", "-n", str(limite),
                      "--pretty=format:%h\x1f%aI\x1f%an\x1f%s\x1e")
    if not ok:
        return []
    commits = []
    for enregistrement in sortie.split("\x1e"):
        enregistrement = enregistrement.strip("\n")
        if not enregistrement:
            continue
        champs = enregistrement.split("\x1f")
        if len(champs) != 4:
            continue
        commits.append({"hash": champs[0], "date": champs[1],
                        "auteur": champs[2], "sujet": champs[3]})
    return commits


def fichiers_du_commit(racine, reference):
    """Fichiers touchés par un commit. La référence est VALIDÉE avant usage.

    ⚠️ Une référence vient potentiellement du navigateur. On refuse tout ce qui
    ressemble à une option (`-`) ou à un chemin, et on laisse ensuite git
    trancher : lui seul sait ce qu'est une référence valide.
    """
    if not isinstance(reference, str) or not reference:
        return []
    # LISTE BLANCHE de caractères : c'est la garde qui porte. Elle exclut `-`
    # (donc toute option), `/` (donc tout chemin), l'espace, `;`, `$`, `(`…
    if not all(c.isalnum() or c in "_.^~@" for c in reference):
        return []
    # `..` passe la liste blanche (le point y est admis, pour `HEAD^^` et les
    # noms de tag) et désignerait un INTERVALLE de commits, pas un commit.
    #
    # ⚠️ Aucun test ne tue le mutant qui supprime cette ligne : `git show ..`
    # échoue de lui-même et rend une liste vide, exactement comme le refus. La
    # garde est donc une ceinture par-dessus les bretelles de git, pas une
    # protection observable. Écrit ici plutôt que compté comme un mutant tué.
    if ".." in reference:
        return []
    ok, sortie = _git(racine, "show", "--name-status", "--pretty=format:", reference)
    if not ok:
        return []
    fichiers = []
    for ligne in sortie.splitlines():
        if not ligne.strip():
            continue
        morceaux = ligne.split("\t")
        if len(morceaux) < 2:
            continue
        code = morceaux[0][0]
        fichiers.append({"code": morceaux[0], "libelle": _LIBELLES.get(code, code),
                         "chemin": morceaux[-1]})
    return fichiers


if __name__ == "__main__":
    import json
    import sys

    racine = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    print(json.dumps({
        "racine": racine,
        "est_un_depot": est_un_depot(racine),
        "branche": branche(racine),
        "touches": etat(racine),
        "derniers_commits": journal(racine, 3),
    }, ensure_ascii=False, indent=2))
    texte, tronque = diff(racine)
    print("diff de l'arbre : %d caractères%s"
          % (len(texte or ""), " (TRONQUÉ)" if tronque else ""))
