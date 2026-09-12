#!/usr/bin/env python3
"""Serveur du centre de contrôle — étape C : LECTURE SEULE.

    python3 control-center/backend/server.py

## La garantie structurelle de cette étape

**Seul `GET` existe.** Il n'y a pas de `do_POST`, pas de `do_PUT`, pas de
`do_DELETE` dans cette classe. Ce n'est pas une politique qu'on pourrait
contourner : les méthodes n'existent pas, donc `http.server` répond 501 de
lui-même. Aucun clic dans l'interface ne peut donc rien lancer, rien écrire,
rien dépenser — c'est le critère de sortie de l'étape C.

Les garde-fous qui manquent encore (jeton, écrivain unique, plafond de dépense)
sont déclarés dans `control-center.toml` et **appliqués par personne**. Ils
devront devenir du code AVANT la première route qui écrit. Tant que `do_POST`
n'existe pas, leur absence ne se paie pas.

## Ce qui peut sortir du serveur, et ce qui ne peut pas

Trois familles de chemins, trois validations différentes, aucune arithmétique de
chemin improvisée :

- **fichiers statiques** : liste FERMÉE, calculée au démarrage depuis `web/`.
  Un nom qui n'y figure pas est refusé sans même toucher au disque ;
- **specs** : l'identifiant demandé doit correspondre EXACTEMENT à une spec
  découverte. On ne construit jamais un chemin à partir de la requête ;
- **diffs** : `vcs.chemin_sur()`, qui résout les liens symboliques et refuse
  tout ce qui sort du dépôt.

`transcript_path` est présent dans le journal d'événements et pointe **hors du
dépôt** vers la conversation entière. Il n'est servi par aucune route et
n'atteint jamais le navigateur : `_evenement_public()` filtre les champs.
"""

import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agents as A  # noqa: E402
import config as C  # noqa: E402
import events as E  # noqa: E402
import specs as S  # noqa: E402
import vcs as V  # noqa: E402

_RACINE_WEB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web")

# Types servis. Une extension absente d'ici n'est pas servie : la liste des
# fichiers ET celle des types doivent toutes deux accepter.
_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".svg": "image/svg+xml",
}

# Champs d'un événement qu'on accepte d'envoyer au navigateur. Liste POSITIVE :
# un champ neuf apparu dans le journal ne sort pas tant qu'il n'est pas ajouté
# ici. `transcript_path` et `cwd` en sont volontairement absents — le premier
# désigne la conversation entière, hors du dépôt.
_CHAMPS_PUBLICS = ("ts", "runtime", "session_id", "hook_event_name",
                   "agent_type", "agent_id", "tool", "command_head", "file", "failed")

_INTERVALLE_SONDAGE = 2.0      # secondes entre deux comparaisons d'empreinte
_BATTEMENT = 20.0              # secondes sans changement avant un signe de vie


def _evenement_public(evenement):
    return {c: evenement[c] for c in _CHAMPS_PUBLICS if c in evenement}


class Etat:
    """Assemble l'état courant. Aucun cache : les sources sont des fichiers.

    Mettre en cache introduirait une quatrième copie de la vérité, après les
    specs, git et le journal — exactement ce que l'architecture refuse. Le coût
    réel est un `git status` et une lecture de journal par sondage.
    """

    def __init__(self, config):
        self.config = config
        self.racine = config["_racine_projet"]
        self.journal = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "var", "agent-events.jsonl")

    def specs(self):
        return S.lister_depuis_config(self.config)

    def instantane(self):
        evenements = E.lire_journal(self.journal)
        workflows = E.reconstruire(evenements, self.config.tetes_de_commande())
        specs = self.specs()
        colonnes = [{"repertoire": r, "classe": c, "libelle": l}
                    for r, c, l in self.config.colonnes()]
        agents = A.lister_depuis_config(self.config)

        # Agent actif = celui d'un passage non terminé, dans un workflow sans fin
        # observée. On ne dit JAMAIS qu'un agent est actif sur la foi d'un
        # passage ouvert dans une session close.
        actifs = {}
        for workflow in workflows:
            if not workflow["fin_observee_absente"]:
                continue
            for passage in workflow["passages"]:
                if not passage["termine"]:
                    actifs[passage["agent"]] = workflow["session_id"]

        return {
            "projet": self.config.section("projet").get("nom"),
            "config_lue": self.config["_fichier_lu"],
            "colonnes": colonnes,
            "specs": specs,
            "divergences": sum(1 for s in specs if s["divergence"]),
            "agents": [dict(a, actif_sur=actifs.get(a["nom"])) for a in agents],
            "workflows": workflows[-10:],
            "evenements": [_evenement_public(e) for e in evenements[-60:]],
            "git": {
                "depot": V.est_un_depot(self.racine),
                "branche": V.branche(self.racine),
                "touches": V.etat(self.racine),
                "commits": V.journal(self.racine, 8),
            },
            "commandes_declarees": sorted(self.config.section("commandes")),
            "garde_fous": self.config.section("garde_fous"),
            "lecture_seule": True,
            "horodatage": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }

    def empreinte(self):
        """Empreinte bon marché de l'état. Change ⇒ l'interface se rafraîchit.

        On ne compare PAS l'instantané complet : il contient un horodatage, donc
        il changerait à chaque sondage et l'interface se rafraîchirait en
        permanence pour rien.
        """
        morceaux = []
        try:
            stat = os.stat(self.journal)
            morceaux.append("j%d" % stat.st_size)
        except OSError:
            morceaux.append("j-")
        ok, sortie = V._git(self.racine, "status", "--porcelain")
        morceaux.append("g%d" % (hash(sortie) if ok else 0))
        for repertoire, _, _ in self.config.colonnes():
            dossier = os.path.join(self.racine,
                                   self.config.section("specs").get("racine", ""), repertoire)
            try:
                morceaux.append("%s:%d" % (repertoire, len(os.listdir(dossier))))
            except OSError:
                morceaux.append("%s:-" % repertoire)
        return "|".join(morceaux)


def _fichiers_servables():
    """Liste FERMÉE des fichiers statiques, calculée au démarrage.

    Un nom absent d'ici est refusé sans que le disque soit touché. Aucun chemin
    n'est construit à partir de la requête.
    """
    servables = {}
    if not os.path.isdir(_RACINE_WEB):
        return servables
    for nom in os.listdir(_RACINE_WEB):
        extension = os.path.splitext(nom)[1].lower()
        chemin = os.path.join(_RACINE_WEB, nom)
        if extension in _TYPES and os.path.isfile(chemin):
            servables[nom] = (chemin, _TYPES[extension])
    return servables


class Handler(BaseHTTPRequestHandler):
    """⚠️ GET UNIQUEMENT. L'absence de do_POST est la garantie de l'étape C."""

    protocol_version = "HTTP/1.1"
    server_version = "control-center"
    sys_version = ""

    # -- utilitaires de réponse ---------------------------------------------
    def _envoyer(self, code, contenu, type_mime, cache=False):
        if isinstance(contenu, str):
            contenu = contenu.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", type_mime)
        self.send_header("Content-Length", str(len(contenu)))
        self.send_header("Cache-Control", "public, max-age=60" if cache else "no-store")
        # L'interface ne charge rien depuis l'extérieur : la politique le dit et
        # le navigateur l'applique. Pas de CDN, pas de police distante.
        self.send_header("Content-Security-Policy",
                         "default-src 'self'; style-src 'self' 'unsafe-inline'; "
                         "img-src 'self' data:; connect-src 'self'")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.end_headers()
        try:
            self.wfile.write(contenu)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _json(self, donnees, code=200):
        self._envoyer(code, json.dumps(donnees, ensure_ascii=False), "application/json; charset=utf-8")

    def _erreur(self, code, message):
        self._json({"erreur": message}, code)

    # -- routage -------------------------------------------------------------
    def do_GET(self):  # noqa: N802 (nom imposé par http.server)
        analyse = urlparse(self.path)
        chemin = analyse.path
        parametres = parse_qs(analyse.query)
        try:
            if chemin == "/":
                return self._statique("index.html")
            if chemin == "/api/etat":
                return self._json(self.server.etat.instantane())
            if chemin == "/api/spec":
                return self._spec(parametres)
            if chemin == "/api/diff":
                return self._diff(parametres)
            if chemin == "/api/flux":
                return self._flux()
            if chemin.count("/") == 1:
                return self._statique(chemin[1:])
            return self._erreur(404, "route inconnue")
        except (BrokenPipeError, ConnectionResetError):
            return None
        except Exception as erreur:  # le serveur ne meurt pas d'une requête
            return self._erreur(500, "erreur interne : %s" % type(erreur).__name__)

    # -- routes ---------------------------------------------------------------
    def _statique(self, nom):
        entree = self.server.servables.get(nom)
        if entree is None:
            return self._erreur(404, "fichier inconnu")
        chemin, type_mime = entree
        try:
            with open(chemin, "rb") as fichier:
                contenu = fichier.read()
        except OSError:
            return self._erreur(404, "fichier illisible")
        return self._envoyer(200, contenu, type_mime)

    def _spec(self, parametres):
        """Contenu d'une spec.

        L'identifiant demandé doit correspondre EXACTEMENT à une spec découverte.
        Aucun chemin n'est construit à partir de la requête : on cherche dans la
        liste, et on sert le fichier qu'elle désigne.
        """
        demande = (parametres.get("id") or [""])[0]
        if not demande:
            return self._erreur(400, "identifiant manquant")
        reglages = self.server.etat.config.section("specs")
        racine = os.path.join(self.server.etat.racine, reglages.get("racine", "specs/features"))
        for spec in self.server.etat.specs():
            if spec["id"] != demande:
                continue
            chemin = os.path.join(racine, spec["colonne"], spec["id"] + ".md")
            try:
                with open(chemin, encoding="utf-8") as fichier:
                    contenu = fichier.read()
            except (OSError, UnicodeDecodeError):
                return self._erreur(404, "spec illisible")
            return self._json(dict(spec, contenu=contenu[:400_000]))
        return self._erreur(404, "spec inconnue")

    def _diff(self, parametres):
        demande = (parametres.get("chemin") or [""])[0]
        texte, tronque = V.diff(self.server.etat.racine, demande or None)
        if texte is None:
            return self._erreur(404, "diff indisponible pour ce chemin")
        return self._json({"chemin": demande, "diff": texte, "tronque": tronque})

    def _flux(self):
        """Server-Sent Events : un signal quand l'état change.

        On envoie un SIGNAL, pas l'état : le navigateur redemande `/api/etat`. Un
        état poussé et un état demandé pourraient sinon diverger, et il faudrait
        décider lequel fait foi.
        """
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        precedente = None
        dernier_signe = time.monotonic()
        try:
            while True:
                empreinte = self.server.etat.empreinte()
                if empreinte != precedente:
                    precedente = empreinte
                    self.wfile.write(b"event: maj\ndata: {}\n\n")
                    self.wfile.flush()
                    dernier_signe = time.monotonic()
                elif time.monotonic() - dernier_signe > _BATTEMENT:
                    # Un commentaire SSE : garde la connexion vivante à travers
                    # les proxys sans déclencher de rafraîchissement côté client.
                    self.wfile.write(b": battement\n\n")
                    self.wfile.flush()
                    dernier_signe = time.monotonic()
                time.sleep(_INTERVALLE_SONDAGE)
        except (BrokenPipeError, ConnectionResetError, OSError):
            return None

    def log_message(self, format, *args):
        # Le journal par défaut écrit une ligne par requête, flux SSE compris.
        # On garde les erreurs, on tait le reste.
        if args and str(args[1] if len(args) > 1 else "").startswith(("4", "5")):
            sys.stderr.write("%s %s\n" % (self.address_string(), format % args))


def demarrer(config=None):
    config = config or C.charger()
    reglages = config.section("serveur")
    hote = reglages.get("hote", "127.0.0.1")
    port = int(reglages.get("port", 8787))

    serveur = ThreadingHTTPServer((hote, port), Handler)
    serveur.daemon_threads = True
    serveur.etat = Etat(config)
    serveur.servables = _fichiers_servables()

    print("Centre de contrôle — LECTURE SEULE")
    print("  projet   : %s" % config.section("projet").get("nom"))
    print("  écoute   : http://%s:%d" % (hote, port))
    print("  dépôt    : %s" % config["_racine_projet"])
    print("  statique : %s" % (", ".join(sorted(serveur.servables)) or "aucun fichier web/"))
    if hote not in ("127.0.0.1", "localhost"):
        print("  ⚠️  ouvert au réseau local. Aucune route n'écrit : `do_POST`")
        print("      n'existe pas dans ce serveur. Le jeton devient obligatoire")
        print("      à l'étape D, avec la première route qui lance un workflow.")
    try:
        serveur.serve_forever()
    except KeyboardInterrupt:
        print("\narrêt")
    finally:
        serveur.server_close()


if __name__ == "__main__":
    demarrer()
