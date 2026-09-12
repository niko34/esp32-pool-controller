#!/usr/bin/env python3
"""Assistant d'installation du centre de contrôle dans un projet.

    python3 control-center/installer.py            # examine et propose
    python3 control-center/installer.py --ecrire   # écrit control-center.toml

Le centre de contrôle arrive dans un projet par copie ou par
`tools/update_kit.sh`. Il lui manque alors deux choses, et une seule peut être
automatisée sans risque.

## Ce que cet assistant fait

Il **découvre** ce qu'il peut lire du projet — répertoires de specs réellement
présents, répertoire d'agents, commandes de test devinées d'après les fichiers
de build — et en tire un `control-center.toml` pré-rempli.

## Ce qu'il ne fait PAS, et pourquoi

**Il n'écrit jamais dans `.claude/settings.json`.** C'est le fichier le plus
personnalisé d'un projet : il porte les permissions, les refus, les garde-fous
que quelqu'un a choisis un par un. Un script qui y écrit, même prudemment, finit
un jour par écraser une décision. L'assistant AFFICHE donc le bloc de hooks à
coller, et s'arrête là.

**Il n'écrase jamais un `control-center.toml` existant.** Une configuration en
place a été relue par quelqu'un ; la regénérer effacerait ce travail. L'assistant
signale les écarts et laisse décider.

## Ce qu'il ne peut pas deviner

Les libellés de colonnes, le vocabulaire des statuts propre au projet, et la
décision d'ouvrir ou non le serveur au réseau. Il le dit explicitement plutôt que
de choisir à votre place.
"""

import argparse
import json
import os
import subprocess
import sys

_ICI = os.path.dirname(os.path.abspath(__file__))

# Indices de build → commandes plausibles. Ce sont des PROPOSITIONS, annoncées
# comme telles : l'assistant ne lance jamais ce qu'il devine.
_INDICES = [
    ("platformio.ini", {"tests": ["pio", "test", "-e", "native"],
                        "build": ["pio", "run"]}),
    ("pyproject.toml", {"tests": ["python3", "-m", "pytest", "-q"]}),
    ("pytest.ini", {"tests": ["python3", "-m", "pytest", "-q"]}),
    ("Makefile", {"build": ["make"]}),
    ("package.json", {"tests": ["npm", "test"], "build": ["npm", "run", "build"]}),
    ("Cargo.toml", {"tests": ["cargo", "test"], "build": ["cargo", "build"]}),
    ("go.mod", {"tests": ["go", "test", "./..."], "build": ["go", "build", "./..."]}),
]

_EVENEMENTS = ("SessionStart", "SessionEnd", "SubagentStart", "SubagentStop",
               "UserPromptSubmit", "Stop", "Notification", "PostToolUseFailure")
_COMMANDE_HOOK = 'python3 "$CLAUDE_PROJECT_DIR/control-center/hooks/record-event.py"'


def _racine_projet():
    """Racine du dépôt git, ou le parent de `control-center/` à défaut."""
    try:
        sortie = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                                cwd=_ICI, capture_output=True, text=True, timeout=10)
        if sortie.returncode == 0 and sortie.stdout.strip():
            return sortie.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return os.path.dirname(_ICI)


def _colonnes_trouvees(racine):
    """Répertoires de specs réellement présents, dans un ordre plausible.

    On ne propose QUE ce qui existe : inventer une colonne `doing/` dans un
    projet qui n'en a pas produirait un Kanban avec une colonne morte.
    """
    candidats = [
        ("specs/features", ["todo", "doing", "done"]),
        ("specs", ["todo", "doing", "done"]),
        ("docs/specs", ["todo", "doing", "done"]),
    ]
    for relatif, sous in candidats:
        base = os.path.join(racine, relatif)
        if not os.path.isdir(base):
            continue
        presents = [s for s in sous if os.path.isdir(os.path.join(base, s))]
        if presents:
            return relatif, presents
    return None, []


def _commandes_devinees(racine):
    trouvees = {}
    indices = []
    for fichier, commandes in _INDICES:
        if os.path.exists(os.path.join(racine, fichier)):
            indices.append(fichier)
            for cle, argv in commandes.items():
                trouvees.setdefault(cle, argv)
    return trouvees, indices


def _hooks_installes(racine):
    """(présents, lisible) — `lisible` distingue « absent » de « illisible »."""
    chemin = os.path.join(racine, ".claude", "settings.json")
    if not os.path.exists(chemin):
        return False, False
    try:
        with open(chemin, encoding="utf-8") as fichier:
            reglages = json.load(fichier)
    except (OSError, ValueError):
        return False, False
    hooks = reglages.get("hooks")
    if not isinstance(hooks, dict):
        return False, True
    return "SubagentStart" in hooks, True


def _bloc_hooks():
    entrees = {}
    for evenement in _EVENEMENTS:
        entrees[evenement] = [{"hooks": [{"type": "command",
                                          "command": _COMMANDE_HOOK,
                                          "timeout": 5}]}]
    entrees["PostToolUse"] = [{"matcher": "Bash|Edit|Write|NotebookEdit",
                               "hooks": [{"type": "command",
                                          "command": _COMMANDE_HOOK,
                                          "timeout": 5}]}]
    return json.dumps({"hooks": entrees}, ensure_ascii=False, indent=2)


def _rendre_toml(nom, racine_specs, colonnes, commandes):
    lignes = [
        "# Configuration du centre de contrôle — écrite par control-center/installer.py.",
        "# Relisez-la : l'assistant devine ce qu'il peut lire, pas ce que vous voulez.",
        "",
        "[projet]",
        'nom = "%s"' % nom.replace('"', "'"),
        "",
    ]
    if racine_specs:
        lignes += ["[specs]", 'racine = "%s"' % racine_specs, ""]
        for repertoire in colonnes:
            lignes += ["[[specs.colonnes]]",
                       'repertoire = "%s"' % repertoire,
                       '# libelle = "…"   # ⚠️ À COMPLÉTER : « %s » ne dit pas si le'
                       % repertoire,
                       "#                 #   travail avance ou s'il est bloqué.",
                       ""]
    if commandes:
        lignes += ["[commandes]",
                   "# Liste FERMÉE : l'interface choisit une CLÉ, jamais une ligne de",
                   "# commande. Vérifiez-les — elles sont DEVINÉES d'après les fichiers",
                   "# de build trouvés, et n'ont jamais été exécutées.",
                   ""]
        for cle, argv in sorted(commandes.items()):
            lignes.append("%s = %s" % (cle, json.dumps(argv, ensure_ascii=False)))
        lignes.append("")
    lignes += [
        "[serveur]",
        "# 127.0.0.1 par défaut. Ouvrir au réseau local est une décision explicite :",
        '# hote = "0.0.0.0"',
        'hote = "127.0.0.1"',
        "port = 8787",
        "",
        "[garde_fous]",
        "budget_max_usd = 5.0",
        "ecrivains_simultanes = 1",
        "",
    ]
    return "\n".join(lignes)


def main():
    analyseur = argparse.ArgumentParser(description=__doc__)
    analyseur.add_argument("--ecrire", action="store_true",
                           help="écrit control-center.toml (sans jamais écraser)")
    options = analyseur.parse_args()

    racine = _racine_projet()
    cible = os.path.join(_ICI, "control-center.toml")
    nom = os.path.basename(racine)

    print("Centre de contrôle — installation")
    print("  projet : %s" % racine)

    if sys.version_info < (3, 11):
        print("\n❌ Python %d.%d — il en faut 3.11 ou plus (tomllib)."
              % sys.version_info[:2])
        return 1
    print("  python : %d.%d ✓" % sys.version_info[:2])

    racine_specs, colonnes = _colonnes_trouvees(racine)
    if colonnes:
        print("  specs  : %s → %s" % (racine_specs, ", ".join(colonnes)))
    else:
        print("  specs  : aucun répertoire reconnu — le Kanban restera vide")

    commandes, indices = _commandes_devinees(racine)
    if commandes:
        print("  build  : %s → %s" % (", ".join(indices), ", ".join(sorted(commandes))))
    else:
        print("  build  : aucun fichier de build reconnu")

    agents = os.path.isdir(os.path.join(racine, ".claude", "agents"))
    print("  agents : %s" % (".claude/agents ✓" if agents else "absent"))

    # --- la configuration ---------------------------------------------------
    print("")
    if os.path.exists(cible):
        print("✓ control-center.toml existe déjà — NON modifié.")
        print("  Une configuration en place a été relue par quelqu'un ; la")
        print("  regénérer effacerait ce travail.")
    elif options.ecrire:
        with open(cible, "w", encoding="utf-8") as fichier:
            fichier.write(_rendre_toml(nom, racine_specs, colonnes, commandes))
        print("✓ control-center.toml écrit. RELISEZ-LE :")
        print("  · les libellés de colonnes sont à compléter ;")
        print("  · les commandes sont DEVINÉES, jamais exécutées ;")
        print("  · l'écoute est en local — l'ouvrir au réseau est votre décision.")
    else:
        print("→ control-center.toml absent. Pour l'écrire :")
        print("      python3 control-center/installer.py --ecrire")

    # --- les hooks ----------------------------------------------------------
    print("")
    installes, lisible = _hooks_installes(racine)
    if installes:
        print("✓ Les hooks sont en place dans .claude/settings.json.")
    else:
        if not lisible:
            print("⚠️  .claude/settings.json absent ou illisible.")
        print("→ SANS HOOKS, LE JOURNAL RESTE VIDE et le tableau de bord n'affiche")
        print("  rien. Collez ce bloc dans .claude/settings.json, au même niveau")
        print('  que "permissions" :')
        print("")
        for ligne in _bloc_hooks().split("\n")[1:-1]:
            print("  " + ligne)
        print("")
        print("  ⚠️ Cet assistant n'écrit PAS dans settings.json à votre place :")
        print("     ce fichier porte vos permissions et vos refus, choisis un par")
        print("     un. Un script qui y écrit finit un jour par écraser une")
        print("     décision.")

    print("")
    print("Ensuite :  python3 control-center/backend/server.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
