#!/usr/bin/env python3
"""Tests du serveur — étape C, lecture seule.

    python3 control-center/backend/test_server.py

Lance un vrai serveur sur un port éphémère et l'interroge par HTTP. Les cas qui
comptent ne sont pas les routes qui marchent, ce sont celles qui doivent
**refuser** : c'est la seule chose qui protégera le dépôt quand l'interface
écoutera au-delà de `localhost`.

La propriété centrale de cette étape est structurelle : `Handler` n'a pas de
`do_POST`. Un test le vérifie par HTTP (501) ET par introspection — si quelqu'un
ajoute une route d'écriture, le second échoue même si le premier est contourné.
"""

import json
import os
import sys
import threading
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import config as C  # noqa: E402
import server as SRV  # noqa: E402

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


def appeler(base, chemin, methode="GET"):
    """Rend (code, corps). Une erreur HTTP est une réponse, pas une exception."""
    requete = urllib.request.Request(base + chemin, method=methode)
    try:
        with urllib.request.urlopen(requete, timeout=10) as reponse:
            return reponse.status, reponse.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as erreur:
        return erreur.code, erreur.read().decode("utf-8", "replace")
    except Exception as erreur:  # connexion refusée, etc.
        return 0, str(erreur)


def main():
    config = C.charger()
    serveur = ThreadingHTTPServer(("127.0.0.1", 0), SRV.Handler)
    serveur.daemon_threads = True
    serveur.etat = SRV.Etat(config)
    serveur.servables = SRV._fichiers_servables()
    base = "http://127.0.0.1:%d" % serveur.server_address[1]
    fil = threading.Thread(target=serveur.serve_forever, daemon=True)
    fil.start()

    try:
        # --- LA garantie de l'étape C ---------------------------------------
        for methode in ("POST", "PUT", "DELETE", "PATCH"):
            code, _ = appeler(base, "/api/etat", methode)
            check("test_%s_refuse" % methode.lower(), code == 501,
                  "toute méthode d'écriture doit être refusée ; obtenu %d" % code)

        # Vérification STRUCTURELLE, et pas seulement par le réseau : si une
        # route d'écriture est ajoutée un jour, ce test échoue même si le
        # comportement HTTP est masqué par ailleurs.
        ecrivantes = [n for n in dir(SRV.Handler)
                      if n in ("do_POST", "do_PUT", "do_DELETE", "do_PATCH")]
        check("test_aucune_methode_d_ecriture_dans_le_code", ecrivantes == [],
              "trouvées : %r — les garde-fous de l'étape D (jeton, écrivain "
              "unique, plafond de dépense) doivent exister AVANT elles" % ecrivantes)

        # --- routes de lecture ----------------------------------------------
        code, corps = appeler(base, "/api/etat")
        check("test_etat_repond", code == 200)
        etat = json.loads(corps) if code == 200 else {}
        check("test_etat_annonce_la_lecture_seule", etat.get("lecture_seule") is True)
        check("test_etat_porte_les_specs", len(etat.get("specs", [])) > 0)
        check("test_etat_porte_les_agents", len(etat.get("agents", [])) > 0)

        # --- AUCUN champ sensible ne sort -----------------------------------
        for interdit in ("transcript_path", "/root/.claude", ".credentials",
                         "permission_mode", "prompt_id"):
            check("test_aucune_fuite_de_%s" % interdit.strip("/.").replace("/", "_"),
                  interdit not in corps,
                  "le journal contient ce champ ; la liste blanche de "
                  "`_evenement_public` doit l'écarter")

        code, _ = appeler(base, "/")
        check("test_page_servie", code == 200)
        code, _ = appeler(base, "/app.css")
        check("test_css_servi", code == 200)

        # --- ce qui doit refuser --------------------------------------------
        refus = [
            ("/api/spec?id=../../../etc/passwd", "spec par remontée"),
            ("/api/spec?id=", "spec sans identifiant"),
            ("/api/spec?id=nexiste-pas", "spec inconnue"),
            ("/api/diff?chemin=../../etc/passwd", "diff par remontée"),
            ("/api/diff?chemin=/etc/passwd", "diff par chemin absolu"),
            ("/server.py", "source du serveur"),
            ("/../backend/server.py", "remontée dans le statique"),
            ("/api/inconnu", "route inconnue"),
        ]
        for chemin, libelle in refus:
            code, _ = appeler(base, chemin)
            check("test_refus_%s" % libelle.replace(" ", "_"), code >= 400,
                  "%s a répondu %d au lieu d'un refus" % (chemin, code))

        # --- une spec réelle se lit ------------------------------------------
        specs = etat.get("specs", [])
        if specs:
            code, corps = appeler(base, "/api/spec?id=" + specs[0]["id"])
            check("test_spec_reelle_lisible", code == 200 and "contenu" in json.loads(corps))
            check("test_spec_rend_son_statut_brut",
                  "statut_texte" in json.loads(corps),
                  "le statut doit être rendu TEL QUEL, jamais réécrit")

        # --- la liste FERMÉE des fichiers statiques --------------------------
        # ⚠️ Ce cas ne va pas de soi. Les refus testés plus haut (`/server.py`)
        # passaient pour une AUTRE raison : le fichier n'existe pas dans `web/`.
        # La contribution propre de la liste fermée est d'écarter un fichier
        # PRÉSENT mais d'extension non prévue — une note, un fichier
        # d'environnement, une sauvegarde. Sans ce cas, le mutant qui supprime la
        # liste survivait.
        import tempfile
        with tempfile.TemporaryDirectory() as faux_web:
            for nom, contenu in (("ok.css", "body{}"),
                                 ("secrets.env", "JETON=valeur"),
                                 ("notes.txt", "interne")):
                with open(os.path.join(faux_web, nom), "w", encoding="utf-8") as f:
                    f.write(contenu)
            ancienne_racine, anciens = SRV._RACINE_WEB, serveur.servables
            SRV._RACINE_WEB = faux_web
            serveur.servables = SRV._fichiers_servables()
            try:
                code, _ = appeler(base, "/ok.css")
                check("test_extension_autorisee_servie", code == 200)
                for nom in ("secrets.env", "notes.txt"):
                    code, corps = appeler(base, "/" + nom)
                    check("test_extension_non_prevue_refusee_%s" % nom.split(".")[-1],
                          code >= 400,
                          "%s présent dans web/ a été servi (%d) : la liste des "
                          "extensions doit trancher, pas l'existence du fichier"
                          % (nom, code))
            finally:
                SRV._RACINE_WEB = ancienne_racine
                serveur.servables = anciens

        # --- l'empreinte ne bouge pas toute seule ----------------------------
        # Sans cela l'interface se rafraîchirait en boucle : l'instantané
        # contient un horodatage, l'empreinte ne doit surtout pas le contenir.
        premiere = serveur.etat.empreinte()
        deuxieme = serveur.etat.empreinte()
        check("test_empreinte_stable_sans_changement", premiere == deuxieme,
              "une empreinte instable rafraîchirait l'interface en permanence")

    finally:
        serveur.shutdown()
        serveur.server_close()

    print("\n%d cas, %d echec(s)" % (_tests, len(_echecs)))
    for echec in _echecs:
        print("  - %s" % echec)
    return 1 if _echecs else 0


if __name__ == "__main__":
    try:
        _code = main()
    except Exception as erreur:
        print("\n%d cas atteints, INTERROMPU par %s : %s"
              % (_tests, type(erreur).__name__, erreur))
        _code = 1
    sys.exit(_code)
