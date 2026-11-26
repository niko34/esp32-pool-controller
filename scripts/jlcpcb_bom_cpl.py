#!/usr/bin/env python3
"""Met la liste des pièces et les positions de KiCad au format de JLCPCB.

Usage : jlcpcb_bom_cpl.py <bom KiCad> <positions KiCad> <bom JLCPCB> <cpl JLCPCB> <carte .kicad_pcb>

- BOM JLCPCB : Comment, Designator, Footprint, LCSC Part # — UNE ligne par référence
  LCSC. ⚠️ Deux lignes portant la même référence (par exemple cinq borniers nommés
  chacun d'après leur usage) sont refusées par le site : quantité 0, ligne décochée
  (constaté à la commande du 2026-11-26). Les noms différents sont réunis par « / ».
- CPL JLCPCB : Designator, Mid X, Mid Y, Layer, Rotation. Deux écarts entre KiCad et
  JLCPCB y sont corrigés (constatés à l'écran de pose le 2026-11-26) :
  1. la POSITION : JLCPCB centre sa pièce sur le milieu de ses pastilles, KiCad donne
     le point d'ancrage de l'empreinte — la broche 1 pour une pièce traversante. Quand
     l'ancrage est sur la broche 1, et pour le module ESP32 (vu décalé de 3,2 mm), on
     écrit le milieu des pastilles, lu sur la carte. Les autres pièces gardent leur
     ancrage : le régulateur 5 V et la prise USB-C étaient bien posés ainsi ;
  2. l'ANGLE : pour certains boîtiers, l'angle zéro de JLCPCB n'est pas celui de
     KiCad. La table ROTATIONS ajoute l'écart. ⚠️ Elle ne dispense pas de regarder
     l'écran de pose : une famille de boîtiers jamais vue n'y figure pas.
- Une pièce sans référence LCSC, ou marquée « ne pas poser », n'est ni dans l'un
  ni dans l'autre : elle est listée à l'écran, pour qu'on sache qu'elle manque.
"""
import csv, sys, re

import pcbnew

bom_in, pos_in, bom_out, cpl_out, carte = sys.argv[1:6]

# Écart d'angle à ajouter, par début de nom d'empreinte. « vu » = vérifié sur l'écran
# de pose de JLCPCB ; les autres viennent de la table publique de l'extension
# kicad-jlcpcb-tools et restent à confirmer à l'écran.
ROTATIONS = [
    ("SOT-23-6", 270),     # vu (protection USB) — à garder AVANT « SOT-23 »
    ("SOT-23", 180),       # vu (transistors)
    ("BatteryHolder_", 180),  # vu (support de pile : corps excentré du mauvais côté sans cela)
    ("SOT-223", 180),      # vu (régulateur 3,3 V)
    ("SOIC-", 270),        # vu (SOIC-8, SOIC-16, SOIC-16W)
    ("IDC-Header", 270),   # vu (embase de débogage)
    ("VSSOP-", 270),       # table publique
    ("CP_EIA-", 180),      # table publique (condensateurs au tantale)
]
def ecart_angle(empreinte):
    for debut, angle in ROTATIONS:
        if empreinte.startswith(debut): return angle
    return 0
milieu = {}
for fp in pcbnew.LoadBoard(carte).GetFootprints():
    xs = [p.GetPosition().x / 1e6 for p in fp.Pads()]; ys = [p.GetPosition().y / 1e6 for p in fp.Pads()]
    if not xs: continue
    nom = str(fp.GetFPID().GetLibItemName()); a = fp.GetPosition()
    sur_broche_1 = any(p.GetNumber() == "1" and abs(p.GetPosition().x - a.x) < 10000
                       and abs(p.GetPosition().y - a.y) < 10000 for p in fp.Pads())
    if sur_broche_1 or nom.startswith("ESP32-"):
        milieu[fp.GetReference()] = ((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, nom)
    else:
        milieu[fp.GetReference()] = (a.x / 1e6, a.y / 1e6, nom)
def cle(ref):
    m = re.match(r"([A-Za-z#]+)(\d+)", ref); return (m.group(1), int(m.group(2))) if m else (ref, 0)

groupes, poses, absentes = {}, set(), []
for l in csv.DictReader(open(bom_in, encoding="utf-8")):
    ref, lcsc = l["Reference"].strip(), l["LCSC"].strip()
    if ref.startswith("#"): continue
    if not lcsc or l.get("DNP", "").strip():
        absentes.append((ref, l["Value"], "ne pas poser" if lcsc else "sans référence LCSC")); continue
    g = groupes.setdefault(lcsc, {"refs": [], "vals": [], "fp": l["Footprint"].split(":")[-1]})
    g["refs"].append(ref); poses.add(ref)
    if l["Value"] not in g["vals"]: g["vals"].append(l["Value"])
with open(bom_out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f); w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
    for lcsc, g in sorted(groupes.items(), key=lambda kv: cle(sorted(kv[1]["refs"], key=cle)[0])):
        w.writerow([" / ".join(g["vals"]), ",".join(sorted(g["refs"], key=cle)), g["fp"], lcsc])

vues = set()
with open(cpl_out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f); w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
    for l in sorted(csv.DictReader(open(pos_in, encoding="utf-8")), key=lambda l: cle(l["Ref"])):
        if l["Ref"] not in poses: continue
        vues.add(l["Ref"])
        x, y, empreinte = milieu[l["Ref"]]       # y de KiCad vers le bas, celui du fichier vers le haut
        w.writerow([l["Ref"], "%.4fmm" % x, "%.4fmm" % -y,
                    "Top" if l["Side"].lower().startswith("top") else "Bottom",
                    "%g" % ((float(l["Rot"]) + ecart_angle(empreinte)) % 360)])
print("pièces à poser : %d repères, %d lignes de BOM" % (len(poses), len(groupes)))
if poses - vues: print("⚠️ dans la BOM mais sans position sur la carte :", ", ".join(sorted(poses - vues, key=cle)))
for ref, val, pourquoi in sorted(absentes, key=lambda a: cle(a[0])): print("   non posée : %s (%s) — %s" % (ref, val, pourquoi))
