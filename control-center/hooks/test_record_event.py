#!/usr/bin/env python3
"""Tests du collecteur d'événements — sans dépendance, lançables tels quels.

    python3 control-center/hooks/test_record_event.py

Le cœur de ces tests est la NON-FUITE. Le collecteur reçoit toute commande shell
exécutée par Claude Code ; ce dépôt en exécute qui portent le mot de passe de
l'appareil, et `ota_update.sh` documente lui-même `POOL_PASSWORD=… ./ota_update.sh`
comme un usage normal. Un journal qui recopie les deux premiers jetons d'une
commande recopie donc ce mot de passe.

Le défaut a été constaté sur le PREMIER événement réel produit par le collecteur,
avant toute mise en service. Ces tests existent pour qu'il ne revienne pas.

VALIDATION PAR MUTATION (discipline du dépôt, cf. CLAUDE.md étape 6) — casser
`_command_head` en retirant le filtre `"=" in token or ":" in token` doit faire
mourir `test_affectation_en_tete` ET `test_identifiant_inline`. Une suite qui ne
tue pas ce mutant ne prouve rien.
"""

import json
import os
import subprocess
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_SCRIPT = os.path.join(_HERE, "record-event.py")

# Un faux secret, jamais le vrai : ce fichier est versionné.
#
# ⚠️ IL DOIT ÊTRE COURT. `_command_head()` a DEUX gardes qui écartent un jeton :
# le filtre `=`/`:` et un plafond de longueur (`_MAX_TOKEN`). Un faux secret long
# est écarté par le plafond AVANT d'atteindre le filtre — le test passe alors
# sans jamais exercer la garde qu'il prétend protéger.
#
# Constaté par mutation : avec un secret de 29 caractères, retirer entièrement le
# filtre `=`/`:` ne tuait AUCUN des deux tests de non-fuite. Un mot de passe court
# aurait fui. `POOL_PASSWORD=Pw4x` fait 17 caractères : sous le plafond, donc seul
# le filtre peut le sauver.
_SECRET = "Pw4x"

_echecs = []
_tests = 0


def _run(payload, journal_root):
    """Exécute le collecteur avec un journal isolé, rend (code, événements)."""
    env = dict(os.environ)
    # Le collecteur écrit à côté de lui-même ; on l'exécute donc depuis une copie
    # placée dans un arbre jetable, pour ne jamais polluer le vrai journal.
    result = subprocess.run(
        [sys.executable, os.path.join(journal_root, "hooks", "record-event.py")],
        input=json.dumps(payload),
        text=True,
        capture_output=True,
        env=env,
    )
    journal = os.path.join(journal_root, "var", "agent-events.jsonl")
    events = []
    if os.path.exists(journal):
        with open(journal, encoding="utf-8") as handle:
            events = [json.loads(line) for line in handle if line.strip()]
    return result.returncode, events


def _bash(command):
    return {
        "session_id": "t",
        "hook_event_name": "PostToolUse",
        "tool_name": "Bash",
        "cwd": "/root/esp32-pool-controller",
        "tool_input": {"command": command},
    }


def check(nom, condition, detail=""):
    global _tests
    _tests += 1
    if not condition:
        _echecs.append("%s — %s" % (nom, detail))
        print("  ECHEC  %s%s" % (nom, (" : " + detail) if detail else ""))
    else:
        print("  ok     %s" % nom)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "hooks"))
        with open(_SCRIPT, encoding="utf-8") as src:
            source = src.read()
        with open(os.path.join(tmp, "hooks", "record-event.py"), "w", encoding="utf-8") as dst:
            dst.write(source)

        def journal_text():
            path = os.path.join(tmp, "var", "agent-events.jsonl")
            if not os.path.exists(path):
                return ""
            with open(path, encoding="utf-8") as handle:
                return handle.read()

        # --- NON-FUITE ------------------------------------------------------
        code, events = _run(_bash("POOL_PASSWORD=%s ./ota_update.sh firmware" % _SECRET), tmp)
        check("test_affectation_en_tete — le secret n'est pas journalise",
              _SECRET not in journal_text(),
              "le journal contient le secret : %r" % events[-1].get("command_head"))
        check("test_affectation_en_tete — l'info utile survit",
              events[-1].get("command_head") == "./ota_update.sh firmware",
              "obtenu %r" % events[-1].get("command_head"))

        _run(_bash("curl -u admin:%s http://192.168.1.48/" % _SECRET), tmp)
        check("test_identifiant_inline — le secret n'est pas journalise",
              _SECRET not in journal_text())

        # Le plafond de longueur est la SECONDE ligne de défense : il écarte un
        # jeton qui ne porte ni `=` ni `:` mais reste manifestement une donnée et
        # non un nom de programme — un jeton d'API collé en argument, par exemple.
        # Sans ce cas, retirer entièrement le plafond ne tuait aucun test
        # (constaté par mutation).
        long_token = "s3cr3t" * 6  # 36 caractères, ni "=" ni ":"
        _, events = _run(_bash("./deploy.sh %s" % long_token), tmp)
        check("test_jeton_trop_long_ecarte",
              long_token not in journal_text() and events[-1].get("command_head") == "./deploy.sh",
              "obtenu %r" % events[-1].get("command_head"))

        # --- CHEMINS --------------------------------------------------------
        payload = {
            "session_id": "t", "hook_event_name": "PostToolUse", "tool_name": "Edit",
            "cwd": "/root/esp32-pool-controller",
            "tool_input": {"file_path": "/etc/passwd"},
        }
        _, events = _run(payload, tmp)
        check("test_chemin_hors_depot_rejete", "file" not in events[-1],
              "un chemin hors du depot a ete journalise : %r" % events[-1].get("file"))

        payload["tool_input"]["file_path"] = "/root/esp32-pool-controller/src/main.cpp"
        _, events = _run(payload, tmp)
        check("test_chemin_interne_relativise", events[-1].get("file") == "src/main.cpp",
              "obtenu %r" % events[-1].get("file"))

        # --- ROBUSTESSE : le hook ne doit JAMAIS faire echouer la session ----
        for nom, entree in (("json_invalide", "pas du json"), ("entree_vide", "")):
            result = subprocess.run(
                [sys.executable, os.path.join(tmp, "hooks", "record-event.py")],
                input=entree, text=True, capture_output=True)
            check("test_%s — code de retour 0" % nom, result.returncode == 0,
                  "code %d" % result.returncode)

        # --- ORDRE : la resolution doit separer deux evenements consecutifs --
        before = len(journal_text().splitlines())
        for i in range(3):
            _run(_bash("pio test"), tmp)
        horodatages = [json.loads(l)["ts"] for l in journal_text().splitlines()[before:]]
        check("test_horodatage_strictement_croissant",
              all(a < b for a, b in zip(horodatages, horodatages[1:])),
              "ordre indecidable : %r" % horodatages)

        # --- ABSENCE ≠ VALEUR (ADR-0072 du depot) ---------------------------
        _, events = _run(_bash("A=1 B=2"), tmp)
        check("test_commande_entierement_filtree_absente_du_journal",
              "command_head" not in events[-1],
              "une commande sans jeton sur doit OMETTRE le champ, pas ecrire une chaine vide")

        _, events = _run({"session_id": "t", "hook_event_name": "SubagentStart",
                          "agent_type": "test-engineer", "agent_id": "a9"}, tmp)
        check("test_sous_agent_identifie",
              events[-1].get("agent_type") == "test-engineer" and events[-1].get("agent_id") == "a9")

        _, events = _run({"session_id": "t", "hook_event_name": "Stop"}, tmp)
        check("test_agent_principal_sans_agent_id",
              "agent_id" not in events[-1] and "agent_type" not in events[-1],
              "l'absence d'agent_id IDENTIFIE l'agent principal : ne jamais la combler")

    print("\n%d cas, %d echec(s)" % (_tests, len(_echecs)))
    for echec in _echecs:
        print("  - %s" % echec)
    return 1 if _echecs else 0


if __name__ == "__main__":
    sys.exit(main())
