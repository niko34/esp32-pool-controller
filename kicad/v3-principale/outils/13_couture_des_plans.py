"""Retouche du 2026-11-28 — étude CEM et fiabilité (kicad/REGLES-CEM-ET-FIABILITE.md, règle « plages de masse »),
accord de l'utilisateur du même jour.

Le vrai plan de masse est la couche interne In1. Le cuivre de masse des deux FACES n'est utile que s'il y est
relié de près : une plage tenue par un seul point se comporte comme une antenne. La carte en avait une de 5 cm²
tenue par une seule broche du connecteur de mesure, et plusieurs bandes de 3 à 5 cm tenues par un bout.

Cet outil pose des passages de masse (« vias de couture ») jusqu'à ce qu'aucun point de cuivre de masse des deux
faces ne soit à plus de DISTANCE mm d'une liaison au plan interne — là où un passage tient. Il ne déplace rien.
Ce qu'il n'a pas pu couvrir est listé à la fin : à regarder, pas à ignorer.

Usage : python3 13_couture_des_plans.py <carte>      (après 12_securite_pompes_et_sonde.py)
Peut se relancer : il ne pose que ce qui manque."""
import sys, os, math, pcbnew
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import routeur

DISTANCE = 12.0        # mm : aucun point de cuivre de masse plus loin que cela d'une liaison au plan interne
RETRAIT = 0.5          # mm : le passage se pose à au moins cette distance du bord de sa plage de cuivre
PAS = 0.5              # mm : finesse de l'examen
pcb = sys.argv[1]; b = pcbnew.LoadBoard(pcb); T = pcbnew.ToMM; mm = pcbnew.FromMM
e = b.GetBoardEdgesBoundingBox(); x0, y0 = T(e.GetLeft()), T(e.GetTop()); nx = int((T(e.GetRight()) - x0) / PAS) + 1; ny = int((T(e.GetBottom()) - y0) / PAS) + 1
X, Y = np.meshgrid(x0 + np.arange(nx) * PAS, y0 + np.arange(ny) * PAS)
def poly(x, c):
    p = pcbnew.SHAPE_POLY_SET(); x.TransformShapeToPolygon(p, c, mm(0.03), 5000, pcbnew.ERROR_OUTSIDE); return p
def masque(contour):
    pts = [(T(contour.CPoint(k).x), T(contour.CPoint(k).y)) for k in range(contour.PointCount())]; m = np.zeros((ny, nx), bool)
    for (xa, ya), (xb, yb) in zip(pts, pts[1:] + pts[:1]):
        if ya != yb: m ^= ((ya > Y) != (yb > Y)) & (X < (xb - xa) * (Y - ya) / (yb - ya) + xa)
    return m

def plages():
    """Pour chaque face : la liste des plages de masse, chacune avec son polygone, son masque et ses liaisons au plan interne."""
    P = []
    for c in (pcbnew.F_Cu, pcbnew.B_Cu):
        liens = [(T(t.GetPosition().x), T(t.GetPosition().y), poly(t, c)) for t in b.GetTracks() if t.Type() == pcbnew.PCB_VIA_T and t.GetNetname() == "GND"]
        liens += [(T(q.GetPosition().x), T(q.GetPosition().y), poly(q, c)) for f in b.GetFootprints() for q in f.Pads() if q.GetNetname() == "GND" and q.GetAttribute() == pcbnew.PAD_ATTRIB_PTH]
        for z in b.Zones():
            if z.GetNetname() != "GND" or not z.GetLayerSet().Contains(c): continue
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
                d = np.full((ny, nx), 1e9, np.float32)
                for (x, y) in L: d = np.minimum(d, np.hypot(X - x, Y - y))
                d[~m] = 0
                dedans = pcbnew.SHAPE_POLY_SET(seul); dedans.Deflate(mm(RETRAIT + routeur.DIAM_VIA / 2), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, mm(0.02))      # là où le passage entier est dans le cuivre
                P.append({"face": b.GetLayerName(c), "aire": float(m.sum()) * PAS * PAS, "liens": L, "d": d, "dedans": dedans, "masque": m})
    return P

def bilan(P, titre):
    print(titre)
    for q in sorted(P, key=lambda q: -q["aire"]):
        n = int((q["d"] > DISTANCE).sum())
        if n or (len(q["liens"]) < 2 and q["aire"] > 25): print("   %s, plage de %7.1f mm2, %3d liaisons : %6.1f mm2 à plus de %g mm d'une liaison (le pire : %.1f mm)" % (q["face"], q["aire"], len(q["liens"]), n * PAS * PAS, DISTANCE, min(float(q["d"].max()), 999)))

SEULE = 25.0           # mm2 : au-delà, une plage tenue par une seule liaison en reçoit une deuxième

def passage(x, y):
    v = pcbnew.PCB_VIA(b); v.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y))); v.SetDrill(mm(routeur.TROU_VIA)); v.SetWidth(pcbnew.F_Cu, mm(routeur.DIAM_VIA))
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(b.FindNet("GND")); b.Add(v)

def une_passe():
    """Pose ce qui manque sur la carte telle qu'elle est remplie. Rend (passages posés, endroits sans place)."""
    R = routeur.Routeur(b); _, pas_de_via = R._grilles("GND", 0.25, routeur.CONFORT)
    P = plages(); poses, rates = [], []
    def place_pres_de(q, px, py, rayon):
        i0, j0 = R.ij(px, py); r = int(rayon / routeur.PAS)
        for _, i, j in sorted((math.hypot(di, dj), i0 + di, j0 + dj) for dj in range(-r, r + 1, 2) for di in range(-r, r + 1, 2) if di * di + dj * dj <= r * r):
            if 0 <= i < R.nx and 0 <= j < R.ny and not pas_de_via[j, i]:
                x, y = R.xy(i, j)
                if q["dedans"].Contains(pcbnew.VECTOR2I(mm(x), mm(y))): return x, y
        return None
    def poser(q, x, y):
        passage(x, y); poses.append((x, y, q["face"])); R._disque(pas_de_via, x, y, 2.0)      # pas deux passages à moins de 2 mm l'un de l'autre
        for z in P:                                                                           # le passage sert toutes les plages qu'il traverse, sur les deux faces
            if z is q or z["dedans"].Contains(pcbnew.VECTOR2I(mm(x), mm(y))):
                z["liens"].append((x, y)); z["d"] = np.where(z["d"] > 0, np.minimum(z["d"], np.hypot(X - x, Y - y)), 0).astype(np.float32)
    for _ in range(400):                       # 1. plus aucun point de cuivre à plus de DISTANCE d'une liaison
        q = max(P, key=lambda q: float(q["d"].max())); dmax = float(q["d"].max())
        if dmax <= DISTANCE: break
        jm, im = divmod(int(np.argmax(q["d"])), nx); px, py = float(X[0, im]), float(Y[jm, 0])
        t = place_pres_de(q, px, py, DISTANCE / 2)
        if t is None: rates.append((q["face"], round(px, 1), round(py, 1), round(min(dmax, 999), 1))); q["d"][np.hypot(X - px, Y - py) <= DISTANCE / 2] = 0
        else: poser(q, *t)
    for q in P:                                # 2. une grande plage ne tient pas par une seule liaison
        if q["aire"] > SEULE and len(q["liens"]) == 1:
            d = np.where(q["masque"], np.hypot(X - q["liens"][0][0], Y - q["liens"][0][1]), 0); jm, im = divmod(int(np.argmax(d)), nx)
            t = place_pres_de(q, float(X[0, im]), float(Y[jm, 0]), float(d.max()) * 0.6)
            if t is None: rates.append((q["face"], round(q["liens"][0][0], 1), round(q["liens"][0][1], 1), -1.0))
            else: poser(q, *t)
    return poses, rates

# Languettes de cuivre trop étroites pour recevoir un passage : on n'y met pas de cuivre du tout.
LANGUETTES = [  # (face, x0, y0, x1, y1, nom)
    (pcbnew.B_Cu, 154.2, 90.93, 173.0, 91.93, "pas de cuivre entre SDA et SCL"),      # 0,6 mm de large sur 18 mm, tenue par un seul bout
]
deja = {z.GetZoneName() for z in b.Zones()}
for couche, xa, ya, xb, yb, nom in LANGUETTES:
    if nom in deja: continue
    z = pcbnew.ZONE(b); z.SetIsRuleArea(True); z.SetLayer(couche); z.SetZoneName(nom); z.SetDoNotAllowCopperPour(True)
    z.SetDoNotAllowTracks(False); z.SetDoNotAllowVias(False); z.SetDoNotAllowPads(False); z.SetDoNotAllowFootprints(False)
    o = z.Outline(); o.NewOutline()
    for x, y in ((xa, ya), (xb, ya), (xb, yb), (xa, yb)): o.Append(mm(x), mm(y))
    b.Add(z); print("zone sans cuivre de remplissage :", nom)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
# Posé à la main : la bande de masse de 1,8 mm entre le module ESP32 et le bord droit est trop étroite pour la recherche
# automatique (elle garde 0,8 mm de cuivre autour de chaque passage). Un passage au milieu, entre ceux des liaisons de sécurité.
A_LA_MAIN = [(177.7, 70.65)]
for x, y in A_LA_MAIN:
    if not any(t.Type() == pcbnew.PCB_VIA_T and abs(T(t.GetPosition().x) - x) < 0.05 and abs(T(t.GetPosition().y) - y) < 0.05 for t in b.GetTracks()): passage(x, y); print("passage posé à la main : (%.2f, %.2f)" % (x, y))
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
P = plages(); bilan(P, "AVANT :")
tout = []
for passe in range(1, 7):                      # un passage posé pour une face peut faire apparaître une plage sur l'autre : on recommence jusqu'à ce que plus rien ne manque
    poses, rates = une_passe(); tout += poses
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    print("passe %d : %d passages posés" % (passe, len(poses)), flush=True)
    if not poses: break
pcbnew.SaveBoard(pcb, b)
print("passages de masse posés en tout : %d" % len(tout))
for x, y, f in tout: print("   (%.3f, %.3f)  pour la face %s" % (x, y, f))
b = pcbnew.LoadBoard(pcb); bilan(plages(), "APRÈS (carte relue) :")
print("endroits où aucun passage ne tient : %d" % len(rates))
for r in rates: print(("   face %s, vers (%.1f, %.1f) : cuivre à %.1f mm d'une liaison" % r) if r[3] >= 0 else ("   face %s : la plage tenue par (%.1f, %.1f) n'a pas reçu de deuxième liaison" % r[:3]))
