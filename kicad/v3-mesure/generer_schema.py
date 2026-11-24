#!/usr/bin/env python3
"""Génère le schéma KiCad 9 de la carte de mesure v3 depuis le tableau des connexions.
Sortie : v3-mesure.kicad_sch, v3-mesure.kicad_sym, sym-lib-table, v3-mesure.kicad_pro.
Le schéma relie par ÉTIQUETTES (même nom = même fil), pas par longs fils."""
import re, sys, uuid, json, os
V2 = "/root/esp32-pool-controller/kicad/esp32-pool-controller/esp32-pool-controller.kicad_sch"
OUT = sys.argv[1]
PRJ = "v3-mesure"
ROOT = str(uuid.uuid4())
def U(): return str(uuid.uuid4())
FONT = "(effects (font (size 1.27 1.27)))"
def r(v): return ("%.4f" % v).rstrip("0").rstrip(".")

# ---------- symboles repris du v2 (lecture seule) ----------
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
def broches(txt):
    d = {}
    for m in re.finditer(r'\(pin\s+\w+\s+\w+\s*\(at\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\)', txt):
        k = txt.index('(number "', m.end()); num = txt[k+9:txt.index('"', k+9)]
        d[num] = (float(m.group(1)), float(m.group(2)))
    return d
REPRIS = {n: bloc(n) for n in ("Device:R", "Device:C", "power:PWR_FLAG")}
PINS = {n: broches(t) for n, t in REPRIS.items()}

# ---------- symboles neufs ----------
def sym_custom(nom, ref, desc, w, top, bot, gauche, droite, lib_prefix):
    """gauche/droite : listes (numéro, nom, type, y). Corps de -w/2..w/2, top..bot."""
    full = (PRJ + ":" + nom) if lib_prefix else nom
    L = ['(symbol "%s" (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)' % full,
         '(property "Reference" "%s" (at %s %s 0) %s)' % (ref, r(-w/2), r(top+1.27), "(effects (font (size 1.27 1.27)) (justify left))"),
         '(property "Value" "%s" (at %s %s 0) %s)' % (nom, r(-w/2), r(bot-1.27), "(effects (font (size 1.27 1.27)) (justify left))"),
         '(property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))',
         '(property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))',
         '(property "Description" "%s" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))' % desc,
         '(symbol "%s_0_1" (rectangle (start %s %s) (end %s %s) (stroke (width 0.254) (type default)) (fill (type background))))' % (nom, r(-w/2), r(top), r(w/2), r(bot)),
         '(symbol "%s_1_1"' % nom]
    pins = {}
    for cote, liste in (("g", gauche), ("d", droite)):
        for num, nm, typ, y in liste:
            x = -w/2 - 2.54 if cote == "g" else w/2 + 2.54
            ang = 0 if cote == "g" else 180
            L.append('(pin %s line (at %s %s %d) (length 2.54) (name "%s" %s) (number "%s" %s))' % (typ, r(x), r(y), ang, nm, FONT, num, FONT))
            pins[num] = (x, y, cote)
    L.append(') (embedded_fonts no))')
    return "\n".join(L), pins

DEF = {
 "ADM3260": ("U", "Isolateur I2C double avec convertisseur isole, SSOP-20", 25.4, 20.32, -22.86,
   [("9","VIN","power_in",17.78),("4","VDDP","power_in",12.7),("3","SDA2","bidirectional",5.08),("2","SCL2","bidirectional",2.54),
    ("8","PDIS","input",-2.54),("6","NC","no_connect",-7.62),
    ("1","GNDP","power_in",-12.7),("5","GNDP","power_in",-15.24),("7","GNDP","power_in",-17.78),("10","GNDP","power_in",-20.32)],
   [("12","VISO","power_out",17.78),("17","VDDISO","power_in",12.7),("13","VSEL","input",7.62),("18","SDA1","bidirectional",2.54),("19","SCL1","bidirectional",0),
    ("15","NC","no_connect",-7.62),
    ("11","GNDISO","power_in",-12.7),("14","GNDISO","power_in",-15.24),("16","GNDISO","power_in",-17.78),("20","GNDISO","power_in",-20.32)]),
 "Atlas_OEM": ("U", "Module Atlas Scientific OEM (pH ou ORP), 10 plots", 25.4, 12.7, -15.24,
   [("3","VCC","power_in",7.62),("1","SDA","bidirectional",2.54),("8","SCL","bidirectional",0),("7","INT","output",-5.08),("6","GND","power_in",-10.16)],
   [("4","PRB","passive",7.62),("5","PRB_GND","passive",2.54),("2","NC","no_connect",-5.08),("9","NC","no_connect",-7.62),("10","NC","no_connect",-10.16)]),
 "BNC": ("J", "Prise BNC femelle pour carte : ame et corps", 12.7, 7.62, -7.62,
   [("1","AME","passive",2.54),("2","CORPS","passive",-2.54)], []),
 "Conn_8": ("J", "Connecteur 8 voies vers la carte principale (JST XH)", 10.16, 12.7, -12.7,
   [], [(str(i+1), "V%d" % (i+1), "passive", 8.89 - 2.54*i) for i in range(8)]),
}
CUSTOM, CPINS, LIBTXT = {}, {}, []
for nom, (ref, desc, w, top, bot, g, d) in DEF.items():
    CUSTOM[nom], CPINS[nom] = sym_custom(nom, ref, desc, w, top, bot, g, d, True)
    LIBTXT.append(sym_custom(nom, ref, desc, w, top, bot, g, d, False)[0])

# ---------- éléments du schéma ----------
E = []          # éléments
def prop(nom, val, x, y, cache=False, just=None):
    eff = "(effects (font (size 1.27 1.27))%s%s)" % (" (justify %s)" % just if just else "", " (hide yes)" if cache else "")
    return '(property "%s" "%s" (at %s %s 0) %s)' % (nom, val, r(x), r(y), eff)
def place(lib_id, ref, val, x, y, pins, fp="", lcsc="", refpos=(2.54, -1.27), valpos=(2.54, 1.27), montre_val=True):
    L = ['(symbol (lib_id "%s") (at %s %s 0) (unit 1) (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no) (uuid "%s")' % (lib_id, r(x), r(y), U()),
         prop("Reference", ref, x+refpos[0], y+refpos[1], just="left"),
         prop("Value", val, x+valpos[0], y+valpos[1], cache=not montre_val, just="left"),
         prop("Footprint", fp, x, y, cache=True), prop("Datasheet", "", x, y, cache=True)]
    if lcsc: L.append(prop("LCSC", lcsc, x, y, cache=True))
    for n in pins: L.append('(pin "%s" (uuid "%s"))' % (n, U()))
    L.append('(instances (project "%s" (path "/%s" (reference "%s") (unit 1)))))' % (PRJ, ROOT, ref))
    E.append("\n".join(L))
def fil(x1, y1, x2, y2):
    E.append('(wire (pts (xy %s %s) (xy %s %s)) (stroke (width 0) (type default)) (uuid "%s"))' % (r(x1), r(y1), r(x2), r(y2), U()))
def etiq(nom, x, y, vers):
    rot, just = {"d": (0, "left bottom"), "g": (180, "right bottom"), "h": (90, "left bottom"), "b": (270, "right bottom")}[vers]
    E.append('(label "%s" (at %s %s %d) (effects (font (size 1.27 1.27)) (justify %s)) (uuid "%s"))' % (nom, r(x), r(y), rot, just, U()))
def nc(x, y): E.append('(no_connect (at %s %s) (uuid "%s"))' % (r(x), r(y), U()))
def texte(t, x, y, taille=1.27, gras=False):
    t = t.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    E.append('(text "%s" (exclude_from_sim no) (at %s %s 0) (effects (font (size %s %s)%s) (justify left bottom)) (uuid "%s"))' % (t, r(x), r(y), r(taille), r(taille), " (bold yes)" if gras else "", U()))
def cadre(x1, y1, x2, y2):
    E.append('(rectangle (start %s %s) (end %s %s) (stroke (width 0.2) (type dash)) (fill (type none)) (uuid "%s"))' % (r(x1), r(y1), r(x2), r(y2), U()))

FP = {"R": "Resistor_SMD:R_0603_1608Metric", "C01": "Capacitor_SMD:C_0402_1005Metric", "C10": "Capacitor_SMD:C_0805_2012Metric",
      "U": "Package_SO:SSOP-20_5.3x7.2mm_P0.65mm", "J1": "Connector_JST:JST_XH_B8B-XH-A_1x08_P2.50mm_Vertical"}
STUB = 7.62
def custom(nom, ref, val, x, y, nets, fp="", lcsc=""):
    """nets : numéro de broche -> nom de net, ou None pour 'rien'."""
    place(PRJ + ":" + nom, ref, val, x, y, CPINS[nom].keys(), fp, lcsc,
          refpos=(-DEF[nom][2]/2, -DEF[nom][3]-1.27), valpos=(-DEF[nom][2]/2, -DEF[nom][4]+2.54))
    for num, (px, py, cote) in CPINS[nom].items():
        ax, ay = x + px, y - py
        net = nets[num]
        if net is None: nc(ax, ay); continue
        bx = ax - STUB if cote == "g" else ax + STUB
        fil(ax, ay, bx, ay); etiq(net, bx, ay, "g" if cote == "g" else "d")
def passif(kind, ref, val, x, y, haut, bas, fp, lcsc):
    lib = "Device:R" if kind == "R" else "Device:C"
    p = PINS[lib]
    place(lib, ref, val, x, y, p.keys(), fp, lcsc)
    (x1, y1), (x2, y2) = p["1"], p["2"]
    a1, a2 = (x + x1, y - y1), (x + x2, y - y2)          # broche 1 en haut, 2 en bas
    for (ax, ay), net, sens in ((a1, haut, -1), (a2, bas, 1)):
        fil(ax, ay, ax, ay + sens*2.54); etiq(net, ax, ay + sens*2.54, "h" if sens < 0 else "b")
def drapeau(net, x, y):
    p = PINS["power:PWR_FLAG"]["1"]
    place("power:PWR_FLAG", "#FLG%02d" % drapeau.n, "PWR_FLAG", x, y, ["1"], refpos=(0, -5.08), valpos=(0, -3.81), montre_val=False)
    drapeau.n += 1
    ax, ay = x + p[0], y - p[1]
    fil(ax, ay, ax, ay + 5.08); etiq(net, ax, ay + 5.08, "b")
drapeau.n = 1

def voie(sfx, n, oy, titre, module):
    V = lambda s: "%s_%s" % (s, sfx)
    cadre(15.24, oy - 7.62, 403.86, oy + 109.22)
    texte(titre, 17.78, oy - 1.27, 2.5, True)
    texte("COTE ESP32 (non isole)", 17.78, oy + 7.62, 1.6, True)
    texte("COTE ISOLE - voie %s : rien n'est partage avec l'autre voie" % sfx, 215.9, oy + 7.62, 1.6, True)
    # barrière
    E.append('(polyline (pts (xy 165.1 %s) (xy 165.1 %s)) (stroke (width 0.5) (type dash_dot)) (uuid "%s"))' % (r(oy + 2.54), r(oy + 106.68), U()))
    texte("BARRIERE D'ISOLATION\naucun cuivre ne la franchit,\nsur aucune couche (5,5 mm)", 132.08, oy + 16.51, 1.1)
    ux, uy = 165.1, oy + 43.18
    custom("ADM3260", "U%d01" % n, "ADM3260ARSZ", ux, uy, {
        "9": "+5V", "4": "+3V3", "3": V("SDA"), "2": V("SCL"), "8": "GND", "6": None,
        "1": "GND", "5": "GND", "7": "GND", "10": "GND",
        "12": V("VISO"), "17": V("VISO"), "13": V("VSEL"), "18": V("SDA_ISO"), "19": V("SCL_ISO"), "15": None,
        "11": V("GNDISO"), "14": V("GNDISO"), "16": V("GNDISO"), "20": V("GNDISO")}, FP["U"], "C208558")
    mx = 276.86
    custom("Atlas_OEM", "U%d02" % n, module, mx, uy, {
        "3": V("VISO"), "1": V("SDA_ISO"), "8": V("SCL_ISO"), "7": None, "6": V("GNDISO"),
        "4": V("PRB"), "5": V("GND_SONDE"), "2": None, "9": None, "10": None})
    custom("BNC", "J%d01" % n, "BNC sonde %s" % sfx, 359.41, uy, {"1": V("PRB"), "2": V("GND_SONDE")}, lcsc="C3174924")
    texte("PRB : LA piste sensible (~100 MOhm).\n0,4 mm, la plus courte possible,\nentouree du plan GND_SONDE dessus et dessous.\nAucun via sous le module.", 300.99, oy + 22.86, 1.1)
    texte("GND_SONDE et GNDISO : deux plans SEPARES,\naucun lien trace entre eux (Atlas, p. 33-34).\nCorps de la BNC = GND_SONDE ;\nil ne touche aucun metal du boitier.", 345.44, oy + 80.01, 1.1)
    texte("INT et NC : en l'air", 264.16, oy + 66.04, 1.1)
    # passifs côté ESP32
    y = oy + 83.82
    cote1 = [("C", "C%d01" % n, "100n", "+5V", "GND", "C01", "C1525"), ("C", "C%d02" % n, "10u", "+5V", "GND", "C10", "C15850"),
             ("C", "C%d03" % n, "100n", "+3V3", "GND", "C01", "C1525"), ("C", "C%d04" % n, "10u", "+3V3", "GND", "C10", "C15850"),
             ("R", "R%d01" % n, "4k7", "+3V3", V("SDA"), "R", "C23162"), ("R", "R%d02" % n, "4k7", "+3V3", V("SCL"), "R", "C23162")]
    for i, (k, ref, val, h, b, f, l) in enumerate(cote1): passif(k, ref, val, 27.94 + 20.32*i, y, h, b, FP[f], l)
    texte("Decouplage : a moins de 2 mm des broches VIN (9) et VDDP (4)", 22.86, oy + 107.95, 1.1)
    cote2 = [("C", "C%d05" % n, "100n", V("VISO"), V("GNDISO"), "C01", "C1525"), ("C", "C%d06" % n, "10u", V("VISO"), V("GNDISO"), "C10", "C15850"),
             ("C", "C%d07" % n, "100n", V("VISO"), V("GNDISO"), "C01", "C1525"), ("C", "C%d08" % n, "10u", V("VISO"), V("GNDISO"), "C10", "C15850"),
             ("R", "R%d03" % n, "4k7", V("VISO"), V("SDA_ISO"), "R", "C23162"), ("R", "R%d04" % n, "4k7", V("VISO"), V("SCL_ISO"), "R", "C23162"),
             ("R", "R%d06" % n, "10k", V("VISO"), V("VSEL"), "R", "C25804"), ("R", "R%d05" % n, "4k7", V("VSEL"), V("GNDISO"), "R", "C23162")]
    for i, (k, ref, val, h, b, f, l) in enumerate(cote2): passif(k, ref, val, 180.34 + 20.32*i, y, h, b, FP[f], l)
    texte("C%d05/06 pres de VISO (12), C%d07/08 pres de VDDISO (17). R%d06 + R%d05 : VISO = 1,23 V x 14,7 / 4,7 = 3,85 V" % (n, n, n, n), 175.26, oy + 107.95, 1.1)
    drapeau(V("GNDISO"), 231.14, oy + 17.78)

voie("PH", 1, 20.32, "VOIE pH", "Atlas pH OEM")
voie("ORP", 2, 139.7, "VOIE ORP", "Atlas ORP OEM")
# connecteur vers la carte principale
jx, jy = 38.1, 63.5
custom("Conn_8", "J1", "Vers carte principale", jx, jy,
       {"1": "+5V", "2": "+3V3", "3": "GND", "4": "GND", "5": "SDA_PH", "6": "SCL_PH", "7": "SDA_ORP", "8": "SCL_ORP"}, FP["J1"], "C157972")
texte("Ordre des voies NON fige.\nLa carte principale reunit\nles deux bus en un seul.", 22.86, 91.44, 1.1)
for i, net in enumerate(("+5V", "+3V3", "GND")): drapeau(net, 88.9 + 15.24*i, 40.64)

lib_symbols = "\n".join(list(REPRIS.values()) + list(CUSTOM.values()))
sch = """(kicad_sch (version 20250114) (generator "eeschema") (generator_version "9.0") (uuid "%s") (paper "A3")
(title_block (title "Carte de mesure v3 - pH et ORP, une barriere par voie") (date "2026-11-24") (rev "0.1 - avant relecture")
 (company "ESP32 Pool Controller") (comment 1 "Reference : kicad/v3-mesure/README.md") (comment 2 "Schema par etiquettes : meme nom = meme fil"))
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
print("ok", len(E), "elements")
