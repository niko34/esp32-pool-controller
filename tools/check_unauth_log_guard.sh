#!/usr/bin/env bash
# =============================================================================
# check_unauth_log_guard.sh — pas de systemLogger non borné avant l'auth (bug-112)
# =============================================================================
# Tout `systemLogger.*` atteignable par un client NON authentifié est un émetteur
# non borné : le journal (6 144 o conservés) est purgé en ~27 requêtes. L'inventaire
# manuel a raté 2 sites sur 8, puis 2 de plus : ce contrôle cherche aussi les sites
# qu'on n'a pas encore vus.
#
# Six règles (exemption : commentaire `AUTH-LOG-EXEMPT(raison)` sur la ligne ou la
# précédente — la raison doit être VRAIE, jamais posée pour faire taire la garde) :
#  (1) web_routes_*.cpp : AUCUN systemLogger dans la branche de refus d'un
#      `if (!…Authorized(` ;
#  (2) web_routes_*.cpp : AUCUN systemLogger entre l'en-tête d'un handler de corps
#      (`size_t index` … `{`) et sa garde ;
#  (3) ⭐ web_routes_*.cpp : dans TOUT handler (lambda ou fonction prenant une
#      AsyncWebServerRequest*), un systemLogger ne peut venir qu'APRÈS un
#      REQUIRE_AUTH / `…Authorized(` / `checkAuth(` du même handler — c'est la règle
#      qui attrape les sites NEUFS (route sans aucune garde) ;
#  (4) ws_manager.cpp : AUCUN systemLogger dans _onData et _onClientConnect ;
#  (5) auth.cpp : AUCUN systemLogger dans checkAuth, checkTokenAuth,
#      checkRateLimit, sendAuthRequired, sendRateLimitExceeded.
#      (checkBasicAuth RETIRÉE du firmware par bug-184 : la voie HTTP Basic
#       n'existe plus, donc plus rien à garder de ce côté.)
#  (6) ⭐ bug-179 : AUCUN texte EXTERNE brut dans un systemLogger.*( de src/**.cpp
#      (->url(), ->value(), ->arg(, ->pathArg(, ->header(, ->filename, filename,
#      payloadStr, cmd.payload, WiFi.SSID(), g_wifiReconnectSsid, oldSsid), sauf
#      passage par un tampon assaini (identifiant préfixé `safe`, rempli par
#      logSanitizeExternalText) ou `LOG-TEXT-EXEMPT(raison)` en ligne. ROUGE si
#      src/log_text_sanitize_logic.h est introuvable (pas de faux vert).
# La journalisation passe par la coquille AuthLogThrottleSite (helpers dédiés).
# Sortie 0 = sain, 1 = violation.
# =============================================================================
set -u
cd "$(dirname "$0")/.."
python3 - "$@" <<'PY'
import re, glob, sys
bad = 0
def err(f, n, msg):
    global bad
    print(f'{f}:{n}: {msg}'); bad += 1

def load(f):
    return open(f, encoding='utf-8').read().split('\n')

def exempt(L, i):
    return 'AUTH-LOG-EXEMPT' in L[i] or (i > 0 and 'AUTH-LOG-EXEMPT' in L[i-1])

def logs(L, i):
    return 'systemLogger' in L[i] and not L[i].lstrip().startswith('//') and not exempt(L, i)

def block_end(L, i):
    """Index de la ligne qui ferme le bloc ouvert à la première '{' depuis i."""
    depth, opened = 0, False
    for j in range(i, len(L)):
        code = re.sub(r'//.*', '', L[j])
        depth += code.count('{') - code.count('}')
        if '{' in code: opened = True
        if opened and depth <= 0: return j
    return len(L) - 1

guard = re.compile(r'if\s*\(\s*!\s*\w*Authorized\s*\(')
authmark = re.compile(r'REQUIRE_AUTH|\w*Authorized\s*\(|checkAuth\s*\(')
handler_open = re.compile(r'\[[^\]]*\]\s*\([^)]*AsyncWebServerRequest\s*\*[^)]*\)\s*(mutable\s*)?\{')

for f in sorted(glob.glob('src/web_routes_*.cpp')):
    L = load(f)
    # --- règles (1) et (2)
    for i, line in enumerate(L):
        if not guard.search(line): continue
        e = block_end(L, i)
        for j in range(i, e + 1):
            if logs(L, j): err(f, j+1, 'systemLogger dans la branche de refus (non borné)')
        k = i - 1
        while k >= 0 and not ('size_t index' in L[k] and L[k].rstrip().endswith('{')): k -= 1
        if k < 0:
            err(f, i+1, 'garde sans en-tête de handler de corps reconnu'); continue
        for m in range(k + 1, i):
            if logs(L, m): err(f, m+1, "systemLogger AVANT la garde d'authentification")
    # --- règle (3) : handlers, bloc le plus interne
    blocks = []
    for i, line in enumerate(L):
        if handler_open.search(line):
            blocks.append((i, block_end(L, i)))
    for j in range(len(L)):
        if not logs(L, j): continue
        enc = [b for b in blocks if b[0] <= j <= b[1]]
        if not enc: continue
        s, e = min(enc, key=lambda b: b[1] - b[0])
        first = next((m for m in range(s, e + 1) if authmark.search(re.sub(r'//.*', '', L[m]))), None)
        # un bloc imbriqué (lambda de complétion) hérite de la garde du bloc englobant
        if first is None or first > j:
            outer = [b for b in enc if b != (s, e)]
            ok = False
            for (os_, oe) in outer:
                fo = next((m for m in range(os_, oe + 1) if authmark.search(re.sub(r'//.*', '', L[m]))), None)
                if fo is not None and fo < s: ok = True
            if not ok:
                err(f, j+1, 'systemLogger dans un handler SANS garde d\'authentification préalable')

def func_body(L, name):
    for i, line in enumerate(L):
        if re.match(r'^\S.*\b' + re.escape(name) + r'\s*\(', line) and not line.rstrip().endswith(';'):
            return i, block_end(L, i)
    return None

# --- règle (4)
L = load('src/ws_manager.cpp')
for name in ('WsManager::_onData', 'WsManager::_onClientConnect'):
    r = func_body(L, name)
    if r is None: err('src/ws_manager.cpp', 1, f'fonction {name} introuvable (garde à mettre à jour)'); continue
    for j in range(r[0], r[1] + 1):
        if logs(L, j): err('src/ws_manager.cpp', j+1, f'systemLogger dans {name} (non borné)')

# --- règle (5)
L = load('src/auth.cpp')
for name in ('AuthManager::checkAuth', 'AuthManager::checkTokenAuth',
             'AuthManager::checkRateLimit', 'AuthManager::sendAuthRequired',
             'AuthManager::sendRateLimitExceeded'):
    r = func_body(L, name)
    if r is None: err('src/auth.cpp', 1, f'fonction {name} introuvable (garde à mettre à jour)'); continue
    for j in range(r[0], r[1] + 1):
        if logs(L, j): err('src/auth.cpp', j+1, f'systemLogger dans {name} (non borné)')

# --- règle (6) : texte externe dans le journal (bug-179)
import os
if not os.path.isfile('src/log_text_sanitize_logic.h'):
    err('src/log_text_sanitize_logic.h', 1, 'helper d\'assainissement INTROUVABLE (règle 6 sans objet = faux vert interdit)')
ext = re.compile(r'->url\(\)|->value\(\)|->arg\(|->pathArg\(|->header\(|->filename|\bfilename\b|\bpayloadStr\b|\bcmd\.payload\b|WiFi\.SSID\(\)|\bg_wifiReconnectSsid\b|\boldSsid\b')
for f in sorted(glob.glob('src/**/*.cpp', recursive=True)):
    L = load(f)
    i = 0
    while i < len(L):
        if re.search(r'systemLogger\s*\.\s*\w+\s*\(', L[i]) and not L[i].lstrip().startswith('//'):
            j = i
            while j < len(L) - 1 and ';' not in re.sub(r'"(\\.|[^"\\])*"', '""', L[j]): j += 1
            stmt = ' '.join(re.sub(r'//.*$', '', re.sub(r'"(\\.|[^"\\])*"', '""', x)) for x in L[i:j+1])
            raw = ' '.join(L[i:j+1])
            if ext.search(stmt) and 'LOG-TEXT-EXEMPT(' not in raw:
                err(f, i+1, "texte externe brut dans systemLogger (bug-179) : passer par logSanitizeExternalText -> tampon 'safe*'")
            i = j + 1
        else:
            i += 1

# --- règle (6b) : auth.cpp ne peut pas contourner le helper par une variable intermédiaire.
# ⚠️ LIMITE ASSUMÉE, écrite pour ne pas se croire protégé : 6b ne couvre que auth.cpp.
# Le même contournement reste possible dans mqtt_manager.cpp, main.cpp et les routes web.
# Généraliser demanderait de SUIVRE LE FLOT DE DONNÉES — hors de portée d'une sentinelle
# textuelle, et un « toute ligne citant payloadStr doit être assainie » noierait la garde
# sous les usages d'ANALYSE, qui sont légitimes et majoritaires.
# ⇒ auth.cpp est couvert parce que c'est le SEUL chemin atteignable SANS identifiants.
# (Mutation M6 de bug-179 : `msg = "..." + req->url()` puis systemLogger(msg) échappait à la
# règle 6, qui ne regarde que l'intérieur de l'appel.) Tout `->url()` de auth.cpp doit être
# soit dans authLogThrottleHash(...), soit lié à une référence aussitôt confiée à
# logSanitizeExternalText(<ref>.c_str() ...).
L = load('src/auth.cpp')
for i, line in enumerate(L):
    code = re.sub(r'//.*$', '', line)
    if '->url()' not in code or 'LOG-TEXT-EXEMPT(' in line: continue
    if 'authLogThrottleHash(' in code: continue
    m = re.search(r'const\s+String&\s+(\w+)\s*=\s*[^;]*->url\(\)\s*;', code)
    if m and any(re.search(r'logSanitizeExternalText\(\s*' + re.escape(m.group(1)) + r'\.', x) for x in L[i+1:i+4]):
        continue
    err('src/auth.cpp', i+1, "->url() hors authLogThrottleHash/logSanitizeExternalText (bug-179 règle 6b)")

if bad:
    print(f'ECHEC : {bad} violation(s) — passer par AuthLogThrottleSite (bug-112).')
    sys.exit(1)
print('OK — aucun systemLogger non borné sur le chemin non authentifié.')
PY
