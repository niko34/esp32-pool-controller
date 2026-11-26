"""Relie aux plans internes les plots CMS de masse et de 3,3 V : une piste courte et un via par plot.
Chaque position de via est essayée autour du plot et gardée seulement si elle respecte les écarts."""
import sys, math, pcbnew
pcb = sys.argv[1]; b = pcbnew.LoadBoard(pcb); mm = pcbnew.FromMM; MM = pcbnew.ToMM
OX, OY, W, H = 60.0, 50.0, 119.0, 100.0
VD, VT, JEU, LARG = 0.6, 0.3, 0.22, 0.4
FORT = ('+12V', '/VIN_BRUT', '/VIN_FUS', '/S_AUX', '/S_ECLAIRAGE', '/S_FILTRATION', '/S_POMPE_ORP', '/S_POMPE_PH')   # classe PUISSANCE : 0,25 mm
def jeu(n): return 0.28 if n in FORT else JEU
FPS = [pcbnew.Cast_to_FOOTPRINT(fp) for fp in b.GetFootprints()]
pads = [p for fp in FPS for p in fp.Pads()]
def boite(p):
    bb = p.GetBoundingBox(); return (MM(bb.GetLeft()), MM(bb.GetTop()), MM(bb.GetRight()), MM(bb.GetBottom()))
PB = [(p, boite(p), p.GetNetname(), p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH or p.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH) for p in pads]
segs = []; vias = []
for t in b.GetTracks():
    if t.Type() == pcbnew.PCB_VIA_T: vias.append((MM(t.GetPosition().x), MM(t.GetPosition().y), 0.3, t.GetNetname()))
    else: segs.append((MM(t.GetStart().x), MM(t.GetStart().y), MM(t.GetEnd().x), MM(t.GetEnd().y), MM(t.GetWidth()), t.GetLayer(), t.GetNetname()))
interdits = []
for fp in FPS:
    for z in fp.Zones():
        bb = z.GetBoundingBox(); interdits.append((MM(bb.GetLeft()), MM(bb.GetTop()), MM(bb.GetRight()), MM(bb.GetBottom())))
def d_seg(px, py, x1, y1, x2, y2):
    dx, dy = x2-x1, y2-y1; L = dx*dx+dy*dy
    u = 0 if L == 0 else max(0, min(1, ((px-x1)*dx+(py-y1)*dy)/L)); return math.hypot(px-(x1+u*dx), py-(y1+u*dy))
def d_boite(px, py, bx):
    return math.hypot(max(bx[0]-px, 0, px-bx[2]), max(bx[1]-py, 0, py-bx[3]))
def libre(x, y, r, net, couches, moi=None):
    if not (OX+0.45+r < x < OX+W-0.45-r and OY+0.45+r < y < OY+H-0.45-r): return False
    moi = moi if isinstance(moi, (list, tuple)) else (moi,)
    for bx in interdits:
        if d_boite(x, y, bx) < r + 0.3: return False
    for p, bx, n, traversant in PB:
        if any(p is q for q in moi): continue
        if n == net and not traversant: 
            continue
        if not traversant and p.GetLayer() not in couches: continue
        if d_boite(x, y, bx) < r + (jeu(n) if n != net else 0.15): return False
    for x1, y1, x2, y2, w, lay, n in segs:
        if n == net or lay not in couches: continue
        if d_seg(x, y, x1, y1, x2, y2) < r + w/2 + jeu(n): return False
    for vx, vy, vr, n in vias:
        if math.hypot(x-vx, y-vy) < r + vr + (jeu(n) if n != net else 0.1): return False
    return True
faits = rates = 0; NOTE = []
for p, bx, n, traversant in PB:
    if n not in ("GND", "+3V3") or traversant: continue
    cx, cy = MM(p.GetPosition().x), MM(p.GetPosition().y); ok = False
    cand = []
    for marge in (0.75, 1.0, 1.4, 1.9, 2.5, 3.2, 4.0):       # points autour du plot, de plus en plus loin
        x1, y1, x2, y2 = bx[0]-marge, bx[1]-marge, bx[2]+marge, bx[3]+marge; pas = 0.4
        k = 0
        while x1 + k*pas <= x2: cand += [(x1 + k*pas, y1), (x1 + k*pas, y2)]; k += 1
        k = 0
        while y1 + k*pas <= y2: cand += [(x1, y1 + k*pas), (x2, y1 + k*pas)]; k += 1
    for x, y in cand:
        if True:
            # départ de la piste : le point du plot le plus proche du via
            sx, sy = min(max(x, bx[0]+0.2), bx[2]-0.2), min(max(y, bx[1]+0.2), bx[3]-0.2)
            if bx[2]-bx[0] < 0.5: sx = cx
            if bx[3]-bx[1] < 0.5: sy = cy
            if not libre(x, y, VD/2, n, (pcbnew.F_Cu, pcbnew.B_Cu), moi=p): continue
            nb = max(4, int(math.hypot(x-sx, y-sy)/0.2))
            if not all(libre(sx + (x-sx)*f/nb, sy + (y-sy)*f/nb, LARG/2, n, (pcbnew.F_Cu,), moi=p) for f in range(1, nb)): continue
            ni = b.FindNet(n)
            t = pcbnew.PCB_TRACK(b); t.SetStart(pcbnew.VECTOR2I(mm(sx), mm(sy))); t.SetEnd(pcbnew.VECTOR2I(mm(x), mm(y))); t.SetWidth(mm(LARG)); t.SetLayer(pcbnew.F_Cu); t.SetNet(ni); b.Add(t)
            v = pcbnew.PCB_VIA(b); v.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y))); v.SetDrill(mm(VT)); v.SetWidth(pcbnew.F_Cu, mm(VD)); v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(ni); b.Add(v)
            t.SetLocked(True); v.SetLocked(True); NOTE.append((n, sx, sy, x, y))
            segs.append((sx, sy, x, y, LARG, pcbnew.F_Cu, n)); vias.append((x, y, VD/2, n)); ok = True; break
        if ok: break
    if not ok:      # pas de place pour un via : rejoindre par une piste coudée un plot voisin du même fil
        cibles = sorted([(math.hypot(MM(q.GetPosition().x)-cx, MM(q.GetPosition().y)-cy), q) for q, qb, qn, qt in PB if qn == n and q is not p], key=lambda c: c[0])
        for dq, q in cibles[:12]:
            if dq > 10: break
            tx, ty = MM(q.GetPosition().x), MM(q.GetPosition().y)
            chemins = [[(cx, cy), (tx, ty)]]
            for o in (0, 0.8, -0.8, 1.2, -1.2, 1.6, -1.6, 2.2, -2.2, 3.0, -3.0):
                chemins += [[(cx, cy), (cx+o, cy), (cx+o, ty), (tx, ty)], [(cx, cy), (cx, cy+o), (tx, cy+o), (tx, ty)]]
            for ch in chemins:
                bon = True
                for (x1, y1), (x2, y2) in zip(ch, ch[1:]):
                    nb = max(2, int(math.hypot(x2-x1, y2-y1)/0.15))
                    if not all(libre(x1+(x2-x1)*f/nb, y1+(y2-y1)*f/nb, 0.15, n, (pcbnew.F_Cu,), moi=(p, q)) for f in range(nb+1)): bon = False; break
                if not bon: continue
                for (x1, y1), (x2, y2) in zip(ch, ch[1:]):
                    if (x1, y1) == (x2, y2): continue
                    tk = pcbnew.PCB_TRACK(b); tk.SetStart(pcbnew.VECTOR2I(mm(x1), mm(y1))); tk.SetEnd(pcbnew.VECTOR2I(mm(x2), mm(y2))); tk.SetWidth(mm(0.3)); tk.SetLayer(pcbnew.F_Cu); tk.SetNet(b.FindNet(n)); b.Add(tk)
                    segs.append((x1, y1, x2, y2, 0.3, pcbnew.F_Cu, n))
                ok = True; print("relie par piste :", p.GetParentFootprint().GetReference(), p.GetNumber(), "->", q.GetParentFootprint().GetReference(), q.GetNumber()); break
            if ok: break
    faits += ok; rates += (not ok)
    if not ok: print("PAS DE PLACE :", p.GetParentFootprint().GetReference(), p.GetNumber(), n)
# vias thermiques de la plage du regulateur 5 V (U6, plot 2 = masse) : grille sous et autour du plot
u6 = [x for x in PB if x[0].GetParentFootprint().GetReference() == "U6" and x[0].GetNumber() == "2"]
if u6:
    bx = max(u6, key=lambda x: (x[1][2]-x[1][0])*(x[1][3]-x[1][1]))[1]; nth = 0
    for i in range(1, 6):
        for j in range(1, 6):
            x, y = bx[0] + (bx[2]-bx[0])*i/6, bx[1] + (bx[3]-bx[1])*j/6
            v = pcbnew.PCB_VIA(b); v.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y))); v.SetDrill(mm(0.3)); v.SetWidth(pcbnew.F_Cu, mm(0.6)); v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(b.FindNet("GND")); v.SetLocked(True); b.Add(v); nth += 1
            NOTE.append(("GND", x, y, x, y))
    print("vias thermiques sous U6 :", nth)
import json; json.dump(NOTE, open(sys.argv[2], "w"))
pcbnew.SaveBoard(pcb, b)
print("bretelles posees :", faits, "| sans place :", rates)
