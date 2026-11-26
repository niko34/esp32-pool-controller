"""Retouche du 2026-11-26, décidée par l'utilisateur : l'horloge PCF8563 et son quartz
sont remplacés par un DS3231SN (quartz interne compensé en température).
Retirés : U4 (PCF8563), Y1, C12 (22 pF), D8 (double diode), R20 (1 kΩ) et leurs pistes.
Posés : U4 (DS3231SN, boîtier large à 16 broches) ; C13 (100 nF) déplacé contre son alimentation.
La pile va directement à la broche VBAT : la puce bascule seule.
Usage : python3 8_horloge_ds3231.py <carte> <netlist exportée du schéma (.net)>"""
import sys, re, pcbnew
pcb, netlist = sys.argv[1], sys.argv[2]
b = pcbnew.LoadBoard(pcb); mm = pcbnew.FromMM; MM = pcbnew.ToMM
OX, OY = 60.0, 50.0
F, B = pcbnew.F_Cu, pcbnew.B_Cu
def V(x, y): return pcbnew.VECTOR2I(mm(OX+x), mm(OY+y))
def net(nom):
    n = b.FindNet(nom)
    if n is None: n = pcbnew.NETINFO_ITEM(b, nom); b.Add(n)
    return n
def piste(nom, pts, w=0.25, layer=F):
    for a, c in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b); t.SetStart(V(*a)); t.SetEnd(V(*c)); t.SetWidth(mm(w)); t.SetLayer(layer); t.SetNet(net(nom)); b.Add(t)
def via(nom, x, y, d=0.6, trou=0.3):
    v = pcbnew.PCB_VIA(b); v.SetPosition(V(x, y)); v.SetDrill(mm(trou)); v.SetWidth(F, mm(d)); v.SetLayerPair(F, B); v.SetNet(net(nom)); b.Add(v)
def xy(v): return (round(MM(v.x)-OX, 2), round(MM(v.y)-OY, 2))
def pres(a, c): return abs(a[0]-c[0]) < 0.03 and abs(a[1]-c[1]) < 0.03
TOUT = list(b.GetTracks()); FPS = {f.GetReference(): pcbnew.Cast_to_FOOTPRINT(f) for f in b.GetFootprints()}
modele = FPS["R21"]

# ---- 1. ce qui disparaît
ote = [FPS[r] for r in ("U4", "Y1", "C12", "D8", "R20")]
MORTS = ("/OSCI", "/OSCO", "/VRTC", "/VBAT_R")
P_OTE = [("GND", (57.04, 55.42), (57.53, 55.91)), ("GND", (56.37, 55.42), (57.04, 55.42)), ("GND", (56.75, 55.81), (55.8, 54.86)),
         ("GND", (65.89, 49.44), (64.94, 48.49)), ("GND", (65.44, 48.97), (66.0, 49.52)), ("GND", (65.42, 48.97), (65.44, 48.97)),
         ("+3V3", (65.49, 53.47), (66.06, 54.05)), ("+3V3", (65.53, 53.95), (64.58, 53.0)),
         ("/RTC_INT", (64.45, 51.8), (64.45, 51.29)), ("/RTC_INT", (58.25, 54.64), (58.87, 55.26)), ("/RTC_INT", (61.4, 52.74), (63.51, 52.74)),
         ("/RTC_INT", (63.51, 52.74), (64.45, 51.8)), ("/RTC_INT", (57.53, 54.64), (58.25, 54.64)), ("/RTC_INT", (58.87, 57.02), (58.87, 55.26)),
         ("/RTC_INT", (56.83, 59.07), (58.87, 57.02)), ("/RTC_INT", (58.87, 55.26), (61.4, 52.74))]
V_OTE = [("GND", (55.8, 54.86)), ("GND", (64.94, 48.49)), ("+3V3", (64.58, 53.0)), ("/RTC_INT", (64.45, 51.29))]
n = 0
for t in TOUT:
    nom = t.GetNetname()
    if nom in MORTS: ote.append(t); continue
    if t.Type() == pcbnew.PCB_VIA_T:
        if any(nom == r and pres(xy(t.GetPosition()), q) for r, q in V_OTE): ote.append(t); n += 1
    elif t.GetLayer() == F:
        s, e = xy(t.GetStart()), xy(t.GetEnd())
        if any(nom == r and ((pres(s, a) and pres(e, c)) or (pres(s, c) and pres(e, a))) for r, a, c in P_OTE): ote.append(t); n += 1
assert n == len(P_OTE) + len(V_OTE), n
for x in ote: b.Remove(x)

# ---- 2. la puce, d'après la netlist du schéma
t = open(netlist).read(); PIN = {}
for name, body in re.findall(r'\(net \(code "\d+"\) \(name "([^"]+)"\)(.*?)(?=\(net \(code|\Z)', t[t.index('(nets'):], re.S):
    for ref, pin in re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', body): PIN[(ref, pin)] = name
u = pcbnew.FootprintLoad("/usr/share/kicad/footprints/Package_SO.pretty", "SOIC-16W_7.5x10.3mm_P1.27mm")
u.SetPosition(V(54, 52)); u.SetReference("U4"); u.SetValue("DS3231SN"); u.SetFPID(pcbnew.LIB_ID("Package_SO", "SOIC-16W_7.5x10.3mm_P1.27mm"))
u.Reference().SetVisible(False); u.Value().SetVisible(False)
P = {}
for pad in u.Pads():
    pad.SetNet(net(PIN[("U4", pad.GetNumber())])); P[pad.GetNumber()] = xy(pad.GetPosition())
b.Add(u)
c13 = FPS["C13"]; c13.SetOrientationDegrees(0); c13.SetPosition(V(51.5, 45.6))
C = {}
for pad in c13.Pads():
    pad.SetNet(net(PIN[("C13", pad.GetNumber())])); C[pad.GetNumber()] = xy(pad.GetPosition())
print("U4", P); print("C13", C)

# ---- 3. les pistes
# alimentation : broche 2 -> via vers le plan 3,3 V, et C13 juste au-dessus
via("+3V3", 51.2, P["2"][1]); piste("+3V3", [P["2"], (51.2, P["2"][1])], 0.4); piste("+3V3", [C["1"], (51.2, C["1"][1] + 0.3), (51.2, P["2"][1])], 0.4)
via("GND", 53.0, C["2"][1]); piste("GND", [C["2"], (53.0, C["2"][1])], 0.4)
# masses : une barre de chaque côté et un via sous la puce
piste("GND", [P["5"], P["8"]], 0.4); piste("GND", [P["13"], P["9"]], 0.4)
via("GND", 51.4, P["7"][1]); piste("GND", [P["7"], (51.4, P["7"][1])], 0.4)
via("GND", 56.7, P["12"][1]); piste("GND", [P["12"], (56.7, P["12"][1])], 0.4)
# alarme (RTC_INT) : sous la puce, un via vers le dessous qui rejoint l'ancienne piste, et la résistance de rappel R21
y3 = P["3"][1]
piste("/RTC_INT", [P["3"], (51.0, y3), (56.0, y3 + 5.0), (56.0, 57.6), (56.83, 58.43), (56.83, 59.07)])
via("/RTC_INT", 51.5, y3 + 0.5); piste("/RTC_INT", [(51.5, y3 + 0.5), (63.76, y3 + 0.5), (64.45, 51.29)], 0.25, B)
# SCL : par la droite, jusqu'au coude de l'ancienne piste
piste("/SCL", [P["16"], (62.2, P["16"][1]), (63.0, P["16"][1] + 0.8), (63.0, 54.43), (63.21, 54.64)])
# SDA : par la droite, puis par le dessous pour passer sous SCL
piste("/SDA", [P["15"], (60.6, P["15"][1]), (61.6, P["15"][1] + 1.0), (61.6, 53.0)]); via("/SDA", 61.6, 53.0)
piste("/SDA", [(61.6, 53.0), (61.6, 55.86), (62.47, 56.73)], 0.25, B)
# pile : sous la puce, sortie par le haut, puis vers l'ancienne piste de la pile
piste("/VBAT", [P["14"], (57.0, P["14"][1]), (56.6, P["14"][1] - 0.4), (56.6, 46.6), (56.9, 46.3), (64.9, 46.3), (71.0, 52.4), (71.0, 54.17)])
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); pcbnew.SaveBoard(pcb, b)
