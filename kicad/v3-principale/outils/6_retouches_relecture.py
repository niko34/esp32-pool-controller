"""Retouches demandées par la relecture du 2026-11-26 (`electronics-reviewer`).
1. coin de la carte découpé sous l'antenne du module ESP32 (guide de dessin d'Espressif) ;
2. condensateur C3 de la pompe pH relié à la cathode de D3 par 8 mm au lieu de 48 ;
3. sortie éclairage : plus de piste entre les plots de D7, liaison directe par le dessous ;
4. vias retirés des carrés de la pastille de masse du module ;
5. plot de masse du régulateur 5 V relié au cuivre sans rayons, pour la chaleur.
Usage : python3 6_retouches_relecture.py <carte>"""
import sys, math, pcbnew
pcb = sys.argv[1]; b = pcbnew.LoadBoard(pcb); mm = pcbnew.FromMM; MM = pcbnew.ToMM
OX, OY, W, H = 60.0, 50.0, 119.0, 100.0
def V(x, y): return pcbnew.VECTOR2I(mm(OX+x), mm(OY+y))
def piste(net, pts, w, layer):
    for a, c in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b); t.SetStart(V(*a)); t.SetEnd(V(*c)); t.SetWidth(mm(w)); t.SetLayer(layer); t.SetNet(b.FindNet(net)); b.Add(t)
def via(net, x, y, d, trou):
    v = pcbnew.PCB_VIA(b); v.SetPosition(V(x, y)); v.SetDrill(mm(trou)); v.SetWidth(pcbnew.F_Cu, mm(d)); v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(b.FindNet(net)); b.Add(v)
PISTES = [t for t in b.GetTracks() if t.Type() != pcbnew.PCB_VIA_T]; VIAS = [t for t in b.GetTracks() if t.Type() == pcbnew.PCB_VIA_T]
FPS = [pcbnew.Cast_to_FOOTPRINT(f) for f in b.GetFootprints()]
def p(t): return (round(MM(t.GetStart().x)-OX, 2), round(MM(t.GetStart().y)-OY, 2), round(MM(t.GetEnd().x)-OX, 2), round(MM(t.GetEnd().y)-OY, 2))
ote = []
# 1. contour : coin haut-droit découpé sous l'antenne (x 97,5 à 119, y 0 à 6,0 : un via de masse du module est à y = 6,7)
for d in list(b.GetDrawings()):
    if d.GetLayer() == pcbnew.Edge_Cuts: ote.append(d)
pts = [(0, 0), (97.5, 0), (97.5, 6.0), (W, 6.0), (W, H), (0, H), (0, 0)]
for a, c in zip(pts, pts[1:]):
    s = pcbnew.PCB_SHAPE(b); s.SetShape(pcbnew.SHAPE_T_SEGMENT); s.SetStart(V(*a)); s.SetEnd(V(*c)); s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(mm(0.05)); b.Add(s)
# 3. sortie éclairage : retirer le détour du dessus (il passait entre les plots de D7)
detour = [(7.0, 44.08, 7.0, 45.77), (12.21, 50.98, 7.0, 45.77), (12.21, 54.2, 12.21, 50.98), (14.57, 56.56, 12.21, 54.2), (17.38, 56.56, 14.57, 56.56), (18.0, 55.94, 17.38, 56.56), (18.0, 43.5, 18.0, 55.94), (20.0, 41.5, 18.0, 43.5)]
n3 = 0
for t in PISTES:
    if t.GetNetname() != "/S_ECLAIRAGE" or t.GetLayer() != pcbnew.F_Cu: continue
    q = p(t)
    if any(all(abs(u-v) < 0.03 for u, v in zip(q, d)) or all(abs(u-v) < 0.03 for u, v in zip(q, (d[2], d[3], d[0], d[1]))) for d in detour): ote.append(t); n3 += 1
via("/S_ECLAIRAGE", 19.6, 43.25, 0.8, 0.4); piste("/S_ECLAIRAGE", [(19.6, 43.25), (20.0, 41.5)], 0.8, pcbnew.F_Cu)
# 2. C3 : liaison directe par le dessous entre la cathode de D3 et le + de C3
via("+12V", 65.2, 13.3, 0.8, 0.4); via("+12V", 61.6, 20.09, 0.8, 0.4); piste("+12V", [(65.2, 13.3), (61.6, 20.09)], 0.8, pcbnew.B_Cu)
# 4. vias dans les carrés de la pastille de masse du module
u1 = [f for f in FPS if f.GetReference() == "U1"][0]; carres = [q.GetBoundingBox() for q in u1.Pads() if q.GetNumber() == "39"]; n4 = 0
for v in VIAS:
    if v.GetNetname() == "GND" and any(c.Contains(v.GetPosition()) for c in carres):
        ote.append(v); n4 += 1
        for t in PISTES:
            if t.GetNetname() == "GND" and (t.GetStart() == v.GetPosition() or t.GetEnd() == v.GetPosition()) and MM(t.GetLength()) < 3: ote.append(t)
# 5. plot de masse du régulateur : connexion pleine
for q in [f for f in FPS if f.GetReference() == "U6"][0].Pads():
    if q.GetNumber() == "2": q.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
for x in ote: b.Remove(x)
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); pcbnew.SaveBoard(pcb, b)
print("detour eclairage : %d segments retires | vias de pastille retires : %d" % (n3, n4))
