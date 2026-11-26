"""Retouche du 2026-11-26, demandée par la relecture (`electronics-reviewer`) : la fiche du
DS3231 (p. 18) demande de ne pas faire passer de signal sous la puce sans plan de masse
entre les deux. L'alarme (RTC_INT) passait sous la puce sur le dessus.
Elle sort maintenant par la gauche de la puce, descend par un via et file sur le dessous
(le plan de masse est entre elle et la puce). La résistance de rappel R21 est déplacée
à l'autre bout, près du point (64,45 ; 51,29).
Usage : python3 9_alarme_hors_de_la_puce.py <carte>"""
import sys, pcbnew
pcb = sys.argv[1]; b = pcbnew.LoadBoard(pcb); mm = pcbnew.FromMM; MM = pcbnew.ToMM
OX, OY = 60.0, 50.0; F, B = pcbnew.F_Cu, pcbnew.B_Cu
def V(x, y): return pcbnew.VECTOR2I(mm(OX+x), mm(OY+y))
def piste(nom, pts, w=0.25, layer=F):
    for a, c in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b); t.SetStart(V(*a)); t.SetEnd(V(*c)); t.SetWidth(mm(w)); t.SetLayer(layer); t.SetNet(b.FindNet(nom)); b.Add(t)
def via(nom, x, y):
    v = pcbnew.PCB_VIA(b); v.SetPosition(V(x, y)); v.SetDrill(mm(0.3)); v.SetWidth(F, mm(0.6)); v.SetLayerPair(F, B); v.SetNet(b.FindNet(nom)); b.Add(v)
def xy(v): return (round(MM(v.x)-OX, 2), round(MM(v.y)-OY, 2))
def pres(a, c): return abs(a[0]-c[0]) < 0.03 and abs(a[1]-c[1]) < 0.03
TOUT = list(b.GetTracks()); FPS = {f.GetReference(): pcbnew.Cast_to_FOOTPRINT(f) for f in b.GetFootprints()}
ote = []
for t in TOUT:
    nom = t.GetNetname()
    if nom == "/RTC_INT":
        pos = xy(t.GetPosition() if t.Type() == pcbnew.PCB_VIA_T else t.GetStart())
        if pos[0] > 70: continue                                                              # côté module ESP32 : on n'y touche pas
        if t.Type() == pcbnew.PCB_VIA_T or t.GetLayer() == F: ote.append(t)                       # le dessus autour de l'horloge, et le via sous la puce
        elif pres(xy(t.GetStart()), (51.5, 50.59)) or pres(xy(t.GetStart()), (63.76, 50.59)) or pres(xy(t.GetEnd()), (63.76, 50.59)): ote.append(t)
    elif nom == "+3V3":                                                                       # ancienne liaison de R21 au plan
        if t.Type() == pcbnew.PCB_VIA_T:
            if pres(xy(t.GetPosition()), (54.03, 58.78)): ote.append(t)
        elif any(pres(xy(t.GetStart()), q) or pres(xy(t.GetEnd()), q) for q in ((54.97, 59.72), (55.17, 60.0))): ote.append(t)
print("retires :", len(ote))
for x in ote: b.Remove(x)
r = FPS["R21"]; r.SetOrientationDegrees(90); r.SetPosition(V(65.6, 52.15))
P = {q.GetNetname(): xy(q.GetPosition()) for q in r.Pads()}; print("R21", P)
p3 = [xy(q.GetPosition()) for q in FPS["U4"].Pads() if q.GetNumber() == "3"][0]
via("/RTC_INT", 47.4, p3[1]); piste("/RTC_INT", [p3, (47.4, p3[1])])
piste("/RTC_INT", [(47.4, p3[1]), (47.9, 50.6), (63.76, 50.6), (64.45, 51.29)], 0.25, B)
via("/RTC_INT", 64.45, 51.29); piste("/RTC_INT", [(64.45, 51.29), P["/RTC_INT"]])
via("+3V3", 64.58, 53.0); piste("+3V3", [(64.58, 53.0), P["+3V3"]], 0.4)
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); pcbnew.SaveBoard(pcb, b)
