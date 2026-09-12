#!/usr/bin/env python3
"""Tests de la reconstruction des workflows.

    python3 control-center/backend/test_events.py

Sans dépendance. Les cas viennent de comportements RÉELLEMENT observés sur le
journal de production, pas d'hypothèses : le runtime émet des `SubagentStop`
sans `SubagentStart`, et il ne met pas le nom de l'agent dans la fermeture.

Le fil conducteur est le même partout : **ne jamais produire un récit plus
affirmatif que les événements**. Un passage sans fermeture est en cours, pas
terminé ; une session sans fin n'est pas active, elle est sans fin observée ;
une durée non mesurable est `None`, jamais 0.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import events as E  # noqa: E402

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


def ev(ts, nom, **extra):
    base = {"ts": ts, "hook_event_name": nom, "session_id": "s1", "runtime": "claude-code"}
    base.update(extra)
    return base


def T(seconde, ms=0):
    return "2026-09-12T10:%02d:%02d.%03dZ" % (seconde // 60, seconde % 60, ms)


def un(workflows):
    return workflows[0] if workflows else None


def main():
    # --- cas nominal : un passage complet -----------------------------------
    w = un(E.reconstruire([
        ev(T(0), "SessionStart"),
        ev(T(1), "SubagentStart", agent_type="test-engineer", agent_id="a1"),
        ev(T(3), "PostToolUse", tool="Bash", command_head="pio test", file="src/main.cpp"),
        ev(T(5), "SubagentStop", agent_type="", agent_id="a1"),
        ev(T(6), "SessionEnd"),
    ], ("pio test",)))
    check("test_passage_complet", len(w["passages"]) == 1 and w["passages"][0]["termine"])
    check("test_duree_mesuree", w["passages"][0]["duree_ms"] == 4000,
          "obtenu %r" % w["passages"][0]["duree_ms"])
    check("test_nom_vient_de_l_ouverture", w["passages"][0]["agent"] == "test-engineer",
          "SubagentStop ne porte PAS le nom (agent_type vide en production) : "
          "l'appariement doit se faire par agent_id")
    check("test_session_close_reconnue", w["fin_observee_absente"] is False)
    check("test_commande_suivie_comptee", w["commandes"].get("pio test") == 1)

    # --- fermeture orpheline : OBSERVÉE en production ------------------------
    # Le runtime émet un SubagentStop par fin de tour de l'agent principal, avec
    # un agent_id neuf et un agent_type vide. Il ne doit fermer AUCUN passage.
    w = un(E.reconstruire([
        ev(T(1), "SubagentStart", agent_type="architect", agent_id="a1"),
        ev(T(2), "Stop"),
        ev(T(3), "SubagentStop", agent_type="", agent_id="INTRUS"),
    ]))
    check("test_fermeture_orpheline_comptee", w["fermetures_orphelines"] == 1)
    check("test_fermeture_orpheline_ne_ferme_pas_le_passage_en_cours",
          w["passages"][0]["termine"] is False and w["passages"][0]["duree_ms"] is None,
          "une fermeture étrangère ne doit pas clore le vrai passage")

    # --- passage jamais fermé -----------------------------------------------
    w = un(E.reconstruire([ev(T(1), "SubagentStart", agent_type="doc-writer", agent_id="a1")]))
    check("test_passage_sans_fermeture_reste_en_cours",
          w["passages"][0]["termine"] is False and w["passages"][0]["duree_ms"] is None,
          "on n'invente ni durée ni verdict")
    check("test_session_sans_fin_n_est_pas_close", w["fin_observee_absente"] is True)

    # --- le travail de l'AGENT PRINCIPAL ne disparaît pas --------------------
    # Défaut relevé en revue : 33 appels d'outils ne rapportaient qu'un fichier,
    # parce que tout ce qui tombait hors d'un passage était jeté.
    w = un(E.reconstruire([
        ev(T(1), "PostToolUse", tool="Write", file="a.py"),
        ev(T(2), "PostToolUse", tool="Write", file="b.py"),
        ev(T(3), "SubagentStart", agent_type="test-engineer", agent_id="a1"),
        ev(T(4), "PostToolUse", tool="Write", file="c.py"),
        ev(T(5), "SubagentStop", agent_id="a1"),
        ev(T(6), "PostToolUse", tool="Write", file="d.py"),
    ]))
    check("test_travail_de_l_agent_principal_conserve",
          w["fichiers"] == ["a.py", "b.py", "c.py", "d.py"],
          "obtenu %r" % w["fichiers"])
    check("test_le_passage_ne_retient_que_ses_propres_fichiers",
          w["passages"][0]["fichiers"] == ["c.py"],
          "obtenu %r" % w["passages"][0]["fichiers"])

    # --- reprises : la trace observable d'une boucle de correction ----------
    w = un(E.reconstruire([
        ev(T(1), "SubagentStart", agent_type="test-engineer", agent_id="a1"),
        ev(T(2), "PostToolUseFailure"),
        ev(T(3), "SubagentStop", agent_id="a1"),
        ev(T(4), "SubagentStart", agent_type="debugger", agent_id="a2"),
        ev(T(5), "SubagentStop", agent_id="a2"),
        ev(T(6), "SubagentStart", agent_type="test-engineer", agent_id="a3"),
        ev(T(7), "SubagentStop", agent_id="a3"),
    ]))
    check("test_reprise_detectee", w["reprises"] == {"test-engineer": 2},
          "obtenu %r" % w["reprises"])
    check("test_echec_attribue_au_bon_passage",
          w["echecs"] == 1 and w["passages"][0]["echecs"] == 1)

    # --- deux sessions entrelacées ------------------------------------------
    a = ev(T(1), "SubagentStart", agent_type="architect", agent_id="a1")
    b = dict(ev(T(2), "SubagentStart", agent_type="debugger", agent_id="b1"), session_id="s2")
    c = ev(T(3), "SubagentStop", agent_id="a1")
    workflows = E.reconstruire([a, b, c])
    check("test_deux_sessions_separees", len(workflows) == 2)
    par_session = {w["session_id"]: w for w in workflows}
    check("test_entrelacement_n_attribue_pas_de_travers",
          par_session["s1"]["passages"][0]["termine"] is True
          and par_session["s2"]["passages"][0]["termine"] is False,
          "la fermeture de s1 ne doit pas clore le passage de s2")

    # --- agent_id réutilisé --------------------------------------------------
    w = un(E.reconstruire([
        ev(T(1), "SubagentStart", agent_type="architect", agent_id="MEME"),
        ev(T(2), "SubagentStop", agent_id="MEME"),
        ev(T(3), "SubagentStart", agent_type="doc-writer", agent_id="MEME"),
        ev(T(4), "SubagentStop", agent_id="MEME"),
    ]))
    check("test_agent_id_reutilise_ferme_le_bon_passage",
          all(p["termine"] for p in w["passages"])
          and w["passages"][0]["duree_ms"] == 1000 and w["passages"][1]["duree_ms"] == 1000,
          "un identifiant réutilisé ne doit pas rouvrir ni refermer un passage clos")

    # --- horloge qui recule --------------------------------------------------
    w = un(E.reconstruire([
        ev(T(5), "SubagentStart", agent_type="architect", agent_id="a1"),
        ev(T(1), "SubagentStop", agent_id="a1"),
    ]))
    check("test_horloge_qui_recule_ferme_bien_le_passage",
          w["passages"][0]["termine"] is True and w["fermetures_orphelines"] == 0,
          "l'appariement par agent_id fonctionne quel que soit l'ordre reçu")
    # ⚠️ `reconstruire()` NE TRIE PAS : elle consomme l'ordre reçu (seul
    # `lire_journal()` trie). La fermeture est donc bien appariée à son
    # ouverture, et la durée calculée est NÉGATIVE — c'est précisément le cas que
    # le garde-fou doit ramener à None.
    #
    # Mon premier commentaire ici affirmait l'inverse (« le tri remet la
    # fermeture avant l'ouverture, elle devient orpheline »). Il était faux, et
    # vérifié seulement après coup : le passage ressort `termine=True` avec
    # `duree_ms=None`, pas orphelin.
    check("test_horloge_qui_recule_ne_produit_pas_de_duree_negative",
          all(p["duree_ms"] is None or p["duree_ms"] >= 0 for p in w["passages"]),
          "une durée négative s'afficherait comme un fait")

    # --- imbrication : signalée, pas maquillée -------------------------------
    w = un(E.reconstruire([
        ev(T(1), "SubagentStart", agent_type="architect", agent_id="a1"),
        ev(T(2), "SubagentStart", agent_type="debugger", agent_id="a2"),
    ]))
    check("test_imbrication_signalee", w["imbrication_suspectee"] is True,
          "deux ouvertures sans fermeture intercalée rendent le récit incertain")

    # --- lecture du journal ---------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        chemin = os.path.join(tmp, "j.jsonl")
        with open(chemin, "w", encoding="utf-8") as f:
            f.write('{"ts":"%s","hook_event_name":"Stop","session_id":"s1"}\n' % T(2))
            f.write('pas du json\n')
            f.write('\n')
            f.write('{"sans_ts":true}\n')
            f.write('{"ts":"%s","hook_event_name":"Stop","session_id":"s1"}' % T(1))  # sans \n
        lus = E.lire_journal(chemin)
        check("test_lignes_illisibles_ignorees_sans_perdre_le_reste", len(lus) == 2,
              "une ligne tronquée par un arrêt brutal ne doit pas rendre tout le "
              "journal inexploitable ; obtenu %d" % len(lus))
        check("test_journal_trie_chronologiquement", lus[0]["ts"] < lus[1]["ts"])

    check("test_journal_absent_rend_une_liste_vide",
          E.lire_journal("/chemin/qui/n/existe/pas.jsonl") == [])
    check("test_journal_vide_ne_produit_aucun_workflow", E.reconstruire([]) == [])
    check("test_evenement_sans_session_ignore",
          E.reconstruire([{"ts": T(1), "hook_event_name": "Stop"}]) == [],
          "sans session_id, un événement n'appartient à aucun workflow")

    print("\n%d cas, %d echec(s)" % (_tests, len(_echecs)))
    for echec in _echecs:
        print("  - %s" % echec)
    return 1 if _echecs else 0


if __name__ == "__main__":
    sys.exit(main())
