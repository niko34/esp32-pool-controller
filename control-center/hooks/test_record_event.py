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

⚠️ UNE PROPRIÉTÉ N'EST PAS COUVERTE ICI, et c'est assumé : l'écriture en UN SEUL
appel `os.write()` sur un descripteur `O_APPEND`. Scinder cet appel en deux ne
tue aucun test — l'entrelacement n'apparaît qu'avec plusieurs écrivains
concurrents, et un test de course serait intermittent. Un test intermittent vaut
moins que pas de test : il apprend à ignorer les échecs. La propriété tient donc
par CONSTRUCTION (un appel, pas de tampon), et ce commentaire est sa seule garde.
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
        # ⚠️ Répertoire courant DIFFÉRENT de celui des charges de test. Sans
        # cela, un chemin relatif résolu contre le cwd du processus donnait le
        # même résultat que résolu contre celui de la charge : le test passait
        # sans rien prouver, et retirer la résolution ne tuait aucun mutant.
        cwd=journal_root,
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

        # Second jeton POSITIONNEL : ni `=`, ni `:`, ni 24 caractères — les deux
        # premières gardes le laissaient passer. Prouvé en revue sur
        # `echo hunter2 | sudo -S reboot`.
        _, events = _run(_bash("echo %s | sudo -S reboot" % _SECRET), tmp)
        check("test_second_jeton_positionnel_ecarte",
              _SECRET not in journal_text() and events[-1].get("command_head") == "echo",
              "obtenu %r" % events[-1].get("command_head"))

        check("test_sous_commande_normale_survit_a_la_regle_de_forme",
              all(_run(_bash(cmd), tmp)[1][-1].get("command_head") == attendu
                  for cmd, attendu in (("pio test -e native", "pio test"),
                                       ("git commit -F /tmp/m", "git commit"),
                                       ("./ota_update.sh firmware 1.2.3.4", "./ota_update.sh firmware"),
                                       ("python3 -c import os", "python3 -c"))),
              "la garde ne doit pas manger les sous-commandes légitimes")

        # `description` est LIBRE et rédigée par le modèle : elle recopie
        # volontiers ce que l'utilisateur a collé. Un champ libre à côté d'un
        # champ durci annule le durcissement — fuite démontrée en revue.
        _, events = _run({
            "session_id": "t", "hook_event_name": "PostToolUse", "tool_name": "Bash",
            "cwd": "/root/esp32-pool-controller",
            "tool_input": {"command": "pio test",
                           "description": "Deployer en OTA avec le mot de passe %s" % _SECRET},
        }, tmp)
        check("test_description_jamais_journalisee",
              _SECRET not in journal_text() and "description" not in events[-1],
              "événement : %r" % events[-1])

        # Une ligne non bornée casserait l'atomicité de l'écriture en ajout.
        avant = len(journal_text().splitlines())
        _run({"session_id": "t" * 5000, "hook_event_name": "Stop"}, tmp)
        apres = len(journal_text().splitlines())
        check("test_champ_demesure_borne_et_ligne_ecrite", apres == avant + 1,
              "un champ démesuré doit être tronqué, pas faire perdre l'événement")
        # Un événement LÉGITIME dont tous les champs repris sont au maximum doit
        # être écrit, pas écarté par le plafond de ligne.
        # 5 000 caractères par champ : bien au-delà de toute borne plausible, pour
        # que le pire cas EXERCE réellement le rapport `_MAX_CHAMP` × nombre de
        # champs / `_MAX_LIGNE`. Avec 400, le test passait quelle que soit la
        # borne — il ne prouvait rien (constaté par mutation).
        maximal = {c: "x" * 5000 for c in ("session_id", "hook_event_name", "agent_id",
                                          "agent_type", "cwd", "transcript_path",
                                          "permission_mode", "prompt_id")}
        avant2 = len(journal_text().splitlines())
        _run(maximal, tmp)
        check("test_evenement_maximal_n_est_pas_perdu",
              len(journal_text().splitlines()) == avant2 + 1,
              "le pire cas de champs bornés doit tenir sous le plafond de ligne")

        check("test_ligne_reste_sous_le_plafond",
              all(len(l.encode("utf-8")) <= 4096 for l in journal_text().splitlines()),
              "une ligne dépasse le plafond : elle pourrait s'entrelacer")

        # --- CHEMINS --------------------------------------------------------
        payload_relatif = {
            "session_id": "t", "hook_event_name": "PostToolUse", "tool_name": "Edit",
            "cwd": "/root/esp32-pool-controller",
            "tool_input": {"file_path": "src/main.cpp"},
        }
        _, events = _run(payload_relatif, tmp)
        check("test_chemin_relatif_resolu_contre_le_cwd_de_la_charge",
              events[-1].get("file") == "src/main.cpp",
              "résolu contre le cwd du PROCESSUS, il désignerait un autre arbre ; obtenu %r"
              % events[-1].get("file"))

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
