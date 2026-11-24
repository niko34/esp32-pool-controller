#!/usr/bin/env python3
"""Écrit le schéma KiCad 9 de la carte de mesure v3 depuis le tableau des connexions
(README.md de ce dossier). Signaux reliés par de VRAIS FILS ; alimentations et masses par
symboles d'alimentation ; seuls les signaux allant au connecteur J1 passent par étiquettes.

Usage : python3 generer_schema.py <dossier de sortie> [fichier des connexions attendues .json]
⚠️ RÉÉCRIT le schéma en entier : ne plus le lancer après une modification à la main."""
import re, sys, uuid, json, os
ICI = os.path.dirname(os.path.abspath(__file__))
V2 = os.path.join(ICI, "..", "esp32-pool-controller", "esp32-pool-controller.kicad_sch")
if not os.path.exists(V2): V2 = "/root/esp32-pool-controller/kicad/esp32-pool-controller/esp32-pool-controller.kicad_sch"
OUT = sys.argv[1]
PRJ = "v3-mesure"
ROOT = "15e90db4-b97c-4655-b3f3-5076f9e0263b"   # fixe : le projet et la carte y font référence
def U(): return str(uuid.uuid4())
FONT = "(effects (font (size 1.27 1.27)))"
def r(v): return ("%.4f" % v).rstrip("0").rstrip(".")

# ---------- symboles repris du schéma v2 (lu, jamais modifié) ----------
v2 = open(V2, encoding="utf-8").read()
def bloc(nom):
    i = v2.index('(symbol "%s"' % nom); n = 0; j = i
    while True:
        c = v2[j]
        if c == "(": n += 1
        elif c == ")":
            n -= 1
            if n == 0: return v2[i:j+1]
        j += 1
REPRIS = {n: bloc(n) for n in ("Device:R", "Device:C", "power:+5V", "power:GND", "power:PWR_FLAG")}

# ---------- symboles propres à la carte ----------
def sym_custom(nom, ref, desc, w, top, bot, gauche, droite, prefixe):
    full = (PRJ + ":" + nom) if prefixe else nom
    L = ['(symbol "%s" (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)' % full,
         '(property "Reference" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))' % (ref, r(-w/2), r(top+1.27)),
         '(property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))' % (nom, r(-w/2), r(bot-1.27)),
         '(property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))',
         '(property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))',
         '(property "Description" "%s" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))' % desc,
         '(symbol "%s_0_1" (rectangle (start %s %s) (end %s %s) (stroke (width 0.254) (type default)) (fill (type background))))' % (nom, r(-w/2), r(top), r(w/2), r(bot)),
         '(symbol "%s_1_1"' % nom]
    pins = {}
    for cote, liste in (("g", gauche), ("d", droite)):
        for num, nm, typ, y in liste:
            x = -w/2 - 2.54 if cote == "g" else w/2 + 2.54
            L.append('(pin %s line (at %s %s %d) (length 2.54) (name "%s" %s) (number "%s" %s))' % (typ, r(x), r(y), 0 if cote == "g" else 180, nm, FONT, num, FONT))
            pins[num] = (x, y)
    L.append(') (embedded_fonts no))')
    return "\n".join(L), pins
DEF = {
 "ADM3260": ("U", "Isolateur I2C double avec convertisseur isole, SSOP-20", 25.4, 20.32, -25.4,
   [("9","VIN","power_in",17.78),("4","VDDP","power_in",12.7),("3","SDA2","bidirectional",5.08),("2","SCL2","bidirectional",2.54),
    ("8","PDIS","input",-2.54),("6","NC","no_connect",-7.62),
    ("1","GNDP","power_in",-12.7),("5","GNDP","power_in",-15.24),("7","GNDP","power_in",-17.78),("10","GNDP","power_in",-20.32)],
   [("12","VISO","power_out",17.78),("17","VDDISO","power_in",12.7),("13","VSEL","input",7.62),("18","SDA1","bidirectional",-2.54),("19","SCL1","bidirectional",-5.08),
    ("15","NC","no_connect",-10.16),
    ("11","GNDISO","power_in",-15.24),("14","GNDISO","power_in",-17.78),("16","GNDISO","power_in",-20.32),("20","GNDISO","power_in",-22.86)]),
 "Atlas_OEM": ("U", "Module Atlas Scientific OEM (pH ou ORP), 10 plots", 25.4, 12.7, -15.24,
   [("3","VCC","power_in",7.62),("1","SDA","bidirectional",2.54),("8","SCL","bidirectional",0),("7","INT","output",-5.08),("6","GND","power_in",-10.16)],
   [("4","PRB","passive",7.62),("5","PRB_GND","passive",2.54),("2","NC","no_connect",-5.08),("9","NC","no_connect",-7.62),("10","NC","no_connect",-10.16)]),
 "BNC": ("J", "Prise BNC femelle pour carte : ame et corps", 10.16, 7.62, -7.62,
   [("1","AME","passive",2.54),("2","CORPS","passive",-2.54)], []),
 "Conn_8": ("J", "Connecteur 8 voies vers la carte principale (JST XH)", 10.16, 12.7, -10.16,
   [], [(str(i+1), "V%d" % (i+1), "passive", 10.16 - 2.54*i) for i in range(8)]),
}
CUSTOM, CPINS, LIBTXT = {}, {}, []
for nom, (ref, desc, w, top, bot, g, d) in DEF.items():
    CUSTOM[nom], CPINS[nom] = sym_custom(nom, ref, desc, w, top, bot, g, d, True)
    LIBTXT.append(sym_custom(nom, ref, desc, w, top, bot, g, d, False)[0])

# ---------- éléments ----------
E = []; ATTENDU = {}; NC = []
def net(nom, *broches): ATTENDU.setdefault(nom, set()).update(broches)
def prop(nom, val, x, y, cache=False, just="left", ang=0):
    eff = "(effects (font (size 1.27 1.27))%s%s)" % (" (justify %s)" % just if just else "", " (hide yes)" if cache else "")
    return '(property "%s" "%s" (at %s %s %d) %s)' % (nom, val, r(x), r(y), ang, eff)
def place(lib_id, ref, val, x, y, pins, fp="", lcsc="", refpos=(2.54, -1.27), valpos=(2.54, 1.27), rot=0, montre_ref=True, justv="left"):
    L = ['(symbol (lib_id "%s") (at %s %s %d) (unit 1) (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no) (uuid "%s")' % (lib_id, r(x), r(y), rot, U()),
         prop("Reference", ref, x+refpos[0], y+refpos[1], cache=not montre_ref, just=(None if rot else "left"), ang=rot),
         prop("Value", val, x+valpos[0], y+valpos[1], just=(None if rot else justv), ang=rot),
         prop("Footprint", fp, x, y, cache=True), prop("Datasheet", "", x, y, cache=True)]
    if lcsc: L.append(prop("LCSC", lcsc, x, y, cache=True))
    for n in pins: L.append('(pin "%s" (uuid "%s"))' % (n, U()))
    L.append('(instances (project "%s" (path "/%s" (reference "%s") (unit 1)))))' % (PRJ, ROOT, ref))
    E.append("\n".join(L))
FILS = []; POINTS = set()
def P(x, y): POINTS.add((round(x, 3), round(y, 3)))
def fil(*pts):
    for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
        FILS.append(((round(x1, 3), round(y1, 3)), (round(x2, 3), round(y2, 3)))); P(x1, y1); P(x2, y2)
def jonction(x, y):
    P(x, y); E.append('(junction (at %s %s) (diameter 0) (color 0 0 0 0) (uuid "%s"))' % (r(x), r(y), U()))
def ecrire_fils():
    """KiCad ne relie un fil à un point que si le fil a une extrémité à ce point :
    chaque segment est donc coupé aux points de connexion qu'il traverse."""
    for (a, b) in FILS:
        dedans = [p for p in POINTS if p != a and p != b and (
            (a[0] == b[0] == p[0] and min(a[1], b[1]) < p[1] < max(a[1], b[1])) or
            (a[1] == b[1] == p[1] and min(a[0], b[0]) < p[0] < max(a[0], b[0])))]
        chaine = sorted([a, b] + dedans, key=lambda q: (q[0] - a[0]) ** 2 + (q[1] - a[1]) ** 2)
        for q1, q2 in zip(chaine, chaine[1:]):
            E.append('(wire (pts (xy %s %s) (xy %s %s)) (stroke (width 0) (type default)) (uuid "%s"))' % (r(q1[0]), r(q1[1]), r(q2[0]), r(q2[1]), U()))
def etiq(nom, x, y, vers):
    rot, just = {"d": (0, "left bottom"), "g": (180, "right bottom")}[vers]
    P(x, y)
    E.append('(label "%s" (at %s %s %d) (effects (font (size 1.27 1.27)) (justify %s)) (uuid "%s"))' % (nom, r(x), r(y), rot, just, U()))
def nc(x, y): E.append('(no_connect (at %s %s) (uuid "%s"))' % (r(x), r(y), U()))
def texte(t, x, y, taille=1.27, gras=False):
    t = t.replace('"', '\\"').replace("\n", "\\n")
    E.append('(text "%s" (exclude_from_sim no) (at %s %s 0) (effects (font (size %s %s)%s) (justify left bottom)) (uuid "%s"))' % (t, r(x), r(y), r(taille), r(taille), " (bold yes)" if gras else "", U()))
def cadre(x1, y1, x2, y2):
    E.append('(rectangle (start %s %s) (end %s %s) (stroke (width 0.2) (type dash)) (fill (type none)) (uuid "%s"))' % (r(x1), r(y1), r(x2), r(y2), U()))
CPT = {"pwr": 1, "flg": 1}
def plus(nom, x, y):      # symbole d'alimentation positive, broche en (x, y), dessiné vers le haut
    P(x, y); place("power:+5V", "#PWR%03d" % CPT["pwr"], nom, x, y, ["1"], refpos=(0, 3.81), valpos=(0, -3.556), montre_ref=False, justv=None); CPT["pwr"] += 1
def masse(nom, x, y):     # symbole de masse, broche en (x, y), dessiné vers le bas
    P(x, y); place("power:GND", "#PWR%03d" % CPT["pwr"], nom, x, y, ["1"], refpos=(0, 6.35), valpos=(0, 4.445), montre_ref=False, justv=None); CPT["pwr"] += 1
def drapeau(x, y):
    P(x, y); place("power:PWR_FLAG", "#FLG%02d" % CPT["flg"], "PWR_FLAG", x, y, ["1"], refpos=(0, -1.905), valpos=(0, -3.81), montre_ref=False, justv=None); CPT["flg"] += 1
FP = {"R": "Resistor_SMD:R_0603_1608Metric", "C01": "Capacitor_SMD:C_0402_1005Metric", "C10": "Capacitor_SMD:C_0805_2012Metric",
      "U": "Package_SO:SSOP-20_5.3x7.2mm_P0.65mm", "J1": "Connector_JST:JST_XH_B8B-XH-A_1x08_P2.50mm_Vertical"}
LCSC = {"100n": "C1525", "10u": "C15850", "4k7": "C23162", "10k": "C25804"}
def custom(nom, ref, val, x, y, fp="", lcsc=""):
    place(PRJ + ":" + nom, ref, val, x, y, CPINS[nom].keys(), fp, lcsc,
          refpos=(-DEF[nom][2]/2, -DEF[nom][3]-1.27), valpos=(-DEF[nom][2]/2, -DEF[nom][4]+1.27))
    d = {num: (x + px, y - py) for num, (px, py) in CPINS[nom].items()}
    for q in d.values(): P(*q)
    return d
def vertical(kind, ref, val, x, ytop):
    """Résistance ou condensateur vertical : broche 1 en (x, ytop), broche 2 en (x, ytop+7.62)."""
    place("Device:" + kind, ref, val, x, ytop + 3.81, ["1", "2"], FP["R"] if kind == "R" else (FP["C01"] if val == "100n" else FP["C10"]), LCSC[val])
    P(x, ytop); P(x, ytop + 7.62)
    return (x, ytop), (x, ytop + 7.62)
def horizontal_R(ref, val, xg, y):
    """Résistance couchée entre (xg, y) et (xg+7.62, y)."""
    place("Device:R", ref, val, xg + 3.81, y, ["1", "2"], FP["R"], LCSC[val], rot=90, refpos=(0, -2.54), valpos=(0, 2.54))
    P(xg, y); P(xg + 7.62, y)
    return (xg, y), (xg + 7.62, y)

def voie(sfx, n, oy, titre, module):
    V = lambda s: "%s_%s" % (s, sfx)
    Uref, Mref, Jref = "U%d01" % n, "U%d02" % n, "J%d01" % n
    C = lambda i: "C%d%02d" % (n, i)
    R = lambda i: "R%d%02d" % (n, i)
    ux, uy = 165.1, oy + 43.18
    RV = uy - 30.48                                   # hauteur des rails d'alimentation
    cadre(15.24, oy - 7.62, 403.86, oy + 81.28)
    texte(titre, 17.78, oy - 1.27, 2.5, True)
    texte("COTE ESP32 (non isole)", 17.78, oy + 5.08, 1.5, True)
    texte("COTE ISOLE - voie %s : rien n'est partage avec l'autre voie" % sfx, 299.72, oy - 1.27, 1.5, True)
    E.append('(polyline (pts (xy %s %s) (xy %s %s)) (stroke (width 0.5) (type dash_dot)) (uuid "%s"))' % (r(ux), r(oy - 5.08), r(ux), r(oy + 78.74), U()))
    texte("BARRIERE D'ISOLATION : aucun cuivre ne la franchit, sur aucune couche (5,5 mm)", 167.64, oy - 2.54, 1.1)
    p = custom("ADM3260", Uref, "ADM3260ARSZ", ux, uy, FP["U"], "C208558")
    # ---- côté ESP32 ----
    # VIN : montée, rail, deux condensateurs, +5V
    x9, y9 = p["9"]
    fil((x9, y9), (144.78, y9), (144.78, RV), (111.76, RV)); plus("+5V", 111.76, RV)
    for i, (x, val) in enumerate(((134.62, "100n"), (121.92, "10u"))):
        a, b = vertical("C", C(1 + i), val, x, RV); jonction(*a); masse("GND", *b); texte("br. 9", x + 2.54, RV + 8.255, 0.9)
        net("+5V", C(1+i) + ".1"); net("GND", C(1+i) + ".2")
    net("+5V", Uref + ".9")
    # VDDP : montée, rail, deux condensateurs, deux rappels, +3V3
    x4, y4 = p["4"]
    fil((x4, y4), (104.14, y4), (104.14, RV), (55.88, RV)); plus("+3V3", 104.14, RV); jonction(104.14, RV)
    for i, (x, val) in enumerate(((93.98, "100n"), (81.28, "10u"))):
        a, b = vertical("C", C(3 + i), val, x, RV); jonction(*a); masse("GND", *b); texte("br. 4", x + 2.54, RV + 8.255, 0.9)
        net("+3V3", C(3+i) + ".1"); net("GND", C(3+i) + ".2")
    net("+3V3", Uref + ".4", R(1) + ".1", R(2) + ".1")
    xs, ys = p["3"]; xc, yc = p["2"]
    a, b = vertical("R", R(1), "4k7", 68.58, RV); jonction(*a); fil(b, (68.58, ys)); jonction(68.58, ys)
    a, b = vertical("R", R(2), "4k7", 55.88, RV); fil(b, (55.88, yc)); jonction(55.88, yc)
    fil((xs, ys), (45.72, ys)); etiq(V("SDA"), 45.72, ys, "g")
    fil((xc, yc), (45.72, yc)); etiq(V("SCL"), 45.72, yc, "g")
    net(V("SDA"), Uref + ".3", R(1) + ".2"); net(V("SCL"), Uref + ".2", R(2) + ".2")
    # PDIS et les quatre GNDP : colonne de masse
    xg = 142.24; yb = uy + 25.4
    fil((p["8"][0], p["8"][1]), (xg, p["8"][1]), (xg, yb)); masse("GND", xg, yb)
    for num in ("1", "5", "7", "10"):
        fil(p[num], (xg, p[num][1])); jonction(xg, p[num][1]); net("GND", Uref + "." + num)
    net("GND", Uref + ".8")
    nc(*p["6"]); NC.append(Uref + ".6")
    texte("Chaque condensateur contre SA broche (br.),\na moins de 2 mm", 111.76, oy + 5.08, 1.1)
    # ---- côté isolé ----
    x12, y12 = p["12"]; x17, y17 = p["17"]
    fil((x12, y12), (187.96, y12), (187.96, RV), (292.1, RV)); plus(V("VISO"), 187.96, RV)
    fil((x17, y17), (193.04, y17), (193.04, RV)); jonction(193.04, RV)
    net(V("VISO"), Uref + ".12", Uref + ".17")
    # diviseur de réglage : R106 (haut), R105 (bas, couchée)
    x13, y13 = p["13"]
    xd = 248.92
    a, b = vertical("R", R(6), "10k", xd, RV); jonction(*a)
    fil(b, (xd, y13)); fil((x13, y13), (xd, y13)); jonction(xd, y13)
    etiq(V("VSEL"), 185.42, y13, "d")
    g, d = horizontal_R(R(5), "4k7", xd, y13); fil(d, (xd + 12.7, y13)); masse(V("GNDISO"), xd + 12.7, y13)
    net(V("VISO"), R(6) + ".1"); net(V("VSEL"), Uref + ".13", R(6) + ".2", R(5) + ".1"); net(V("GNDISO"), R(5) + ".2")
    # trois condensateurs sur le rail isolé. Les broches 12 (VISO) et 17 (VDDISO) sont reliées :
    # le 10 µF de VISO sert aussi VDDISO, qui ne demande que 0,01 à 0,1 µF (fiche ADM3260).
    # ⇒ pas de C108 : décision du 2026-11-24, le numéro reste libre.
    for i, x, val, br in ((5, 205.74, "100n", "br. 12"), (6, 218.44, "10u", "br. 12"), (7, 231.14, "100n", "br. 17")):
        i -= 5
        a, b = vertical("C", C(5 + i), val, x, RV); jonction(*a); masse(V("GNDISO"), *b); texte(br, x + 2.54, RV + 8.255, 0.9)
        net(V("VISO"), C(5+i) + ".1"); net(V("GNDISO"), C(5+i) + ".2")
    # module Atlas et rappels du bus isolé
    mx, my = 320.04, uy + 5.08
    m = custom("Atlas_OEM", Mref, module, mx, my, PRJ + ":Atlas_OEM")
    x18, y18 = p["18"]; x19, y19 = p["19"]
    fil((x18, y18), m["1"]); fil((x19, y19), m["8"])
    etiq(V("SDA_ISO"), 200.66, y18, "d"); etiq(V("SCL_ISO"), 228.6, y19, "d")
    a, b = vertical("R", R(3), "4k7", 281.94, RV); jonction(*a); fil(b, (281.94, y18)); jonction(281.94, y18)
    a, b = vertical("R", R(4), "4k7", 292.1, RV); fil(b, (292.1, y19)); jonction(292.1, y19)
    net(V("VISO"), R(3) + ".1", R(4) + ".1")
    net(V("SDA_ISO"), Uref + ".18", Mref + ".1", R(3) + ".2"); net(V("SCL_ISO"), Uref + ".19", Mref + ".8", R(4) + ".2")
    fil(m["3"], (299.72, m["3"][1]), (299.72, m["3"][1] - 5.08)); plus(V("VISO"), 299.72, m["3"][1] - 5.08); net(V("VISO"), Mref + ".3")
    fil(m["6"], (299.72, m["6"][1]), (299.72, m["6"][1] + 5.08)); masse(V("GNDISO"), 299.72, m["6"][1] + 5.08); net(V("GNDISO"), Mref + ".6")
    for num in ("7", "2", "9", "10"): nc(*m[num]); NC.append(Mref + "." + num)
    nc(*p["15"]); NC.append(Uref + ".15")
    # masses isolées de l'isolateur + drapeau
    xg2 = 187.96; yb2 = uy + 30.48
    fil(p["11"], (xg2, p["11"][1]), (xg2, yb2)); masse(V("GNDISO"), xg2, yb2)
    for num in ("14", "16", "20"): fil(p[num], (xg2, p[num][1])); jonction(xg2, p[num][1])
    for num in ("11", "14", "16", "20"): net(V("GNDISO"), Uref + "." + num)
    fil((xg2, yb2), (198.12, yb2)); jonction(xg2, yb2); drapeau(198.12, yb2)
    # prise BNC
    j = custom("BNC", Jref, "BNC coudee %s" % sfx, 381, uy, PRJ + ":BNC_Dosin_801-0080_Coudee", "C521219")
    fil(m["4"], j["1"]); fil(m["5"], j["2"])
    etiq(V("PRB"), 342.9, m["4"][1], "d")
    xj = 353.06; fil((xj, m["5"][1]), (xj, m["5"][1] + 10.16)); jonction(xj, m["5"][1]); masse(V("GND_SONDE"), xj, m["5"][1] + 10.16)
    fil((xj, m["5"][1] + 10.16), (xj + 10.16, m["5"][1] + 10.16)); drapeau(xj + 10.16, m["5"][1] + 10.16)
    net(V("PRB"), Mref + ".4", Jref + ".1"); net(V("GND_SONDE"), Mref + ".5", Jref + ".2")
    texte("PRB : LA piste sensible (~100 MOhm). 0,4 mm, la plus courte possible,\nentouree du plan GND_SONDE dessus et dessous. Aucun via sous le module.", 314.96, oy + 26.67, 1.1)
    texte("GND_SONDE et GNDISO : deux plans SEPARES,\naucun lien trace entre eux (Atlas, p. 33-34).\nCorps de la BNC = GND_SONDE :\nil ne touche aucun metal du boitier.", 345.44, oy + 78.74, 1.1)
    texte("INT et NC : en l'air", 307.34, oy + 71.12, 1.1)
    texte("Chaque condensateur contre SA broche (br.), a moins de 2 mm.   R%d06 + R%d05 : VISO = 1,23 V x (10k + 4k7) / 4k7 = 3,85 V" % (n, n), 193.04, oy + 5.08, 1.1)

voie("PH", 1, 20.32, "VOIE pH", "Atlas pH OEM")
voie("ORP", 2, 111.76, "VOIE ORP", "Atlas ORP OEM")

# ---------- liaison vers la carte principale ----------
cadre(15.24, 195.58, 294.64, 248.92)
texte("LIAISON VERS LA CARTE PRINCIPALE", 17.78, 201.93, 2.5, True)
j = custom("Conn_8", "J1", "Vers carte principale", 43.18, 223.52, FP["J1"], "C157972")
# Ordre des voies arrêté au routage (2026-11-24) : bus pH à gauche, alimentations au centre, bus ORP à droite.
for num, nom in (("1", "SDA_PH"), ("2", "SCL_PH"), ("7", "SDA_ORP"), ("8", "SCL_ORP")):
    fil(j[num], (60.96, j[num][1])); etiq(nom, 60.96, j[num][1], "d"); net(nom, "J1." + num)
# 1 SDA_PH, 2 SCL_PH, 3 GND, 4 +5V, 5 +3V3, 6 GND, 7 SDA_ORP, 8 SCL_ORP : une masse contre chaque paire de données
xg = 114.3
fil(j["3"], (124.46, j["3"][1])); drapeau(124.46, j["3"][1])
fil(j["6"], (xg, j["6"][1])); fil((xg, j["3"][1]), (xg, j["6"][1] + 10.16)); jonction(xg, j["3"][1]); jonction(xg, j["6"][1]); masse("GND", xg, j["6"][1] + 10.16)
fil(j["4"], (78.74, j["4"][1])); plus("+5V", 68.58, j["4"][1]); jonction(68.58, j["4"][1]); drapeau(78.74, j["4"][1])
fil(j["5"], (101.6, j["5"][1])); plus("+3V3", 91.44, j["5"][1]); jonction(91.44, j["5"][1]); drapeau(101.6, j["5"][1])
net("+5V", "J1.4"); net("+3V3", "J1.5"); net("GND", "J1.3", "J1.6")
texte("Les fils sont traces a l'interieur de chaque voie.\nSeuls les quatre signaux du bus rejoignent J1 par etiquette (meme nom = meme fil).\n"
      "Ordre des voies de J1 arrete au routage.\nLa carte principale reunit SDA_PH/SDA_ORP et SCL_PH/SCL_ORP en un seul bus.\n\n"
      "Carte en 4 couches. Zone interdite au cuivre de 5,5 mm, sur les 4 couches,\nsous chaque ADM3260 et entre les domaines. Aucun via sous les modules Atlas.\n"
      "Nettoyage du flux OBLIGATOIRE apres soudure : docs/FABRICATION.md", 142.24, 241.3, 1.3)

ecrire_fils()
lib_symbols = "\n".join(list(REPRIS.values()) + list(CUSTOM.values()))
sch = """(kicad_sch (version 20250114) (generator "eeschema") (generator_version "9.0") (uuid "%s") (paper "A3")
(title_block (title "Carte de mesure v3 - pH et ORP, une barriere par voie") (date "2026-11-24") (rev "0.7")
 (company "ESP32 Pool Controller") (comment 1 "Reference : kicad/v3-mesure/README.md"))
(lib_symbols
%s
)
%s
(sheet_instances (path "/" (page "1")))
(embedded_fonts no)
)
""" % (ROOT, lib_symbols, "\n".join(E))
open(os.path.join(OUT, PRJ + ".kicad_sch"), "w", encoding="utf-8").write(sch)
open(os.path.join(OUT, PRJ + ".kicad_sym"), "w", encoding="utf-8").write(
    '(kicad_symbol_lib (version 20241209) (generator "kicad_symbol_editor") (generator_version "9.0")\n%s\n)\n' % "\n".join(LIBTXT))
open(os.path.join(OUT, "sym-lib-table"), "w", encoding="utf-8").write(
    '(sym_lib_table\n  (version 7)\n  (lib (name "%s")(type "KiCad")(uri "${KIPRJMOD}/%s.kicad_sym")(options "")(descr "Symboles propres a la carte de mesure v3"))\n)\n' % (PRJ, PRJ))
pro = os.path.join(OUT, PRJ + ".kicad_pro")
if not os.path.exists(pro):
    json.dump({"meta": {"filename": PRJ + ".kicad_pro", "version": 3}, "sheets": [[ROOT, "Root"]], "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []}},
              open(pro, "w", encoding="utf-8"), indent=2)
if len(sys.argv) > 2:   # liste des connexions attendues, pour la comparer à celle que KiCad extrait
    json.dump({"nets": {k: sorted(v) for k, v in ATTENDU.items()}, "en_l_air": sorted(NC)}, open(sys.argv[2], "w"), indent=1)
print("ok", len(E), "elements,", len(ATTENDU), "nets attendus,", len(NC), "broches en l'air")
