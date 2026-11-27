#!/usr/bin/env python3
"""Construit la carte de mesure v3 a composants courants : squelette, placement, pistes, plans de cuivre.

Usage : python3 generer_carte.py <netlist exportée par kicad-cli (.net)> [fichier de sortie]
⚠️ RÉÉCRIT la carte en entier : ne plus le lancer après une modification à la main dans KiCad.

Repère : X vers la droite, Y vers le bas, origine au coin haut-gauche de la carte.
La voie ORP est décalée de DX vers la droite ; ses parties communes avec la voie pH
(alimentation, isolateur, convertisseur, prise) sont aux mêmes places."""
import re, sys, os, uuid, math, pcbnew
ICI = os.path.dirname(os.path.abspath(__file__))
NETLIST = sys.argv[1]
SORTIE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ICI, "v3-mesure-std.kicad_pcb")
STD = "/usr/share/kicad/footprints/"
OX, OY = 100.0, 60.0          # position de la carte sur la feuille
W, H = 73.0, 54.0             # dimensions de la carte
DX = 36.3                     # décalage pH -> ORP = entraxe des BNC
BAR = (16.0, 19.0)            # barrière ESP32 / voies isolées (Y) : 3,0 mm, la largeur entre les deux rangées de l isolateur
XM = 10.0                     # colonne des broches du module d alimentation (voie pH)
COL = (7.5, 11.6)             # autour de cette colonne, la barrière se réduit à un col de 0,5 mm
SEP = (34.0, 37.0)            # séparation voie pH / voie ORP (X) : 3,0 mm
ILE = {"PH": (0.5, SEP[0] - 0.25), "ORP": (SEP[1] + 0.25, W - 0.5)}   # étendue en X de chaque île, en coordonnées de la carte
U = lambda: str(uuid.uuid4())
mm = pcbnew.FromMM
def V(x, y): return pcbnew.VECTOR2I(mm(OX + x), mm(OY + y))

# ---------------------------------------------------------------- squelette (texte)
def squelette():
    CU = '"F.Cu" "In1.Cu" "In2.Cu" "B.Cu"'; E = []
    def rect(x1, y1, x2, y2, layer, w=0.1): E.append('(gr_rect (start %g %g) (end %g %g) (stroke (width %g) (type default)) (fill no) (layer "%s") (uuid "%s"))' % (OX+x1, OY+y1, OX+x2, OY+y2, w, layer, U()))
    def texte(t, x, y, layer="Cmts.User", h=1.0): E.append('(gr_text "%s" (at %g %g 0) (layer "%s") (uuid "%s") (effects (font (size %g %g) (thickness %g)) (justify left bottom)))' % (t, OX+x, OY+y, layer, U(), h, h, h*0.15))
    def interdit(nom, x1, y1, x2, y2):
        E.append('''(zone (net 0) (net_name "") (layers %s) (uuid "%s") (name "%s") (hatch edge 0.5)
 (connect_pads (clearance 0)) (min_thickness 0.25) (filled_areas_thickness no)
 (keepout (tracks not_allowed) (vias not_allowed) (pads allowed) (copperpour not_allowed) (footprints allowed))
 (placement (enabled no) (sheetname ""))
 (fill (thermal_gap 0.5) (thermal_bridge_width 0.5))
 (polygon (pts (xy %g %g) (xy %g %g) (xy %g %g) (xy %g %g))))''' % (CU, U(), nom, OX+x1, OY+y1, OX+x2, OY+y1, OX+x2, OY+y2, OX+x1, OY+y2))
    rect(0, 0, W, H, "Edge.Cuts", 0.05)
    xs = [0, COL[0], COL[1], COL[0] + DX, COL[1] + DX, W]
    for k in (0, 2, 4):
        interdit("BARRIERE ESP32 / voies isolees : aucun cuivre, aucune couche", xs[k], BAR[0], xs[k+1], BAR[1])
    for k in (1, 3):
        interdit("BARRIERE au droit du module d alimentation : col de 0,5 mm", xs[k], 17.25, xs[k+1], 17.75)
    interdit("SEPARATION voie pH / voie ORP : aucun cuivre, aucune couche", SEP[0], BAR[1], SEP[1], H)
    for ref, x, y in TROUS:
        interdit("Fixation %s : cuivre ecarte de 2 mm" % ref, x-3.6, y-3.6, x+3.6, y+3.6)
    texte("COTE ESP32", 9, 2.5, h=1.0); texte("ILE pH", 26.0, 52.5, h=1.2); texte("ILE ORP", DX+26.0, 52.5, h=1.2)
    # nom de chaque prise, IMPRIMÉ sur la carte (les deux textes « ILE » ci-dessus sont des commentaires, non imprimés)
    texte("pH", 19.0, 53.2, "F.SilkS", 1.4); texte("ORP", DX+19.0, 53.2, "F.SilkS", 1.4)
    texte("paroi du boitier a ~0,7 mm sous ce bord", 20, H+3.0, "Dwgs.User", 0.8)
    couches = '''(0 "F.Cu" signal) (4 "In1.Cu" signal) (6 "In2.Cu" signal) (2 "B.Cu" signal)
 (9 "F.Adhes" user "F.Adhesive") (11 "B.Adhes" user "B.Adhesive") (13 "F.Paste" user) (15 "B.Paste" user)
 (5 "F.SilkS" user "F.Silkscreen") (7 "B.SilkS" user "B.Silkscreen") (1 "F.Mask" user) (3 "B.Mask" user)
 (17 "Dwgs.User" user "User.Drawings") (19 "Cmts.User" user "User.Comments") (21 "Eco1.User" user "User.Eco1") (23 "Eco2.User" user "User.Eco2")
 (25 "Edge.Cuts" user) (27 "Margin" user) (31 "F.CrtYd" user "F.Courtyard") (29 "B.CrtYd" user "B.Courtyard") (35 "F.Fab" user) (33 "B.Fab" user)'''
    return '''(kicad_pcb (version 20241229) (generator "pcbnew") (generator_version "9.0")
(general (thickness 1.6) (legacy_teardrops no))
(paper "A4")
(title_block (title "Carte de mesure v3 a composants courants - pH et ORP") (date "2026-11-25") (rev "0.1") (company "ESP32 Pool Controller") (comment 1 "Ecrite par generer_carte.py - voir kicad/v3-mesure-std/README.md"))
(layers %s)
(setup (pad_to_mask_clearance 0) (allow_soldermask_bridges_in_footprints no))
(net 0 "")
%s
(embedded_fonts no)
)
''' % (couches, "\n".join(E))
TROUS = [("H1", 3.5, 3.5), ("H2", W-3.5, 3.5), ("H3", 3.5, 25.5), ("H4", W-3.5, 25.5)]

# ---------------------------------------------------------------- netlist
t = open(NETLIST).read()
COMP = {}
for m in re.finditer(r'\(comp \(ref "([^"]+)"\)\s*\(value "([^"]*)"\)\s*\(footprint "([^"]*)"\)', t):
    COMP[m.group(1)] = (m.group(2), m.group(3))
PIN = {}
i = t.index('(nets')
for name, body in re.findall(r'\(net \(code "\d+"\) \(name "([^"]+)"\)(.*?)(?=\(net \(code|\Z)', t[i:], re.S):
    for ref, pin in re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', body):
        PIN[(ref, pin)] = name

# ---------------------------------------------------------------- carte
open(SORTIE, "w", encoding="utf-8").write(squelette())
board = pcbnew.LoadBoard(SORTIE)
NETS = {}
def net(nom):
    if nom not in NETS:
        ni = pcbnew.NETINFO_ITEM(board, nom); board.Add(ni); NETS[nom] = ni
    return NETS[nom]
PADS = {}; MODELES = {}
def poser(ref, x, y, pad1=None, angle=None, txt=None):
    """Pose `ref` en (x, y). `pad1` = où doit tomber le plot 1 : l'angle est alors choisi tout seul."""
    val, fpid = COMP[ref]; lib, nom = fpid.split(":")
    chemin = os.path.join(ICI, lib + ".pretty") if lib == "v3-mesure-std" else STD + lib + ".pretty"
    def essai(a):
        # chaque empreinte n'est lue qu'une fois puis copiée : relire la bibliothèque à chaque
        # pose fait gonfler la mémoire jusqu'à l'arrêt (constaté le 2026-11-25, 60 composants)
        if (chemin, nom) not in MODELES: MODELES[(chemin, nom)] = pcbnew.FootprintLoad(chemin, nom)
        fp = pcbnew.Cast_to_FOOTPRINT(MODELES[(chemin, nom)].Duplicate()); fp.SetPosition(V(x, y)); fp.SetOrientationDegrees(a); return fp
    if pad1 is not None:
        best = None
        for a in (0, 90, 180, 270):
            fp = essai(a); p = [q for q in fp.Pads() if q.GetNumber() == "1"][0].GetPosition()
            d = math.hypot(pcbnew.ToMM(p.x) - OX - pad1[0], pcbnew.ToMM(p.y) - OY - pad1[1])
            if best is None or d < best[0]: best = (d, a)
        assert best[0] < 0.6, "%s : plot 1 a %.2f mm de la cible" % (ref, best[0])
        angle = best[1]
    fp = essai(angle or 0)
    fp.SetReference(ref); fp.SetValue(val); fp.SetFPID(pcbnew.LIB_ID(lib, nom))
    for pad in fp.Pads():
        n = PIN.get((ref, pad.GetNumber()))
        if n: pad.SetNet(net(n))
        q = pad.GetPosition(); PADS.setdefault((ref, pad.GetNumber()), []).append((round(pcbnew.ToMM(q.x) - OX, 3), round(pcbnew.ToMM(q.y) - OY, 3)))
    # inscriptions : les repères des petits composants sont masqués (carte assemblée en atelier,
    # ils se chevaucheraient) ; ceux des circuits et des connecteurs restent, en petit.
    rf = fp.Reference()
    if txt is None: rf.SetVisible(False)
    else:
        rf.SetTextSize(pcbnew.VECTOR2I(mm(0.8), mm(0.8))); rf.SetTextThickness(mm(0.12))
        rf.SetPosition(V(x + txt[0], y + txt[1])); rf.SetTextAngleDegrees(0)
    board.Add(fp); return fp
def P(ref, n): return PADS[(ref, str(n))][0]
def piste(nom, pts, w=0.25, layer=pcbnew.F_Cu):
    for a, b in zip(pts, pts[1:]):
        if a == b: continue
        tr = pcbnew.PCB_TRACK(board); tr.SetStart(V(*a)); tr.SetEnd(V(*b)); tr.SetWidth(mm(w)); tr.SetLayer(layer); tr.SetNet(net(nom)); board.Add(tr)
def via(nom, x, y, d=0.6, trou=0.3):
    v = pcbnew.PCB_VIA(board); v.SetPosition(V(x, y)); v.SetDrill(mm(trou))
    try: v.SetWidth(pcbnew.F_Cu, mm(d))
    except TypeError: v.SetWidth(mm(d))
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(net(nom)); board.Add(v)
def bretelle(nom, depuis, vers, w=0.25, d=0.6):
    """Piste courte depuis un plot, terminée par un via vers les plans internes."""
    piste(nom, [depuis, vers], w); via(nom, vers[0], vers[1], d)
def plan(nom, layer, pts, prio=0, jeu=0.3, plein=False):
    z = pcbnew.ZONE(board); z.SetLayer(layer); z.SetNet(net(nom)); z.SetAssignedPriority(prio)
    o = z.Outline(); o.NewOutline()
    for x, y in pts: o.Append(mm(OX + x), mm(OY + y))
    z.SetLocalClearance(mm(jeu)); z.SetMinThickness(mm(0.2))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL if plein else pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetThermalReliefGap(mm(0.3)); z.SetThermalReliefSpokeWidth(mm(0.4))
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    board.Add(z)
def R4(x1, y1, x2, y2): return [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]

NOMS = set(PIN.values())
def voie(n, sfx, dx):
    PH = (sfx == "PH")
    c = lambda i: "C%d%02d" % (n, i); r = lambda i: "R%d%02d" % (n, i)
    PS, UI, UR, UA, UE, UD, J = "PS%d01" % n, "U%d01" % n, "U%d02" % n, "U%d03" % n, "U%d04" % n, "U%d05" % n, "J%d01" % n
    def N(s):
        if s in ("+5V", "+3V3", "GND"): return s
        for cand in ("/%s_%s" % (s, sfx), "%s_%s" % (s, sfx)):
            if cand in NOMS: return cand
        raise KeyError(s)
    X = lambda x: x + dx
    L = lambda pts: [(X(a), b) for a, b in pts]
    T = lambda nom, pts, w=0.25, layer=pcbnew.F_Cu: piste(N(nom), L(pts), w, layer)
    B = lambda nom, a, b, w=0.25: bretelle(N(nom), (X(a[0]), a[1]), (X(b[0]), b[1]), w)
    Vv = lambda nom, x, y: via(N(nom), X(x), y)
    Pp = lambda ref, x, y, px, py, **k: poser(ref, X(x), y, pad1=(X(px), py), **k)

    # ============ module d'alimentation : broches en colonne, à cheval sur la barrière
    fp = poser(PS, X(XM), 13.69, angle=270, txt=(3.0, -3.0))
    assert abs(P(PS, 4)[0] - X(XM)) < 0.01 and abs(P(PS, 4)[1] - 21.31) < 0.01, P(PS, 4)
    bb = fp.GetCourtyard(pcbnew.F_CrtYd).BBox()
    assert pcbnew.ToMM(bb.GetRight()) - OX - dx > XM + 4, "le corps du module doit etre a droite de ses broches"
    # ---- côté ESP32
    Pp(c(1), 6.7, 14.2, 7.65, 14.2)
    T("+5V", [(XM, 16.23), (8.3, 16.23), (8.3, 14.9), (7.65, 14.2)], 0.4); B("+5V", (7.65, 14.2), (8.6, 13.0), 0.4)
    B("GND", (5.75, 14.2), (4.6, 14.2))
    xc, yc = 22.0, 17.5
    Pp(UI, xc, yc, xc - 1.905, 19.975, txt=(4.6, -1.2))
    px = lambda k: xc - 1.905 + 1.27 * (k - 1) if k <= 4 else xc - 1.905 + 1.27 * (8 - k)   # broche k de l'isolateur
    yh, yb = 15.025, 19.975
    assert abs(P(UI, 7)[0] - X(px(7))) < 0.01 and abs(P(UI, 7)[1] - yh) < 0.01, P(UI, 7)
    Pp(c(5), 18.4, 14.7, 18.88, 14.7)
    T("+3V3", [(18.88, 14.7), (px(8), 14.7)]); B("+3V3", (px(8), 14.3), (px(8), 13.2)); B("GND", (17.92, 14.7), (17.0, 14.7))
    B("GND", (px(5), 14.3), (px(5), 13.2))
    Pp(r(1), 20.54, 10.6, 19.715, 10.6); Pp(r(2), 23.46, 12.2, 24.285, 12.2)
    B("+3V3", (19.715, 10.6), (18.7, 10.6)); B("+3V3", (24.285, 12.2), (25.3, 12.2))
    ysda, yscl = (8.6, 9.4) if PH else (9.4, 8.6)                         # pas de croisement vers J1
    T("SDA", [(px(7), yh), (px(7), ysda)]); T("SCL", [(px(6), yh), (px(6), yscl)])
    # ---- côté isolé : alimentation
    T("GNDISO", [(XM, 18.77), (8.3, 18.77), (8.3, 20.3)], 0.4); Vv("GNDISO", 8.3, 20.3)
    Pp(c(3), 10.0, 24.9, 8.525, 24.9)
    Pp(UR, 4.7, 34.5, 2.4, 37.65, txt=(0.0, 0.0))                       # languette (sortie) en haut, broches en bas
    T("VBRUT", [(XM, 21.31), (XM, 22.9), (8.525, 24.0), (8.525, 36.6), (7.9, 37.65), (7.0, 37.65)], 0.4)
    B("GNDISO", (11.475, 24.9), (12.9, 24.9), 0.4)
    B("GNDISO", (2.4, 37.65), (1.4, 38.9), 0.4); B("VISO", (4.7, 31.35), (4.7, 29.8), 0.4)
    Pp(c(4), 4.7, 40.6, 3.16, 40.6)
    T("VISO", [(4.7, 37.65), (4.7, 38.5), (3.16, 39.7), (3.16, 40.6)], 0.4); B("VISO", (3.16, 40.6), (1.5, 40.6), 0.4)
    B("GNDISO", (6.24, 40.6), (7.9, 40.6), 0.4)
    # ---- côté isolé : isolateur et convertisseur
    Pp(c(6), 17.9, 20.3, 18.38, 20.3)
    T("VISO", [(18.38, 20.3), (px(1), 20.3)]); B("GNDISO", (17.42, 20.3), (16.4, 20.3))
    Pp(r(3), 20.2, 22.1, 19.375, 22.1); Pp(r(4), 18.4, 24.0, 17.575, 24.0)
    T("VISO", [(px(1), 20.6), (19.375, 21.6), (19.375, 22.1)]); B("VISO", (19.375, 22.1), (18.4, 22.1)); B("VISO", (17.575, 24.0), (16.6, 24.0))
    B("GNDISO", (px(4), 20.6), (px(4), 21.9))
    Pp(UA, 22.0, 28.0, 21.0, 30.2, txt=(4.2, 0.0))
    at = lambda k: 21.0 + 0.5 * (10 - k)      # rangée du haut : broches 10..6
    ab = lambda k: 21.0 + 0.5 * (k - 1)       # rangée du bas : broches 1..5
    assert abs(P(UA, 9)[0] - X(at(9))) < 0.01 and abs(P(UA, 9)[1] - 25.8) < 0.01, P(UA, 9)
    T("SDA_ISO", [(px(2), yb), (px(2), 21.4), (21.025, 22.1), (at(9), 22.8), (at(9), 25.8)])
    # SCL : un saut de 3 mm par la face du dessous, pour passer sous SDA
    T("SCL_ISO", [(px(3), yb), (px(3), 21.7)]); Vv("SCL_ISO", px(3), 21.7)
    T("SCL_ISO", [(px(3), 21.7), (px(3), 22.6), (21.2, 24.0), (20.0, 24.0)], 0.25, pcbnew.B_Cu); Vv("SCL_ISO", 20.0, 24.0)
    T("SCL_ISO", [(19.225, 24.0), (20.0, 24.0), (at(10), 25.0), (at(10), 25.8)])
    Pp(c(7), 22.6, 23.9, 22.6, 24.38)
    T("VISO", [(22.6, 24.38), (at(8), 24.95), (at(8), 25.8)]); B("VISO", (22.6, 24.38), (23.7, 24.3)); B("GNDISO", (22.6, 23.42), (23.7, 23.2))
    T("GNDISO", [(ab(3), 30.2), (ab(3), 28.6)]); Vv("GNDISO", ab(3), 28.6)
    if PH: T("GNDISO", [(ab(1), 30.2), (ab(1), 29.1), (21.6, 28.6), (ab(3), 28.6)])
    else:  T("VISO", [(ab(1), 30.2), (ab(1), 31.6), (20.2, 32.2)]); Vv("VISO", 20.2, 32.2)
    # ============ entrée de la sonde : prise, résistance de protection, amplificateur
    poser(J, X(11.0), 48.0, angle=0, txt=(8.8, 3.0))
    Pp(r(5), 11.0, 41.0, 11.0, 40.088)
    T("AME", [(11.0, 48.0), (11.0, 41.912)], 0.3)
    anneau = [(12.27, 37.975), (12.27, 39.0), (12.7, 39.45), (12.7, 44.5), (13.4, 45.2), (13.4, 49.4), (12.4, 50.4),
              (9.6, 50.4), (8.6, 49.4), (8.6, 45.2), (9.6, 44.2)]
    if PH:
        # LMP7721 : 1 IN+, 2 et 7 libres (reliées à la garde), 3 V-, 4 sortie, 5 libre, 6 V+, 8 IN-.
        # Rangée du bas (y = 37,975) : broches 1 à 4, de x = 11,0 à 14,81. Rangée du haut : 8 à 5.
        Pp(UE, 12.905, 35.5, 11.0, 37.975, txt=(4.6, 3.2))
        T("ENTREE", [(11.0, 40.088), (11.0, 37.975)], 0.3)
        # la garde est la sortie de l'amplificateur (suiveur) : anneau autour de la prise et de l'entrée,
        # refermé sous le boîtier entre les broches 2 et 7, et relié à IN- (broche 8)
        T("MESURE", anneau + [(9.6, 31.7), (12.27, 31.7), (12.27, 33.025)], 0.3); garde = "MESURE"; ytop = 31.3
        T("MESURE", [(12.27, 33.025), (12.27, 37.975)], 0.3)
        T("MESURE", [(11.0, 33.025), (11.0, 31.7)])
        T("MESURE", [(14.81, 37.975), (14.81, 39.45), (12.7, 39.45)])                          # sortie -> garde
        T("MESURE", [(14.81, 37.975), (17.0, 37.975), (19.5, 35.475), (19.5, 32.4), (20.0, 31.9), (22.0, 31.9), (ab(4), 31.4), (ab(4), 30.2)])
        Pp(c(8), 13.54, 30.75, 13.54, 31.23)
        T("VISO", [(13.54, 31.23), (13.54, 33.025)]); B("VISO", (13.54, 31.23), (16.0, 31.5)); B("GNDISO", (13.54, 30.27), (16.0, 30.27))
        T("GNDISO", [(13.54, 37.975), (13.54, 35.5), (16.2, 35.5)]); Vv("GNDISO", 16.2, 35.5)   # V- : sort sous le boîtier, via hors du plan de garde
        # amplificateur double : Vcm
        Pp(UD, 22.0, 44.0, 19.525, 42.095, txt=(0.0, 3.4))
        Pp(r(8), 17.0, 40.4, 16.175, 40.4)
        T("VCM", [(17.825, 40.4), (18.6, 40.4), (19.525, 41.3), (19.525, 43.365)])
        T("LIBRE", [(24.475, 43.365), (24.475, 44.635)])
        Pp(c(9), 26.9, 42.095, 26.42, 42.095)
        T("VISO", [(24.475, 42.095), (26.42, 42.095)]); B("VISO", (26.42, 42.095), (26.42, 40.9)); B("GNDISO", (27.38, 42.095), (28.4, 42.095))
        B("GNDISO", (19.525, 45.905), (18.2, 46.8))
        Pp(c(10), 26.6, 46.385, 26.6, 45.905); Pp(r(7), 28.2, 46.73, 28.2, 45.905); Pp(r(6), 29.9, 45.08, 29.9, 44.255)
        T("VCM_REF", [(19.525, 44.635), (21.6, 44.635), (22.87, 45.905), (24.475, 45.905), (29.9, 45.905)])
        B("GNDISO", (26.6, 46.865), (26.6, 47.9)); B("GNDISO", (28.2, 47.555), (28.2, 48.6)); B("VISO", (29.9, 44.255), (29.9, 43.2))
        T("REF", [(ab(5), 30.2), (ab(5), 31.4), (23.6, 32.0), (23.6, 39.0), (16.175, 39.0), (16.175, 40.4), (16.0, 43.0), (16.0, 44.0)])
    else:
        Pp(UD, 12.905, 35.5, 14.81, 33.025, txt=(4.6, 3.2))
        T("ENTREE", [(11.0, 40.088), (11.0, 37.975)], 0.3)
        T("MESURE", anneau + [(9.6, 35.6), (12.27, 35.6)], 0.3); garde = "MESURE"; ytop = 35.2
        T("MESURE", [(13.54, 37.6), (12.27, 37.6), (12.27, 35.6), (19.0, 35.6), (22.0, 32.6), (ab(4), 32.1), (ab(4), 30.2)])
        Pp(c(9), 14.81, 40.2, 14.81, 39.72)
        T("VISO", [(14.81, 37.975), (14.81, 39.72)]); B("VISO", (14.81, 39.72), (16.0, 39.72)); B("GNDISO", (14.81, 40.68), (16.0, 40.9))
        B("GNDISO", (11.0, 33.025), (11.0, 31.0))
        Pp(r(8), 17.0, 31.4, 17.825, 31.4)
        T("VCM", [(13.54, 32.4), (14.81, 32.4), (14.81, 31.4), (16.175, 31.4)])
        Pp(c(10), 13.4, 29.12, 13.4, 29.6); Pp(r(7), 15.0, 28.775, 15.0, 29.6); Pp(r(6), 16.6, 28.775, 16.6, 27.95)
        T("VCM_REF", [(12.27, 33.025), (12.27, 29.6), (16.6, 29.6)])
        B("GNDISO", (13.4, 28.64), (13.4, 27.6)); B("GNDISO", (15.0, 27.95), (15.0, 26.9)); B("VISO", (16.6, 27.95), (16.6, 26.9))
        T("REF", [(ab(5), 30.2), (ab(5), 31.4), (23.6, 32.0), (23.6, 43.6)])
        B("REF", (17.825, 31.4), (18.9, 31.4)); T("REF", [(18.9, 31.4), (18.9, 41.1), (16.0, 44.0)], 0.25, pcbnew.B_Cu)
    # ============ plans de cuivre
    Vv(garde, 9.6, 42.6)
    zg = L(R4(8.2, ytop, 13.8, 50.8))                                   # garde sous l'entrée, sur les trois autres couches
    for layer in (pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu):
        plan(N(garde), layer, zg, prio=5, jeu=0.5, plein=True)
    # relie les quatre pattes de la prise, en plein : à 8 mm d'écart, les deux pattes du haut n'ont
    # plus la place pour deux rayons (l'anneau de garde d'un côté, le bord du plan de l'autre)
    plan(N("REF"), pcbnew.F_Cu, R4(ILE[sfx][0], 42.9, X(24.5), H - 0.5), prio=3, jeu=0.3, plein=True)
    ile = R4(ILE[sfx][0], BAR[1] + 0.25, ILE[sfx][1], H - 0.5)
    plan(N("GNDISO"), pcbnew.In1_Cu, ile); plan(N("GNDISO"), pcbnew.B_Cu, ile); plan(N("VISO"), pcbnew.In2_Cu, ile)
    for a, b in ((28.5, 25.0), (30.0, 36.0), (32.0, 50.5), (3.0, 46.0)):          # couture des plans de masse isolée
        Vv("GNDISO", a, b)
    return px, yh, ysda, yscl

px, yh, ys1, yc1 = voie(1, "PH", 0.0)
_, _, ys2, yc2 = voie(2, "ORP", DX)
# connecteur J1 : 1 SDA_PH, 2 SCL_PH, 3 GND, 4 +5V, 5 +3V3, 6 GND, 7 SDA_ORP, 8 SCL_ORP
x0, yj = 27.75, 3.4
poser("J1", x0, yj, pad1=(x0, yj), txt=(8.75, 5.3))
jx = lambda k: x0 + 2.5 * (k - 1)
piste("/SDA_PH", [(px(7), ys1), (jx(1), ys1), (jx(1), yj)]); piste("/SCL_PH", [(px(6), yc1), (jx(2), yc1), (jx(2), yj)])
piste("/SDA_ORP", [(px(7) + DX, ys2), (jx(7), ys2), (jx(7), yj)]); piste("/SCL_ORP", [(px(6) + DX, yc2), (jx(8), yc2), (jx(8), yj)])
piste("+5V", [(jx(4), yj), (jx(4), 7.9), (8.6, 7.9), (8.6, 13.0)], 0.4, pcbnew.B_Cu)
piste("+5V", [(jx(4), 7.9), (8.6 + DX, 7.9), (8.6 + DX, 13.0)], 0.4, pcbnew.B_Cu)
prim = R4(0.5, 0.5, W - 0.5, BAR[0] - 0.25)
plan("GND", pcbnew.In1_Cu, prim); plan("GND", pcbnew.B_Cu, prim); plan("+3V3", pcbnew.In2_Cu, prim)
# trous de fixation (hors schéma)
for ref, x, y in TROUS:
    fp = pcbnew.FootprintLoad(os.path.join(ICI, "v3-mesure-std.pretty"), "Trou_M3_NPTH"); fp.SetPosition(V(x, y)); fp.SetReference(ref)
    fp.SetFPID(pcbnew.LIB_ID("v3-mesure-std", "Trou_M3_NPTH")); board.Add(fp)
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
pcbnew.SaveBoard(SORTIE, board)
print("carte ecrite :", len(board.GetFootprints()), "empreintes,", len(board.GetTracks()), "pistes et vias,", len(board.Zones()), "zones")
