#!/usr/bin/env python3
"""Construit la carte de mesure v3 : squelette, placement, pistes, plans de cuivre.

Usage : python3 generer_carte.py <netlist exportée par kicad-cli (.net)> [fichier de sortie]
⚠️ RÉÉCRIT la carte en entier : ne plus le lancer après une modification à la main dans KiCad.

Repère : X vers la droite, Y vers le bas, origine au coin haut-gauche de la carte.
La voie ORP est la voie pH décalée de DX vers la droite."""
import re, sys, os, uuid, math, pcbnew
ICI = os.path.dirname(os.path.abspath(__file__))
NETLIST = sys.argv[1]
SORTIE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ICI, "v3-mesure.kicad_pcb")
STD = "/usr/share/kicad/footprints/"
OX, OY = 100.0, 60.0          # position de la carte sur la feuille
W, H = 73.0, 54.0             # dimensions de la carte
DX = 36.3                     # décalage pH -> ORP = entraxe des BNC
BAR = (16.0, 21.5)            # barrière ESP32 / voies isolées (Y)
SEP = (35.25, 40.75)          # séparation voie pH / voie ORP (X)
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
    interdit("BARRIERE ESP32 / voies isolees : aucun cuivre, aucune couche", 0, BAR[0], W, BAR[1])
    interdit("SEPARATION voie pH / voie ORP : aucun cuivre, aucune couche", SEP[0], BAR[1], SEP[1], H)
    for ref, x, y in TROUS:
        interdit("Fixation %s : cuivre ecarte de 2 mm" % ref, x-3.6, y-3.6, x+3.6, y+3.6)
    texte("COTE ESP32", 9, 2.5, h=1.0); texte("ILE pH", 1.0, 34.0, h=1.2); texte("ILE ORP", SEP[1]+0.6, 34.0, h=1.2)
    texte("paroi du boitier a ~0,7 mm sous ce bord", 20, H+3.0, "Dwgs.User", 0.8)
    couches = '''(0 "F.Cu" signal) (4 "In1.Cu" signal) (6 "In2.Cu" signal) (2 "B.Cu" signal)
 (9 "F.Adhes" user "F.Adhesive") (11 "B.Adhes" user "B.Adhesive") (13 "F.Paste" user) (15 "B.Paste" user)
 (5 "F.SilkS" user "F.Silkscreen") (7 "B.SilkS" user "B.Silkscreen") (1 "F.Mask" user) (3 "B.Mask" user)
 (17 "Dwgs.User" user "User.Drawings") (19 "Cmts.User" user "User.Comments") (21 "Eco1.User" user "User.Eco1") (23 "Eco2.User" user "User.Eco2")
 (25 "Edge.Cuts" user) (27 "Margin" user) (31 "F.CrtYd" user "F.Courtyard") (29 "B.CrtYd" user "B.Courtyard") (35 "F.Fab" user) (33 "B.Fab" user)'''
    return '''(kicad_pcb (version 20241229) (generator "pcbnew") (generator_version "9.0")
(general (thickness 1.6) (legacy_teardrops no))
(paper "A4")
(title_block (title "Carte de mesure v3 - pH et ORP") (date "2026-11-24") (rev "0.2") (company "ESP32 Pool Controller") (comment 1 "Ecrite par generer_carte.py - voir kicad/v3-mesure/README.md"))
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
        PIN[(ref, pin)] = None if name.startswith("unconnected-") else name

# ---------------------------------------------------------------- carte
open(SORTIE, "w", encoding="utf-8").write(squelette())
board = pcbnew.LoadBoard(SORTIE)
NETS = {}
def net(nom):
    if nom not in NETS:
        ni = pcbnew.NETINFO_ITEM(board, nom); board.Add(ni); NETS[nom] = ni
    return NETS[nom]
PADS = {}
def poser(ref, x, y, pad1=None, angle=None):
    """Pose `ref` en (x, y). `pad1` = où doit tomber le plot 1 : l'angle est alors choisi tout seul."""
    val, fpid = COMP[ref]; lib, nom = fpid.split(":")
    chemin = os.path.join(ICI, lib + ".pretty") if lib == "v3-mesure" else STD + lib + ".pretty"
    def essai(a):
        fp = pcbnew.FootprintLoad(chemin, nom); fp.SetPosition(V(x, y)); fp.SetOrientationDegrees(a); return fp
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
    if ref[0] in "CR": rf.SetVisible(False)
    else:
        rf.SetTextSize(pcbnew.VECTOR2I(mm(0.8), mm(0.8))); rf.SetTextThickness(mm(0.12))
        dxy = {"J1": (8.75, 5.3), "U101": (-6.0, 0), "U201": (-6.0, 0), "U102": (0, 0), "U202": (0, 0), "J101": (0, -6.3), "J201": (0, -6.3)}[ref]
        rf.SetPosition(V(x + dxy[0], y + dxy[1])); rf.SetTextAngleDegrees(0)
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

def voie(n, sfx, dx):
    c = lambda i: "C%d%02d" % (n, i); r = lambda i: "R%d%02d" % (n, i)
    Uc, Um, Jb = "U%d01" % n, "U%d02" % n, "J%d01" % n
    N = lambda s: (s if s in ("+5V", "+3V3", "GND") else ("/" if s in ("SDA", "SCL", "SDA_ISO", "SCL_ISO", "VSEL", "PRB") else "") + s + "_" + sfx)
    X = lambda x: x + dx
    T = lambda nom, pts, w=0.25, layer=pcbnew.F_Cu: piste(N(nom), [(X(a), b) for a, b in pts], w, layer)
    B = lambda nom, a, b, d=0.6: bretelle(N(nom), (X(a[0]), a[1]), (X(b[0]), b[1]), 0.25, d)
    xc, yc = 22.0, 18.75
    poser(Uc, X(xc), yc, pad1=(X(xc + 2.925), yc - 3.5))                  # broches 1-10 en haut (côté ESP32)
    hx = lambda k: xc + 2.925 - 0.65 * (k - 1)                             # broche k (1..10), rangée du haut
    bx = lambda k: xc - 2.925 + 0.65 * (k - 11)                            # broche k (11..20), rangée du bas
    yh, yb = yc - 3.5, yc + 3.5
    assert abs(P(Uc, 12)[0] - X(bx(12))) < 0.01 and abs(P(Uc, 12)[1] - yb) < 0.01, P(Uc, 12)
    # ---------------- côté ESP32
    yC = 13.15
    poser(c(1), X(19.3), yC, pad1=(X(19.78), yC)); poser(c(3), X(22.55), yC, pad1=(X(23.03), yC))
    poser(c(2), X(16.5), 12.6, pad1=(X(15.55), 12.6)); poser(c(4), X(12.0), 12.6, pad1=(X(11.05), 12.6))
    poser(r(1), X(22.8), 10.5, pad1=(X(21.975), 10.5)); poser(r(2), X(25.725), 12.3, pad1=(X(26.55), 12.3))
    T("+5V", [(hx(9), yh), (hx(9), yC)]); T("GND", [(hx(10), yh), (hx(10), yC)])
    T("+3V3", [(hx(4), yh), (hx(4), yC)]); T("GND", [(hx(5), yh), (hx(5), yC)])
    T("GND", [(hx(8), yh), (hx(8), 13.3), (hx(7), 13.3), (hx(7), yh)]); B("GND", (20.7, 13.3), (20.7, 12.3))
    B("GND", (hx(1), yh), (25.9, 15.1))
    B("GND", (18.82, yC), (18.82, 11.9)); B("+5V", (19.78, yC), (19.78, 11.85))
    B("GND", (22.07, yC), (22.07, 11.9)); B("+3V3", (23.03, yC), (23.03, 11.9))
    B("+5V", (15.55, 12.6), (15.55, 11.2)); B("GND", (17.45, 12.6), (17.45, 11.2))
    B("+3V3", (11.05, 12.6), (11.05, 11.2)); B("GND", (12.95, 12.6), (12.95, 11.2))
    B("+3V3", (21.975, 10.5), (20.95, 10.5)); B("+3V3", (26.55, 12.3), (26.55, 10.9))
    T("+5V", [(19.78, 11.85), (19.78, 10.2), (15.55, 10.2), (15.55, 11.2)], 0.4, pcbnew.B_Cu)
    ysda, yscl = (8.6, 9.4) if n == 1 else (9.4, 8.6)                     # pas de croisement vers J1
    T("SDA", [(hx(3), yh), (hx(3), ysda)])
    T("SCL", [(hx(2), yh), (hx(2), 14.0), (24.9, 13.375), (24.9, yscl)])
    # ---------------- côté isolé : découplage, diviseur, rappels
    yD = 24.35
    poser(c(5), X(19.3), yD, pad1=(X(19.78), yD)); poser(c(7), X(22.55), yD, pad1=(X(23.03), yD))
    poser(c(6), X(15.0), 26.0, pad1=(X(14.05), 26.0))
    poser(r(6), X(18.5), 28.0, pad1=(X(17.675), 28.0)); poser(r(5), X(21.6), 28.0, pad1=(X(20.775), 28.0))
    poser(r(3), X(24.45), 33.0, pad1=(X(25.275), 33.0)); poser(r(4), X(29.325), 32.0, pad1=(X(30.15), 32.0))
    T("GNDISO", [(bx(11), yb), (bx(11), yD)]); T("VISO", [(bx(12), yb), (bx(12), yD)])
    T("GNDISO", [(bx(16), yb), (bx(16), yD)]); T("VISO", [(bx(17), yb), (bx(17), yD)])
    B("GNDISO", (bx(14), yb), (bx(14), 24.9)); B("GNDISO", (bx(20), yb), (bx(20), 24.6))
    B("GNDISO", (18.82, yD), (18.82, 25.6)); B("VISO", (19.78, yD), (19.78, 25.75))
    B("GNDISO", (22.07, yD), (22.07, 25.6)); B("VISO", (23.03, yD), (23.03, 25.6))
    B("VISO", (14.05, 26.0), (14.05, 27.6)); B("GNDISO", (15.95, 26.0), (15.95, 27.6))
    T("VSEL", [(bx(13), yb), (bx(13), 28.0)]); T("VSEL", [(19.325, 28.0), (20.775, 28.0)])
    B("VISO", (17.675, 28.0), (17.675, 29.3)); B("GNDISO", (22.425, 28.0), (22.425, 29.3))
    B("VISO", (25.275, 33.0), (25.275, 34.3)); B("VISO", (30.15, 32.0), (30.15, 33.3))
    # ---------------- module Atlas et prise BNC
    mx, my = 26.75, 47.0
    poser(Um, X(mx), my, pad1=(X(mx - 6), my - 4.6))
    poser(Jb, X(11.0), 48.0, angle=0)
    T("SDA_ISO", [(bx(18), yb), (bx(18), 40.6), (mx - 6, 40.6), (mx - 6, my - 4.6)])
    T("SCL_ISO", [(bx(19), yb), (bx(19), 26.0), (28.5, 30.225), (28.5, 34.4), (34.6, 40.5), (34.6, my), (mx + 6, my)])
    B("VISO", (mx - 6, my), (18.6, my)); B("GNDISO", (mx + 6, my + 4.6), (34.5, my + 4.6))
    T("PRB", [(mx - 6, my + 2.3), (18.0, my + 2.3), (16.7, 48.0), (11.0, 48.0)], 0.4)
    # ---------------- plans de cuivre
    # Atlas (pH OEM p. 33, fig. 2) : le plan de sonde couvre aussi le dessous de la partie analogique du
    # module, sous la limite entre les plots 3 et 4, jusqu'aux deux tiers de sa largeur (relecture du 2026-11-24).
    sonde = [(0.5, 41.0), (17.6, 41.0), (17.6, 48.15), (28.4, 48.15), (28.4, H - 0.5), (0.5, H - 0.5)]
    for layer in (pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu):
        plan(N("GND_SONDE"), layer, [(X(a), b) for a, b in sonde], prio=5, jeu=0.5)   # freins thermiques : les pattes des BNC doivent pouvoir se souder
    ile = [(X(a), b) for a, b in R4(0.5, BAR[1] + 0.25, SEP[0] - 0.25, H - 0.5)]
    plan(N("GNDISO"), pcbnew.In1_Cu, ile); plan(N("GNDISO"), pcbnew.B_Cu, ile); plan(N("VISO"), pcbnew.In2_Cu, ile)
    for a, b in ((6.0, 38.5), (33.5, 31.0), (26.0, 38.5)):          # couture des plans de masse isolée entre couches
        via(N("GNDISO"), X(a), b)
    return hx, yh, ysda, yscl

hx, yh, ys1, yc1 = voie(1, "PH", 0.0)
_, _, ys2, yc2 = voie(2, "ORP", DX)
# connecteur J1 : 1 SDA_PH, 2 SCL_PH, 3 GND, 4 +5V, 5 +3V3, 6 GND, 7 SDA_ORP, 8 SCL_ORP
x0, yj = 27.75, 3.4
poser("J1", x0, yj, pad1=(x0, yj))
jx = lambda k: x0 + 2.5 * (k - 1)
piste("/SDA_PH", [(hx(3), ys1), (jx(1), ys1), (jx(1), yj)]); piste("/SCL_PH", [(24.9, yc1), (jx(2), yc1), (jx(2), yj)])
piste("/SDA_ORP", [(hx(3) + DX, ys2), (jx(7), ys2), (jx(7), yj)]); piste("/SCL_ORP", [(24.9 + DX, yc2), (jx(8), yc2), (jx(8), yj)])
piste("+5V", [(jx(4), yj), (jx(4), 7.9), (19.78, 7.9), (19.78, 10.2)], 0.4, pcbnew.B_Cu)
piste("+5V", [(jx(4), 7.9), (19.78 + DX, 7.9), (19.78 + DX, 10.2)], 0.4, pcbnew.B_Cu)
prim = R4(0.5, 0.5, W - 0.5, BAR[0] - 0.25)
plan("GND", pcbnew.In1_Cu, prim); plan("GND", pcbnew.B_Cu, prim); plan("+3V3", pcbnew.In2_Cu, prim)
# trous de fixation (hors schéma)
for ref, x, y in TROUS:
    fp = pcbnew.FootprintLoad(os.path.join(ICI, "v3-mesure.pretty"), "Trou_M3_NPTH"); fp.SetPosition(V(x, y)); fp.SetReference(ref)
    fp.SetFPID(pcbnew.LIB_ID("v3-mesure", "Trou_M3_NPTH")); board.Add(fp)
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
pcbnew.SaveBoard(SORTIE, board)
print("carte ecrite :", len(board.GetFootprints()), "empreintes,", len(board.GetTracks()), "pistes et vias,", len(board.Zones()), "zones")
