#!/usr/bin/env python3
"""Écrit le schéma KiCad 9 de la carte de mesure v3 à composants courants depuis le
tableau des connexions (README.md de ce dossier). Signaux reliés par de VRAIS FILS ;
alimentations et masses par symboles d'alimentation ; les quatre signaux du bus rejoignent
J1 par étiquette. Chaque fil de signal porte une étiquette : c'est elle qui nomme le net.

Usage : python3 generer_schema.py <dossier de sortie> [fichier des connexions attendues .json]
⚠️ RÉÉCRIT le schéma en entier : ne plus le lancer après une modification à la main."""
import re, sys, uuid, json, os
ICI = os.path.dirname(os.path.abspath(__file__))
V2 = os.path.join(ICI, "..", "esp32-pool-controller", "esp32-pool-controller.kicad_sch")
if not os.path.exists(V2): V2 = "/root/esp32-pool-controller/kicad/esp32-pool-controller/esp32-pool-controller.kicad_sch"
OUT = sys.argv[1]
PRJ = "v3-mesure-std"
ROOT = "7c1f6a52-3e0b-4d6e-9a41-b2c8d5e7f013"   # fixe : le projet et la carte y font référence
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
 "DCDC_SIP4": ("PS", "Module d'alimentation isolee 5 V vers 9 V, 1 W, SIP-4", 15.24, 7.62, -7.62,
   [("2","+Vin","power_in",2.54),("1","-Vin","power_in",-2.54)],
   [("4","+Vo","power_out",2.54),("3","0V","power_out",-2.54)]),
 "ISO1540": ("U", "Isolateur I2C bidirectionnel, SOIC-8", 20.32, 10.16, -12.7,
   [("8","VCC2","power_in",5.08),("7","SDA2","bidirectional",0),("6","SCL2","bidirectional",-2.54),("5","GND2","power_in",-7.62)],
   [("1","VCC1","power_in",5.08),("2","SDA1","bidirectional",0),("3","SCL1","bidirectional",-2.54),("4","GND1","power_in",-7.62)]),
 "REG_1117": ("U", "Regulateur 5 V fixe, famille 1117, SOT-223", 15.24, 7.62, -7.62,
   [("3","IN","power_in",2.54),("1","GND","power_in",-2.54)],
   [("2","OUT","power_out",2.54)]),
 "ADS1115": ("U", "Convertisseur 16 bits I2C, entrees differentielles", 20.32, 12.7, -17.78,
   [("8","VDD","power_in",7.62),("9","SDA","bidirectional",2.54),("10","SCL","input",0),("1","ADDR","input",-5.08),("3","GND","power_in",-10.16)],
   [("4","AIN0","input",5.08),("5","AIN1","input",0),("6","AIN2","input",-5.08),("7","AIN3","input",-7.62),("2","ALERT","open_collector",-10.16)]),
 "AMPLI_ENTREE": ("U", "Amplificateur a tres faible courant d'entree avec sortie de garde (OPA928 ou ADA4530-1), SOIC-8", 20.32, 10.16, -12.7,
   [("5","V+","power_in",7.62),("6","OUT","output",2.54),("8","IN-","input",-2.54),("4","V-","power_in",-10.16)],
   [("1","IN+","input",2.54),("2","GRD","passive",-2.54),("7","GRD","passive",-5.08),("3","NE_PAS_RELIER","no_connect",-10.16)]),
 "AMPLI_DOUBLE": ("U", "Amplificateur double CMOS (LMC6482), SOIC-8", 20.32, 12.7, -12.7,
   [("8","V+","power_in",10.16),("1","OUT_A","output",5.08),("2","IN-_A","input",2.54),("7","OUT_B","output",-2.54),("6","IN-_B","input",-5.08),("4","V-","power_in",-10.16)],
   [("3","IN+_A","input",5.08),("5","IN+_B","input",-2.54)]),
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
FP = {"R": "Resistor_SMD:R_0603_1608Metric", "R08": "Resistor_SMD:R_0805_2012Metric",
      "C01": "Capacitor_SMD:C_0402_1005Metric", "C08": "Capacitor_SMD:C_0805_2012Metric", "C12": "Capacitor_SMD:C_1206_3216Metric",
      "CT": "Capacitor_Tantalum_SMD:CP_EIA-3528-21_Kemet-B",
      "SO8": "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "ADS": "Package_SO:VSSOP-10_3x3mm_P0.5mm",
      "REG": "Package_TO_SOT_SMD:SOT-223-3_TabPin2", "J1": "Connector_JST:JST_XH_B8B-XH-A_1x08_P2.50mm_Vertical"}
# valeur -> (empreinte, référence au catalogue). Références relues le 2026-11-25 ; "" = à relever.
PASSIF = {"100n": ("C01", "C1525"), "4u7": ("C08", "C1779"), "10u 50V": ("C12", "C13585"), "22u tantale": ("CT", ""),
          "4k7": ("R", "C23162"), "3k3": ("R", "C22978"), "10k": ("R", "C25804"), "30k": ("R", "C22984"),
          "100R": ("R", "C22775"), "2M2": ("R08", "C26113")}
def custom(nom, ref, val, x, y, fp="", lcsc=""):
    place(PRJ + ":" + nom, ref, val, x, y, CPINS[nom].keys(), fp, lcsc,
          refpos=(-DEF[nom][2]/2, -DEF[nom][3]-1.27), valpos=(-DEF[nom][2]/2, -DEF[nom][4]+1.27))
    d = {num: (x + px, y - py) for num, (px, py) in CPINS[nom].items()}
    for q in d.values(): P(*q)
    return d
def vertical(kind, ref, val, x, ytop):
    """Résistance ou condensateur vertical : broche 1 en (x, ytop), broche 2 en (x, ytop+7.62)."""
    fp, lcsc = PASSIF[val]
    place("Device:" + kind, ref, val, x, ytop + 3.81, ["1", "2"], FP[fp], lcsc)
    P(x, ytop); P(x, ytop + 7.62)
    return (x, ytop), (x, ytop + 7.62)
def horizontal_R(ref, val, xg, y):
    """Résistance couchée entre (xg, y) et (xg+7.62, y) : broche 1 à gauche... voir rot=90."""
    fp, lcsc = PASSIF[val]
    place("Device:R", ref, val, xg + 3.81, y, ["1", "2"], FP[fp], lcsc, rot=90, refpos=(0, -2.54), valpos=(0, 2.54))
    P(xg, y); P(xg + 7.62, y)
    return (xg, y), (xg + 7.62, y)


def voie(sfx, n, oy, titre):
    PH = (sfx == "PH")
    V = lambda s: "%s_%s" % (s, sfx)
    C = lambda i: "C%d%02d" % (n, i)
    R = lambda i: "R%d%02d" % (n, i)
    PS, UI, UR, UA, UE, UD, J = "PS%d01" % n, "U%d01" % n, "U%d02" % n, "U%d03" % n, "U%d04" % n, "U%d05" % n, "J%d01" % n
    GI, VI = V("GNDISO"), V("VISO")
    by = oy + 60.96                  # ligne des signaux
    RV = by - 17.78                  # rail des découplages
    BX = 109.22                      # barrière
    cadre(15.24, oy - 7.62, 403.86, oy + 162.56)
    texte(titre, 17.78, oy - 1.27, 2.5, True)
    texte("COTE ESP32 (non isole)", 17.78, oy + 5.08, 1.5, True)
    texte("COTE ISOLE - voie %s : rien n'est partage avec l'autre voie" % sfx, 299.72, oy - 1.27, 1.5, True)
    E.append('(polyline (pts (xy %s %s) (xy %s %s)) (stroke (width 0.5) (type dash_dot)) (uuid "%s"))' % (r(BX), r(oy - 5.08), r(BX), r(oy + 160.02), U()))
    texte("BARRIERE D'ISOLATION : aucun cuivre ne la franchit, sur aucune couche", 111.76, oy - 2.54, 1.1)

    # ---- alimentation isolée : module 5 V -> 9 V, puis régulateur 5 V ----
    ya = oy + 17.78
    p = custom("DCDC_SIP4", PS, "B0509S-1WR3", BX, oy + 20.32, "", "C5369463")
    fil(p["2"], (81.28, ya)); plus("+5V", 81.28, ya)
    a, b = vertical("C", C(1), "4u7", 88.9, ya); jonction(*a); masse("GND", *b)
    fil(p["1"], (93.98, p["1"][1]), (93.98, oy + 27.94)); masse("GND", 93.98, oy + 27.94)
    net("+5V", PS + ".2", C(1) + ".1"); net("GND", PS + ".1", C(1) + ".2")
    q = custom("REG_1117", UR, "AMS1117-5.0", 160.02, oy + 20.32, FP["REG"], "C6187")
    fil(p["4"], q["3"]); etiq(V("VBRUT"), 121.92, ya, "d")
    a, b = vertical("C", C(3), "10u 50V", 134.62, ya); jonction(*a); masse(GI, *b)
    fil(p["3"], (124.46, p["3"][1]), (124.46, oy + 27.94)); masse(GI, 124.46, oy + 27.94)
    fil(q["1"], (147.32, q["1"][1]), (147.32, oy + 27.94)); masse(GI, 147.32, oy + 27.94)
    fil(q["2"], (187.96, ya)); plus(VI, 187.96, ya)
    a, b = vertical("C", C(4), "22u tantale", 177.8, ya); jonction(*a); masse(GI, *b)
    texte("+", 175.26, ya + 2.54, 1.5, True)
    net(V("VBRUT"), PS + ".4", UR + ".3", C(3) + ".1"); net(GI, PS + ".3", UR + ".1", C(3) + ".2", C(4) + ".2")
    net(VI, UR + ".2", C(4) + ".1")
    texte("Module non regule : ~9 a 10 V sur VBRUT.\nLe regulateur lui sert aussi de charge (5 mA).\nC%d04 : tantale, le + cote VISO." % n, 195.58, oy + 22.86, 1.1)

    # ---- isolateur de signaux ----
    u = custom("ISO1540", UI, "ISO1540DR", BX, by, FP["SO8"], "C179739")
    #   côté ESP32 (côté 2)
    x, y = u["8"]; fil((x, y), (91.44, y), (91.44, RV), (81.28, RV)); plus("+3V3", 91.44, RV); jonction(91.44, RV)
    a, b = vertical("C", C(5), "100n", 81.28, RV); masse("GND", *b)
    x, y = u["5"]; fil((x, y), (91.44, y), (91.44, y + 5.08)); masse("GND", 91.44, y + 5.08)
    ys, yc = u["7"][1], u["6"][1]
    fil(u["7"], (45.72, ys)); etiq(V("SDA"), 45.72, ys, "g")
    fil(u["6"], (45.72, yc)); etiq(V("SCL"), 45.72, yc, "g")
    a, b = vertical("R", R(1), "4k7", 68.58, RV); plus("+3V3", *a); fil(b, (68.58, ys)); jonction(68.58, ys)
    a, b = vertical("R", R(2), "4k7", 58.42, RV); plus("+3V3", *a); fil(b, (58.42, yc)); jonction(58.42, yc)
    net("+3V3", UI + ".8", C(5) + ".1", R(1) + ".1", R(2) + ".1"); net("GND", UI + ".5", C(5) + ".2")
    net(V("SDA"), UI + ".7", R(1) + ".2"); net(V("SCL"), UI + ".6", R(2) + ".2")
    texte("Cote 2 de l'isolateur : vers le bus de l'ESP32", 45.72, by + 12.7, 1.1)
    #   côté isolé (côté 1)
    x, y = u["1"]; fil((x, y), (127, y), (127, RV), (137.16, RV)); plus(VI, 127, RV); jonction(127, RV)
    a, b = vertical("C", C(6), "100n", 137.16, RV); masse(GI, *b)
    x, y = u["4"]; fil((x, y), (127, y), (127, y + 5.08)); masse(GI, 127, y + 5.08)
    net(VI, UI + ".1", C(6) + ".1"); net(GI, UI + ".4", C(6) + ".2")

    # ---- convertisseur ----
    c = custom("ADS1115", UA, "ADS1115IDGSR", 215.9, by + 2.54, FP["ADS"], "C37593")
    fil(u["2"], c["9"]); fil(u["3"], c["10"])
    etiq(V("SDA_ISO"), 142.24, ys, "d"); etiq(V("SCL_ISO"), 170.18, yc, "d")
    a, b = vertical("R", R(3), "3k3", 154.94, RV); plus(VI, *a); fil(b, (154.94, ys)); jonction(154.94, ys)
    a, b = vertical("R", R(4), "3k3", 165.1, RV); plus(VI, *a); fil(b, (165.1, yc)); jonction(165.1, yc)
    net(VI, R(3) + ".1", R(4) + ".1")
    net(V("SDA_ISO"), UI + ".2", UA + ".9", R(3) + ".2"); net(V("SCL_ISO"), UI + ".3", UA + ".10", R(4) + ".2")
    x, y = c["8"]; fil((x, y), (198.12, y), (198.12, RV), (187.96, RV)); plus(VI, 198.12, RV); jonction(198.12, RV)
    a, b = vertical("C", C(7), "100n", 187.96, RV); masse(GI, *b)
    x, y = c["3"]; fil((x, y), (195.58, y), (195.58, y + 5.08)); masse(GI, 195.58, y + 5.08)
    net(VI, UA + ".8", C(7) + ".1"); net(GI, UA + ".3", C(7) + ".2")
    xa, ya_ = c["1"]
    if PH:
        fil((xa, ya_), (195.58, ya_), (195.58, c["3"][1])); jonction(195.58, c["3"][1]); net(GI, UA + ".1")
        texte("ADDR a la masse : adresse 0x48", 200.66, by + 27.94, 1.1)
    else:
        fil((xa, ya_), (190.5, ya_)); plus(VI, 190.5, ya_); net(VI, UA + ".1")
        texte("ADDR au + : adresse 0x49", 200.66, by + 27.94, 1.1)
    for num in ("6", "7", "2"): nc(*c[num]); NC.append(UA + "." + num)
    xm, ym = c["4"]; xr, yr = c["5"]
    YB = by + 93.98                  # retour de REF, en bas de la voie
    XR = 236.22
    fil((xr, yr), (XR, yr), (XR, YB), (363.22, YB)); etiq(V("REF"), 243.84, YB, "d")

    # ---- amplificateur double : Vcm (et entrée de l'ORP) ----
    d = custom("AMPLI_DOUBLE", UD, "LMC6482AIMX", 289.56, by + 53.34, FP["SO8"], "C12620")
    RD = d["8"][1] - 15.24
    x, y = d["8"]; fil((x, y), (271.78, y), (271.78, RD), (261.62, RD)); plus(VI, 271.78, RD); jonction(271.78, RD)
    a, b = vertical("C", C(9), "100n", 261.62, RD); masse(GI, *b)
    x, y = d["4"]; fil((x, y), (271.78, y), (271.78, y + 5.08)); masse(GI, 271.78, y + 5.08)
    net(VI, UD + ".8", C(9) + ".1"); net(GI, UD + ".4", C(9) + ".2")
    yA, yB = d["1"][1], d["7"][1]
    # rebouclages en suiveur
    fil(d["2"], (269.24, d["2"][1]), (269.24, yA)); jonction(269.24, yA)
    fil(d["6"], (269.24, d["6"][1]), (269.24, yB)); jonction(269.24, yB)
    yv = yA if PH else yB           # sortie qui porte Vcm
    fil(d["1" if PH else "7"], (248.92, yv)); etiq(V("VCM"), 251.46, yv, "d")
    g, dr = horizontal_R(R(8), "100R", 241.3, yv); fil(g, (XR, yv)); jonction(XR, yv)
    vcm_out, vcm_inm, vcm_inp = ("1", "2", "3") if PH else ("7", "6", "5")
    net(V("VCM"), UD + "." + vcm_out, UD + "." + vcm_inm, R(8) + ".2")
    # pont de Vcm
    xp = 314.96 if PH else 337.82
    yp = yA if PH else by + 71.12
    x, y = d[vcm_inp]
    if PH:
        fil((x, y), (xp + 20.32, yp)); etiq(V("VCM_REF"), xp + 20.32, yp, "d")
        fil(d["5"], (307.34, d["5"][1]), (307.34, yp)); jonction(307.34, yp)      # moitié B inutilisée : suiveur sur Vcm
        fil(d["7"], (266.7, yB)); etiq(V("LIBRE"), 266.7, yB, "g")
        net(V("LIBRE"), UD + ".6", UD + ".7"); net(V("VCM_REF"), UD + ".5")
        texte("Moitie B inutilisee :\nmontee en suiveur sur Vcm", 299.72, by + 76.2, 1.1)
    else:
        fil((x, y), (307.34, y), (307.34, yp), (xp + 15.24, yp)); etiq(V("VCM_REF"), 332.74, yp, "g")
    a, b = vertical("R", R(6), "30k" if PH else "10k", xp, yp - 12.7); plus(VI, *a); fil(b, (xp, yp)); jonction(xp, yp)
    a, b = vertical("R", R(7), "10k", xp, yp + 5.08); fil((xp, yp), a); masse(GI, *b)
    a, b = vertical("C", C(10), "100n", xp + 15.24, yp); masse(GI, *b)
    if PH: jonction(xp + 15.24, yp)
    net(V("VCM_REF"), UD + "." + vcm_inp, R(6) + ".2", R(7) + ".1", C(10) + ".1"); net(VI, R(6) + ".1"); net(GI, R(7) + ".2", C(10) + ".2")
    texte("Vcm = %s" % ("1,25 V (30k / 10k)" if PH else "2,5 V (10k / 10k)"), xp + 27.94, yp + 10.16, 1.1)

    # ---- entrée de la sonde ----
    if PH:
        e = custom("AMPLI_ENTREE", UE, "OPA928DR", 289.56, by, FP["SO8"], "C32418779")
        x, y = e["5"]; fil((x, y), (271.78, y), (271.78, RV), (261.62, RV)); plus(VI, 271.78, RV); jonction(271.78, RV)
        a, b = vertical("C", C(8), "100n", 261.62, RV); masse(GI, *b)
        x, y = e["4"]; fil((x, y), (271.78, y), (271.78, y + 5.08)); masse(GI, 271.78, y + 5.08)
        net(VI, UE + ".5", C(8) + ".1"); net(GI, UE + ".4", C(8) + ".2")
        fil((xm, ym), e["6"]); etiq(V("MESURE"), 231.14, ym, "d")
        fil(e["8"], (269.24, e["8"][1]), (269.24, ym)); jonction(269.24, ym)
        net(V("MESURE"), UA + ".4", UE + ".6", UE + ".8")
        fil(e["2"], (309.88, e["2"][1]), (309.88, e["7"][1]), e["7"]); etiq(V("GARDE"), 309.88, e["2"][1], "d")
        net(V("GARDE"), UE + ".2", UE + ".7")
        nc(*e["3"]); NC.append(UE + ".3")
        xin, yin = e["1"]; jy = by
        texte("ENTREE : LE point sensible (~100 MOhm et plus).\nPiste la plus courte possible, entouree de l'anneau GARDE\nsur les deux faces, vernis retire. Rien d'autre sur ce fil.", 304.8, by - 12.7, 1.1)
        texte("Meme empreinte pour l'ADA4530-1 (autre fabricant) :\nrien d'autre a changer.", 279.4, by + 22.86, 1.1)
    else:
        fil((xm, ym), (251.46, ym), (251.46, yA), d["1"]); etiq(V("MESURE"), 231.14, ym, "d")
        net(V("MESURE"), UA + ".4", UD + ".1", UD + ".2")
        xin, yin = d["3"]; jy = yin + 2.54
        texte("Anneau de cuivre relie a MESURE autour de la broche 3 et de R%d05" % n, 304.8, yin - 7.62, 1.1)
    g, dr = horizontal_R(R(5), "2M2", 320.04, yin); fil((xin, yin), g); etiq(V("ENTREE"), 304.8, yin, "d")
    j = custom("BNC", J, "BNC coudee %s" % sfx, 381, jy, "", "C521219")
    fil(dr, j["1"]); etiq(V("AME"), 335.28, yin, "d")
    fil(j["2"], (363.22, j["2"][1]), (363.22, YB))
    net(V("ENTREE"), UE + ".1" if PH else UD + ".3", R(5) + ".1"); net(V("AME"), R(5) + ".2", J + ".1")
    net(V("REF"), UA + ".5", R(8) + ".1", J + ".2")
    texte("Corps de la BNC = REF (Vcm a travers 100 Ohm), PAS la masse :\nil ne touche aucun metal du boitier.\nAucun condensateur ni diode sur l'entree.", 142.24, by + 60.96, 1.1)

voie("PH", 1, 20.32, "VOIE pH")
voie("ORP", 2, 198.12, "VOIE ORP")

# ---------- liaison vers la carte principale ----------
JX, JY = 421.64, 20.32
cadre(JX, JY - 7.62, 576.58, JY + 99.06)
texte("LIAISON VERS LA CARTE PRINCIPALE", JX + 2.54, JY - 1.27, 2.5, True)
j = custom("Conn_8", "J1", "Vers carte principale", JX + 27.94, JY + 25.4, FP["J1"], "C157972")
for num, nom in (("1", "SDA_PH"), ("2", "SCL_PH"), ("7", "SDA_ORP"), ("8", "SCL_ORP")):
    fil(j[num], (JX + 45.72, j[num][1])); etiq(nom, JX + 45.72, j[num][1], "d"); net(nom, "J1." + num)
xg = JX + 99.06
fil(j["3"], (JX + 109.22, j["3"][1])); drapeau(JX + 109.22, j["3"][1])
fil(j["6"], (xg, j["6"][1])); fil((xg, j["3"][1]), (xg, j["6"][1] + 10.16)); jonction(xg, j["3"][1]); jonction(xg, j["6"][1]); masse("GND", xg, j["6"][1] + 10.16)
fil(j["4"], (JX + 63.5, j["4"][1])); plus("+5V", JX + 53.34, j["4"][1]); jonction(JX + 53.34, j["4"][1]); drapeau(JX + 63.5, j["4"][1])
fil(j["5"], (JX + 86.36, j["5"][1])); plus("+3V3", JX + 76.2, j["5"][1]); jonction(JX + 76.2, j["5"][1]); drapeau(JX + 86.36, j["5"][1])
net("+5V", "J1.4"); net("+3V3", "J1.5"); net("GND", "J1.3", "J1.6")
texte("Meme connecteur et meme ordre que la carte a modules Atlas.\nLa carte principale reunit SDA_PH/SDA_ORP et SCL_PH/SCL_ORP en un seul bus :\n"
      "convertisseur du pH en 0x48, de l'ORP en 0x49.\n\nCarte en 4 couches. Aucun cuivre ne franchit une barriere, ni entre les deux voies.\n"
      "Nettoyage du flux OBLIGATOIRE apres soudure : docs/FABRICATION.md\n\nReference : kicad/v3-mesure-std/README.md", JX + 2.54, JY + 93.98, 1.3)

ecrire_fils()
lib_symbols = "\n".join(list(REPRIS.values()) + list(CUSTOM.values()))
sch = """(kicad_sch (version 20250114) (generator "eeschema") (generator_version "9.0") (uuid "%s") (paper "A2")
(title_block (title "Carte de mesure v3 a composants courants - pH et ORP, une barriere par voie") (date "2026-11-25") (rev "0.1")
 (company "ESP32 Pool Controller") (comment 1 "Reference : kicad/v3-mesure-std/README.md"))
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
    '(sym_lib_table\n  (version 7)\n  (lib (name "%s")(type "KiCad")(uri "${KIPRJMOD}/%s.kicad_sym")(options "")(descr "Symboles propres a la carte de mesure v3 a composants courants"))\n)\n' % (PRJ, PRJ))
pro = os.path.join(OUT, PRJ + ".kicad_pro")
if not os.path.exists(pro):
    json.dump({"meta": {"filename": PRJ + ".kicad_pro", "version": 3}, "sheets": [[ROOT, "Root"]], "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []}},
              open(pro, "w", encoding="utf-8"), indent=2)
if len(sys.argv) > 2:   # liste des connexions attendues, pour la comparer à celle que KiCad extrait
    json.dump({"nets": {k: sorted(v) for k, v in ATTENDU.items()}, "en_l_air": sorted(NC)}, open(sys.argv[2], "w"), indent=1)
print("ok", len(E), "elements,", len(ATTENDU), "nets attendus,", len(NC), "broches en l'air")
