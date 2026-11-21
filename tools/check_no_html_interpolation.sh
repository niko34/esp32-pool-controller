#!/usr/bin/env bash
# =============================================================================
# check_no_html_interpolation.sh — aucune écriture de HTML qui ne soit un littéral pur (bug-179)
# =============================================================================
# Contexte : `script-src 'unsafe-inline'` reste en place dans la CSP du firmware,
# donc le rendu en TEXTE est la seule barrière contre un XSS stocké (journal,
# SSID diffusé, adresse de bus...). Cette sentinelle interdit, dans data/*.js et
# data/*.html (hors *.min.js) :
#     innerHTML = / innerHTML += / outerHTML = / insertAdjacentHTML( /
#     document.write( / document.writeln(
# dont le membre droit n'est PAS un littéral de chaîne pur : une interpolation
# `${...}`, une concaténation, un ternaire, et AUSSI un simple identifiant
# (`el.innerHTML = message`).
#
# Exemption EN LIGNE uniquement, sur la ligne ou la précédente :
#     // HTML-SAFE-EXEMPT(raison écrite)      ou      <!-- HTML-SAFE-EXEMPT(raison) -->
# La raison est obligatoire. Jamais de liste fichier:ligne (elle pourrit).
#
# Usage : ./tools/check_no_html_interpolation.sh [fichier ...]   Sortie : 0 propre, 1 sinon.
# =============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
exec python3 -I - "$@" <<'PY'
import glob, re, sys

files = sys.argv[1:] or sorted(
    f for f in glob.glob("data/*.js") + glob.glob("data/*.html") if not f.endswith(".min.js"))

SINK = re.compile(r'(?:\b(?:innerHTML|outerHTML)\s*\+?=(?!=)|\binsertAdjacentHTML\s*\(|\bdocument\s*\.\s*write(?:ln)?\s*\()')
EXEMPT = re.compile(r'HTML-SAFE-EXEMPT\(\s*[^)\s][^)]*\)')

def pure_literal_end(t, i):
    """Si t[i:] commence par un littéral de chaîne pur, renvoie l'index après lui, sinon None."""
    if i >= len(t) or t[i] not in "'\"`":
        return None
    q = t[i]; j = i + 1
    while j < len(t):
        c = t[j]
        if c == "\\":
            j += 2; continue
        if q == "`" and c == "$" and t[j+1:j+2] == "{":
            return None
        if c == q:
            return j + 1
        if q != "`" and c == "\n":
            return None
        j += 1
    return None

bad = 0
n_sinks = 0
n_exempt = 0
for f in files:
    try:
        text = open(f, encoding="utf-8").read()
    except OSError as e:
        print(f"ERREUR : {f} illisible ({e})", file=sys.stderr); sys.exit(1)
    lines = text.split("\n")
    starts = [0]
    for ln in lines[:-1]:
        starts.append(starts[-1] + len(ln) + 1)
    for m in SINK.finditer(text):
        lineno = text.count("\n", 0, m.start()) + 1
        line = lines[lineno - 1]
        col = m.start() - starts[lineno - 1]
        head = line[:col]
        stripped = line.strip()
        # ⚠️ FAUX VERT #2 (audit P2) : `const u = "http://x"; el.innerHTML = y;`
        # portait un `//` DANS UNE CHAÎNE avant le puits, et la ligne entière
        # passait pour un commentaire. On retire d'abord les littéraux de head.
        head_nostr = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|`(?:\\.|[^`\\])*`', '""', head)
        if stripped.startswith(("//", "*", "/*", "<!--")) or "//" in head_nostr:
            continue  # commentaire
        n_sinks += 1
        i = m.end()
        if m.group(0).rstrip().endswith("("):
            # insertAdjacentHTML(pos, X) : X est le 2e argument ; write(X) : le 1er
            if "insertAdjacentHTML" in m.group(0):
                k = text.find(",", i)
                if k < 0:
                    ok = False
                else:
                    i = k + 1; ok = None
            else:
                ok = None
        else:
            ok = None
        if ok is None:
            while i < len(text) and text[i] in " \t\r\n":
                i += 1
            e = pure_literal_end(text, i)
            ok = False
            if e is not None:
                k = e
                while k < len(text) and text[k] in " \t":
                    k += 1
                if k >= len(text):
                    ok = True
                elif text[k] in ";)}":
                    ok = True
                elif text[k] in "\n\r":
                    # ⚠️ FAUX VERT #1 (audit P2) : `el.innerHTML = "a"` suivi d'un
                    # `+ x` À LA LIGNE SUIVANTE passait pour un littéral pur.
                    # ⇒ on saute TOUS les blancs et on refuse si l'expression
                    #   continue (opérateur, appel, indexation, gabarit...).
                    while k < len(text) and text[k] in " \t\r\n":
                        k += 1
                    ok = k >= len(text) or text[k] not in "+?:.,[(`-*/%&|"
                else:
                    ok = False
        if ok:
            continue
        prev = lines[lineno - 2] if lineno >= 2 else ""
        if EXEMPT.search(line) or EXEMPT.search(prev):
            n_exempt += 1
            continue
        bad += 1
        print(f"{f}:{lineno}: écriture HTML non littérale : {stripped[:110]}", file=sys.stderr)

if bad:
    print(f"ERREUR bug-179 : {bad} écriture(s) HTML dont le membre droit n'est pas un littéral pur.\n"
          "Utiliser textContent / createElement / replaceChildren, ou exempter EN LIGNE avec une raison :\n"
          "  // HTML-SAFE-EXEMPT(raison)", file=sys.stderr)
    sys.exit(1)
print(f"OK — {n_sinks} écriture(s) HTML, toutes littérales ou exemptées en ligne ({n_exempt} exemption(s)).")
PY
