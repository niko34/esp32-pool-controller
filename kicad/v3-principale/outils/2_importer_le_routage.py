import sys, pcbnew
pcb, ses = sys.argv[1], sys.argv[2]
OX, OY, W, H = 60.0, 50.0, 119.0, 100.0; mm = pcbnew.FromMM
b = pcbnew.LoadBoard(pcb)
print("import SES :", pcbnew.ImportSpecctraSES(b, ses))
import json, math
MM = pcbnew.ToMM
note = json.load(open(sys.argv[3])); vus = set((round(MM(t.GetPosition().x), 2), round(MM(t.GetPosition().y), 2)) for t in b.GetTracks() if t.Type() == pcbnew.PCB_VIA_T)
remis = 0
for n, sx, sy, x, y in note:        # bretelles posees avant le routage : les remettre si l'import les a retirees
    if (round(x, 2), round(y, 2)) in vus: continue
    ni = b.FindNet(n)
    if (sx, sy) != (x, y):
        t = pcbnew.PCB_TRACK(b); t.SetStart(pcbnew.VECTOR2I(mm(sx), mm(sy))); t.SetEnd(pcbnew.VECTOR2I(mm(x), mm(y))); t.SetWidth(mm(0.4)); t.SetLayer(pcbnew.F_Cu); t.SetNet(ni); b.Add(t)
    v = pcbnew.PCB_VIA(b); v.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y))); v.SetDrill(mm(0.3)); v.SetWidth(pcbnew.F_Cu, mm(0.6)); v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(ni); b.Add(v); remis += 1
print("bretelles remises apres import :", remis, "sur", len(note))
gnd = b.FindNet("GND")
for layer in (pcbnew.F_Cu, pcbnew.B_Cu):     # plans de masse sur les deux faces extérieures, autour des pistes
    z = pcbnew.ZONE(b); z.SetLayer(layer); z.SetNet(gnd); z.SetAssignedPriority(0)
    o = z.Outline(); o.NewOutline()
    for x, y in ((0.5, 0.5), (W-0.5, 0.5), (W-0.5, H-0.5), (0.5, H-0.5)): o.Append(mm(OX+x), mm(OY+y))
    z.SetLocalClearance(mm(0.3)); z.SetMinThickness(mm(0.25)); z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetThermalReliefGap(mm(0.3)); z.SetThermalReliefSpokeWidth(mm(0.5)); z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS); b.Add(z)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
pcbnew.SaveBoard(pcb, b)
tr = list(b.GetTracks()); print("pistes et vias :", len(tr), "dont vias :", sum(1 for t in tr if t.Type() == pcbnew.PCB_VIA_T))
