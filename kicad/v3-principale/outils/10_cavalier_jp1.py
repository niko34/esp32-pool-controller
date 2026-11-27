"""Retouche du 2026-11-26, demandée par l'utilisateur : JP1 était un pont de cuivre à
couper au cutter. Cette carte sert à comparer plusieurs alimentations, donc à passer
plusieurs fois du régulateur au 5 V externe : JP1 devient une barrette de deux broches
au pas de 2,54 mm (C124375), fermée par un capuchon amovible.
⚠️ La carte arrive OUVERTE : sans capuchon sur JP1, pas de 5 V ni de 3,3 V.
Les deux broches sont centrées sur l'ancien pont ; les deux pistes sont prolongées.
Usage : python3 10_cavalier_jp1.py <carte>"""
import sys, pcbnew
pcb = sys.argv[1]; b = pcbnew.LoadBoard(pcb); mm = pcbnew.FromMM
def V(x, y): return pcbnew.VECTOR2I(mm(x), mm(y))
ancien = [f for f in b.GetFootprints() if f.GetReference() == "JP1"][0]
if "PinHeader" in str(ancien.GetFPID().GetLibItemName()): sys.exit("déjà fait")
nets = {p.GetNumber(): p.GetNet() for p in ancien.Pads()}
neuf = pcbnew.FootprintLoad("/usr/share/kicad/footprints/Connector_PinHeader_2.54mm.pretty", "PinHeader_1x02_P2.54mm_Vertical")
neuf.SetFPID(pcbnew.LIB_ID("Connector_PinHeader_2.54mm", "PinHeader_1x02_P2.54mm_Vertical"))
neuf.SetReference("JP1"); neuf.SetValue("Cavalier 5 V"); neuf.SetPath(ancien.GetPath())
neuf.SetSheetname(ancien.GetSheetname()); neuf.SetSheetfile(ancien.GetSheetfile())
neuf.Reference().SetVisible(False); neuf.Value().SetVisible(False)
b.Add(neuf); neuf.SetOrientationDegrees(90); neuf.SetPosition(V(116.73, 90.0))     # broche 1 à gauche, broche 2 à droite
for p in neuf.Pads(): p.SetNet(nets[p.GetNumber()])
b.Remove(ancien)
for nom, x0, x1 in (("/5V_REG", 117.35, 116.73), ("+5V", 118.65, 119.27)):
    t = pcbnew.PCB_TRACK(b); t.SetStart(V(x0, 90.0)); t.SetEnd(V(x1, 90.0)); t.SetWidth(mm(0.5)); t.SetLayer(pcbnew.F_Cu); t.SetNet(b.FindNet(nom)); b.Add(t)
for d in b.GetDrawings():
    if d.GetClass() == "PCB_TEXT" and d.GetText() == "couper JP1 d'abord": d.SetText("retirer JP1 d'abord")
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); pcbnew.SaveBoard(pcb, b)
print("JP1 :", [(p.GetNumber(), pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y), p.GetNetname()) for p in neuf.Pads()])
