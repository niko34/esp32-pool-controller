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
     le point d'ancrage de l'empreinte — la broche 1 pour une pièce traversante, un
     autre point pour le module ESP32 (3,2 mm), le régulateur 5 V (3,1 mm) ou la prise
     USB-C (1,5 mm). On écrit donc toujours le milieu des pastilles numérotées, lu sur
     la carte ; DECALAGES couvre les rares pièces où JLCPCB fait autrement ;
  2. l'ANGLE : pour certains boîtiers, l'angle zéro de JLCPCB n'est pas celui de
     KiCad. La table ROTATIONS ajoute l'écart. ⚠️ Elle ne dispense ni de lancer
     scripts/verifier_pose_jlcpcb.py, ni de regarder l'écran de pose : une famille de
     boîtiers jamais vue n'y figure pas.
- Une pièce sans référence LCSC, ou marquée « ne pas poser », n'est ni dans l'un
  ni dans l'autre : elle est listée à l'écran, pour qu'on sache qu'elle manque.
"""
import csv, sys, re, math

import pcbnew

bom_in, pos_in, bom_out, cpl_out, carte = sys.argv[1:6]

# Écart d'angle à ajouter, par début de nom d'empreinte. « vu » = vérifié sur l'écran
# de pose de JLCPCB ; « empreinte » = établi en comparant l'empreinte de JLCPCB à celle
# de KiCad, pastille par pastille, avec scripts/verifier_pose_jlcpcb.py.
# ⛔ Ne rien ajouter ici depuis une table trouvée ailleurs : la ligne « CP_EIA- +180 »,
# reprise d'une table publique, posait les condensateurs au tantale À L'ENVERS.
ROTATIONS = [
    ("SOT-23-6", 270),     # vu (protection USB) — à garder AVANT « SOT-23 »
    ("SOT-23", 180),       # vu (transistors)
    ("BatteryHolder_", 180),  # vu (support de pile : corps excentré du mauvais côté sans cela)
    ("SOT-223", 180),      # vu (régulateur 3,3 V)
    ("SOIC-", 270),        # vu (SOIC-8, SOIC-16, SOIC-16W)
    ("IDC-Header", 270),   # vu (embase de débogage)
    ("VSSOP-", 270),       # empreinte (convertisseur de mesure)
    ("JST_XH_", 180),      # empreinte : sans cela la broche 1 tombe sur la pastille 8
    ("BNC_Dosin", 180),    # empreinte : sans cela la prise pointe vers l'intérieur de la carte
    ("PhoenixContact_", 180),  # empreinte : corps excentré du côté opposé au dessin KiCad
    ("PinHeader_1x", 270),  # empreinte : barrette couchée chez JLCPCB, debout dans KiCad (cavalier JP1)
]
# Pièces dont l'origine JLCPCB n'est pas au milieu de ses pastilles : position de ce
# milieu dans l'empreinte JLCPCB (mm, y vers le bas), lue dans leur bibliothèque.
DECALAGES = [
    ("PhoenixContact_MSTBVA_2,5_3-G", (0.0, -0.5)),   # bornier 3 points C505789
]
def ecart_angle(empreinte):
    for debut, angle in ROTATIONS:
        if empreinte.startswith(debut): return angle
    return 0
milieu = {}
for fp in pcbnew.LoadBoard(carte).GetFootprints():
    pastilles = [p for p in fp.Pads() if p.GetNumber()]    # sans numéro = découpe de pâte à braser
    xs = [p.GetPosition().x / 1e6 for p in pastilles]; ys = [p.GetPosition().y / 1e6 for p in pastilles]
    if not xs: continue
    nom = str(fp.GetFPID().GetLibItemName())
    milieu[fp.GetReference()] = ((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, nom)
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
        angle = (float(l["Rot"]) + ecart_angle(empreinte)) % 360
        for debut, (dx, dy) in DECALAGES:
            if empreinte.startswith(debut):
                t = math.radians(angle)
                x -= dx * math.cos(t) + dy * math.sin(t); y -= -dx * math.sin(t) + dy * math.cos(t)
        w.writerow([l["Ref"], "%.4fmm" % x, "%.4fmm" % -y,
                    "Top" if l["Side"].lower().startswith("top") else "Bottom", "%g" % angle])
print("pièces à poser : %d repères, %d lignes de BOM" % (len(poses), len(groupes)))
if poses - vues: print("⚠️ dans la BOM mais sans position sur la carte :", ", ".join(sorted(poses - vues, key=cle)))
for ref, val, pourquoi in sorted(absentes, key=lambda a: cle(a[0])): print("   non posée : %s (%s) — %s" % (ref, val, pourquoi))
