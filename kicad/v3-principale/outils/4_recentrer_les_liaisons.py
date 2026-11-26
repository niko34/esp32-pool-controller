"""Après le routage : l'outil a raccordé en T certains plots à la liaison déjà posée vers le plan.
On retire ce raccord et on fait partir la liaison du centre du plot."""
import sys, re, math, pcbnew
pcb, rpt = sys.argv[1], sys.argv[2]; b = pcbnew.LoadBoard(pcb); MM = pcbnew.ToMM
pts = [(float(x), float(y), float(l)) for x, y, l in re.findall(r'\[track_dangling\][^\n]*\n[^\n]*\n\s+@\(([\d.]+) mm, ([\d.]+) mm\): Track \[[^\]]*\] on \S+, length ([\d.]+) mm', open(rpt).read())]
pads = [p for fp in [pcbnew.Cast_to_FOOTPRINT(f) for f in b.GetFootprints()] for p in fp.Pads()]
PISTES = [x for x in b.GetTracks() if x.Type() != pcbnew.PCB_VIA_T]
def dseg(px, py, t):
    x1, y1, x2, y2 = MM(t.GetStart().x), MM(t.GetStart().y), MM(t.GetEnd().x), MM(t.GetEnd().y); dx, dy = x2-x1, y2-y1; L = dx*dx+dy*dy
    u = 0 if L == 0 else max(0, min(1, ((px-x1)*dx+(py-y1)*dy)/L)); return math.hypot(px-(x1+u*dx), py-(y1+u*dy))
ote = []; n = 0
for x, y, l in pts:
    racc = [t for t in PISTES if abs(MM(t.GetLength())-l) < 0.002 and (abs(MM(t.GetStart().x)-x) < 0.002 and abs(MM(t.GetStart().y)-y) < 0.002 or abs(MM(t.GetEnd().x)-x) < 0.002 and abs(MM(t.GetEnd().y)-y) < 0.002)]
    if not racc: continue
    r = racc[0]
    for s in PISTES:       # la liaison sur laquelle le raccord vient buter
        if s is r or s.GetNetname() != r.GetNetname() or s.GetLayer() != r.GetLayer() or dseg(x, y, s) > 0.15: continue
        for quoi in ("Start", "End"):
            q = getattr(s, "Get" + quoi)()
            for p in pads:
                if p.GetNetname() == s.GetNetname() and p.IsOnLayer(s.GetLayer()) and p.GetBoundingBox().Contains(q):
                    getattr(s, "Set" + quoi)(p.GetPosition()); n += 1; ote.append(r); break
for t in ote: b.Remove(t)
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); pcbnew.SaveBoard(pcb, b); print("raccords retires :", len(ote), "| liaisons recentrees :", n)
