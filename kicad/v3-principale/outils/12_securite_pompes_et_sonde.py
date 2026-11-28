"""Retouche du 2026-11-28 — étude CEM et fiabilité (kicad/REGLES-CEM-ET-FIABILITE.md), accord de
l'utilisateur du même jour. Trois sujets, dans l'ordre du fichier :

1. SÉCURITÉ DES POMPES. Jusqu'ici, un transistor de pompe en panne « passant » faisait doser sans
   arrêt, et rien ne le montrait. Désormais :
   - un interrupteur commun (Q11, AO4407A) coupe le 12 V des DEUX pompes ; il n'est fermé que si
     l'ESP32 met ARME_POMPES (IO4) à 1 — au repos, au démarrage, après un plantage : coupé ;
   - le 12 V des borniers et des diodes de roue libre devient V_POMPES (rail commuté) ;
     les condensateurs 10 µF C3 et C4 restent sur le +12V, EN AMONT ; un 100 nF au pied de chaque
     diode (C21, C23) referme au plus court le courant haché de sa pompe ;
   - deux diviseurs (100 kΩ / 33 kΩ) ramènent chaque sortie de pompe à 3 V : LU_POMPE_PH (IO18)
     et LU_POMPE_ORP (IO23). Ces trois signaux restent sur le connecteur de débogage (13, 14, 16).
2. SONDE DE TEMPÉRATURE. Son câble sort du boîtier : le 3,3 V et la broche de l'ESP32 n'y vont plus
   en direct. 100 Ω en série sur chacun (R39, R40), 100 nF (C22) et une protection contre les
   décharges (U8) au ras du bornier J8.
3. PISTE AU BORD. La liaison IO19 longeait le bord droit sur 50 mm, à 0,3 mm du bord et hors du plan
   de masse. Elle est RETIRÉE : IO19 et la broche 15 du connecteur de débogage ne sont plus reliées.
   (La relecture de la pompe chlore prend IO23, dont la liaison au connecteur existait déjà, loin du bord.)

Les liaisons longues sont cherchées par outils/routeur.py ; le reste est posé à la main ci-dessous.
Usage : python3 12_securite_pompes_et_sonde.py <carte> <netlist exportée du schéma (.net)>
⚠️ Ne se lance qu'UNE fois : il s'arrête si la carte porte déjà Q11."""
import sys, re, os, json, pcbnew
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import routeur

pcb, netlist = sys.argv[1], sys.argv[2]
b = pcbnew.LoadBoard(pcb); mm = pcbnew.FromMM; T = pcbnew.ToMM
if b.FindFootprintByReference("Q11"): sys.exit("déjà fait")
F, B = pcbnew.F_Cu, pcbnew.B_Cu
STD = "/usr/share/kicad/footprints/"
def V(x, y): return pcbnew.VECTOR2I(mm(x), mm(y))
def net(nom):
    n = b.FindNet(nom)
    if n is None: n = pcbnew.NETINFO_ITEM(b, nom); b.Add(n)
    return n
def piste(nom, pts, w=0.25, layer=F):
    for a, c in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b); t.SetStart(V(*a)); t.SetEnd(V(*c)); t.SetWidth(mm(w)); t.SetLayer(layer); t.SetNet(net(nom)); b.Add(t)
def via(nom, x, y, d=0.6, trou=0.3):
    v = pcbnew.PCB_VIA(b); v.SetPosition(V(x, y)); v.SetDrill(mm(trou)); v.SetWidth(F, mm(d)); v.SetLayerPair(F, B); v.SetNet(net(nom)); b.Add(v)
def xy(v): return (T(v.x), T(v.y))
def pres(a, c): return abs(a[0] - c[0]) < 0.03 and abs(a[1] - c[1]) < 0.03

# ---- la netlist du schéma : valeur et empreinte de chaque pièce, réseau de chaque broche
t = open(netlist, encoding="utf-8").read()
COMP = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(r'\(comp \(ref "([^"]+)"\)\s*\(value "([^"]*)"\)\s*\(footprint "([^"]*)"\)', t)}
PIN = {}
for name, body in re.findall(r'\(net \(code "\d+"\) \(name "([^"]+)"\)(.*?)(?=\(net \(code|\Z)', t[t.index('(nets'):], re.S):
    for ref, pin in re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', body): PIN[(ref, pin)] = name

# ---- 0. les familles de liaisons du projet : le rail commuté est une liaison de puissance, l'alimentation de la sonde une alimentation
pro = pcb[:-len(".kicad_pcb")] + ".kicad_pro"
P = json.load(open(pro, encoding="utf-8")); motifs = P["net_settings"]["netclass_patterns"]
for classe, motif in (("PUISSANCE", "/V_POMPES"), ("ALIM", "/V_SONDE")):
    if not any(m["pattern"] == motif for m in motifs): motifs.append({"netclass": classe, "pattern": motif})
open(pro, "w", encoding="utf-8").write(json.dumps(P, indent=2, ensure_ascii=False))      # comme KiCad : sans saut de ligne final

# ---- 1. les trois liaisons du connecteur de débogage changent de nom (mêmes broches de l'ESP32, mêmes pistes)
for ancien, neuf in (("/IO4", "/ARME_POMPES"), ("/IO18", "/LU_POMPE_PH"), ("/IO23", "/LU_POMPE_ORP")):
    a, n = b.FindNet(ancien), net(neuf); k = 0
    for x in list(b.GetTracks()) + [q for f in b.GetFootprints() for q in f.Pads()]:
        if x.GetNetCode() == a.GetNetCode(): x.SetNet(n); k += 1
    b.Remove(a); print("%s -> %s : %d éléments" % (ancien, neuf, k))

# ---- 2. ce qui disparaît
OTE_P = [  # (réseau, face, bout, bout)
    ("+12V", B, (126.00, 56.00), (117.50, 64.50)), ("+12V", B, (117.50, 64.50), (109.00, 64.50)),   # ancienne arrivée directe du 12 V sur le bornier de la pompe pH
    ("+12V", B, (125.20, 63.30), (121.60, 70.09)),
    ("+12V", F, (137.08, 66.58), (135.50, 65.00)), ("+12V", F, (137.08, 70.38), (137.08, 66.58)),   # ancien lien diode D4 - condensateur C4
    ("+12V", F, (135.94, 71.53), (137.08, 70.38)), ("+12V", F, (135.50, 71.53), (135.94, 71.53)),
    ("/TEMP", B, (67.00, 126.16), (80.19, 126.16)), ("/TEMP", B, (80.19, 126.16), (85.27, 121.08)), ("/TEMP", B, (85.27, 120.00), (85.27, 121.08)),   # le bornier n'est plus relié en direct
]
OTE_V = [("+12V", (125.20, 63.30)), ("+12V", (121.60, 70.09))]
ote = []; n = 0
for x in b.GetTracks():
    nom = x.GetNetname()
    if nom == "/IO19": ote.append(x)                  # toute la liaison IO19 : 50 mm au bord droit, puis 50 mm jusqu'au connecteur de débogage
    elif x.Type() == pcbnew.PCB_VIA_T:
        if any(nom == r and pres(xy(x.GetPosition()), q) for r, q in OTE_V): ote.append(x); n += 1
    else:
        s, e = xy(x.GetStart()), xy(x.GetEnd())
        if any(nom == r and x.GetLayer() == c and ((pres(s, p) and pres(e, q)) or (pres(s, q) and pres(e, p))) for r, c, p, q in OTE_P): ote.append(x); n += 1
assert n == len(OTE_P) + len(OTE_V), n
for x in ote: b.Remove(x)
print("retirés : %d pistes et vias" % len(ote))

# ---- 3. le 12 V des borniers et des diodes devient le rail commuté
RAIL = [(F, (126.0, 56.0), (126.0, 62.5)), (F, (126.0, 62.5), (123.5, 65.0)), (F, (138.0, 56.0), (138.0, 62.5)), (F, (138.0, 62.5), (135.5, 65.0)),
        (B, (126.0, 56.0), (128.5, 53.5)), (B, (128.5, 53.5), (135.5, 53.5)), (B, (135.5, 53.5), (138.0, 56.0))]
n = 0
for x in b.GetTracks():
    if x.Type() != pcbnew.PCB_VIA_T and x.GetNetname() == "+12V":
        s, e = xy(x.GetStart()), xy(x.GetEnd())
        if any(x.GetLayer() == c and ((pres(s, p) and pres(e, q)) or (pres(s, q) and pres(e, p))) for c, p, q in RAIL): x.SetNet(net("/V_POMPES")); n += 1
assert n == len(RAIL), n
# ... et chaque pastille des pièces en place prend le réseau que lui donne le schéma
change = []
for f in b.GetFootprints():
    for q in f.Pads():
        voulu = PIN.get((f.GetReference(), q.GetNumber()))
        if voulu and q.GetNetname() != voulu: change.append("%s.%s %s -> %s" % (f.GetReference(), q.GetNumber(), q.GetNetname(), voulu)); q.SetNet(net(voulu))
print("pastilles changées de réseau :", sorted(change))
assert sorted(c.split()[0] for c in change) == ["D3.1", "D4.1", "J11.15", "J2.1", "J3.1", "J8.2", "J8.3", "U1.31"], change
b.Remove(b.FindNet("/IO19"))
b.FindFootprintByReference("C2").SetValue(COMP["C2"][0])      # 100 nF 50 V : l'écrêteur du 12 V laisse monter à 21,5 V

# ---- 4. les pièces nouvelles
POSE = {  # repère : (x, y, angle)
    # coupure commune du 12 V des pompes, à gauche du bornier de la pompe pH
    "Q11": (114.0, 57.0, 90),      # sources en bas (vers l'arrivée du 12 V), drains en haut (vers les borniers)
    "R33": (115.3, 61.7, 0), "R32": (117.9, 58.3, 90),
    "Q10": (108.5, 57.0, 0), "R31": (105.7, 57.0, 270), "R30": (106.6, 54.3, 0),
    "C21": (126.3, 65.0, 0), "R34": (126.85, 66.45, 0),          # contre la diode de roue libre de la pompe pH
    "C23": (138.3, 65.0, 0),                                     # contre celle de la pompe chlore (relecture du 2026-11-28 : C21 en est à 36 mm)
    # relecture des deux sorties, contre leurs pistes
    "R35": (119.8, 62.6, 180), "R36": (119.8, 64.4, 0), "R37": (131.6, 66.0, 180), "R38": (131.6, 67.8, 0),
    # bornier de la sonde de température
    "U8": (74.6, 123.62, 0), "C22": (73.4625, 120.85, 90), "R39": (75.7375, 119.95, 270), "R40": (78.35, 125.6, 0),
}
Q = {}
for ref, (x, y, angle) in POSE.items():
    lib, nom = COMP[ref][1].split(":")
    f = pcbnew.FootprintLoad(STD + lib + ".pretty", nom); f.SetFPID(pcbnew.LIB_ID(lib, nom)); f.SetReference(ref); f.SetValue(COMP[ref][0])
    for champ in (f.Reference(), f.Value()): champ.SetVisible(False)
    f.Reference().SetTextSize(pcbnew.VECTOR2I(mm(1.0), mm(1.0))); f.Reference().SetTextThickness(mm(0.15))
    b.Add(f); f.SetPosition(V(x, y)); f.SetOrientationDegrees(angle)
    for q in f.Pads(): q.SetNet(net(PIN[(ref, q.GetNumber())])); Q[(ref, q.GetNumber())] = xy(q.GetPosition())
    print("%-4s %-12s %s" % (ref, COMP[ref][0], {q.GetNumber(): "%s (%.2f, %.2f)" % (q.GetNetname(), *xy(q.GetPosition())) for q in f.Pads()}))
for ref in ("J2", "J3", "J8", "D3", "D4", "C3", "C4", "U1", "J11"):
    for q in b.FindFootprintByReference(ref).Pads(): Q[(ref, q.GetNumber())] = xy(q.GetPosition())

# ---- 5. pistes posées à la main : la puissance et les liaisons courtes
# 12 V vers l'interrupteur : une barre sous les trois sources, et une descente jusqu'à l'arrivée existante (Y = 64,05)
ys = Q[("Q11", "1")][1] + 0.4
piste("+12V", [(Q[("Q11", "1")][0], ys), (Q[("Q11", "3")][0], ys)], 0.8); piste("+12V", [(112.5, ys), (112.5, 64.05)], 1.0)
piste("+12V", [(112.5, Q[("R33", "1")][1]), Q[("R33", "1")]], 0.4)
# rail commuté : une barre sur les quatre drains, puis par le haut jusqu'au bornier de la pompe pH (la pompe chlore suit par le dessous, déjà tracé)
yd = Q[("Q11", "8")][1] - 0.4
piste("/V_POMPES", [(Q[("Q11", "8")][0], yd), (117.4, yd), (118.6, 52.9), (124.9, 52.9), (126.0, 54.0), Q[("J2", "1")]], 1.0)
piste("/V_POMPES", [Q[("C21", "1")], Q[("D3", "1")]], 0.5); piste("/V_POMPES", [Q[("C23", "1")], Q[("D4", "1")]], 0.5)
piste("/V_POMPES", [Q[("C21", "1")], (Q[("C21", "1")][0], 66.245), Q[("R34", "1")]], 0.3)
# les condensateurs 10 µF restent en amont : C4 rejoint C3 par le dessous
for c in ("C3", "C4"): x, y = Q[(c, "1")]; via("+12V", x, 73.0, 0.8, 0.4); piste("+12V", [(x, y), (x, 73.0)], 0.8)
piste("+12V", [(123.5, 73.0), (125.35, 74.85), (133.65, 74.85), (135.5, 73.0)], 0.8, B)
# commande de l'interrupteur
g = Q[("Q11", "4")]
piste("/G_HAUT", [Q[("R33", "2")], (Q[("R33", "2")][0], 60.6), (g[0], 60.6 - (Q[("R33", "2")][0] - g[0])), g])
piste("/G_HAUT", [Q[("R32", "1")], (116.6, Q[("R32", "1")][1]), (116.6 - (g[1] - Q[("R32", "1")][1]), g[1]), g])
piste("/D_ARME", [Q[("Q10", "3")], (Q[("R32", "2")][0] - (Q[("R32", "2")][1] - 57.0), 57.0), Q[("R32", "2")]])      # passe sous l'interrupteur, entre ses deux rangées de broches
piste("/G_ARME", [Q[("R30", "2")], (Q[("Q10", "1")][0], Q[("R30", "2")][1] + (Q[("Q10", "1")][0] - Q[("R30", "2")][0])), Q[("Q10", "1")]])
piste("/G_ARME", [Q[("Q10", "1")], (Q[("R31", "1")][0] + 0.125, Q[("Q10", "1")][1]), Q[("R31", "1")]])
piste("GND", [Q[("Q10", "2")], (Q[("R31", "2")][0] + 0.125, Q[("Q10", "2")][1]), Q[("R31", "2")]], 0.4)
# relectures
piste("/S_POMPE_PH", [Q[("R35", "1")], (121.89, Q[("R35", "1")][1])], 0.3); piste("/S_POMPE_ORP", [Q[("R37", "1")], (133.89, Q[("R37", "1")][1])], 0.3)
# sonde de température : bornier -> condensateur -> protection -> résistance -> carte
j2, j3 = Q[("J8", "2")], Q[("J8", "3")]; c1 = Q[("C22", "1")]; u = {k: Q[("U8", k)] for k in "123456"}
piste("/V_SONDE", [j2, (c1[0] - (c1[1] - j2[1]), j2[1]), c1, u["1"], u["6"], Q[("R39", "2")]], 0.4)
piste("+3V3", [Q[("R39", "1")], (76.94, 119.49)], 0.4)                               # jusqu'au passage existant vers le plan 3,3 V (celui de C14)
piste("GND", [u["2"], (71.95, u["2"][1])], 0.4); via("GND", 71.95, u["2"][1])        # le chemin de la décharge : au plus court vers le plan de masse
piste("GND", [Q[("C22", "2")], (Q[("C22", "2")][0], 119.25)], 0.4); via("GND", Q[("C22", "2")][0], 119.25)
piste("/TEMP_EXT", [j3, (u["3"][0] - (j3[1] - u["3"][1]), j3[1]), u["3"], u["4"], (Q[("R40", "1")][0] - (Q[("R40", "1")][1] - u["4"][1]), u["4"][1]), Q[("R40", "1")]], 0.3)
r2 = Q[("R40", "2")]
piste("/TEMP", [r2, (80.0, r2[1] - (80.0 - r2[0])), (80.0, 118.923 + (80.0 - 79.098)), (79.098, 118.923)])

# ---- 6. les trois liaisons de sécurité : du module vers les pompes PAR LE DESSOUS, côte à côte, loin des bords
# Chacune quitte sa broche du module par un passage posé juste à droite de celui-ci, puis file vers l'ouest sous le module.
# Les trois passent entre la liaison RX (Y = 65,24) et la commande de la pompe pH (Y = 69,16), à 0,4 mm l'une de l'autre.
OUEST = {"/LU_POMPE_ORP": (129.5, 66.85), "/LU_POMPE_PH": (124.0, 67.5), "/ARME_POMPES": (122.0, 68.15)}      # où chacune s'arrête, avant de monter vers ses pièces
for nom, broche in (("/LU_POMPE_ORP", "37"), ("/LU_POMPE_PH", "30"), ("/ARME_POMPES", "26")):
    p = Q[("U1", broche)]; xv, yv = 177.6, p[1]; yo = OUEST[nom][1]
    piste(nom, [p, (xv, yv)]); via(nom, xv, yv)
    if broche == "37": piste(nom, [(xv, yv), (xv, yo - 0.75), (xv - 0.75, yo), OUEST[nom]], 0.25, B)      # IO23 est plus haut : 7 mm de descente à 1,3 mm du bord droit
    else: piste(nom, [(xv, yv), (xv - (yv - yo), yo), OUEST[nom]], 0.25, B)                                # à 45 degrés jusqu'à la hauteur de la liaison
R = routeur.Routeur(b)
# d'abord les liaisons aux plans (au plus court), ensuite la fin des trois liaisons, enfin les deux résistances de chaque diviseur
print("plan 3,3 V  U8.5      :", R.vers_le_plan("+3V3", u["5"] + ("F",), 0.3))
for ref, pin in (("R31", "2"), ("C21", "2"), ("C23", "2"), ("R36", "2"), ("R38", "2")): print("plan de masse %s.%s :" % (ref, pin), R.vers_le_plan("GND", Q[(ref, pin)] + ("F",), 0.4))
print("masse R34.2           :", R.relier("GND", Q[("R34", "2")] + ("F",), [Q[("C21", "2")] + ("F",)], 0.4, "F"))
print("ARME_POMPES             :", R.relier("/ARME_POMPES", Q[("R30", "1")] + ("F",), [OUEST["/ARME_POMPES"] + ("B",)]), flush=True)
print("LU_POMPE_PH             :", R.relier("/LU_POMPE_PH", Q[("R36", "1")] + ("F",), [OUEST["/LU_POMPE_PH"] + ("B",)]), flush=True)
print("LU_POMPE_ORP            :", R.relier("/LU_POMPE_ORP", Q[("R38", "1")] + ("F",), [OUEST["/LU_POMPE_ORP"] + ("B",)]), flush=True)
piste("/LU_POMPE_PH", [Q[("R35", "2")], Q[("R36", "1")]]); piste("/LU_POMPE_ORP", [Q[("R37", "2")], Q[("R38", "1")]])

# ---- 7. inscriptions
for d in b.GetDrawings():
    if d.GetClass() == "PCB_TEXT" and d.GetText() == "SONDE TEMPERATURE": d.SetPosition(V(81.2, 121.0))          # à droite des pièces ajoutées
    if d.GetClass() == "PCB_TEXT" and "rev 0.1" in d.GetText(): d.SetText(d.GetText().replace("rev 0.1", "rev 0.3"))
tb = b.GetTitleBlock(); tb.SetRevision("0.3"); tb.SetDate("2026-11-28")
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); pcbnew.SaveBoard(pcb, b)
import resource; print("carte enregistrée ; mémoire maximale utilisée : %d Mo" % (resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024))
