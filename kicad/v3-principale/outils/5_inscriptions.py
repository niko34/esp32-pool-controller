"""Inscriptions de la carte : nom de chaque prise, repère du +, consigne du pont 5 V."""
import sys, pcbnew
pcb = sys.argv[1]; b = pcbnew.LoadBoard(pcb); mm = pcbnew.FromMM; OX, OY = 60.0, 50.0
def txt(s, x, y, h=1.0, ang=0):
    t = pcbnew.PCB_TEXT(b); t.SetText(s); t.SetLayer(pcbnew.F_SilkS); t.SetPosition(pcbnew.VECTOR2I(mm(OX+x), mm(OY+y)))
    t.SetTextSize(pcbnew.VECTOR2I(mm(h), mm(h))); t.SetTextThickness(mm(h*0.15)); t.SetTextAngleDegrees(ang); b.Add(t)
# colonne de gauche : nom écrit de bas en haut, entre la prise et ses composants
for nom, y in (("12V", 14.5), ("FILTRATION", 28), ("ECLAIRAGE", 41.5), ("AUXILIAIRE", 55), ("DEBIT", 86)): txt(nom, 13.85, y, 1.0, 90)
txt("SONDE TEMPERATURE", 21.2, 71, 1.0, 90)        # plus à droite depuis le 2026-11-28 : la protection de la sonde occupe la place (outil 12)
for y in (12, 25.5, 39, 52.5): txt("+", 1.6, y, 1.0)        # broche 1 = le + (12 V)
txt("POMPE pH", 67.9, 12.6, 1.0); txt("POMPE Cl", 80.4, 12.6, 1.0)
txt("+", 64.4, 2.0, 1.0); txt("+", 76.4, 2.0, 1.0)
txt("5V EXT", 46.5, 79.6, 1.0); txt("retirer JP1 d'abord", 48.5, 92.2, 1.0); txt("JP1", 58, 37.4, 1.0)
txt("MESURE", 71.6, 90.2, 1.0)
b.FindFootprintByReference("J9").Reference().SetVisible(False); txt("RESET", 106.3, 60.6, 1.0); txt("BOOT", 106.6, 75.6, 1.0); txt("USINE", 106.3, 84.4, 1.0)
txt("Pool Controller - carte principale v3 rev 0.3", 34, 98.6, 1.0)        # la révision suit celle du schéma
for fp in b.GetFootprints():        # repères visibles : taille lisible par l'atelier (1,0 mm, trait 0,15 mm)
    rf = fp.Reference()
    if rf.IsVisible(): rf.SetTextSize(pcbnew.VECTOR2I(mm(1.0), mm(1.0))); rf.SetTextThickness(mm(0.15))
pcbnew.SaveBoard(pcb, b); print("inscriptions posees")
