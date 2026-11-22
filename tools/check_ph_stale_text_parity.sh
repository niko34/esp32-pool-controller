#!/usr/bin/env bash
# =============================================================================
# check_ph_stale_text_parity.sh — la phrase d'ACTE est identique octet pour octet
# dans le firmware et dans l'interface (feature-170, ADR-0139)
# =============================================================================
# 🔴 PREMIÈRE sentinelle de ce projet qui compare un texte de .cpp à un texte de
# .js. Les harnais ADR-0099 extraient du JS VIVANT évalué sous Node ; les
# marqueurs PARITE-DATA comparent deux .cpp et des jeux de CLÉS. Aucun ne lit un
# littéral de C++ pour le confronter à un littéral de JS.
#
# Le firmware notifie un ÉVÉNEMENT (src/ph_stale_message.h), l'écran rend un ÉTAT
# (data/app.js) : ils ne partagent qu'UNE phrase, l'ACTE — « Refaites un test
# manuel et accordez la mesure dessus, ou remettez la correction à zéro. ».
# Elle est encadrée de part et d'autre par deux ancres :
#     // >>> PH-STALE-ACTE:DEBUT   …   // <<< PH-STALE-ACTE:FIN
# (macro PH_STALE_ACTE côté C++, constante `acte` côté JS).
#
# Le script extrait ce qui est entre les ancres, normalise la SYNTAXE de chaîne
# (guillemets, `+` JS, concaténation implicite C, retours à la ligne, `\` de
# continuation, séquences d'échappement) puis exige l'ÉGALITÉ OCTET POUR OCTET
# (UTF-8) des deux textes.
#
# ⛔ ÉCHEC FERMÉ — exigence du gate : la disparition d'une ancre, une ancre en
# double, des ancres dans le désordre, une région sans aucune chaîne, une région
# vide, ou une séquence d'échappement inconnue FONT ÉCHOUER le script. Une
# sentinelle qui ne trouve plus ce qu'elle garde et rend 0 est pire que pas de
# sentinelle : c'est une garantie fausse.
#
# ⛔ CE QU'ELLE NE PEUT PAS FERMER : elle prouve que les deux littéraux ANCRÉS
# sont égaux, pas que l'écran AFFICHE la constante (le harnais
# test/ph_stale_banner_vectors.js évalue le code vivant pour cela), ni que
# l'alerte est émise (check_ph_stale_alert_emitted.sh).
#
# Usage : ./tools/check_ph_stale_text_parity.sh [fichier.h] [fichier.js]
#         (défauts : src/ph_stale_message.h  data/app.js)
# Sortie : 0 si identiques, 1 sinon (diagnostic sur stderr).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

CPP="${1:-src/ph_stale_message.h}"
JS="${2:-data/app.js}"

for f in "$CPP" "$JS"; do
  [ -f "$f" ] || { echo "ERREUR feature-170 (parité de texte) : fichier introuvable : $f" >&2; exit 1; }
done

if ! command -v python3 >/dev/null 2>&1; then
  echo "ERREUR : python3 introuvable sur le PATH" >&2
  exit 1
fi

python3 -I - "$CPP" "$JS" <<'PY'
import re
import sys

BEGIN = "// >>> PH-STALE-ACTE:DEBUT"
END = "// <<< PH-STALE-ACTE:FIN"
ESC = {'"': '"', "'": "'", "\\": "\\", "n": "\n", "t": "\t", "r": "\r", "`": "`", "/": "/"}


def die(msg):
    sys.stderr.write("ERREUR feature-170 (parité de texte) : %s\n" % msg)
    sys.exit(1)


def region(path):
    lines = open(path, encoding="utf-8").read().split("\n")
    # Seules comptent les lignes qui SONT l'ancre (pas un commentaire qui la cite).
    b = [i for i, l in enumerate(lines) if l.strip() == BEGIN]
    e = [i for i, l in enumerate(lines) if l.strip() == END]
    if len(b) != 1:
        die("%s : %d ancre(s) « %s » (attendu : exactement 1) — la sentinelle ne garde plus rien." % (path, len(b), BEGIN))
    if len(e) != 1:
        die("%s : %d ancre(s) « %s » (attendu : exactement 1) — la sentinelle ne garde plus rien." % (path, len(e), END))
    if not b[0] < e[0]:
        die("%s : ancres dans le désordre (FIN avant DEBUT)." % path)
    return lines[b[0] + 1:e[0]]


def literals(path, lines):
    """Concatène tous les littéraux de chaîne de la région ; tout le reste doit
    être de la syntaxe de liaison (define/const, +, ;, continuation, espaces)."""
    text = "\n".join(l for l in lines if not l.strip().startswith("//"))
    out, i, n = [], 0, len(text)
    leftovers = []
    while i < n:
        c = text[i]
        if c in "\"'":
            quote, i, buf = c, i + 1, []
            while True:
                if i >= n or text[i] == "\n":
                    die("%s : chaîne non terminée dans la région ancrée." % path)
                ch = text[i]
                if ch == quote:
                    i += 1
                    break
                if ch == "\\":
                    nxt = text[i + 1] if i + 1 < n else ""
                    if nxt not in ESC:
                        die("%s : séquence d'échappement « \\%s » non gérée — la comparaison ne serait pas fiable." % (path, nxt))
                    buf.append(ESC[nxt])
                    i += 2
                    continue
                buf.append(ch)
                i += 1
            out.append("".join(buf))
        else:
            leftovers.append(c)
            i += 1
    # Hors littéraux, on n'accepte que la syntaxe de liaison.
    rest = "".join(leftovers)
    rest = re.sub(r"#define\s+[A-Za-z_][A-Za-z0-9_]*", "", rest)
    rest = re.sub(r"(const|let|var)\s+[A-Za-z_][A-Za-z0-9_]*\s*=", "", rest)
    rest = re.sub(r"[\s+;\\]", "", rest)
    if rest:
        die("%s : éléments inattendus dans la région ancrée : %r" % (path, rest))
    if not out:
        die("%s : aucune chaîne dans la région ancrée — la sentinelle ne garde plus rien." % path)
    s = "".join(out)
    if not s.strip():
        die("%s : phrase d'acte vide dans la région ancrée." % path)
    return s


cpp_path, js_path = sys.argv[1], sys.argv[2]
cpp = literals(cpp_path, region(cpp_path))
js = literals(js_path, region(js_path))

if cpp.encode("utf-8") != js.encode("utf-8"):
    sys.stderr.write("ERREUR feature-170 (parité de texte) : la phrase d'acte DIVERGE entre le firmware et l'interface.\n")
    sys.stderr.write("  %s : %r\n  %s : %r\n" % (cpp_path, cpp, js_path, js))
    n = min(len(cpp), len(js))
    k = next((j for j in range(n) if cpp[j] != js[j]), n)
    sys.stderr.write("  première différence au caractère %d\n" % k)
    sys.exit(1)

print("OK — phrase d'acte identique octet pour octet (%d octets) : %s / %s" % (len(cpp.encode("utf-8")), cpp_path, js_path))
PY
