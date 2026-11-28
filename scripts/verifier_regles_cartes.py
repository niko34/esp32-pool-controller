#!/usr/bin/env python3
"""Vérifie une carte du PCB v3 contre les règles de kicad/REGLES-CEM-ET-FIABILITE.md.

Usage : verifier_regles_cartes.py <carte> [dossier]      (carte : v3-principale, v3-mesure-std)
        dossier : une copie du projet de la carte, à la place de kicad/<carte> (sert à scripts/essai_verificateur_regles.py)
Sort avec le code 1 si une règle n'est pas tenue : scripts/build-jlcpcb.sh s'arrête alors
AVANT de produire les fichiers de fabrication.

Chaque contrôle porte le numéro de sa règle dans le document. ✅ tenue, ⚠️ à lire, ❌ non tenue.
Ce script ne remplace ni le contrôle des règles de KiCad (il le lance) ni la relecture : il empêche
qu'une modification future défasse, sans que personne le voie, ce que l'étude du 2026-11-28 a établi.
⚠️ Une règle qui gêne se discute dans le document, AVEC sa raison — elle ne se retire pas d'ici en silence.
"""
import fnmatch, json, math, os, re, shutil, subprocess, sys, tempfile
import numpy as np
import pcbnew

def empreinte_de_carte(fichier):
    """Résumé d'une carte : cuivre, pièces et leurs places, dessins, zones. Deux cartes de même résumé sont la même carte."""
    import hashlib
    c = pcbnew.LoadBoard(fichier); t = pcbnew.ToMM; E = []
    for x in c.GetTracks():
        if x.Type() == pcbnew.PCB_VIA_T: E.append(("via", x.GetNetname(), round(t(x.GetPosition().x), 3), round(t(x.GetPosition().y), 3), round(t(x.GetDrill()), 2)))
        else:
            a, z = (round(t(x.GetStart().x), 3), round(t(x.GetStart().y), 3)), (round(t(x.GetEnd().x), 3), round(t(x.GetEnd().y), 3))
            E.append(("piste", x.GetNetname(), c.GetLayerName(x.GetLayer()), min(a, z), max(a, z), round(t(x.GetWidth()), 3)))
    for f in c.GetFootprints(): E.append(("piece", f.GetReference(), f.GetValue(), round(t(f.GetPosition().x), 3), round(t(f.GetPosition().y), 3), f.GetOrientationDegrees(), tuple(sorted((q.GetNumber(), q.GetNetname()) for q in f.Pads()))))
    for d in c.GetDrawings():
        if d.GetClass() == "PCB_SHAPE": E.append(("forme", c.GetLayerName(d.GetLayer()), round(t(d.GetStart().x), 3), round(t(d.GetStart().y), 3), round(t(d.GetEnd().x), 3), round(t(d.GetEnd().y), 3), round(t(d.GetWidth()), 3)))
        elif d.GetClass() == "PCB_TEXT": E.append(("texte", c.GetLayerName(d.GetLayer()), d.GetText(), round(t(d.GetPosition().x), 2), round(t(d.GetPosition().y), 2)))
    for z in c.Zones(): bb = z.GetBoundingBox(); E.append(("zone", z.GetNetname(), z.GetIsRuleArea(), round(t(bb.GetLeft()), 2), round(t(bb.GetTop()), 2), round(t(bb.GetRight()), 2), round(t(bb.GetBottom()), 2)))
    return hashlib.sha256(repr(sorted(E, key=repr)).encode()).hexdigest()
if len(sys.argv) == 3 and sys.argv[1] == "--empreinte":      # appelé par lui-même : une carte par processus
    print(empreinte_de_carte(sys.argv[2])); sys.stdout.flush(); os._exit(0)      # sortie sèche : pcbnew plante parfois en rendant la mémoire

NOM = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
RACINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DOS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(RACINE, "kicad", NOM); PCB = os.path.join(DOS, NOM + ".kicad_pcb"); SCH = os.path.join(DOS, NOM + ".kicad_sch")
b = pcbnew.LoadBoard(PCB); T = pcbnew.ToMM; mm = pcbnew.FromMM
F, B = pcbnew.F_Cu, pcbnew.B_Cu
ECHECS = []
def ok(regle, texte): print("✅ %-4s %s" % (regle, texte))
def lire(regle, texte): print("⚠️  %-4s %s" % (regle, texte))
def non(regle, texte): print("❌ %-4s %s" % (regle, texte)); ECHECS.append(regle)
def juge(regle, cond, bien, mal): ok(regle, bien) if cond else non(regle, mal)

# ---------------------------------------------------------------- ce que la carte contient
FP = {f.GetReference(): f for f in b.GetFootprints()}
RES = {}                                    # réseau -> broches « R1.2 »
for f in b.GetFootprints():
    for q in f.Pads():
        if q.GetNetname(): RES.setdefault(q.GetNetname(), set()).add("%s.%s" % (f.GetReference(), q.GetNumber()))
def reseau(broche):
    r, n = broche.split(".")
    for q in FP[r].Pads():
        if q.GetNumber() == n: return q.GetNetname()
def valeur(ref): return FP[ref].GetValue()
def ohms(v):
    m = re.match(r"\s*(\d+)([RkM])(\d*)", v)
    return float(m.group(1) + "." + (m.group(3) or "0")) * {"R": 1, "k": 1e3, "M": 1e6}[m.group(2)] if m else None
def farads(v):
    m = re.match(r"\s*(\d+)([pnu])(\d*)", v)
    return float(m.group(1) + "." + (m.group(3) or "0")) * {"p": 1e-12, "n": 1e-9, "u": 1e-6}[m.group(2)] if m else None
def entre(ref, a, c):
    """La pièce à deux broches `ref` est-elle branchée entre les réseaux a et c ?"""
    return ref in FP and {reseau(ref + ".1"), reseau(ref + ".2")} == {a, c}
def poly(x, couche, gonfle=0.0):
    p = pcbnew.SHAPE_POLY_SET(); x.TransformShapeToPolygon(p, couche, mm(gonfle), 5000, pcbnew.ERROR_OUTSIDE); return p
contour = pcbnew.SHAPE_POLY_SET(); b.GetBoardPolygonOutlines(contour)
BORD = [(T(contour.Outline(0).CPoint(k).x), T(contour.Outline(0).CPoint(k).y)) for k in range(contour.Outline(0).PointCount())]
def d_seg(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay; L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L)); return math.hypot(px - ax - t * dx, py - ay - t * dy)
def d_bord(x, y): return min(d_seg(x, y, *a, *c) for a, c in zip(BORD, BORD[1:] + BORD[:1]))

# ---------------------------------------------------------------- C1 : le contrôle des règles de KiCad
with tempfile.TemporaryDirectory() as tmp:
    rap = os.path.join(tmp, "drc.json")
    subprocess.run(["kicad-cli", "pcb", "drc", "--schematic-parity", "--severity-all", "--format", "json", "-o", rap, PCB], capture_output=True)
    d = json.load(open(rap)); err = [x for x in d.get("violations", []) if x["severity"] == "error"]
    av = sorted({x["type"] for x in d.get("violations", []) if x["severity"] != "error" and not x["type"].startswith("lib_footprint")})
    juge("C1", not err and not d.get("unconnected_items") and not d.get("schematic_parity"),
         "KiCad : 0 erreur, 0 liaison manquante, 0 écart entre schéma et carte" + (" (avertissements à lire : %s)" % ", ".join(av) if av else ""),
         "KiCad : %d erreurs, %d liaisons manquantes, %d écarts schéma/carte — lancer « kicad-cli pcb drc »" % (len(err), len(d.get("unconnected_items", [])), len(d.get("schematic_parity", []))))
    # ------------------------------------------------------------ C2 : le schéma est bien celui de son générateur
    def nets(fichier):
        t = open(fichier, encoding="utf-8").read(); N = {}
        for nom, corps in re.findall(r'\(net \(code "\d+"\) \(name "([^"]+)"\)(.*?)(?=\(net \(code|\Z)', t[t.index("(nets"):], re.S):
            N[nom] = frozenset(re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', corps))
        return N
    a = os.path.join(tmp, "a.net"); c = os.path.join(tmp, "c.net"); g = os.path.join(tmp, "gen"); os.makedirs(g)
    subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr", "-o", a, SCH], capture_output=True)
    r = subprocess.run([sys.executable, os.path.join(DOS, "generer_schema.py"), g], capture_output=True, text=True)
    subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr", "-o", c, os.path.join(g, NOM + ".kicad_sch")], capture_output=True)
    if r.returncode or not os.path.exists(c): non("C2", "generer_schema.py ne tourne plus : %s" % (r.stderr or r.stdout)[-200:])
    else: juge("C2", nets(a) == nets(c), "le schéma du projet est exactement celui que produit generer_schema.py",
               "le schéma du projet ne correspond plus à generer_schema.py : il a été retouché à la main, ou le générateur a changé sans être relancé")
    if NOM == "v3-mesure-std" and os.path.exists(c):      # cette carte-là se régénère en entier : elle doit être celle de son script
        for x in (NOM + ".pretty", "fp-lib-table", NOM + ".kicad_pro", NOM + ".kicad_dru"):
            src = os.path.join(DOS, x); (shutil.copytree if os.path.isdir(src) else shutil.copy)(src, os.path.join(g, x))
        r = subprocess.run([sys.executable, os.path.join(DOS, "generer_carte.py"), c, os.path.join(g, NOM + ".kicad_pcb")], capture_output=True, text=True)
        moi = lambda f: subprocess.run([sys.executable, os.path.abspath(__file__), "--empreinte", f], capture_output=True, text=True).stdout.strip()
        if r.returncode: non("C2", "generer_carte.py ne tourne plus : %s" % (r.stderr or r.stdout)[-200:])
        else: juge("C2", moi(PCB) == moi(os.path.join(g, NOM + ".kicad_pcb")) != "", "la carte du projet est exactement celle que produit generer_carte.py",
                   "la carte du projet n'est plus celle de generer_carte.py : elle a été retouchée à la main, ou le script a changé sans être relancé")

# ---------------------------------------------------------------- C3 : chaque plan de cuivre est d'un seul tenant
morceaux = []
for z in b.Zones():
    if z.GetIsRuleArea(): continue
    for l in z.GetLayerSet().Seq():
        if l in (F, B): continue
        n = z.GetFilledPolysList(l).OutlineCount()
        if n != 1: morceaux.append("%s sur %s : %d morceaux" % (z.GetNetname(), b.GetLayerName(l), n))
juge("C3", not morceaux, "plans internes d'un seul tenant, aucune piste ne les coupe", "plan interne coupé : " + " ; ".join(morceaux))

# ---------------------------------------------------------------- C4 : aucune piste hors de son plan ni contre le bord
pres, limite = [], []
PLANS = {z.GetNetname() for z in b.Zones() if not z.GetIsRuleArea()}       # une piste au potentiel d'un plan n'a pas besoin de lui comme référence
for t in b.GetTracks():
    if t.GetNetname() in PLANS: continue
    if t.Type() == pcbnew.PCB_VIA_T: x, y = T(t.GetPosition().x), T(t.GetPosition().y); d = d_bord(x, y) - T(t.GetWidth(F)) / 2; lon = 0
    else:
        xs, ys, xe, ye = T(t.GetStart().x), T(t.GetStart().y), T(t.GetEnd().x), T(t.GetEnd().y); n = max(1, int(math.hypot(xe - xs, ye - ys) / 0.5))
        d = min(d_bord(xs + (xe - xs) * k / n, ys + (ye - ys) * k / n) for k in range(n + 1)) - T(t.GetWidth()) / 2; lon = T(t.GetLength())
    if d < 0.5 - 1e-6: pres.append((round(d, 2), t.GetNetname()))
    elif d < 1.0 - 1e-6 and lon > 3: limite.append((round(d, 2), t.GetNetname(), round(lon)))
juge("C4", not pres, "aucune piste de signal hors du plan de masse (toutes à 0,5 mm du bord au moins)", "piste à moins de 0,5 mm du bord, donc HORS du plan : %s" % sorted(set(pres))[:6])
if limite: lire("C4", "pistes entre 0,5 et 1 mm du bord (tolérées si elles existaient ; une piste NOUVELLE se trace à 1 mm au moins) : %s" % ", ".join("%s %d mm à %.2f" % (n, l, d) for d, n, l in sorted(limite)))

# ---------------------------------------------------------------- C5 : le cuivre de masse des faces est cousu au plan interne
PAS = 0.5; e = b.GetBoardEdgesBoundingBox(); x0, y0 = T(e.GetLeft()), T(e.GetTop()); nx = int((T(e.GetRight()) - x0) / PAS) + 1; ny = int((T(e.GetBottom()) - y0) / PAS) + 1
X, Y = np.meshgrid(x0 + np.arange(nx) * PAS, y0 + np.arange(ny) * PAS)
def masque(c):
    pts = [(T(c.CPoint(k).x), T(c.CPoint(k).y)) for k in range(c.PointCount())]; m = np.zeros((ny, nx), bool)
    for (xa, ya), (xb, yb) in zip(pts, pts[1:] + pts[:1]):
        if ya != yb: m ^= ((ya > Y) != (yb > Y)) & (X < (xb - xa) * (Y - ya) / (yb - ya) + xa)
    return m
internes = {z.GetNetname() for z in b.Zones() if not z.GetIsRuleArea() and any(l not in (F, B) for l in z.GetLayerSet().Seq())}
pire, seules = [], []
for z in b.Zones():
    if z.GetIsRuleArea() or z.GetNetname() not in internes: continue
    for c in (F, B):
        if not z.GetLayerSet().Contains(c): continue
        liens = [(T(t.GetPosition().x), T(t.GetPosition().y), poly(t, c, 0.03)) for t in b.GetTracks() if t.Type() == pcbnew.PCB_VIA_T and t.GetNetname() == z.GetNetname()]
        liens += [(T(q.GetPosition().x), T(q.GetPosition().y), poly(q, c, 0.03)) for f in b.GetFootprints() for q in f.Pads() if q.GetNetname() == z.GetNetname() and q.GetAttribute() == pcbnew.PAD_ATTRIB_PTH]
        p = z.GetFilledPolysList(c)
        for i in range(p.OutlineCount()):
            o = p.Outline(i); bb = o.BBox(); seul = pcbnew.SHAPE_POLY_SET(); seul.AddOutline(o); m = masque(o)
            for h in range(p.HoleCount(i)): seul.AddHole(p.Hole(i, h)); m &= ~masque(p.Hole(i, h))
            if not m.any(): continue
            L = []
            for (x, y, pg) in liens:
                if T(bb.GetLeft()) - 1 <= x <= T(bb.GetRight()) + 1 and T(bb.GetTop()) - 1 <= y <= T(bb.GetBottom()) + 1:
                    k = pcbnew.SHAPE_POLY_SET(pg); k.BooleanIntersection(seul)
                    if k.OutlineCount(): L.append((x, y))
            dd = np.full((ny, nx), 1e9, np.float32)
            for (x, y) in L: dd = np.minimum(dd, np.hypot(X - x, Y - y))
            dd[~m] = 0; jm, im = divmod(int(np.argmax(dd)), nx); aire = float(m.sum()) * PAS * PAS
            pire.append((float(dd.max()), z.GetNetname(), b.GetLayerName(c), float(X[0, im]), float(Y[jm, 0]), aire, len(L)))
            if len(L) == 1 and aire > 25: seules.append("%s %s %.0f mm2" % (z.GetNetname(), b.GetLayerName(c), aire))
if pire:
    w = max(pire); mauvais = [q for q in pire if q[0] > 15.0]
    juge("C5", not mauvais, "cuivre de masse des deux faces : aucun point à plus de %.1f mm d'une liaison au plan interne (limite : 15 mm)" % min(w[0], 999),
         "cuivre de masse loin de toute liaison au plan interne : " + " ; ".join("%s %s, %s mm vers (%.0f, %.0f), plage de %.0f mm2 à %d liaison(s)" % (n, l, ("%.0f" % d) if d < 900 else "aucune liaison à", x, y, a, k) for d, n, l, x, y, a, k in sorted(mauvais, reverse=True)[:5]))
    if seules: lire("C5", "plages de plus de 25 mm2 tenues par une seule liaison (admis si elles sont compactes) : " + " ; ".join(seules))
else: ok("C5", "pas de cuivre de masse sur les faces en plus des plans internes")

# ---------------------------------------------------------------- C6 : tension de service des condensateurs du 12 V
RAILS_12V = [n for n in RES if n == "+12V" or n.startswith("/VIN_") or n.startswith("/S_") or n == "/V_POMPES" or n.startswith("/VBRUT_")]
faibles = []
for n in RAILS_12V:
    for br in RES[n]:
        r = br.split(".")[0]
        if r.startswith("C"):
            m = re.search(r"(\d+)\s*V", valeur(r))
            if not m or int(m.group(1)) < 25: faibles.append("%s (%s) sur %s" % (r, valeur(r), n))
if RAILS_12V: juge("C6", not faibles, "condensateurs du 12 V et des rails bruts : 25 V au moins, écrit dans leur valeur", "condensateur sans tension de service suffisante (25 V mini, 50 V préféré) : " + " ; ".join(sorted(set(faibles))))

# ================================================================= CARTE PRINCIPALE
if NOM == "v3-principale":
    # ---- C7 : aucun fil sortant du boîtier ne va en direct sur une puce ni sur le 3,3 V
    directs = []
    for bornier, quoi in (("J8", "sonde de température"), ("J10", "détecteur de débit")):
        for q in FP[bornier].Pads():
            n = q.GetNetname(); puces = sorted(x for x in RES.get(n, ()) if x.startswith("U") and x.split(".")[0] != "U8")
            if n == "+3V3" or (puces and n != "GND"): directs.append("%s.%s (%s) sur %s%s" % (bornier, q.GetNumber(), quoi, n, " avec " + ", ".join(puces) if puces else ""))
    juge("C7", not directs, "sonde de température et détecteur de débit : aucune borne reliée en direct à une puce ni au 3,3 V", "borne extérieure en direct : " + " ; ".join(directs))
    # ---- P1 : deux interrupteurs en série par pompe, et les relectures
    attendu = {
        "/V_POMPES": {"Q11.5", "Q11.6", "Q11.7", "Q11.8", "J2.1", "J3.1", "D3.1", "D4.1", "R34.1", "C21.1", "C23.1"},
        "/G_HAUT": {"Q11.4", "R32.1", "R33.2"}, "/D_ARME": {"Q10.3", "R32.2"}, "/G_ARME": {"Q10.1", "R30.2", "R31.1"},
        "/ARME_POMPES": {"U1.26", "R30.1", "J11.13"},
        "/LU_POMPE_PH": {"U1.30", "R35.2", "R36.1", "J11.14"}, "/LU_POMPE_ORP": {"U1.37", "R37.2", "R38.1", "J11.16"},
        "/S_POMPE_PH": {"J2.2", "D3.2", "Q2.3", "R35.1"}, "/S_POMPE_ORP": {"J3.2", "D4.2", "Q3.3", "R37.1"},
    }
    faux = ["%s : %s au lieu de %s" % (n, sorted(RES.get(n, ())), sorted(v)) for n, v in attendu.items() if RES.get(n, set()) != v]
    faux += [x for x, c in (("sources de Q11 hors du +12V", all(reseau("Q11.%d" % k) == "+12V" for k in (1, 2, 3))), ("R33 n'est plus entre +12V et la grille", entre("R33", "+12V", "/G_HAUT")),
                            ("R31 ne rappelle plus la commande à la masse", entre("R31", "/G_ARME", "GND")), ("R34 ne décharge plus le rail", entre("R34", "/V_POMPES", "GND")),
                            ("R36 ou R38 hors de la masse", entre("R36", "/LU_POMPE_PH", "GND") and entre("R38", "/LU_POMPE_ORP", "GND"))) if not c]
    juge("P1", not faux, "pompes : interrupteur commun (Q11) + un interrupteur par pompe, rappel à la masse de la commande, relecture des deux sorties", "sécurité des pompes défaite : " + " ; ".join(faux))
    # ---- P2 : rien ne garde d'énergie en aval de l'interrupteur
    gros = [r for r in {x.split(".")[0] for x in RES.get("/V_POMPES", ())} if r.startswith("C") and (farads(valeur(r)) or 1) > 100e-9 * 1.01]
    amont = all(reseau(c + ".1") == "+12V" for c in ("C3", "C4"))
    juge("P2", not gros and amont, "rail des pompes : 100 nF au plus en aval de l'interrupteur, les 10 µF restent en amont",
         "réserve d'énergie en aval de l'interrupteur des pompes (%s) ou 10 µF déplacés : le rail ne se vide plus en 1 ms et la relecture ment" % (gros or "C3/C4"))
    # ---- P3 : ohms des diviseurs de relecture (12 V -> moins de 3,3 V, et niveau haut franc)
    for hi, lo in (("R35", "R36"), ("R37", "R38")):
        v = 12.0 * ohms(valeur(lo)) / (ohms(valeur(hi)) + ohms(valeur(lo)))
        juge("P3", 2.6 <= v <= 3.3, "relecture %s/%s : %.2f V pour 12 V (entre 2,6 et 3,3 V)" % (hi, lo, v), "relecture %s/%s : %.2f V pour 12 V — hors de 2,6 à 3,3 V" % (hi, lo, v))
    # ---- P4 : l'antenne
    za = [z for z in FP["U1"].Zones() if z.GetIsRuleArea()]
    if za:
        zb = za[0].GetBoundingBox(); zx0, zy0, zx1, zy1 = T(zb.GetLeft()), T(zb.GetTop()), T(zb.GetRight()), T(zb.GetBottom()); dedans = []
        for t in b.GetTracks():
            for p in ([t.GetPosition()] if t.Type() == pcbnew.PCB_VIA_T else [t.GetStart(), t.GetEnd()]):
                if zx0 <= T(p.x) <= zx1 and zy0 <= T(p.y) <= zy1: dedans.append(t.GetNetname())
        proches = []
        for r, f in FP.items():
            if not (r.startswith(("J", "SW", "BT", "PS")) or r == "C1"): continue      # seules les pièces hautes ou métalliques comptent
            f.BuildCourtyardCaches(); cb = f.GetCourtyard(F).BBox()
            if cb.GetWidth() <= 0: continue
            dx = max(zx0 - T(cb.GetRight()), T(cb.GetLeft()) - zx1, 0); dy = max(zy0 - T(cb.GetBottom()), T(cb.GetTop()) - zy1, 0)
            if math.hypot(dx, dy) < 15: proches.append("%s à %.0f mm" % (r, math.hypot(dx, dy)))
        juge("P4", not dedans, "antenne : aucun cuivre dans sa zone", "cuivre dans la zone de l'antenne : %s" % sorted(set(dedans)))
        if proches: lire("P4", "pièces hautes ou métalliques à moins de 15 mm de l'antenne : " + ", ".join(sorted(proches)))
    else: non("P4", "le module n'a plus sa zone interdite d'antenne")
    # ---- P5 : broches de démarrage du module
    juge("P5", RES.get("/IO12", set()) <= {"U1.14", "J11.11"}, "IO12 : rien ne la tire (elle fixe la tension de la mémoire au démarrage)", "IO12 est reliée à autre chose que le connecteur de débogage : %s" % sorted(RES.get("/IO12", ())))
    # ---- P6 : bornier de la sonde
    c = [x for x, k in (("R39 entre +3V3 et l'alimentation de la sonde", entre("R39", "+3V3", "/V_SONDE")), ("R40 entre la borne et la broche du module", entre("R40", "/TEMP_EXT", "/TEMP")),
                        ("C22 sur l'alimentation de la sonde", entre("C22", "/V_SONDE", "GND")), ("protection U8 sur les deux fils", "U8" in FP and reseau("U8.1") == "/V_SONDE" and reseau("U8.3") == "/TEMP_EXT" and reseau("U8.2") == "GND"),
                        ("R39 en boîtier 0805 (109 mW si le câble est en court-circuit)", "R39" in FP and "0805" in str(FP["R39"].GetFPID().GetLibItemName())), ("R39 de 100 ohms au plus", "R39" in FP and ohms(valeur("R39")) <= 100)) if not k]
    juge("P6", not c, "sonde de température : 100 Ω sur chaque fil, 100 nF et protection contre les décharges au bornier", "protection de la sonde défaite — manque : " + " ; ".join(c))

# ================================================================= CARTE DE MESURE
if NOM == "v3-mesure-std":
    P = json.load(open(os.path.join(DOS, NOM + ".kicad_pro"), encoding="utf-8")); motifs = P["net_settings"]["netclass_patterns"]
    def famille(n):
        for m in motifs:
            if fnmatch.fnmatchcase(n, m["pattern"]): return m["netclass"]
    # ---- M1 : tout réseau est déclaré dans sa famille
    hors = sorted(n for n in RES if not n.startswith("unconnected-") and famille(n) not in ("PRIMAIRE", "ISO_PH", "ISO_ORP"))
    juge("M1", not hors, "les %d réseaux sont tous déclarés dans leur domaine (côté ESP32, voie pH, voie ORP)" % sum(1 for n in RES if not n.startswith("unconnected-")),
         "réseaux hors de toute famille — la règle d'isolement de KiCad ne les voit pas : %s  ⇒ les ajouter dans %s.kicad_pro" % (hors, NOM))
    # domaine de chaque pièce (celui de ses broches reliées), pour les broches laissées en l'air
    def domaines(ref): return {famille(q.GetNetname()) for q in FP[ref].Pads() if q.GetNetname() and not q.GetNetname().startswith("unconnected-")} - {None}
    # ---- M3 : aucune pièce ne relie deux domaines, sauf l'isolateur de bus et le module d'alimentation de chaque voie
    PONTS = {"U101", "U201", "PS101", "PS201"}
    ponts = sorted("%s (%s) : %s" % (r, valeur(r), sorted(domaines(r))) for r in FP if len(domaines(r)) > 1 and r not in PONTS)
    juge("M3", not ponts, "seuls les isolateurs de bus et les modules d'alimentation franchissent la barrière — aucun condensateur entre masses isolées",
         "pièce branchée entre deux domaines isolés : " + " ; ".join(ponts))
    # ---- M2 : isolement mesuré sur le cuivre, toutes couches confondues, sans se fier aux familles de KiCad pour les broches en l'air
    objets = {"PRIMAIRE": [], "ISO_PH": [], "ISO_ORP": []}
    def ajoute(dom, p, quoi):
        if dom in objets and p.OutlineCount(): bb = p.BBox(); objets[dom].append((T(bb.GetLeft()), T(bb.GetTop()), T(bb.GetRight()), T(bb.GetBottom()), p, quoi))
    for t in b.GetTracks(): ajoute(famille(t.GetNetname()), poly(t, t.GetLayer() if t.Type() != pcbnew.PCB_VIA_T else F), t.GetNetname())
    for r, f in FP.items():
        dom = domaines(r)
        for q in f.Pads():
            n = q.GetNetname(); c = F if q.IsOnLayer(F) else B
            if q.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH: continue
            ajoute(famille(n) if n and not n.startswith("unconnected-") else (next(iter(dom)) if len(dom) == 1 else None), poly(q, c), "%s.%s" % (r, q.GetNumber()))
    for z in b.Zones():
        if not z.GetIsRuleArea():
            for l in z.GetLayerSet().Seq(): ajoute(famille(z.GetNetname()), pcbnew.SHAPE_POLY_SET(z.GetFilledPolysList(l)), "plan " + z.GetNetname())
    def ecart(da, db, plafond):
        """Plus petite distance entre le cuivre de deux domaines, cherchée par pas de 0,02 mm jusqu'à `plafond`."""
        mini = (plafond, "", "")
        for (ax0, ay0, ax1, ay1, pa, qa) in objets[da]:
            for (bx0, by0, bx1, by1, pb, qb) in objets[db]:
                if ax0 - mini[0] > bx1 or bx0 - mini[0] > ax1 or ay0 - mini[0] > by1 or by0 - mini[0] > ay1: continue
                lo, hi = 0.0, mini[0]
                g = pcbnew.SHAPE_POLY_SET(pa); g.Inflate(mm(hi), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, mm(0.005)); g.BooleanIntersection(pb)
                if not g.OutlineCount(): continue
                while hi - lo > 0.02:
                    mid = (lo + hi) / 2; g = pcbnew.SHAPE_POLY_SET(pa); g.Inflate(mm(mid), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, mm(0.005)); g.BooleanIntersection(pb)
                    lo, hi = (lo, mid) if g.OutlineCount() else (mid, hi)
                if hi < mini[0]: mini = (hi, qa, qb)
        return mini
    for da, db, seuil in (("PRIMAIRE", "ISO_PH", 0.8), ("PRIMAIRE", "ISO_ORP", 0.8), ("ISO_PH", "ISO_ORP", 3.0)):
        d, qa, qb = ecart(da, db, seuil + 1.5)
        juge("M2", d >= seuil - 0.03, "isolement %s / %s : %s sur le cuivre (minimum %.1f mm)" % (da, db, ("%.2f mm entre %s et %s" % (d, qa, qb)) if qa else "plus de %.1f mm" % d, seuil),
             "isolement %s / %s : %.2f mm seulement, entre %s et %s (minimum %.1f mm)" % (da, db, d, qa, qb, seuil))
    for v, (n1, n2) in (("PH", ("1", "2")), ("ORP", ("2", "2"))):
        k = "1" if v == "PH" else "2"; adc, amp = "U%s03" % k, ("U104" if v == "PH" else "U205")
        # ---- M4 : un filtre entre l'amplificateur et le convertisseur
        r9, r10, c11 = "R%s09" % k, "R%s10" % k, "C%s11" % k
        f = [x for x, c in (("%s entre la mesure et l'entrée + du convertisseur" % r9, entre(r9, "/MESURE_" + v, "/FILTP_" + v)), ("%s entre la référence et l'entrée −" % r10, entre(r10, "/REF_" + v, "/FILTN_" + v)),
                            ("%s entre les deux entrées" % c11, entre(c11, "/FILTP_" + v, "/FILTN_" + v)), ("entrées du convertisseur sur le filtre", reseau(adc + ".4") == "/FILTP_" + v and reseau(adc + ".5") == "/FILTN_" + v)) if not c]
        r_max = max(ohms(valeur(r9)) or 9e9, ohms(valeur(r10)) or 9e9) if not f else 9e9; rs = (ohms(valeur(r9)) or 0) + (ohms(valeur(r10)) or 0)
        juge("M4", not f and r_max < 1000, "voie %s : filtre devant le convertisseur, %s sur chaque entrée (moins de 1 kΩ chacune) et %s, coupure à %.0f Hz" % (v, valeur(r9), valeur(c11), 1 / (2 * math.pi * rs * (farads(valeur(c11)) or 1))) if not f else "",
             "voie %s : filtre devant le convertisseur défait (%s)%s" % (v, " ; ".join(f) or "valeurs", "" if r_max > 9e8 else " — %.0f Ω sur une entrée, la fiche de l'ADS1115 (§ 9.2.2.6) demande moins de 1 kΩ" % r_max))
        # ---- M5 : le corps de la prise ne va pas en direct sur une puce
        puces = sorted(x for x in RES.get("/REF_" + v, ()) if x.startswith("U")); c12 = "C%s12" % k
        juge("M5", not puces and entre(c12, "/REF_" + v, "GNDISO_" + v), "voie %s : le corps de la prise passe par %s (%s) et la résistance du filtre avant toute puce" % (v, c12, valeur(c12) if c12 in FP else "?"),
             "voie %s : le corps de la prise (2 kV au toucher) va en direct sur %s, ou %s manque" % (v, puces or "—", c12))
        ame = RES.get("/AME_" + v, set()); j1, r5b = "J%s01.1" % k, "R%s05.2" % k
        juge("M5", ame == {j1, r5b} and (ohms(valeur("R%s05" % k)) or 0) >= 1e6, "voie %s : l'âme de la prise ne va qu'à sa résistance de %s" % (v, valeur("R%s05" % k)),
             "voie %s : l'âme de la prise est reliée à autre chose que sa résistance d'entrée (%s), ou cette résistance fait moins de 1 MΩ" % (v, sorted(ame - {j1, r5b}) or valeur("R%s05" % k)))
        # ---- M6 : la garde
        r11 = "R%s11" % k; g = "/GARDE_" + v
        juge("M6", entre(r11, "/MESURE_" + v, g) and 100 <= (ohms(valeur(r11)) or 0) <= 1000, "voie %s : garde pilotée par l'amplificateur à travers %s (%s)" % (v, r11, valeur(r11) if r11 in FP else "?"),
             "voie %s : la garde n'est plus pilotée à travers une résistance de 100 Ω à 1 kΩ (%s)" % (v, r11))
        # ---- M7 : la pastille « âme » de la résistance d'entrée, loin du corps métallique de la prise
        j, r5 = "J%s01" % k, "R%s05" % k; FP[j].BuildCourtyardCaches(); cb = FP[j].GetCourtyard(F).BBox()
        q = [q for q in FP[r5].Pads() if q.GetNetname() == "/AME_" + v][0]; qb = q.GetBoundingBox(); d = T(cb.GetTop()) - T(qb.GetBottom())
        juge("M7", d >= 1.5, "voie %s : pastille d'entrée de %s à %.2f mm du corps de la prise (1,5 mm au moins)" % (v, r5, d), "voie %s : pastille d'entrée de %s à %.2f mm du corps métallique de la prise — moins de 1,5 mm" % (v, r5, d))
    # ---- M6 (suite) : le vernis n'est ouvert que sur du cuivre de garde, et jamais sous une prise
    mal = []
    for dsn in b.GetDrawings():
        if dsn.GetLayer() not in (pcbnew.F_Mask, pcbnew.B_Mask): continue
        c = F if dsn.GetLayer() == pcbnew.F_Mask else B; ouv = poly(dsn, dsn.GetLayer())
        for t in b.GetTracks():
            if (t.Type() == pcbnew.PCB_VIA_T or t.GetLayer() == c) and not t.GetNetname().startswith("/GARDE_"):
                k = pcbnew.SHAPE_POLY_SET(ouv); k.BooleanIntersection(poly(t, c))
                if k.OutlineCount(): mal.append("cuivre %s mis à nu" % t.GetNetname())
        for r, f in FP.items():
            for q in f.Pads():
                if q.IsOnLayer(c) and not q.GetNetname().startswith("/GARDE_") and q.GetAttribute() != pcbnew.PAD_ATTRIB_NPTH:
                    k = pcbnew.SHAPE_POLY_SET(ouv); k.Inflate(mm(-0.02), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, mm(0.005)); k.BooleanIntersection(poly(q, c))
                    if k.OutlineCount() and not (r.startswith("J") and q.GetNumber() == "1"): mal.append("ouverture sur %s.%s (%s)" % (r, q.GetNumber(), q.GetNetname()))
            if c == F and r.startswith("J") and r != "J1":
                f.BuildCourtyardCaches(); cy = pcbnew.SHAPE_POLY_SET(f.GetCourtyard(F)); cy.BooleanIntersection(ouv)
                if cy.OutlineCount(): mal.append("ouverture de vernis SOUS la prise %s, côté dessus" % r)
    n_ouv = sum(1 for dsn in b.GetDrawings() if dsn.GetLayer() in (pcbnew.F_Mask, pcbnew.B_Mask))
    juge("M6", n_ouv > 0 and not mal, "anneaux de garde sans vernis (%d traits), uniquement sur du cuivre de garde, rien sous les prises" % n_ouv,
         "ouvertures de vernis : " + ("aucune — l'anneau de garde est de nouveau sous vernis (fiche du LMP7721, § 10.1)" if not n_ouv else " ; ".join(sorted(set(mal)))))
    # ---- M8 : alimentation filtrée de l'amplificateur pH
    c = [x for x, k in (("R112 entre l'alimentation de la voie et celle de l'amplificateur", entre("R112", "VISO_PH", "/VAMP_PH")), ("10 µF C113", entre("C113", "/VAMP_PH", "GNDISO_PH")),
                        ("100 nF C108", entre("C108", "/VAMP_PH", "GNDISO_PH")), ("broche d'alimentation de l'amplificateur", reseau("U104.6") == "/VAMP_PH")) if not k]
    juge("M8", not c, "amplificateur pH : alimentation filtrée (100 Ω, 10 µF, 100 nF), anneau de garde sur ses broches 2 et 7" if reseau("U104.2") == reseau("U104.7") == "/GARDE_PH" else "",
         "amplificateur pH : filtre d'alimentation défait — manque : " + " ; ".join(c) if c else "amplificateur pH : les broches 2 et 7 ne sont plus sur la garde")

print()
if ECHECS: print("❌ %s : %d règle(s) non tenue(s) — %s. Voir kicad/REGLES-CEM-ET-FIABILITE.md." % (NOM, len(ECHECS), ", ".join(sorted(set(ECHECS))))); sys.exit(1)
print("✅ %s : toutes les règles vérifiables par calcul sont tenues. Restent les contrôles humains de la liste de kicad/REGLES-CEM-ET-FIABILITE.md." % NOM)
