#!/usr/bin/env python3
"""Vérifie le fichier de pose d'une carte contre les empreintes de JLCPCB.

Usage : verifier_pose_jlcpcb.py <carte>      (v3-principale, v3-mesure-std)
À lancer après ./scripts/build-jlcpcb.sh <carte>, avant de commander.

Pour chaque pièce, l'empreinte que JLCPCB associe à sa référence est lue dans leur
bibliothèque (seule la référence LCSC est envoyée), posée à la position et à l'angle du
fichier de pose, puis comparée à l'empreinte KiCad de la carte :
- « numéro »  : distance entre chaque pastille JLCPCB et la pastille KiCad de MÊME numéro.
  Un grand écart = pièce tournée, ou numérotation différente (voir la colonne suivante) ;
- « position » : distance à la pastille KiCad la plus proche, quel que soit son numéro.
  Un grand écart = les trous ou pastilles de la carte ne sont pas ceux de la pièce ;
- « corps »   : écart entre le centre du dessin du corps des deux côtés. Sert pour les
  connecteurs à deux pattes, que les pastilles seules ne permettent pas d'orienter.
⚠️ Les numéros diffèrent sans défaut pour : LED (JLCPCB numérote l'anode 1), boutons,
prise USB-C, prise BNC, borniers. Une ligne ⚠️ se lit, elle ne se corrige pas d'office.
Né le 2026-11-26 : une table d'angles recopiée posait les condensateurs au tantale à
l'envers, et l'empreinte de la prise BNC avait ses pattes à 10 mm au lieu de 8.
"""
import csv, json, math, os, subprocess, sys, time
import pcbnew

nom = sys.argv[1]
d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kicad", nom)
# Hors du dépôt, et hors de fabrication/ que build-jlcpcb.sh efface : leur serveur refuse les rafales.
cache = os.path.expanduser("~/.cache/pool-controller/empreintes-jlcpcb"); os.makedirs(cache, exist_ok=True)
lcsc = {}
for l in csv.DictReader(open(os.path.join(d, "fabrication", "bom-%s.csv" % nom), encoding="utf-8")):
    for r in l["Designator"].split(","): lcsc[r] = l["LCSC Part #"].strip()
fps = {f.GetReference(): f for f in pcbnew.LoadBoard(os.path.join(d, nom + ".kicad_pcb")).GetFootprints()}
K = 0.254      # l'unité de leur bibliothèque vaut 10 mils

def empreinte_jlcpcb(c):
    p = os.path.join(cache, c + ".json")
    for essai in range(3):
        if not os.path.exists(p):
            time.sleep(1.5 * essai)
            subprocess.run(["curl", "-s", "-m", "30", "-A", "curl", "-o", p,
                            "https://easyeda.com/api/products/%s/components?version=6.4.19.5" % c])
        try: e = json.load(open(p))["result"]["packageDetail"]["dataStr"]; break
        except Exception:
            if os.path.exists(p): os.remove(p)      # réponse refusée ou tronquée : ne pas la garder
    else: return None, None
    ox, oy = float(e["head"]["x"]), float(e["head"]["y"]); pastilles = []; xs = []; ys = []
    for s in e["shape"]:
        f = s.split("~")
        if f[0] == "PAD": pastilles.append((f[8], (float(f[2]) - ox) * K, (float(f[3]) - oy) * K))
        elif f[0] == "TRACK" and f[2] == "3":
            v = [float(x) for x in f[4].split()]; xs += v[0::2]; ys += v[1::2]
    corps = (((min(xs) + max(xs)) / 2 - ox) * K, ((min(ys) + max(ys)) / 2 - oy) * K) if xs else None
    return pastilles, corps

bilan = {}
for l in csv.DictReader(open(os.path.join(d, "fabrication", "cpl-%s.csv" % nom), encoding="utf-8")):
    r = l["Designator"]; fp = fps[r]; pastilles, corps = empreinte_jlcpcb(lcsc[r])
    type_ = str(fp.GetFPID().GetLibItemName())[:32]
    if not pastilles: bilan.setdefault((9, 9, 9, lcsc[r], type_ + " — empreinte JLCPCB illisible"), []).append(r); continue
    t = math.radians(float(l["Rotation"])); X = float(l["Mid X"][:-2]); Y = -float(l["Mid Y"][:-2])
    place = lambda x, y: (X + x * math.cos(t) + y * math.sin(t), Y - x * math.sin(t) + y * math.cos(t))
    kicad = [(p.GetNumber(), p.GetPosition().x / 1e6, p.GetPosition().y / 1e6) for p in fp.Pads() if p.GetNumber()]
    numero = position = 0
    for n, x, y in pastilles:
        gx, gy = place(x, y)
        position = max(position, min(math.hypot(gx - a, gy - b) for _, a, b in kicad))
        memes = [math.hypot(gx - a, gy - b) for m, a, b in kicad if m == n]
        numero = max(numero, min(memes) if memes else 99)
    ecart = 0
    xs = []; ys = []
    for g in fp.GraphicalItems():
        if g.GetLayerName() == "F.Fab" and g.GetClass() == "PCB_SHAPE":
            b = g.GetBoundingBox(); xs += [b.GetLeft() / 1e6, b.GetRight() / 1e6]; ys += [b.GetTop() / 1e6, b.GetBottom() / 1e6]
    if corps and xs and r.startswith("J") and len(pastilles) <= 8:     # connecteurs seulement : ailleurs leur dessin est partiel
        jx, jy = place(*corps); ecart = math.hypot(jx - (min(xs) + max(xs)) / 2, jy - (min(ys) + max(ys)) / 2)
    bilan.setdefault((round(position, 2), round(numero, 2), round(ecart, 2), lcsc[r], type_), []).append(r)

print("%-3s %8s %8s %6s  %-10s %-32s %s" % ("", "position", "numéro", "corps", "référence", "empreinte KiCad", "repères"))
for (position, numero, ecart, c, type_), refs in sorted(bilan.items(), key=lambda kv: (-kv[0][0], -kv[0][1], -kv[0][2])):
    drapeau = "⛔" if position > 0.6 else ("⚠️" if numero > 0.6 or ecart > 0.8 else "✅")
    print("%-3s %7.2f  %7s  %5.2f  %-10s %-32s %s" % (drapeau, position, "autre" if numero > 90 else "%.2f" % numero, ecart, c, type_, ",".join(refs)[:50]))
