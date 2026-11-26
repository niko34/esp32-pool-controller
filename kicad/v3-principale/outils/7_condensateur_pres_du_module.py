"""Retouche du 2026-11-26, demandée par l'utilisateur : le condensateur de découplage
C9 (100 nF) était à 19 mm de la broche 3,3 V du module ESP32. Il passe à 2,5 mm,
entre le via de masse et le via 3,3 V qui desservent les broches 1 et 2.
Pour lui faire la place : R12 (rappel de EN) est retournée, et la piste EN qui
longeait le module est redessinée 0,6 mm plus à gauche.
Usage : python3 7_condensateur_pres_du_module.py <carte>"""
import sys, pcbnew
pcb = sys.argv[1]; b = pcbnew.LoadBoard(pcb); mm = pcbnew.FromMM; MM = pcbnew.ToMM
OX, OY = 60.0, 50.0
def V(x, y): return pcbnew.VECTOR2I(mm(OX+x), mm(OY+y))
def piste(net, pts, w):
    for a, c in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b); t.SetStart(V(*a)); t.SetEnd(V(*c)); t.SetWidth(mm(w)); t.SetLayer(pcbnew.F_Cu); t.SetNet(b.FindNet(net)); b.Add(t)
TOUT = list(b.GetTracks()); FPS = {f.GetReference(): pcbnew.Cast_to_FOOTPRINT(f) for f in b.GetFootprints()}
def xy(v): return (round(MM(v.x)-OX, 2), round(MM(v.y)-OY, 2))
def pres(a, c): return abs(a[0]-c[0]) < 0.03 and abs(a[1]-c[1]) < 0.03
# ce qui disparaît : (réseau, une extrémité, l'autre) pour les pistes, (réseau, position) pour les vias
P_OTE = [("+3V3", (94.0, 27.68), (92.94, 28.71)), ("GND", (93.41, 26.17), (93.44, 26.17)), ("GND", (93.44, 26.17), (94.0, 26.72)),
         ("GND", (93.89, 26.64), (92.94, 25.69)), ("+3V3", (94.0, 8.02), (92.78, 6.88)),
         ("/EN", (95.49, 10.48), (95.49, 7.87)), ("/EN", (95.49, 7.87), (94.0, 6.38))]
V_OTE = [("+3V3", (92.94, 28.71)), ("GND", (92.94, 25.69))]
ote = []
for t in TOUT:
    n = t.GetNetname()
    if t.Type() == pcbnew.PCB_VIA_T:
        if any(n == r and pres(xy(t.GetPosition()), q) for r, q in V_OTE): ote.append(t)
    else:
        s, e = xy(t.GetStart()), xy(t.GetEnd())
        if any(n == r and ((pres(s, a) and pres(e, c)) or (pres(s, c) and pres(e, a))) for r, a, c in P_OTE): ote.append(t)
assert len(ote) == len(P_OTE) + len(V_OTE), len(ote)
for x in ote: b.Remove(x)
# R12 retournée : 3,3 V en haut (y 6,38), EN en bas (y 8,02)
FPS["R12"].SetOrientationDegrees(270)
# C9 : 3,3 V en bas (95,85 ; 8,28), masse en haut (95,85 ; 7,32)
FPS["C9"].SetPosition(V(95.85, 7.8))
plots = {q.GetNetname(): xy(q.GetPosition()) for r in ("R12", "C9") for q in FPS[r].Pads() if r == "C9"}
r12 = {q.GetNetname(): xy(q.GetPosition()) for q in FPS["R12"].Pads()}
print("C9", plots, "R12", r12)
piste("+3V3", [plots["+3V3"], (96.75, 8.81)], 0.4)
piste("GND", [plots["GND"], (96.75, 6.74)], 0.4)
piste("+3V3", [r12["+3V3"], (92.78, 6.88)], 0.4)
piste("/EN", [r12["/EN"], (94.9, 8.92), (94.9, 11.08)], 0.25)
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); pcbnew.SaveBoard(pcb, b)
