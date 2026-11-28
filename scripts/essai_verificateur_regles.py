#!/usr/bin/env python3
"""Éprouve scripts/verifier_regles_cartes.py : chaque règle est CASSÉE exprès sur une copie de la carte,
et le vérificateur doit le dire. Un contrôle qui ne voit pas sa propre panne ne prouve rien.

Usage : essai_verificateur_regles.py            (quelques minutes ; ne touche à aucun fichier du dépôt)
Sort avec le code 1 si une panne volontaire passe inaperçue.
À relancer après toute modification du vérificateur, ou après l'ajout d'une règle (ajouter alors sa panne ici)."""
import json, os, re, shutil, subprocess, sys, tempfile
import pcbnew

ICI = os.path.dirname(os.path.abspath(__file__)); KICAD = os.path.join(ICI, "..", "kicad"); mm = pcbnew.FromMM
def V(x, y): return pcbnew.VECTOR2I(mm(x), mm(y))
def pastille(b, ref, n): return [q for q in b.FindFootprintByReference(ref).Pads() if q.GetNumber() == n][0]
def sur(b, ref, n, reseau): pastille(b, ref, n).SetNet(b.FindNet(reseau))
POSEES = []
def piste(b, reseau, a, c, couche=pcbnew.F_Cu, w=0.25):
    t = pcbnew.PCB_TRACK(b); t.SetStart(V(*a)); t.SetEnd(V(*c)); t.SetWidth(mm(w)); t.SetLayer(couche); t.SetNet(b.FindNet(reseau)); b.Add(t); POSEES.append((t, reseau))
def remplir(b): pcbnew.ZONE_FILLER(b).Fill(b.Zones())

# ---- les pannes : (carte, règle qui doit tomber, description, fonction qui abîme la copie)
def p_bornier_sur_12v(b, d): sur(b, "J2", "1", "+12V")
def p_reserve_en_aval(b, d): b.FindFootprintByReference("C21").SetValue("10u 50V")
def p_diviseur(b, d): b.FindFootprintByReference("R36").SetValue("100k")
def p_piste_au_bord(b, d): piste(b, "/LED", (178.75, 100.0), (178.75, 110.0))
def p_plage_decousue(b, d):
    for t in list(b.GetTracks()):
        x, y = pcbnew.ToMM(t.GetPosition().x), pcbnew.ToMM(t.GetPosition().y)
        if t.Type() == pcbnew.PCB_VIA_T and t.GetNetname() == "GND" and 119 < x < 141.5 and 96 < y < 144: b.Remove(t)
    remplir(b)
def p_sonde_en_direct(b, d): sur(b, "J8", "3", "/TEMP")
def p_io12_tiree(b, d): sur(b, "R13", "2", "/IO12")
def p_condensateur_16v(b, d): b.FindFootprintByReference("C2").SetValue("100n")
def p_plan_coupe(b, d): piste(b, "+12V", (60.2, 100.0), (178.8, 100.0), pcbnew.In1_Cu, 0.5); remplir(b)
def p_schema_retouche(b, d):
    f = os.path.join(d, "v3-principale.kicad_sch"); s = open(f, encoding="utf-8").read()
    open(f, "w", encoding="utf-8").write(s.replace('(label "ARME_POMPES"', '(label "ARME_POMPE"', 1))
def p_cuivre_dans_antenne(b, d): piste(b, "GND", (160.0, 53.0), (170.0, 53.0))
def p_motif_oublie(b, d):
    f = os.path.join(d, "v3-mesure-std.kicad_pro"); P = json.load(open(f, encoding="utf-8"))
    P["net_settings"]["netclass_patterns"] = [m for m in P["net_settings"]["netclass_patterns"] if m["pattern"] != "/FILTP_PH"]; open(f, "w", encoding="utf-8").write(json.dumps(P, indent=2))
def p_condensateur_entre_masses(b, d): sur(b, "C112", "2", "GND")
def p_piste_trop_pres(b, d): piste(b, "GND", (102.5, 93.0), (102.5, 96.0))      # sur le dessus, au-dessus du plan de masse de la voie pH, sans rien toucher
def p_sans_filtre(b, d): sur(b, "U103", "4", "/MESURE_PH")
def p_corps_en_direct(b, d): sur(b, "U103", "5", "/REF_PH")
def p_filtre_trop_resistant(b, d): b.FindFootprintByReference("R209").SetValue("4k7")
def p_garde_sous_vernis(b, d):
    for x in list(b.GetDrawings()):
        if x.GetLayer() in (pcbnew.F_Mask, pcbnew.B_Mask): b.Remove(x)
def p_vernis_ouvert_ailleurs(b, d):
    g = pcbnew.PCB_SHAPE(b); g.SetShape(pcbnew.SHAPE_T_SEGMENT); q = pastille(b, "C106", "1").GetPosition(); g.SetStart(q); g.SetEnd(pcbnew.VECTOR2I(q.x + mm(2), q.y)); g.SetLayer(pcbnew.F_Mask); g.SetWidth(mm(0.4)); b.Add(g)
def p_resistance_contre_la_prise(b, d): f = b.FindFootprintByReference("R105"); p = f.GetPosition(); f.SetPosition(pcbnew.VECTOR2I(p.x, p.y + mm(1.0)))
def p_ampli_sans_filtre(b, d): sur(b, "C113", "1", "VISO_PH")
def p_garde_en_direct(b, d): b.FindFootprintByReference("R211").SetValue("0R")
def p_ame_en_direct(b, d): sur(b, "U104", "1", "/AME_PH")
def p_carte_retouchee(b, d):          # un passage de masse de plus, là où KiCad n'y voit rien à redire
    v = pcbnew.PCB_VIA(b); v.SetPosition(V(120.0, 68.0)); v.SetDrill(mm(0.3)); v.SetWidth(pcbnew.F_Cu, mm(0.6)); v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(b.FindNet("GND")); b.Add(v)
PANNES = [
    ("v3-principale", "P1", "bornier de la pompe pH remis en direct sur le 12 V", p_bornier_sur_12v),
    ("v3-principale", "P2", "10 µF en aval de l'interrupteur des pompes", p_reserve_en_aval),
    ("v3-principale", "P3", "diviseur de relecture faussé (100 kΩ / 100 kΩ : 6 V sur la broche)", p_diviseur),
    ("v3-principale", "C4", "piste de signal à 0,1 mm du bord", p_piste_au_bord),
    ("v3-principale", "C5", "plage de masse de 5 cm² décousue", p_plage_decousue),
    ("v3-principale", "C7", "borne de la sonde reliée en direct à la broche du module", p_sonde_en_direct),
    ("v3-principale", "P5", "une résistance tire IO12", p_io12_tiree),
    ("v3-principale", "C6", "condensateur du 12 V sans tension de service écrite", p_condensateur_16v),
    ("v3-principale", "C3", "piste dans le plan de masse interne, d'un bord à l'autre", p_plan_coupe),
    ("v3-principale", "C2", "schéma retouché à la main", p_schema_retouche),
    ("v3-principale", "P4", "cuivre dans la zone de l'antenne", p_cuivre_dans_antenne),
    ("v3-mesure-std", "M1", "un réseau de la voie pH oublié dans sa famille", p_motif_oublie),
    ("v3-mesure-std", "M3", "condensateur entre la masse de la voie pH et celle de l'ESP32", p_condensateur_entre_masses),
    ("v3-mesure-std", "M2", "piste du côté ESP32 posée au-dessus du plan de masse de la voie pH", p_piste_trop_pres),
    ("v3-mesure-std", "M4", "convertisseur relié en direct à l'amplificateur", p_sans_filtre),
    ("v3-mesure-std", "M4", "résistance de filtre de 4,7 kΩ devant le convertisseur de la voie ORP", p_filtre_trop_resistant),
    ("v3-mesure-std", "M5", "corps de la prise en direct sur le convertisseur", p_corps_en_direct),
    ("v3-mesure-std", "M6", "anneaux de garde remis sous vernis", p_garde_sous_vernis),
    ("v3-mesure-std", "M6", "vernis ouvert sur une alimentation", p_vernis_ouvert_ailleurs),
    ("v3-mesure-std", "M7", "résistance d'entrée rapprochée de 1 mm du corps de la prise", p_resistance_contre_la_prise),
    ("v3-mesure-std", "M8", "10 µF de l'amplificateur pH branché avant sa résistance", p_ampli_sans_filtre),
    ("v3-mesure-std", "M6", "garde ORP pilotée en direct (0 Ω)", p_garde_en_direct),
    ("v3-mesure-std", "C2", "carte de mesure retouchée à la main (un passage de masse de plus)", p_carte_retouchee),
    ("v3-mesure-std", "M5", "âme de la prise en direct sur l'amplificateur, sans sa résistance", p_ame_en_direct),
]
if len(sys.argv) == 4 and sys.argv[1] == "--abimer":          # appelé par lui-même : une panne par processus (pcbnew supporte mal plusieurs cartes à la suite)
    carte, regle, quoi, abime = PANNES[int(sys.argv[2])]; d = sys.argv[3]
    f = os.path.join(d, carte + ".kicad_pcb"); b = pcbnew.LoadBoard(f); abime(b, d); pcbnew.SaveBoard(f, b)
    # ⚠️ À l'enregistrement, KiCad donne à une piste le réseau de ce qu'elle TOUCHE : une piste posée contre un autre cuivre change de nom sans rien dire.
    change = [(voulu, t.GetNetname()) for t, voulu in POSEES if t.GetNetname() != voulu]
    if change: print("la piste posée a changé de réseau à l'enregistrement (%s -> %s) : elle touche un autre cuivre" % change[0], file=sys.stderr); os._exit(2)
    os._exit(0)          # sortie sèche : pcbnew plante parfois en rendant la mémoire, après le travail fait
seul = sys.argv[1] if len(sys.argv) > 1 else None; rates = 0
for k, (carte, regle, quoi, abime) in enumerate(PANNES):
    if seul and seul not in (regle, carte): continue
    with tempfile.TemporaryDirectory() as tmp:
        d = os.path.join(tmp, carte); shutil.copytree(os.path.join(KICAD, carte), d, ignore=shutil.ignore_patterns("fabrication", "*-backups", "*.kicad_prl", "fp-info-cache"))
        a = subprocess.run([sys.executable, os.path.abspath(__file__), "--abimer", str(k), d], capture_output=True, text=True)
        if a.returncode: print("❌ %-14s %-3s %-66s -> la panne n'a pas pu être posée : %s" % (carte, regle, quoi, ((a.stderr.strip().splitlines() or ["code de sortie %d, sans message" % a.returncode])[-1])[:150])); rates += 1; continue
        r = subprocess.run([sys.executable, os.path.join(ICI, "verifier_regles_cartes.py"), carte, d], capture_output=True, text=True)
        tombees = sorted(set(re.findall(r"^❌ ([A-Z]\d+) ", r.stdout, re.M))); vu = regle in tombees and r.returncode == 1
        rates += not vu
        print("%s %-14s %-3s %-66s -> %s" % ("✅" if vu else "❌", carte, regle, quoi, ("vue" + (" (tombent aussi : %s)" % ", ".join(x for x in tombees if x != regle) if len(tombees) > 1 else "")) if vu else "PAS VUE — règles tombées : %s" % (tombees or "aucune")), flush=True)
print("\n%s" % ("❌ %d panne(s) volontaire(s) non vue(s) : le vérificateur est à corriger." % rates if rates else "✅ Toutes les pannes volontaires sont vues."))
sys.exit(1 if rates else 0)
