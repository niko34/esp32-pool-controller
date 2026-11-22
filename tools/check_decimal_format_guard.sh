#!/usr/bin/env bash
# =============================================================================
# check_decimal_format_guard.sh — aucun formateur à virgule sur une valeur machine (bug-199 D1)
# =============================================================================
# Les formateurs fmtFr / _fmtPh / _fmt2 / _fmtPhOffset2 écrivent une VIRGULE. Elle est
# correcte dans un texte affiché, et CASSE tout ce qui est relu par du code :
#   - `.value =` d'un <input>   : la saisie et l'enregistrement, en silence ;
#   - `dataset.x =`             : valeur relue par le code ;
#   - `style.x = ` / setProperty : `width: 72,4%` est INVALIDE, le navigateur ignore la
#                                  règle, la barre reste figée, sans aucune erreur.
# Deux familles de formateurs rendent la virgule : les quatre fonctions ci-dessus ET
# `toLocaleString("fr-FR", ...)` — les deux sont gardées (volet 1).
# Volet 2 : data/*.html — un attribut numérique (`step`, `min`, `max`, `value`) ne porte
# JAMAIS de virgule décimale (`step="0,1"` rend le champ invalide, sans erreur).
# Ce script échoue si un de ces formateurs apparaît dans le membre droit (jusqu'au `;`)
# d'une telle écriture, dans data/*.js (hors *.min.js).
#
# ÉCHOUE FERMÉ : fichier absent, ou l'un des quatre formateurs introuvable (la garde ne
# saurait plus rien garder) => rouge, jamais vert.
#
# Usage : ./tools/check_decimal_format_guard.sh [fichier ...]   Sortie : 0 propre, 1 sinon.
# =============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
exec python3 -I - "$@" <<'PY'
import glob, re, sys

args = sys.argv[1:]
files = args or sorted(f for f in glob.glob("data/*.js") if not f.endswith(".min.js"))
if not files:
    print("ECHEC : aucun fichier data/*.js trouve (garde aveugle)"); sys.exit(1)

# ⚠️ LIMITE CONNUE, assumee (relevee par test-engineer) : une garde SYNTAXIQUE ne
# voit pas un formateur passe par une variable intermediaire —
#   const t = fmtFr(v, 1); el.value = t;   ⇒ NON detecte.
# ⛔ Ne pas laisser croire le contraire : c'est ecrit dans docs/DESIGN_SYSTEM.md.
FORMATTERS = ("fmtFr", "_fmtPh", "_fmt2", "_fmtPhOffset2")
FMT_CALL = re.compile(r'\b(?:' + "|".join(FORMATTERS) + r')\s*\(|\.toLocaleString\s*\(')
SINK = re.compile(
    r'(?:\.value\s*=(?!=)'
    r'|\bdataset(?:\.\w+|\[[^\]]*\])\s*=(?!=)'
    r'|\.style\.\w+\s*=(?!=)'
    r'|\.style\s*\[[^\]]*\]\s*=(?!=)'
    r'|\.style\.setProperty\s*\('
    r'|\.setAttribute\s*\(\s*[\'"](?:value|style|data-[\w-]+)[\'"]\s*,)')

texts = {}
for f in files:
    try:
        texts[f] = open(f, encoding="utf-8").read()
    except OSError as e:
        print(f"ECHEC : lecture impossible de {f} ({e})"); sys.exit(1)

# ANCRAGE — bug-199, correctif apres attaque par test-engineer (2026-11-22).
# ⚠️ La version precedente sautait ce controle des qu'un fichier etait passe en
# ARGUMENT : la garde rendait alors VERT sur un fichier vide, c'est-a-dire une
# GARANTIE FAUSSE. ⭐ Une sentinelle qui ne trouve plus ce qu'elle garde et rend 0
# est pire que pas de sentinelle. ⇒ L'ancrage s'applique DESORMAIS dans les deux
# modes, sur l'UNION des fichiers examines : si aucun d'eux ne definit les quatre
# formateurs, la garde est AVEUGLE et doit le dire.
js_files = [f for f in files if f.endswith(".js")]
html_files = sorted(glob.glob("data/*.html")) if not args else [a for a in args if a.endswith(".html")]
if not js_files and not html_files:
    print("ECHEC : ni .js ni .html dans le perimetre — rien n'est examine"); sys.exit(1)
blob = "\n".join(texts[f] for f in js_files)
for name in (FORMATTERS if js_files else ()):
    if not re.search(r'function\s+' + re.escape(name) + r'\s*\(', blob):
        print(f"ECHEC : formateur {name} introuvable dans le perimetre examine "
              f"({', '.join(files)}) — garde AVEUGLE, refus de rendre vert."); sys.exit(1)

bad = 0
for f, t in ((f, texts[f]) for f in js_files):
    for m in SINK.finditer(t):
        end = t.find(";", m.end())
        rhs = t[m.end(): end if end != -1 else len(t)]
        if FMT_CALL.search(rhs):
            line = t.count("\n", 0, m.start()) + 1
            print(f"{f}:{line}: formateur a virgule dans une valeur machine "
                  f"(.value / dataset / style) : {t[m.start():m.end()+len(rhs)].strip()[:110]}")
            bad += 1
# Volet 2 : attributs numeriques HTML. Echoue ferme : aucun fichier => rouge.
if not args and not html_files:
    print("ECHEC : aucun fichier data/*.html trouve (volet HTML aveugle)"); sys.exit(1)
ATTR = re.compile(r'\b(step|min|max|value)\s*=\s*(?:"([^"]*)"|\'([^\']*)\')')
NUMTYPE = re.compile(r'type\s*=\s*(?:["\']?)(?:number|range)\b')
numeric_tags = 0
for f in html_files:
    try:
        h = open(f, encoding="utf-8").read()
    except OSError as e:
        print(f"ECHEC : lecture impossible de {f} ({e})"); sys.exit(1)
    for tag in re.finditer(r'<[^>]*>', h):
        if not NUMTYPE.search(tag.group(0)):
            continue
        numeric_tags += 1
        for m in ATTR.finditer(tag.group(0)):
            if re.search(r'\d,\d', m.group(2) or m.group(3) or ""):
                line = h.count("\n", 0, tag.start()) + 1
                print(f'{f}:{line}: virgule decimale dans un attribut numerique : {m.group(0)}')
                bad += 1
if html_files and numeric_tags == 0:
    print("ECHEC : aucun champ type=number|range trouve dans " + ", ".join(html_files) +
          " (volet HTML AVEUGLE, data/index.html en porte une vingtaine) — refus de rendre vert."); sys.exit(1)
if bad:
    print(f"ECHEC : {bad} ecriture(s) machine avec une virgule (bug-199)"); sys.exit(1)
ok = []
ok.append("aucun formateur a virgule sur un .value, un dataset ou une valeur CSS" if js_files else "volet JS NON EXAMINE (aucun .js fourni)")
ok.append("aucun attribut numerique HTML a virgule" if html_files else "volet HTML NON EXAMINE (aucun .html fourni)")
print("OK : " + " ; ".join(ok))
PY
