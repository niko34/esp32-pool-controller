"""Retouches après le routage automatique : bouts de piste sans suite, piste trop près du bord."""
import sys, re, pcbnew
pcb, rpt = sys.argv[1], sys.argv[2]; b = pcbnew.LoadBoard(pcb); mm = pcbnew.FromMM; MM = pcbnew.ToMM
OX, W = 60.0, 119.0
pts = [(float(x), float(y), float(l)) for x, y, l in re.findall(r'\[track_dangling\][^\n]*\n[^\n]*\n\s+@\(([\d.]+) mm, ([\d.]+) mm\): Track \[[^\]]*\] on \S+, length ([\d.]+) mm', open(rpt).read())]
PISTES = [x for x in b.GetTracks() if x.Type() != pcbnew.PCB_VIA_T]
LIM = OX + W      # pas de recul : deux pistes longent le bord droit a 0,30 mm, ecart admis par l atelier
recules = 0
for t in PISTES:
    s, e = t.GetStart(), t.GetEnd(); ch = False
    if MM(s.x) > LIM: t.SetStart(pcbnew.VECTOR2I(mm(LIM), s.y)); ch = True
    if MM(e.x) > LIM: t.SetEnd(pcbnew.VECTOR2I(mm(LIM), e.y)); ch = True
    recules += ch
a_retirer = []
for t in PISTES:
    L = MM(t.GetLength())
    for x, y, l in pts:
        if abs(L - l) < 0.002 and L < 0.6 and (abs(MM(t.GetStart().x)-x) < 0.002 and abs(MM(t.GetStart().y)-y) < 0.002 or abs(MM(t.GetEnd().x)-x) < 0.002 and abs(MM(t.GetEnd().y)-y) < 0.002):
            a_retirer.append(t); break
retires = len(a_retirer)
for t in a_retirer: b.Remove(t)
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); pcbnew.SaveBoard(pcb, b)
print("bouts retires :", retires, "| segments recules du bord :", recules)
