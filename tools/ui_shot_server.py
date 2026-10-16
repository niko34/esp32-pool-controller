#!/usr/bin/env python3
"""Serveur local servant `data/` avec des réponses d'API figées.

Permet d'afficher l'interface SANS l'appareil, pour la photographier
(cf. tools/ui_shot.sh et docs/DESIGN_SYSTEM.md §9).

⚠️ Les réponses figées sont lues dans un dossier passé en argument : elles
contiennent le SSID Wi-Fi et l'identifiant MQTT, et ne doivent JAMAIS être
versionnées. ui_shot.sh les écrit dans un dossier temporaire.

Usage : ui_shot_server.py <port> <dossier_data> <dossier_fixtures>
"""
import http.server, json, os, sys

PORT = int(sys.argv[1])
ROOT = sys.argv[2]
API = sys.argv[3]

# Clics a rejouer avant la capture (option -k de ui_shot.sh), un selecteur par
# ligne. Permet de photographier un ecran situe DERRIERE un bouton : sous-vue
# ouverte en JavaScript, panneau repliable, modale. Sans ce mecanisme ces
# ecrans n'etaient pas photographiables, et le brief d'ergonomie les concernant
# partait sans image.
CLICKS = [c for c in os.environ.get('UI_SHOT_CLICK', '').splitlines() if c.strip()]

CANNED = {'/data': 'data.json',
          '/get-config': 'get-config.json',
          '/get-system-info': 'get-system-info.json'}

# Page d'amorçage : pose un jeton de session bidon (sinon l'application
# redirige vers l'écran de connexion) puis rejoue la route demandée.
BOOT = (b"<!doctype html><meta charset=utf-8><script>"
        b"try{sessionStorage.setItem('authToken','mock')}catch(e){};"
        b"location.replace('/index.html'+location.hash)</script>")


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
        if CLICKS and path in ('/', '/index.html'):
            with open(os.path.join(ROOT, 'index.html'), 'rb') as fh:
                page = fh.read()
            return self._send(page + self._click_script(), 'text/html')
        if path in CANNED:
            with open(os.path.join(API, CANNED[path]), 'rb') as fh:
                return self._send(fh.read())
        if path.startswith('/get-history') or path == '/get-logs':
            return self._send(b'{"entries":[],"data":[]}')
        return super().do_GET()

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


http.server.HTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
