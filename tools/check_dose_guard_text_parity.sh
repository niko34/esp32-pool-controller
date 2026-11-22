#!/usr/bin/env bash
# =============================================================================
# check_dose_guard_text_parity.sh — les motifs du front reconnaissent encore les
# messages de garde du firmware (bug-199)
# =============================================================================
# Le front convertit « 7.6 » en « 7,6 » dans les causes de garde pH/ORP, mais UNIQUEMENT
# via une table de motifs ancrés (data/app.js, bloc FIRMWARE-TEXT-MOTIFS). Défaut de cette
# approche : si le firmware REFORMULE un message, la conversion s'arrête EN SILENCE.
# Cette sentinelle lit les chaînes réelles des appels setGuardRefusalCause( de
# src/pump_controller.cpp (celles qui formatent un nombre avec String(x, N)) et exige :
#   - chaque message est reconnu par UN motif et converti (point -> virgule, rien d'autre) ;
#   - chaque motif reconnaît au moins un message (aucun motif orphelin).
# ÉCHOUE FERMÉ : fichier introuvable, bloc de motifs absent/vide, aucun message extrait,
# message non reconnu, motif orphelin, node absent => rouge.
# Usage : ./tools/check_dose_guard_text_parity.sh [pump_controller.cpp] [app.js]
# =============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
command -v node >/dev/null 2>&1 || { echo "ECHEC : node introuvable" >&2; exit 1; }
exec python3 -I - "${1:-src/pump_controller.cpp}" "${2:-data/app.js}" <<'PY'
import itertools
import json, re, subprocess, sys

cpp_path, js_path = sys.argv[1], sys.argv[2]
def fail(msg):
    print("ECHEC : " + msg); sys.exit(1)
try:
    cpp = open(cpp_path, encoding="utf-8").read()
    js = open(js_path, encoding="utf-8").read()
except OSError as e:
    fail(f"lecture impossible ({e})")

# --- 1. messages C++ : arguments de setGuardRefusalCause( qui formatent un nombre
msgs = []
for m in re.finditer(r'setGuardRefusalCause\s*\(', cpp):
    pre = cpp[max(0, m.start() - 12):m.start()]
    if "::" in pre or "void" in pre:
        continue  # définition
    i, depth, instr, esc = m.end(), 1, False, False
    while i < len(cpp) and depth:
        c = cpp[i]
        if instr:
            if esc: esc = False
            elif c == "\\": esc = True
            elif c == '"': instr = False
        else:
            if c == '"': instr = True
            elif c == "(": depth += 1
            elif c == ")": depth -= 1
        i += 1
    if depth: fail("parenthese non fermee dans un appel setGuardRefusalCause")
    args = cpp[m.end():i - 1]
    # Durcissement n°2 (revue 2026-11-22) : ⛔ ne sauter QUE les messages sans
    # AUCUN String(. Un String(x) SANS precision rend 2 decimales en Arduino
    # (« 7.60 ») et passait ici en SILENCE, sans orpheliner aucun motif.
    if "String" not in args:
        continue  # message sans valeur — rien a convertir, rien a ancrer
    if not re.search(r'String\s*\([^"]*?,\s*\d\s*\)', args):
        fail("message de garde avec String(x) SANS precision : Arduino rend 2 "
             "decimales (« 7.60 »). Preciser String(x, N) et ajouter un motif "
             "dans _FIRMWARE_TEXT_MOTIFS.")
    # retire le premier argument (index de pompe)
    args = args.split(",", 1)[1]
    # Durcissement n°3 : cycle, pour qu'un message a 4 valeurs rende un message
    # LISIBLE et non un StopIteration (il echouait fermé, mais illisiblement).
    samples = itertools.cycle(["6.1", "6.8", "7.3"])
    out, k, n = [], 0, len(args)
    vals = []
    while k < n:
        c = args[k]
        if c == '"':
            j, lit = k + 1, ""
            while j < n and args[j] != '"':
                if args[j] == "\\":
                    if args[j + 1] not in '"\\': fail(f"sequence d'echappement inconnue dans {args[:40]!r}")
                    j += 1
                lit += args[j]; j += 1
            out.append(lit); k = j + 1
        elif args.startswith("String", k):
            d, j = 0, k + 6
            while j < n:
                if args[j] == "(": d += 1
                elif args[j] == ")":
                    d -= 1
                    if d == 0: break
                j += 1
            v = next(samples); vals.append(v); out.append(v); k = j + 1
        elif c in " \t\r\n+":
            k += 1
        else:
            fail(f"expression inattendue dans un message de garde : {args[k:k+40]!r} (sentinelle a adapter)")
    msgs.append("".join(out))
if not msgs:
    fail(f"aucun message de garde a valeur extrait de {cpp_path} (sentinelle AVEUGLE)")

# --- Durcissement n°1 (revue 2026-11-22) — LE PLUS IMPORTANT ---------------
# ⚠️ L'appareil TRONQUE : kDoseBlockedReasonMaxLen = 96 (constants.h), strlcpy +
# utf8ClampInPlace (pump_controller.cpp). La sentinelle testait le message ENTIER,
# donc elle validait un texte que l'ECRAN NE RECOIT PAS.
# 🔴 Et ce n'est pas theorique : le message a DEUX valeurs fait 91 octets sur 95
# (4 o de marge), et le firmware porte la cicatrice — son ancien texte « etait
# tronque en production ». Le jour ou il s'allonge, la coupe peut tomber ENTRE
# les deux valeurs : la conversion cesserait, et la sentinelle resterait VERTE.
# ⇒ On clampe comme l'appareil, et on teste ce que l'ecran recoit.
# ⭐ Bonus gratuit : la sentinelle devient aussi une ALERTE DE LONGUEUR.
MAXLEN = 95  # kDoseBlockedReasonMaxLen - 1 (octet NUL)
clamped = []
for msg in msgs:
    raw = msg.encode("utf-8")
    if len(raw) > MAXLEN:
        print(f"ALERTE LONGUEUR : message de garde a {len(raw)} octets > {MAXLEN} "
              f"— il sera TRONQUE sur l'appareil : {msg[:60]!r}")
        cut = raw[:MAXLEN]
        while cut and (cut[-1] & 0xC0) == 0x80:   # ne pas couper un caractere UTF-8
            cut = cut[:-1]
        msg = cut.decode("utf-8", "ignore")
    clamped.append(msg)
msgs = clamped

# --- 2. motifs du front
b = re.search(r'// >>> FIRMWARE-TEXT-MOTIFS:DEBUT\n(.*?)^\s*// <<< FIRMWARE-TEXT-MOTIFS:FIN', js, re.S | re.M)
if not b or "_FIRMWARE_TEXT_MOTIFS" not in b.group(1):
    fail(f"bloc FIRMWARE-TEXT-MOTIFS introuvable dans {js_path}")
node = r'''
const fs=require("fs");const input=JSON.parse(fs.readFileSync(0,"utf8"));
const motifs=new Function(input.block+"; return _FIRMWARE_TEXT_MOTIFS;")();
if(!Array.isArray(motifs)||motifs.length===0){console.log(JSON.stringify({err:"table vide"}));process.exit(0);}
const hits=motifs.map(()=>0);const res=[];
for(const t of input.msgs){const w=[];motifs.forEach((m,i)=>{if(m[0].test(t)){w.push(i);hits[i]++;}});
  res.push({t,w,out:w.length?t.replace(motifs[w[0]][0],motifs[w[0]][1]):t});}
console.log(JSON.stringify({n:motifs.length,hits,res}));'''
r = subprocess.run(["node", "-e", node], input=json.dumps({"block": b.group(1), "msgs": msgs}),
                   capture_output=True, text=True)
if r.returncode: fail("node a echoue : " + r.stderr.strip()[:200])
d = json.loads(r.stdout)
if "err" in d: fail(d["err"])
bad = 0
for e in d["res"]:
    # ⚠️ Cet ATTENDU est lui-meme une regle de FORME — celle que l'utilisateur a
    # ecartee pour le code livre. Inoffensif ICI (tous les nombres des messages de
    # garde SONT des valeurs de capteur), mais si un message futur portait une
    # version ou une IP, la sentinelle deviendrait ROUGE A TORT. ⛔ Rouge, donc
    # jamais dangereux — juste deroutant. A relire ce jour-la.
    want = re.sub(r'(\d)\.(\d)', r'\1,\2', e["t"])
    if len(e["w"]) != 1:
        print(f"message NON reconnu ou ambigu ({len(e['w'])} motifs) : {e['t']}"); bad += 1
    elif e["out"] != want:
        print(f"conversion incorrecte : {e['out']!r} au lieu de {want!r}"); bad += 1
for i, h in enumerate(d["hits"]):
    if h == 0:
        print(f"motif n°{i + 1} ORPHELIN : ne reconnait plus aucun message du firmware"); bad += 1
if bad: fail(f"{bad} ecart(s) entre les messages de {cpp_path} et les motifs de {js_path}")
print(f"OK : {len(msgs)} messages de garde reconnus, {d['n']} motifs, aucun orphelin")
PY
