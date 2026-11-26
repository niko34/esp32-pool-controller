#!/usr/bin/env python3
"""Construit la carte principale v3 : squelette, placement, plans de cuivre.
Les pistes sont tracées ensuite par l'outil de routage (voir README.md, « La carte »).

Usage : python3 generer_carte.py <netlist exportée par kicad-cli (.net)> [fichier de sortie]
⚠️ RÉÉCRIT la carte en entier : ne plus le lancer après une modification à la main dans KiCad.

Repère : X vers la droite, Y vers le bas, origine au coin haut-gauche de la carte.
Dans le boîtier (option N), ce coin est en X = 77, Y = 162 du boîtier."""
import re, sys, os, uuid, math, pcbnew
ICI = os.path.dirname(os.path.abspath(__file__))
NETLIST = sys.argv[1]
SORTIE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ICI, "v3-principale.kicad_pcb")
STD = "/usr/share/kicad/footprints/"
PRJ = "v3-principale"
OX, OY = 60.0, 50.0           # position de la carte sur la feuille
W, H = 119.0, 100.0           # dimensions de la carte
U = lambda: str(uuid.uuid4())
mm = pcbnew.FromMM
def V(x, y): return pcbnew.VECTOR2I(mm(OX + x), mm(OY + y))
TROUS = [("H1", 4.0, 4.0), ("H2", 90.0, 4.0), ("H3", 4.0, 96.0), ("H4", 115.0, 96.0)]
POMPE = (68.0, 71.0, 12.75)   # moteur de la pompe de droite, au-dessus de la carte : centre et rayon

def squelette():
    E = []
    def texte(t, x, y, layer="Cmts.User", h=1.0): E.append('(gr_text "%s" (at %g %g 0) (layer "%s") (uuid "%s") (effects (font (size %g %g) (thickness %g)) (justify left bottom)))' % (t, OX+x, OY+y, layer, U(), h, h, h*0.15))
    E.append('(gr_rect (start %g %g) (end %g %g) (stroke (width 0.05) (type default)) (fill no) (layer "Edge.Cuts") (uuid "%s"))' % (OX, OY, OX+W, OY+H, U()))
    E.append('(gr_circle (center %g %g) (end %g %g) (stroke (width 0.15) (type default)) (fill no) (layer "Cmts.User") (uuid "%s"))' % (OX+POMPE[0], OY+POMPE[1], OX+POMPE[0]+POMPE[2], OY+POMPE[1], U()))
    texte("moteur de la pompe de droite : rien de haut ici", POMPE[0]-12, POMPE[1]+15, h=0.8)
    couches = '''(0 "F.Cu" signal) (4 "In1.Cu" power) (6 "In2.Cu" power) (2 "B.Cu" signal)
 (9 "F.Adhes" user "F.Adhesive") (11 "B.Adhes" user "B.Adhesive") (13 "F.Paste" user) (15 "B.Paste" user)
 (5 "F.SilkS" user "F.Silkscreen") (7 "B.SilkS" user "B.Silkscreen") (1 "F.Mask" user) (3 "B.Mask" user)
 (17 "Dwgs.User" user "User.Drawings") (19 "Cmts.User" user "User.Comments") (21 "Eco1.User" user "User.Eco1") (23 "Eco2.User" user "User.Eco2")
 (25 "Edge.Cuts" user) (27 "Margin" user) (31 "F.CrtYd" user "F.Courtyard") (29 "B.CrtYd" user "B.Courtyard") (35 "F.Fab" user) (33 "B.Fab" user)'''
    return '''(kicad_pcb (version 20241229) (generator "pcbnew") (generator_version "9.0")
(general (thickness 1.6) (legacy_teardrops no))
(paper "A4")
(title_block (title "Carte principale v3 - ESP32, USB, horloge, sorties 12 V") (date "2026-11-26") (rev "0.1") (company "ESP32 Pool Controller") (comment 1 "Ecrite par generer_carte.py - voir kicad/v3-principale/README.md"))
(layers %s)
(setup (pad_to_mask_clearance 0) (allow_soldermask_bridges_in_footprints no))
(net 0 "")
%s
(embedded_fonts no)
)
''' % (couches, "\n".join(E))

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
PADS = {}; MODELES = {}; POSES = set()
def poser(ref, x, y, angle=0, txt=None):
    val, fpid = COMP[ref]; lib, nom = fpid.split(":")
    chemin = os.path.join(ICI, lib + ".pretty") if lib == PRJ else STD + lib + ".pretty"
    # chaque empreinte n'est lue qu'une fois puis copiée (sinon la mémoire explose)
    if (chemin, nom) not in MODELES: MODELES[(chemin, nom)] = pcbnew.FootprintLoad(chemin, nom)
    fp = pcbnew.Cast_to_FOOTPRINT(MODELES[(chemin, nom)].Duplicate()); fp.SetPosition(V(x, y)); fp.SetOrientationDegrees(angle)
    fp.SetReference(ref); fp.SetValue(val); fp.SetFPID(pcbnew.LIB_ID(lib, nom))
    for pad in fp.Pads():
        n = PIN.get((ref, pad.GetNumber()))
        if n: pad.SetNet(net(n))
        q = pad.GetPosition(); PADS.setdefault((ref, pad.GetNumber()), []).append((round(pcbnew.ToMM(q.x) - OX, 3), round(pcbnew.ToMM(q.y) - OY, 3)))
    rf = fp.Reference()
    if txt is None: rf.SetVisible(False)
    else:
        rf.SetTextSize(pcbnew.VECTOR2I(mm(0.8), mm(0.8))); rf.SetTextThickness(mm(0.12))
        rf.SetPosition(V(x + txt[0], y + txt[1])); rf.SetTextAngleDegrees(0)
    fp.Value().SetVisible(False)
    board.Add(fp); POSES.add(ref); return fp
def bloc(ancre, rot, pieces):
    """Pose un groupe de composants : positions relatives à l'ancre, le tout tourné de `rot` degrés."""
    ax, ay = ancre; c, s = math.cos(math.radians(rot)), math.sin(math.radians(rot))
    for ref, (dx, dy, a) in pieces.items():
        poser(ref, ax + dx*c + dy*s, ay - dx*s + dy*c, (a + rot) % 360, txt=TXT.get(ref))
TXT = {}

# ---------------------------------------------------------------- placement
def sortie(j, d, c, q, rg, rp): return {j: (0, 0, 0), d: (2.5, -11, 90), c: (2.5, -17, 90), q: (2.5, -23, 90), rg: (0.5, -28, 0), rp: (4.5, -28, 0)}
# BORD GAUCHE : tous les câbles du boîtier, en une colonne (utilisateur, 2026-11-26).
# De haut en bas : arrivée 12 V (en haut, face à la sortie de l'alimentation), filtration,
# éclairage, auxiliaire, sonde d'eau, détecteur de débit. Borniers débrochables, enfichés par le dessus.
bloc((7, 12), 270, {"J1": (0, 0, 0), "F1": (2.5, -11, 90), "Q1": (2.5, -20, 90), "D1": (-1, -27, 0), "R1": (5, -27, 0), "D2": (2.5, -33, 90), "C1": (2.5, -42, 0), "C2": (2.5, -48, 0)})
bloc((7, 25.5), 270, sortie("J4", "D5", "C5", "Q4", "R6", "R7"))
bloc((7, 39), 270, sortie("J5", "D6", "C6", "Q5", "R8", "R9"))
bloc((7, 52.5), 270, sortie("J6", "D7", "C7", "Q6", "R10", "R11"))
bloc((7, 66), 270, {"J8": (0, 0, 0), "R24": (1, -11, 0), "C14": (5, -11, 0), "U5": (4, -17, 90)})
bloc((7, 84), 270, {"J10": (0, 0, 0), "R27": (0, -10, 0), "R28": (4, -10, 0), "C15": (2, -14, 90)})
# BORD DU HAUT : les deux pompes, dont les fils viennent du capot (3,81 mm)
bloc((66, 6), 180, sortie("J2", "D3", "C3", "Q2", "R2", "R3"))
bloc((78, 6), 180, sortie("J3", "D4", "C4", "Q3", "R4", "R5"))
# alimentations : 7805 sur sa plage de cuivre, pont, puis 3,3 V
bloc((48, 36), 0, {"U6": (0, 0, 0), "C16": (-10, -7, 0), "C17": (10, -5, 90), "C18": (10, -1, 90), "JP1": (10, 4, 0), "U7": (22, 5, 0), "C19": (16, 8, 90), "C20": (29, 5, 90)})
# 5 V d'essai : à l'intérieur de la carte, il ne reçoit aucun câble du boîtier
bloc((44, 86), 0, {"J12": (0, 0, 0), "Q9": (12, -2, 0), "R29": (12, 2, 0)})
# ESP32 dans le coin haut-droit, antenne vers le haut
bloc((107, 16.2), 0, {"U1": (0, 0, 0), "R12": (-13, -9, 90), "C10": (-13, -5, 90), "R13": (-13, -1, 90), "R14": (-13, 3, 90), "C8": (-13, 7, 90), "C9": (-13, 11, 90)})
# USB sur le bord droit, pont USB et remise à zéro automatique
bloc((114.8, 50), 0, {"J7": (0, 0, 90), "U2": (-8.8, 0, 90), "R15": (-8.8, -6, 0), "R16": (-8.8, 6, 0), "U3": (-19.8, 0, 90), "C11": (-25.8, -7, 0), "R19": (-25.8, 7, 0),
                      "Q7": (-31.8, -4, 0), "Q8": (-31.8, 2, 0), "R17": (-35.8, -4, 90), "R18": (-35.8, 2, 90)})
bloc((113, 64), 0, {"SW1": (0, 0, 0), "SW2": (0, 8, 0), "SW3": (0, 16, 0), "R25": (-7, 16, 90), "D9": (-7, 4, 0), "R26": (-7, 8, 90)})
poser("J11", 46, 70, 90, txt=(8, -7))
bloc((60, 54), 0, {"U4": (0, 0, 0), "Y1": (-7, -3, 0), "C12": (-7, 1, 90), "C13": (6, -4, 90), "D8": (7, 1, 0), "R20": (11, 1, 90), "R21": (-4, 6, 0), "R22": (0, 6, 0), "R23": (4, 6, 0)})
poser("BT1", 80, 81, 0, txt=(8, -13))
# BORD DU BAS : seulement la liaison vers la carte de mesure, face à son connecteur
poser("J9", 62.85, 95, 0, txt=(6, -4))
manque = sorted(set(COMP) - POSES)
assert not manque, "composants non poses : %s" % manque
for ref, x, y in TROUS:
    fp = pcbnew.FootprintLoad(os.path.join(ICI, PRJ + ".pretty"), "Trou_M3_NPTH"); fp.SetPosition(V(x, y)); fp.SetReference(ref); fp.Reference().SetVisible(False); fp.Value().SetVisible(False); board.Add(fp)

# ---------------------------------------------------------------- plans de cuivre internes
def plan(nom, layer, pts, prio=0, jeu=0.3, plein=False):
    z = pcbnew.ZONE(board); z.SetLayer(layer); z.SetNet(net(nom)); z.SetAssignedPriority(prio)
    o = z.Outline(); o.NewOutline()
    for x, y in pts: o.Append(mm(OX + x), mm(OY + y))
    z.SetLocalClearance(mm(jeu)); z.SetMinThickness(mm(0.2))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL if plein else pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetThermalReliefGap(mm(0.3)); z.SetThermalReliefSpokeWidth(mm(0.5))
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    board.Add(z); return z
def R4(x1, y1, x2, y2): return [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]
TOUT = R4(0.5, 0.5, W - 0.5, H - 0.5)
plan("GND", pcbnew.In1_Cu, TOUT)      # couche 2 : masse, continue sous toute la carte
plan("+3V3", pcbnew.In2_Cu, TOUT)     # couche 3 : 3,3 V

pcbnew.SaveBoard(SORTIE, board)
print("carte ecrite :", len(POSES), "composants poses,", len(TROUS), "trous")
