#!/usr/bin/env bash
# =============================================================================
# check_ph_unusual_alert_head.sh — la TÊTE du texte de feature-168 (C7) survit à
# la troncature MQTT (feature-170, vecteur V7bis, condition feature-170 (C23))
# =============================================================================
# publishAlert() tronque à ~46 octets utiles (kMaxPayloadLen = 128, enveloppe
# JSON ~62 o, queue de troncature 19 o). Le texte de l'alerte `ph_offset_unusual`
# (setPhOffsetStamped, src/config.cpp) doit donc porter la VALEUR et le VERDICT
# « le dosage en dépend » dans ses 46 premiers OCTETS, y compris pour une
# correction à deux chiffres (+10,50). L'ancien texte y perdait le verdict.
#
# POURQUOI UNE SENTINELLE ET PAS UN VECTEUR NATIF : ce texte est construit avec
# des `String` Arduino — non testable en natif tel quel. Le gate (feature-170,
# C32) a interdit toute fonction nouvelle décorative : extraire le texte dans un
# module pur n'aurait servi qu'à ce test. On mesure donc la TÊTE DU LITTÉRAL.
#
# Le script lit l'expression `const String msg = …;` de src/config.cpp,
# remplace chaque variable par sa valeur la plus LARGE (offsetStr = « +10,50 »
# ou « -10,50 », 6 octets ; seuils = « 0,50 » / « 5,50 »), concatène les
# littéraux (concaténation implicite C et `+` String), puis exige :
#   - « le dosage en dépend » ENTIER dans les 46 premiers octets (UTF-8, `é` = 2 o),
#     avec au moins 4 octets de marge (une tête à 46 pile coupe dès +10,50) ;
#   - aucun « sonde » (la correction compense la MESURE) ;
#   - ni « inhabituelle » ni « ancienne borne » (formule contestée par
#     l'utilisateur, feature-170 (C23)) ;
#   - les deux affirmations qui survivent : « Rien ne la limite » et « telle quelle ».
#
# ⛔ ÉCHEC FERMÉ : expression introuvable, identifiant inconnu, chaîne non
# terminée => le script ÉCHOUE (jamais un 0 silencieux).
# ⛔ NE PEUT PAS FERMER : que l'expression mesurée soit bien celle qui est publiée
# (un second `msg` ailleurs lui échapperait) ; le test d'appel est du ressort de
# la revue.
#
# Usage : ./tools/check_ph_unusual_alert_head.sh [fichier.cpp]  (défaut src/config.cpp)
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."
FILE="${1:-src/config.cpp}"
[ -f "$FILE" ] || { echo "ERREUR feature-170 (V7bis) : fichier introuvable : $FILE" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "ERREUR : python3 introuvable" >&2; exit 1; }

python3 -I - "$FILE" <<'PY'
import re
import sys

BUDGET = 46
MARGIN = 4
NEEDLE = "le dosage en dépend"
VARS = {"offsetStr": "+10,50", "unusualStr": "0,50", "floorStr": "5,50"}


def die(msg):
    sys.stderr.write("ERREUR feature-170 (V7bis) : %s\n" % msg)
    sys.exit(1)


src = open(sys.argv[1], encoding="utf-8").read()
# Retire les commentaires `//` (hors chaînes) ligne à ligne.
def strip_comment(line):
    out, inq, i = [], False, 0
    while i < len(line):
        c = line[i]
        if inq:
            out.append(c)
            if c == "\\" and i + 1 < len(line):
                out.append(line[i + 1]); i += 1
            elif c == '"':
                inq = False
        else:
            if c == '"':
                inq = True; out.append(c)
            elif c == "/" and line[i:i + 2] == "//":
                break
            else:
                out.append(c)
        i += 1
    return "".join(out)

code = "\n".join(strip_comment(l) for l in src.split("\n"))
m = re.search(r"const\s+String\s+msg\s*=(.*?);", code, re.S)
if not m:
    die("expression « const String msg = …; » introuvable dans %s (texte déplacé ou renommé ?)" % sys.argv[1])
expr = m.group(1)

parts, i, n = [], 0, len(expr)
while i < n:
    c = expr[i]
    if c == '"':
        i += 1; buf = []
        while True:
            if i >= n:
                die("chaîne non terminée")
            ch = expr[i]
            if ch == '"':
                i += 1; break
            if ch == "\\":
                nxt = expr[i + 1]
                if nxt not in '"\\':
                    die("séquence d'échappement « \\%s » non gérée" % nxt)
                buf.append(nxt); i += 2; continue
            buf.append(ch); i += 1
        parts.append("".join(buf))
    elif c.isspace() or c == "+":
        i += 1
    elif c.isalpha() or c == "_":
        j = i
        while j < n and (expr[j].isalnum() or expr[j] == "_"):
            j += 1
        name = expr[i:j]
        if name not in VARS:
            die("identifiant inattendu « %s » dans l'expression du message (VARS à mettre à jour avec la sentinelle)" % name)
        parts.append(VARS[name]); i = j
    else:
        die("caractère inattendu %r dans l'expression du message" % c)

if not parts:
    die("message vide")
msg = "".join(parts)
raw = msg.encode("utf-8")
head = raw[:BUDGET]
needle = NEEDLE.encode("utf-8")
pos = raw.find(needle)
if pos < 0:
    die("« %s » absent du message" % NEEDLE)
end = pos + len(needle)
if end > BUDGET:
    die("« %s » dépasse les %d premiers octets (fin à l'octet %d) : le verdict serait perdu par la troncature MQTT. Tête mesurée : %r" % (NEEDLE, BUDGET, end, msg[:60]))
if end + MARGIN > BUDGET:
    die("« %s » finit à l'octet %d : il reste moins de %d octets de marge (+10,50 coupe dans le verdict)." % (NEEDLE, end, MARGIN))
low = msg.lower()
for banned in ("sonde", "inhabituelle", "ancienne borne"):
    if banned in low:
        die("terme banni « %s » présent dans le texte (feature-170 (C23))" % banned)
for kept in ("Rien ne la limite", "telle quelle"):
    if kept not in msg:
        die("affirmation conservée « %s » absente (feature-168 (C7))" % kept)
print("OK — « %s » finit à l'octet %d/%d (marge %d o) pour +10,50." % (NEEDLE, end, BUDGET, BUDGET - end))
PY
