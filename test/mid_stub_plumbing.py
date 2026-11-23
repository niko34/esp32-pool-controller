#!/usr/bin/env python3
"""Verifie le CABLAGE de l'option -m, de bout en bout (feature-213).

⭐ POURQUOI CE FICHIER EXISTE. Les vecteurs de `test/mid_stub_vectors.js`
prouvent que la POSE fige l'etat. ⛔ Ils ne prouvent PAS qu'elle arrive jusqu'a
la page : ils interrogent `ui_shot_server.py` directement. Une mutation posee le
2026-11-23 l'a montre -- remplacer `export UI_SHOT_MID="$MID_STUB"` par
`export UI_SHOT_MID=0` laissait TOUTE la garde au vert, alors que `-m` ne figeait
plus rien. ⚠️ L'option serait morte EN SILENCE, exactement comme `littlefs.bin`
pendant six semaines (bug-170).

⇒ On lance donc le VRAI `ui_shot.sh`, avec deux doublures :
  - un faux APPAREIL (ce fichier) a la place de 192.168.1.48 ;
  - un faux NAVIGATEUR qui, au lieu de rendre la page, la TELECHARGE depuis le
    serveur local et dit si la pose y est.
⛔ Aucun appareil, aucun Chromium, aucune compilation : ce controle tourne en CI
et ne prend pas la ressource exclusive.

Usage : python3 test/mid_stub_plumbing.py
"""
import http.server, json, os, re, shutil, socket, subprocess, sys, tempfile, threading

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Reponses minimales : ui_shot.sh exige un jeton, puis trois corps NON VIDES.
FAUX_APPAREIL = {
    '/data': b'{"temperature":21.0,"ph":7.2,"orp":650}',
    '/get-config': b'{"filtration_mode":"auto","lighting_mode":"off"}',
    '/get-system-info': b'{"version":"test"}',
    '/get-logs': b'{"logs":[],"uptime_ms":0,"truncated":false}',
}


class Appareil(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, body):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        # ⚠️ ui_shot.sh lit le jeton par `grep -oE '"token"[^,]*' | cut -d'"' -f4`.
        self._send(b'{"success":true,"token":"jeton-de-test"}')

    def do_GET(self):
        self._send(FAUX_APPAREIL.get(self.path.split('?')[0], b'{}'))


def port_libre():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


FAUX_CHROME = r'''#!/usr/bin/env python3
# Faux navigateur : au lieu de RENDRE la page, il la telecharge depuis le serveur
# local de ui_shot.sh et ecrit ce qu'il a recu. C'est ce fichier qui transforme
# « l'option est passee » en une observation, et non en une supposition.
import os, re, sys, urllib.request
url = [a for a in sys.argv if a.startswith('http://')][-1]
sortie = [a for a in sys.argv if a.startswith('--screenshot=')][-1].split('=', 1)[1]
# Le boot redirige vers /index.html : on demande directement la page servie.
base = re.sub(r'/__boot\.html.*$', '', url)
page = urllib.request.urlopen(base + '/index.html', timeout=10).read()
open(os.environ['UI_SHOT_TEST_PAGE'], 'wb').write(page)
open(sortie, 'wb').write(b'\x89PNG\r\n\x1a\n')   # ui_shot.sh exige un fichier non vide
'''


def lancer(args, page_out):
    env = dict(os.environ)
    env['PATH'] = os.path.join(BAC, 'bin') + os.pathsep + env['PATH']
    env['POOL_PASSWORD'] = 'mot-de-passe-de-test'
    env['OTA_HOST'] = '127.0.0.1:%d' % PORT
    env['UI_SHOT_TEST_PAGE'] = page_out
    env.pop('UI_SHOT_MID', None)
    env.pop('UI_SHOT_WS', None)
    r = subprocess.run([os.path.join(REPO, 'tools', 'ui_shot.sh')] + args,
                       cwd=REPO, env=env, capture_output=True, timeout=120)
    if r.returncode != 0:
        sys.stderr.write(r.stdout.decode('utf-8', 'replace'))
        sys.stderr.write(r.stderr.decode('utf-8', 'replace'))
        raise SystemExit('ERREUR : ui_shot.sh %s a echoue (code %d).' % (' '.join(args), r.returncode))
    return open(page_out, 'rb').read().decode('utf-8', 'replace')


BAC = tempfile.mkdtemp(prefix='mid-plumbing-')
PORT = port_libre()
echecs = []
try:
    os.makedirs(os.path.join(BAC, 'bin'))
    faux = os.path.join(BAC, 'bin', 'chromium')
    open(faux, 'w').write(FAUX_CHROME)
    os.chmod(faux, 0o755)

    srv = http.server.HTTPServer(('127.0.0.1', PORT), Appareil)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    # La pose, demandee au fichier qui la sert — ⛔ jamais recopiee ici.
    sys.path.insert(0, os.path.join(REPO, 'tools'))
    os.environ['UI_SHOT_MID'] = '1'
    import ui_shot_server
    POSE = ui_shot_server.Handler._mid_script().decode()
    del os.environ['UI_SHOT_MID']
    # ⚠️ EMPREINTE CHOISIE AVEC SOIN : `window.WebSocket=F` est commun AUX DEUX
    # poses (-n en a une aussi) — le premier jet de ce fichier s'en servait et
    # accusait -n a tort. Le detournement de `fetch` n'appartient qu'a -m.
    empreinte = 'window.fetch=function'

    avec = lancer(['-m', '-w', '390', '-o', os.path.join(BAC, 'avec.png')],
                  os.path.join(BAC, 'avec.html'))
    if POSE not in avec:
        echecs.append("avec -m : la pose « Connexion… » N'EST PAS dans la page servie.\n"
                      "  ⇒ l'option ne figerait plus rien, en silence.")

    sans = lancer(['-w', '390', '-o', os.path.join(BAC, 'sans.png')],
                  os.path.join(BAC, 'sans.html'))
    if empreinte in sans:
        echecs.append("sans -m : la pose « Connexion… » est quand meme injectee.\n"
                      "  ⇒ les captures « Deconnecte » et « En ligne » seraient fausses.")

    en_ligne = lancer(['-n', '-w', '390', '-o', os.path.join(BAC, 'n.png')],
                      os.path.join(BAC, 'n.html'))
    if empreinte in en_ligne:
        echecs.append("avec -n : la pose « Connexion… » est injectee en plus du faux socket.\n"
                      "  ⇒ deux WebSocket contradictoires sur la meme page.")
    # Temoin : -m ne doit pas avoir EFFACE la pose de -n. `s.onopen({target:s})`
    # n'appartient qu'a elle (c'est l'ouverture differee du socket).
    if 's.onopen({target:s})' not in en_ligne:
        echecs.append("avec -n : le faux WebSocket d'origine a disparu de la page.")
finally:
    shutil.rmtree(BAC, ignore_errors=True)

if echecs:
    for e in echecs:
        sys.stderr.write('ERREUR : ' + e + '\n')
    raise SystemExit(1)
print('mid_stub_plumbing : cablage -m verifie de bout en bout (3 executions)')
