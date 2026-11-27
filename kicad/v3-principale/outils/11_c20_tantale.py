"""Retouche du 2026-11-27 (audit extérieur demandé par l'utilisateur, accord du même jour) :
C20, en sortie du régulateur 3,3 V (AMS1117), était un 22 µF céramique. La seule condition
de stabilité que donne la fiche de l'AMS1117 est 22 µF AU TANTALE ; la carte de mesure
l'applique déjà (C104, C204). C20 devient donc le même condensateur : 22 µF 16 V au
tantale, boîtier B (C8020), au même endroit et dans le même sens.
⚠️ Pièce POLARISÉE : la pastille 1 est le +, elle va au +3V3 (en bas sur la carte).
Les deux pistes existantes finissent déjà sous les nouvelles pastilles, plus grandes ;
elles sont prolongées jusqu'à leur centre.
Usage : python3 11_c20_tantale.py <carte>"""
import sys, pcbnew
pcb = sys.argv[1]; b = pcbnew.LoadBoard(pcb); mm = pcbnew.FromMM
def V(x, y): return pcbnew.VECTOR2I(mm(x), mm(y))
ancien = b.FindFootprintByReference("C20")
if "CP_EIA" in str(ancien.GetFPID().GetLibItemName()): sys.exit("déjà fait")
nets = {p.GetNumber(): p.GetNet() for p in ancien.Pads()}
assert nets["1"].GetNetname() == "+3V3" and nets["2"].GetNetname() == "GND"
pos, angle = ancien.GetPosition(), ancien.GetOrientationDegrees()
neuf = pcbnew.FootprintLoad("/usr/share/kicad/footprints/Capacitor_Tantalum_SMD.pretty", "CP_EIA-3528-21_Kemet-B")
neuf.SetFPID(pcbnew.LIB_ID("Capacitor_Tantalum_SMD", "CP_EIA-3528-21_Kemet-B"))
neuf.SetReference("C20"); neuf.SetValue("22u tantale"); neuf.SetPath(ancien.GetPath())
neuf.SetSheetname(ancien.GetSheetname()); neuf.SetSheetfile(ancien.GetSheetfile())
rf, ra = neuf.Reference(), ancien.Reference()
rf.SetTextSize(ra.GetTextSize()); rf.SetTextThickness(ra.GetTextThickness()); neuf.Value().SetVisible(False)
b.Add(neuf); neuf.SetOrientationDegrees(angle); neuf.SetPosition(pos)
rf.SetPosition(V(139.6, 91.0)); rf.SetTextAngleDegrees(90)      # repère à droite du boîtier, hors des pastilles
for p in neuf.Pads(): p.SetNet(nets[p.GetNumber()])
b.Remove(ancien)
for p in neuf.Pads():      # prolonge l'ancienne arrivée (centre de l'ancienne pastille) jusqu'au centre de la nouvelle
    c = p.GetPosition(); y0 = 91.95 if p.GetNumber() == "1" else 90.05
    t = pcbnew.PCB_TRACK(b); t.SetStart(V(137.0, y0)); t.SetEnd(c); t.SetWidth(mm(0.5)); t.SetLayer(pcbnew.F_Cu); t.SetNet(p.GetNet()); b.Add(t)
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); pcbnew.SaveBoard(pcb, b)
print("C20 :", [(p.GetNumber(), pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y), p.GetNetname()) for p in neuf.Pads()])
