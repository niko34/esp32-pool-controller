#!/usr/bin/env python3
"""Serveur local servant `data/` avec des réponses d'API figées.

Permet d'afficher l'interface SANS l'appareil, pour la photographier
(cf. tools/ui_shot.sh et docs/DESIGN_SYSTEM.md §9).

⚠️ Les réponses figées sont lues dans un dossier passé en argument : elles
contiennent le SSID Wi-Fi et l'identifiant MQTT, et ne doivent JAMAIS être
versionnées. ui_shot.sh les écrit dans un dossier temporaire.

Usage : ui_shot_server.py <port> <dossier_data> <dossier_fixtures>

⚠️ Ce module est IMPORTABLE sans rien demarrer (feature-213) : le serveur ne se
lance que sous `__main__`, et les arguments ont un repli. C'est ce qui permet a
`tools/check_mid_stub_vectors.sh` de demander a CE fichier le script qu'il
injecte, puis de l'EXECUTER sous node -- plutot que d'en recopier le texte dans
un harnais, ou de verifier sa forme a la lecture. Esprit d'ADR-0099 : on teste
ce qui est LIVRE, jamais une copie.
"""
import http.server, json, os, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 0
ROOT = sys.argv[2] if len(sys.argv) > 2 else '.'
API = sys.argv[3] if len(sys.argv) > 3 else '.'

# Clics a rejouer avant la capture (option -k de ui_shot.sh), un selecteur par
# ligne. Permet de photographier un ecran situe DERRIERE un bouton : sous-vue
# ouverte en JavaScript, panneau repliable, modale. Sans ce mecanisme ces
# ecrans n'etaient pas photographiables, et le brief d'ergonomie les concernant
# partait sans image.
CLICKS = [c for c in os.environ.get('UI_SHOT_CLICK', '').splitlines() if c.strip()]

# Faux WebSocket (option -n de ui_shot.sh). Sans lui, l'application ne voit
# AUCUN socket s'ouvrir, se declare « Deconnecte » et REMPLACE les trois
# mesures du tableau de bord par « - » (app.js, setNetState('bad')).
# ⛔ Consequence mesuree le 2026-11-18 : une capture du tableau de bord ne
# montrait AUCUN chiffre -- or les mesures sont tout le sujet d'un dossier
# d'ergonomie sur cette page. Le stub ouvre un socket qui ne dit rien, ce qui
# est exactement l'etat d'un appareil connecte et silencieux.
# ⚠️ Reste OPTIONNEL : l'ecran hors ligne est lui aussi a photographier, et il
# l'est par defaut.
WS_STUB = os.environ.get('UI_SHOT_WS') == '1'

# Troisieme etat : « Connexion… » (option -m de ui_shot.sh). feature-213.
#
# L'application connait TROIS etats de liaison -- setNetStatus('ok'|'mid'|'bad').
# L'outil n'en photographiait que DEUX : avec -n l'etat « En ligne », sans rien
# l'etat « Deconnecte ». ⛔ L'etat INTERMEDIAIRE, celui que voit TOUT LE MONDE a
# chaque ouverture de page, n'etait pas capturable -- et un faux y a vecu sans
# que personne ne le voie (bug-196 pour l'en-tete, bug-212 pour les pastilles).
#
# ⭐ Il est fige par CONSTRUCTION, pas par une course gagnee. Les SEULES sorties
# de « mid » sont, dans le code livre :
#   - `_ws.onopen`                      -> setNetStatus('ok')
#   - `_onDisconnect` (onerror/onclose) -> setNetStatus('bad')
#   - le battement de coeur WS          -> setNetStatus('bad')  [arme par onopen seul]
#   - l'issue de `/data`, succes ou echec -> setNetStatus('ok' | 'mid' | 'bad')
# Le faux socket reste en COURS D'OUVERTURE et n'emet JAMAIS d'evenement ; la
# requete `/data` rend une promesse qui ne se resout JAMAIS, donc ni le `try` ni
# le `catch` de loadSensorData ne tournent. ⇒ Aucun appel a setNetStatus ne peut
# suivre : deux executions donnent le meme ecran.
# ⛔ Aucune temporisation : pas de « attendre 300 ms et esperer ».
#
# ⚠️ Ce que la capture montre est l'ecran REEL de la premiere seconde : les
# mesures pH/ORP/temperature y sont encore absentes, puisque `/data` n'est pas
# arrive. ⛔ Ce n'est pas un defaut de la capture, c'est l'etat photographie.
MID_STUB = os.environ.get('UI_SHOT_MID') == '1'

CANNED = {'/data': 'data.json',
          '/get-config': 'get-config.json',
          '/get-system-info': 'get-system-info.json'}

# Page d'amorçage : pose un jeton de session bidon (sinon l'application
# redirige vers l'écran de connexion) puis rejoue la route demandée.
# bug-189 : la page cible est paramétrable (?page=/login.html). Sans cela l'outil
# ne savait photographier QUE /index.html, et un brief d'ergonomie sur l'écran de
# connexion partait sans image — ce que CLAUDE.md interdit.
# ⛔ Le jeton bidon n'est posé QUE pour /index.html : sur /login.html il ferait
# fuir la page vers le tableau de bord, et on photographierait le mauvais écran.
BOOT = (b"<!doctype html><meta charset=utf-8><script>"
        b"var p=new URLSearchParams(location.search).get('page')||'/index.html';"
        b"if(p==='/index.html'){try{sessionStorage.setItem('authToken','mock')}catch(e){}};"
        b"location.replace(p+location.hash)</script>")


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def log_message(self, *a):
        pass  # silence

    def _send(self, body, ctype='application/json'):
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split('?')[0]
        if path == '/__boot.html':
            return self._send(BOOT, 'text/html')
        if (CLICKS or WS_STUB or MID_STUB) and path in ('/', '/index.html'):
            with open(os.path.join(ROOT, 'index.html'), 'rb') as fh:
                page = fh.read()
            return self._send(page + self._ws_script() + self._mid_script()
                              + self._click_script(),
                              'text/html')
        if path in CANNED:
            with open(os.path.join(API, CANNED[path]), 'rb') as fh:
                return self._send(fh.read())
        if path == '/get-logs':
            # bug-179 : si le journal a ete recopie depuis l'appareil (ou fabrique
            # a la main dans $TMP/api/get-logs.json), on le SERT — sinon le panneau
            # Journal est toujours vide et personne ne peut voir son rendu avant
            # de deployer. C'est exactement l'angle mort qui a laisse passer un
            # rendu en innerHTML pendant des mois.
            cached = os.path.join(API, 'get-logs.json')
            if os.path.exists(cached):
                with open(cached, 'rb') as fh:
                    return self._send(fh.read())
            return self._send(b'{"logs":[],"uptime_ms":0,"truncated":false}')
        if path.startswith('/get-history'):
            return self._send(b'{"entries":[],"data":[]}')
        return super().do_GET()

    @staticmethod
    def _ws_script():
        """Remplace window.WebSocket par un socket qui s'ouvre et se tait.

        L'application branche ses propres gestionnaires APRES construction :
        on appelle donc onopen au tour de boucle suivant, jamais dans le
        constructeur (sinon le gestionnaire n'est pas encore pose)."""
        if not WS_STUB:
            return b''
        return (b"<script>(function(){function F(u){this.url=u;this.readyState=1;"
                b"var s=this;setTimeout(function(){s.readyState=1;"
                b"if(typeof s.onopen==='function')s.onopen({target:s});},0);}"
                b"F.prototype.send=function(){};"
                b"F.prototype.close=function(){this.readyState=3;};"
                b"F.prototype.addEventListener=function(t,f){"
                b"if(t==='open')this.onopen=f;else if(t==='message')this.onmessage=f;"
                b"else if(t==='close')this.onclose=f;else if(t==='error')this.onerror=f;};"
                b"F.prototype.removeEventListener=function(){};"
                b"F.CONNECTING=0;F.OPEN=1;F.CLOSING=2;F.CLOSED=3;"
                b"window.WebSocket=F;})()</script>")

    @staticmethod
    def _mid_script():
        """Fige l'application dans « Connexion… » (option -m). feature-213.

        Deux poses, aucune minuterie :
          1. un WebSocket qui reste en COURS D'OUVERTURE -- readyState 0, et
             ⛔ ni onopen, ni onerror, ni onclose, jamais ;
          2. `/data` qui rend une promesse JAMAIS resolue. ⛔ Pas un echec : un
             echec appellerait setNetStatus('mid') au 1er puis ('bad') au 2e, et
             l'ecran basculerait. Une promesse en attente ne reveille NI le `try`
             NI le `catch` de loadSensorData -- rien ne peut plus ecrire l'etat.

        ⚠️ Le `signal` d'abandon de l'application (10 s) est volontairement
        IGNORE : il annule une vraie requete reseau, pas cette promesse. C'est ce
        qui rend l'etat insensible au temps de capture.
        ⇒ `/get-config` passe au contraire NORMALEMENT : c'est lui qui prouve que
        l'appareil est joignable, et c'est tout l'interet de cet etat."""
        if not MID_STUB:
            return b''
        return (b"<script>(function(){"
                b"function F(u){this.url=u;this.readyState=0;}"
                b"F.prototype.send=function(){};"
                b"F.prototype.close=function(){this.readyState=3;};"
                b"F.prototype.addEventListener=function(){};"
                b"F.prototype.removeEventListener=function(){};"
                b"F.CONNECTING=0;F.OPEN=1;F.CLOSING=2;F.CLOSED=3;"
                b"window.WebSocket=F;"
                b"var f=window.fetch.bind(window);"
                b"window.fetch=function(i,o){"
                b"var u=typeof i==='string'?i:((i&&i.url)||'');"
                b"if(/(^|\\/)data(\\?|$)/.test(u))return new Promise(function(){});"
                b"return f(i,o);};"
                b"})()</script>")

    @staticmethod
    def _click_script():
        """Script injecte en fin de page : clique chaque selecteur des qu'il
        existe ET qu'il est visible, dans l'ordre donne. Attend, plutot que de
        supposer un delai : l'application peuple ses vues apres /data."""
        return (b"<script>(function(){var S=" + json.dumps(CLICKS).encode()
                + b";var i=0,n=0;function visible(e){return e&&e.offsetParent!==null"
                  b"&&!e.disabled;}function step(){if(i>=S.length)return;"
                  b"var e=document.querySelector(S[i]);"
                  b"if(visible(e)){e.click();i++;n=0;setTimeout(step,250);return;}"
                  b"if(++n>80){i++;n=0;}setTimeout(step,50);}"
                  b"if(document.readyState==='complete')setTimeout(step,400);"
                  b"else addEventListener('load',function(){setTimeout(step,400);});"
                  b"})()</script>")

    def do_POST(self):
        self._send(b'{"success":true,"token":"mock"}')


if __name__ == '__main__':
    http.server.HTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
