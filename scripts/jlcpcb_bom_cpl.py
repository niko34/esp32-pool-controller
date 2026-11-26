#!/usr/bin/env python3
"""Met la liste des pièces et les positions de KiCad au format de JLCPCB.

Usage : jlcpcb_bom_cpl.py <bom KiCad> <positions KiCad> <bom JLCPCB> <cpl JLCPCB>

- BOM JLCPCB : Comment, Designator, Footprint, LCSC Part # — UNE ligne par référence
  LCSC. ⚠️ Deux lignes portant la même référence (par exemple cinq borniers nommés
  chacun d'après leur usage) sont refusées par le site : quantité 0, ligne décochée
  (constaté à la commande du 2026-11-26). Les noms différents sont réunis par « / ».
- CPL JLCPCB : Designator, Mid X, Mid Y, Layer, Rotation.
- Une pièce sans référence LCSC, ou marquée « ne pas poser », n'est ni dans l'un
  ni dans l'autre : elle est listée à l'écran, pour qu'on sache qu'elle manque.
"""
import csv, sys, re

bom_in, pos_in, bom_out, cpl_out = sys.argv[1:5]
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
        w.writerow([l["Ref"], "%.4fmm" % float(l["PosX"]), "%.4fmm" % float(l["PosY"]),
                    "Top" if l["Side"].lower().startswith("top") else "Bottom", "%g" % float(l["Rot"])])
print("pièces à poser : %d repères, %d lignes de BOM" % (len(poses), len(groupes)))
if poses - vues: print("⚠️ dans la BOM mais sans position sur la carte :", ", ".join(sorted(poses - vues, key=cle)))
for ref, val, pourquoi in sorted(absentes, key=lambda a: cle(a[0])): print("   non posée : %s (%s) — %s" % (ref, val, pourquoi))
