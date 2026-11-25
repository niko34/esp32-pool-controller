#!/usr/bin/env python3
"""Écrit le schéma KiCad 9 de la carte principale v3 depuis le tableau des connexions
(README.md de ce dossier). Chaque broche porte un bout de fil et une étiquette (ou un symbole
d'alimentation) : ce sont les étiquettes qui relient. Les symboles viennent des bibliothèques
standard de KiCad (paquet kicad-symbols), sauf le module ESP32, dessiné ici.

Usage : python3 generer_schema.py <dossier de sortie> [fichier des connexions attendues .json]
⚠️ RÉÉCRIT le schéma en entier : ne plus le lancer après une modification à la main."""
import re, sys, uuid, json, os
OUT = sys.argv[1]
PRJ = "v3-principale"
ROOT = "3b8e2d14-9a6f-4c57-8e21-5d0c7a9b4f62"   # fixe : le projet et la carte y font référence
LIBDIR = "/usr/share/kicad/symbols"
def U(): return str(uuid.uuid4())
FONT = "(effects (font (size 1.27 1.27)))"
def r(v): return ("%.4f" % v).rstrip("0").rstrip(".")
def g(n): return n * 2.54

# ---------- symboles ----------
def bloc(t, nom):
    i = t.index('(symbol "%s"' % nom); n = 0; j = i
    while True:
        c = t[j]
        if c == "(": n += 1
        elif c == ")":
            n -= 1
            if n == 0: return t[i:j+1]
        j += 1
LIBS, SYMS, PINS = {}, {}, {}
RE_PIN = re.compile(r'\(pin (\w+) \w+\s*\(at ([-\d.]+) ([-\d.]+) (\d+)\)\s*\(length [-\d.]+\).*?\(number "([^"]+)"', re.S)
def charge(lib, nom):
    """Rend l'identifiant « lib:nom » après avoir chargé le symbole (une seule fois)."""
    lid = lib + ":" + nom
    if lid in SYMS: return lid
    if lib not in LIBS: LIBS[lib] = open(os.path.join(LIBDIR, lib + ".kicad_sym"), encoding="utf-8").read()
    b = bloc(LIBS[lib], nom)
    m = re.search(r'\(extends "([^"]+)"\)', b)
    if m:   # symbole dérivé : on prend le dessin du parent sous le nom du dérivé
        base = m.group(1); b = bloc(LIBS[lib], base)
        b = b.replace('(symbol "%s"' % base, '(symbol "%s"' % nom, 1)
        b = re.sub(r'\(symbol "%s_(\d+_\d+)"' % re.escape(base), r'(symbol "%s_\1"' % nom, b)
    b = b.replace('(symbol "%s"' % nom, '(symbol "%s"' % lid, 1)
    SYMS[lid] = b
    PINS[lid] = [(num, typ, float(x), float(y), int(rot)) for typ, x, y, rot, num in RE_PIN.findall(b)]
    return lid
def sym_custom(nom, ref, desc, w, top, bot, gauche, droite):
    lid = PRJ + ":" + nom
    def corps(full):
        L = ['(symbol "%s" (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)' % full,
             '(property "Reference" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))' % (ref, r(-w/2), r(top+1.27)),
             '(property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))' % (nom, r(-w/2), r(bot-1.27)),
             '(property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))',
             '(property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))',
             '(property "Description" "%s" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))' % desc,
             '(symbol "%s_0_1" (rectangle (start %s %s) (end %s %s) (stroke (width 0.254) (type default)) (fill (type background))))' % (nom, r(-w/2), r(top), r(w/2), r(bot)),
             '(symbol "%s_1_1"' % nom]
        for cote, liste in (("g", gauche), ("d", droite)):
            for num, nm, typ, y in liste:
                x = -w/2 - 2.54 if cote == "g" else w/2 + 2.54
                L.append('(pin %s line (at %s %s %d) (length 2.54) (name "%s" %s) (number "%s" %s))' % (typ, r(x), r(y), 0 if cote == "g" else 180, nm, FONT, num, FONT))
        L.append(') (embedded_fonts no))')
        return "\n".join(L)
    SYMS[lid] = corps(lid); LIBTXT.append(corps(nom))
    PINS[lid] = [(num, typ, float(x), float(y), int(rot)) for typ, x, y, rot, num in RE_PIN.findall(SYMS[lid])]
    return lid
LIBTXT = []

# ---------- éléments ----------
E = []; ATTENDU = {}; NC = []; FILS = set(); VUS = {}
def prop(nom, val, x, y, cache=False, just="left", ang=0):
    eff = "(effects (font (size 1.27 1.27))%s%s)" % (" (justify %s)" % just if just else "", " (hide yes)" if cache else "")
    return '(property "%s" "%s" (at %s %s %d) %s)' % (nom, val, r(x), r(y), ang, eff)
def place(lid, ref, val, x, y, nums, fp="", lcsc="", refpos=(2.54, -1.27), valpos=(2.54, 1.27), montre_ref=True, just="left"):
    L = ['(symbol (lib_id "%s") (at %s %s 0) (unit 1) (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no) (uuid "%s")' % (lid, r(x), r(y), U()),
         prop("Reference", ref, x+refpos[0], y+refpos[1], cache=not montre_ref, just=just),
         prop("Value", val, x+valpos[0], y+valpos[1], just=just),
         prop("Footprint", fp, x, y, cache=True), prop("Datasheet", "", x, y, cache=True)]
    if lcsc: L.append(prop("LCSC", lcsc, x, y, cache=True))
    for n in nums: L.append('(pin "%s" (uuid "%s"))' % (n, U()))
    L.append('(instances (project "%s" (path "/%s" (reference "%s") (unit 1)))))' % (PRJ, ROOT, ref))
    E.append("\n".join(L))
def fil(a, b):
    k = (round(a[0], 3), round(a[1], 3), round(b[0], 3), round(b[1], 3))
    if k in FILS: return
    FILS.add(k)
    E.append('(wire (pts (xy %s %s) (xy %s %s)) (stroke (width 0) (type default)) (uuid "%s"))' % (r(a[0]), r(a[1]), r(b[0]), r(b[1]), U()))
CPT = {"pwr": 1, "flg": 1}
POSITIFS = ("+3V3", "+5V", "+12V")
def plus(nom, x, y):
    place(charge("power", "+5V"), "#PWR%03d" % CPT["pwr"], nom, x, y, ["1"], refpos=(0, 3.81), valpos=(0, -3.556), montre_ref=False, just=None); CPT["pwr"] += 1
def masse(x, y):
    place(charge("power", "GND"), "#PWR%03d" % CPT["pwr"], "GND", x, y, ["1"], refpos=(0, 6.35), valpos=(0, 4.445), montre_ref=False, just=None); CPT["pwr"] += 1
def drapeau(nom, x, y):
    """Indique au contrôle électrique que ce fil d'alimentation a bien une source."""
    place(charge("power", "PWR_FLAG"), "#FLG%02d" % CPT["flg"], "PWR_FLAG", x, y, ["1"], refpos=(0, -1.905), valpos=(0, -3.81), montre_ref=False, just=None); CPT["flg"] += 1
    relie((x, y), (0, 1), nom)
ETQ = {(1, 0): (0, "left bottom"), (-1, 0): (180, "right bottom"), (0, -1): (90, "left bottom"), (0, 1): (270, "right bottom")}
def relie(pt, d, nom, lg=2.54):
    """Bout de fil partant de la broche dans la direction d, terminé par l'étiquette du fil."""
    x, y = pt; ex, ey = x + d[0]*lg, y + d[1]*lg
    k = (round(ex, 2), round(ey, 2))
    if VUS.get(k, nom) != nom: raise SystemExit("COLLISION d'etiquettes en %s : %s / %s" % (k, VUS[k], nom))
    deja = k in VUS; VUS[k] = nom
    fil(pt, (ex, ey))
    if deja: return
    if nom == "GND" and d == (0, 1): masse(ex, ey)
    elif nom in POSITIFS and d == (0, -1): plus(nom, ex, ey)
    else:
        rot, just = ETQ[d]
        E.append('(label "%s" (at %s %s %d) (effects (font (size 1.27 1.27)) (justify %s)) (uuid "%s"))' % (nom, r(ex), r(ey), rot, just, U()))
DEHORS = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}
NUM = {}
def comp(lid, prefixe, val, x, y, nets, fp="", lcsc="", lg=2.54, **kw):
    """Pose un composant et relie chaque broche. nets : numéro de broche -> nom du fil, ou None (en l'air)."""
    NUM[prefixe] = NUM.get(prefixe, 0) + 1; ref = "%s%d" % (prefixe, NUM[prefixe])
    pins = PINS[lid]
    manque = set(p[0] for p in pins) ^ set(nets)
    if manque: raise SystemExit("%s (%s) : broches sans decision %s" % (ref, lid, sorted(manque)))
    place(lid, ref, val, x, y, [p[0] for p in pins], fp, lcsc, **kw)
    for num, typ, px, py, rot in pins:
        pt = (x + px, y - py); nom = nets[num]
        if nom is None:
            NC.append("%s.%s" % (ref, num))
            k = ("nc", round(pt[0], 2), round(pt[1], 2))
            if k not in VUS: VUS[k] = 1; E.append('(no_connect (at %s %s) (uuid "%s"))' % (r(pt[0]), r(pt[1]), U()))
        else:
            ATTENDU.setdefault(nom, set()).add("%s.%s" % (ref, num)); relie(pt, DEHORS[rot], nom, lg)
    return ref
def texte(t, x, y, taille=1.27, gras=False):
    t = t.replace('"', '\\"').replace("\n", "\\n")
    E.append('(text "%s" (exclude_from_sim no) (at %s %s 0) (effects (font (size %s %s)%s) (justify left bottom)) (uuid "%s"))' % (t, r(x), r(y), r(taille), r(taille), " (bold yes)" if gras else "", U()))
def cadre(x1, y1, x2, y2, titre):
    E.append('(rectangle (start %s %s) (end %s %s) (stroke (width 0.2) (type dash)) (fill (type none)) (uuid "%s"))' % (r(x1), r(y1), r(x2), r(y2), U()))
    texte(titre, x1 + 2.54, y1 + 6.35, 2.5, True)

# ---------- empreintes et références au catalogue (relevées le 2026-11-25) ----------
FP = {"R": "Resistor_SMD:R_0603_1608Metric", "C04": "Capacitor_SMD:C_0402_1005Metric", "C06": "Capacitor_SMD:C_0603_1608Metric",
      "C08": "Capacitor_SMD:C_0805_2012Metric", "C12": "Capacitor_SMD:C_1206_3216Metric",
      "SOT23": "Package_TO_SOT_SMD:SOT-23", "SO8": "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "SMA": "Diode_SMD:D_SMA", "SMB": "Diode_SMD:D_SMB",
      "SOD123": "Diode_SMD:D_SOD-123", "BORNIER": "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-2_1x02_P5.00mm_Horizontal"}
PASSIF = {"100R": ("R", "C22775"), "470R": ("R", "C23179"), "1k": ("R", "C21190"), "4k7": ("R", "C23162"), "5k1": ("R", "C23186"), "10k": ("R", "C25804"),
          "100n": ("C04", "C1525"), "1u": ("C06", "C15849"), "22p": ("C06", "C1653"), "10u": ("C08", "C15850"), "22u": ("C08", "C45783"), "10u 50V": ("C12", "C13585")}
def R(val, x, y, haut, bas):
    fp, lcsc = PASSIF[val]; return comp(charge("Device", "R"), "R", val, x, y, {"1": haut, "2": bas}, FP[fp], lcsc)
def C(val, x, y, haut, bas):
    fp, lcsc = PASSIF[val]; return comp(charge("Device", "C"), "C", val, x, y, {"1": haut, "2": bas}, FP[fp], lcsc)

# =====================================================================================
# 1. ENTRÉE 12 V
X, Y = g(8), g(10)
cadre(X, Y, X + g(72), Y + g(34), "ENTREE 12 V : fusible, anti-inversion, ecreteur")
comp(charge("Connector", "Screw_Terminal_01x02"), "J", "12 V", X + g(8), Y + g(16), {"1": "VIN_BRUT", "2": "GND"}, FP["BORNIER"], "C474881", refpos=(0, -5.08), valpos=(0, 5.08))
comp(charge("Device", "Polyfuse"), "F", "2A 24V", X + g(16), Y + g(16), {"1": "VIN_BRUT", "2": "VIN_FUS"}, "Fuse:Fuse_1812_4532Metric", "C434887")
comp(charge("Transistor_FET", "IRF7404"), "Q", "AO4407A", X + g(28), Y + g(16),
     {"4": "GRILLE_INV", "5": "VIN_FUS", "6": "VIN_FUS", "7": "VIN_FUS", "8": "VIN_FUS", "1": "+12V", "2": "+12V", "3": "+12V"}, FP["SO8"], "C16072", refpos=(5.08, -1.27), valpos=(5.08, 1.27))
R("10k", X + g(36), Y + g(16), "GRILLE_INV", "GND")
comp(charge("Device", "D_Zener"), "D", "BZT52C12", X + g(46), Y + g(16), {"1": "+12V", "2": "GRILLE_INV"}, FP["SOD123"], "C173429", refpos=(0, -3.81), valpos=(0, 3.81), just=None)
comp(charge("Device", "D_Zener"), "D", "SMBJ13A", X + g(46), Y + g(27), {"1": "+12V", "2": "GND"}, FP["SMB"], "C404130", refpos=(0, -3.81), valpos=(0, 3.81), just=None)
comp(charge("Device", "C_Polarized"), "C", "470u 25V", X + g(56), Y + g(16), {"1": "+12V", "2": "GND"}, "Capacitor_THT:CP_Radial_D8.0mm_P3.50mm", "C106735")
C("100n", X + g(62), Y + g(16), "+12V", "GND")
drapeau("+12V", X + g(68), Y + g(14)); drapeau("GND", X + g(68), Y + g(24))
texte("Le DRAIN du transistor est cote fusible : sa diode interne laisse passer le\ncourant dans le sens normal (note TI SLVA139). Le v2 etait monte a l'inverse.\n"
      "Ecreteur : au repos jusqu'a 13 V, limite a 21,5 V.", X + g(2), Y + g(33), 1.1)

# 2. SORTIES DE PUISSANCE
X, Y = g(8), g(48)
cadre(X, Y, X + g(72), Y + g(98), "SORTIES 12 V : 2 pompes, filtration, eclairage, auxiliaire")
for i, (sig, nom) in enumerate((("POMPE_PH", "Pompe pH"), ("POMPE_ORP", "Pompe chlore"), ("FILTRATION", "Filtration"), ("ECLAIRAGE", "Eclairage"), ("AUX", "Auxiliaire"))):
    y = Y + g(16 + 17 * i); gr = "G_" + sig; so = "S_" + sig
    texte(nom, X + g(2), y - g(7.5), 1.6, True)
    R("100R", X + g(6), y, sig, gr)
    R("10k", X + g(13), y, gr, "GND")
    comp(charge("Transistor_FET", "AO3400A"), "Q", "AO3400A", X + g(27), y, {"1": gr, "3": so, "2": "GND"}, FP["SOT23"], "C20917", refpos=(5.08, -1.27), valpos=(5.08, 1.27))
    comp(charge("Device", "D_Schottky"), "D", "SS34", X + g(39), y, {"1": "+12V", "2": so}, FP["SMA"], "C8678", refpos=(0, -3.81), valpos=(0, 3.81), just=None)
    C("10u 50V", X + g(52), y, "+12V", "GND")
    comp(charge("Connector", "Screw_Terminal_01x02"), "J", nom, X + g(68), y, {"1": "+12V", "2": so}, FP["BORNIER"], "C474881", refpos=(0, -5.08), valpos=(-10.16, 6.35))
texte("Borne 1 = + de la charge, borne 2 = - commute. Diode de roue libre et 10 uF au pied de chaque sortie.", X + g(2), Y + g(97), 1.1)

# 3. CŒUR : ESP32
X, Y = g(86), g(10)
cadre(X, Y, X + g(64), Y + g(66), "ESP32 (module WROVER-E, 8 Mo flash, 8 Mo PSRAM)")
G = [("2", "3V3", "power_in"), ("3", "EN", "input"), ("4", "SENSOR_VP/IO36", "input"), ("5", "SENSOR_VN/IO39", "input"), ("6", "IO34", "input"), ("7", "IO35", "input"),
     ("8", "IO32", "bidirectional"), ("9", "IO33", "bidirectional"), ("10", "IO25", "bidirectional"), ("11", "IO26", "bidirectional"), ("12", "IO27", "bidirectional"),
     ("13", "IO14", "bidirectional"), ("14", "IO12", "bidirectional"), ("16", "IO13", "bidirectional"),
     ("1", "GND", "power_in"), ("15", "GND", "power_in"), ("38", "GND", "power_in"), ("39", "GND/pastille", "power_in")]
D = [("37", "IO23", "bidirectional"), ("36", "IO22", "bidirectional"), ("35", "TXD0", "bidirectional"), ("34", "RXD0", "bidirectional"), ("33", "IO21", "bidirectional"),
     ("31", "IO19", "bidirectional"), ("30", "IO18", "bidirectional"), ("29", "IO5", "bidirectional"), ("26", "IO4", "bidirectional"), ("25", "IO0", "bidirectional"),
     ("24", "IO2", "bidirectional"), ("23", "IO15", "bidirectional")] + [(str(n), "NC", "no_connect") for n in (17, 18, 19, 20, 21, 22, 27, 28, 32)]
H = 2.54 * 11
esp = sym_custom("ESP32-WROVER-E", "U", "Module ESP32-WROVER-E-N8R8", 30.48, H, -H,
                 [(n, nm, t, H - 2.54 * (i + 1)) for i, (n, nm, t) in enumerate(G)], [(n, nm, t, H - 2.54 * (i + 1)) for i, (n, nm, t) in enumerate(D)])
N = {"2": "+3V3", "3": "EN", "4": "RTC_INT", "5": None, "6": None, "7": "BOUTON", "8": "AUX", "9": "POMPE_ORP", "10": "POMPE_PH", "11": "FILTRATION", "12": "ECLAIRAGE",
     "13": None, "14": None, "16": None, "1": "GND", "15": "GND", "38": "GND", "39": "GND",
     "37": None, "36": "SCL", "35": "TX_ESP", "34": "RX", "33": "SDA", "31": None, "30": None, "29": "TEMP", "26": None, "25": "IO0", "24": "LED", "23": None}
N.update({str(n): None for n in (17, 18, 19, 20, 21, 22, 27, 28, 32)})
comp(esp, "U", "ESP32-WROVER-E-N8R8", X + g(22), Y + g(28), N, PRJ + ":ESP32-WROVER-E", "C2973667", refpos=(-15.24, -H - 1.27), valpos=(-15.24, H + 1.27))
C("22u", X + g(46), Y + g(16), "+3V3", "GND"); C("100n", X + g(52), Y + g(16), "+3V3", "GND")
R("10k", X + g(46), Y + g(34), "+3V3", "EN"); C("1u", X + g(46), Y + g(46), "EN", "GND")
R("10k", X + g(54), Y + g(34), "+3V3", "IO0")
R("470R", X + g(54), Y + g(52), "TX_ESP", "TX")
texte("IO12 en l'air : elle fixe la tension de la memoire au demarrage.\nIO16 et IO17 n'existent pas sur ce module (prises par la PSRAM).\n"
      "Memes broches que le v2 (ADR-0012) : le firmware ne change pas.", X + g(2), Y + g(64), 1.1)

# 4. USB
X, Y = g(86), g(80)
cadre(X, Y, X + g(64), Y + g(66), "USB-C : programmation et remise a zero automatique")
comp(charge("Connector", "USB_C_Receptacle_USB2.0_16P"), "J", "USB-C", X + g(10), Y + g(26),
     {"S1": "GND", "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "A4": "VBUS", "A9": "VBUS", "B4": "VBUS", "B9": "VBUS",
      "A5": "CC1", "B5": "CC2", "A7": "USB_DM", "B7": "USB_DM", "A6": "USB_DP", "B6": "USB_DP", "A8": None, "B8": None},
     "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12", "C165948", refpos=(-10.16, -19.05), valpos=(-10.16, -21.59))
R("5k1", X + g(24), Y + g(14), "CC1", "GND"); R("5k1", X + g(30), Y + g(14), "CC2", "GND")
comp(charge("Power_Protection", "USBLC6-2SC6"), "U", "USBLC6-2SC6", X + g(30), Y + g(30), {"1": "USB_DP", "6": "USB_DP", "3": "USB_DM", "4": "USB_DM", "5": "VBUS", "2": "GND"},
     "Package_TO_SOT_SMD:SOT-23-6", "C7519", refpos=(2.54, -7.62), valpos=(2.54, 8.89))
comp(charge("Interface_USB", "CH340C"), "U", "CH340C", X + g(46), Y + g(24),
     {"15": None, "5": "USB_DP", "6": "USB_DM", "7": None, "8": None, "16": "+3V3", "4": "+3V3", "1": "GND", "2": "RX", "3": "TX",
      "9": None, "10": None, "11": None, "12": None, "13": "DTR", "14": "RTS"}, "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm", "C84681", refpos=(2.54, -13.97), valpos=(2.54, 13.97))
C("100n", X + g(58), Y + g(14), "+3V3", "GND")
R("10k", X + g(10), Y + g(52), "DTR", "B_EN")
comp(charge("Transistor_BJT", "SS8050"), "Q", "SS8050", X + g(18), Y + g(52), {"1": "B_EN", "3": "EN", "2": "RTS"}, FP["SOT23"], "C2150", refpos=(5.08, -1.27), valpos=(5.08, 1.27))
R("10k", X + g(30), Y + g(52), "RTS", "B_IO0")
comp(charge("Transistor_BJT", "SS8050"), "Q", "SS8050", X + g(38), Y + g(52), {"1": "B_IO0", "3": "IO0", "2": "DTR"}, FP["SOT23"], "C2150", refpos=(5.08, -1.27), valpos=(5.08, 1.27))
for i, s in enumerate(("TX", "RX", "EN", "IO0", "GND")):
    comp(charge("Connector", "TestPoint"), "TP", s, X + g(48 + 3 * i), Y + g(50), {"1": s}, "TestPoint:TestPoint_Pad_D1.5mm", refpos=(0, -8.89), valpos=(0, -6.35), just=None)
for i, (s, nom) in enumerate((("EN", "RESET"), ("IO0", "BOOT"))):
    comp(charge("Switch", "SW_Push"), "SW", nom, X + g(46 + 12 * i), Y + g(38), {"1": s, "2": "GND"}, "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A", "C318884", refpos=(0, -5.08), valpos=(0, 3.81), just=None)
texte("Le pont USB est alimente en 3,3 V : le 5 V de l'USB n'alimente rien.\nLes emetteurs des deux transistors sont croises (DTR/RTS).\n"
      "Boutons RESET et BOOT comme sur une carte de developpement ; pastilles d'essai en dernier secours.", X + g(2), Y + g(64), 1.1)

# 5. HORLOGE
X, Y = g(154), g(10)
cadre(X, Y, X + g(64), Y + g(44), "HORLOGE PCF8563 et pile")
comp(charge("Timer_RTC", "PCF8563T"), "U", "PCF8563T", X + g(22), Y + g(20),
     {"1": "OSCI", "2": "OSCO", "8": "VRTC", "4": "GND", "6": "SCL", "5": "SDA", "7": None, "3": "RTC_INT"}, FP["SO8"], "C7440", refpos=(2.54, -8.89), valpos=(2.54, 8.89))
comp(charge("Device", "Crystal"), "Y", "32.768kHz 12.5pF", X + g(8), Y + g(14), {"1": "OSCI", "2": "OSCO"}, "Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm", "C271654", refpos=(0, -3.81), valpos=(0, 3.81), just=None)
C("22p", X + g(8), Y + g(26), "VRTC", "OSCI")
C("100n", X + g(34), Y + g(14), "VRTC", "GND")
comp(charge("Diode", "BAT54C"), "D", "BAT54C", X + g(44), Y + g(14), {"1": "+3V3", "2": "VBAT_R", "3": "VRTC"}, FP["SOT23"], "C37704", refpos=(0, -5.08), valpos=(0, -3.175), just=None)
comp(charge("Device", "Battery_Cell"), "BT", "CR2032", X + g(56), Y + g(16), {"1": "VBAT", "2": "GND"}, "Battery:BatteryHolder_Keystone_3002_1x2032", "C70377")
R("1k", X + g(56), Y + g(30), "VBAT_R", "VBAT")
R("10k", X + g(28), Y + g(32), "+3V3", "RTC_INT"); R("10k", X + g(36), Y + g(32), "+3V3", "SDA"); R("10k", X + g(44), Y + g(32), "+3V3", "SCL")
drapeau("VRTC", X + g(20), Y + g(34))
texte("L'horloge est alimentee par le 3,3 V ou par la pile, a travers les deux diodes.\n1 kOhm en serie avec la pile : limite le courant si une diode claque.\n10 kOhm sur SDA et SCL : l'horloge repond meme carte de mesure debranchee.", X + g(2), Y + g(42.5), 1.1)

# 6. TEMPÉRATURE, BOUTON, VOYANT
X, Y = g(154), g(58)
cadre(X, Y, X + g(64), Y + g(40), "TEMPERATURE, BOUTON, VOYANT")
comp(charge("Sensor_Temperature", "DS18B20"), "U", "DS18B20", X + g(8), Y + g(18), {"3": "+3V3", "1": "GND", "2": "TEMP"}, "Package_TO_SOT_THT:TO-92_Inline", "C9753", refpos=(-7.62, -6.35), valpos=(2.54, 6.35))
R("4k7", X + g(18), Y + g(18), "+3V3", "TEMP"); C("100n", X + g(24), Y + g(18), "+3V3", "GND")
comp(charge("Connector", "Screw_Terminal_01x03"), "J", "Sonde d'eau", X + g(34), Y + g(18), {"1": "GND", "2": "+3V3", "3": "TEMP"}, "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-3_1x03_P5.00mm_Horizontal", "C474882", refpos=(0, -5.08), valpos=(0, 7.62))
comp(charge("Switch", "SW_Push"), "SW", "Reinitialisation", X + g(46), Y + g(14), {"1": "+3V3", "2": "BOUTON"}, "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A", "C318884", refpos=(0, -5.08), valpos=(0, 3.81), just=None)
R("10k", X + g(46), Y + g(28), "BOUTON", "GND")
R("1k", X + g(56), Y + g(14), "LED", "A_LED")
comp(charge("Device", "LED"), "D", "LED rouge", X + g(56), Y + g(28), {"2": "A_LED", "1": "GND"}, "LED_SMD:LED_0603_1608Metric", "C2286", refpos=(0, -3.81), valpos=(0, 3.81), just=None)
texte("Bouton : appuye = niveau HAUT (c'est ce que lit le firmware).", X + g(2), Y + g(39), 1.1)

# 7. LIAISON VERS LA CARTE DE MESURE
X, Y = g(154), g(102)
cadre(X, Y, X + g(64), Y + g(44), "LIAISON VERS LA CARTE DE MESURE (cable droit)")
comp(charge("Connector_Generic", "Conn_01x08"), "J", "Carte de mesure", X + g(20), Y + g(22),
     {"1": "SDA", "2": "SCL", "3": "GND", "4": "+5V", "5": "+3V3", "6": "GND", "7": "SDA", "8": "SCL"}, "Connector_JST:JST_XH_B8B-XH-A_1x08_P2.50mm_Vertical", "C157972", refpos=(0, -12.7), valpos=(0, 15.24))
texte("Meme ordre que le J1 de la carte de mesure.\nVoies 1 et 7 reunies (SDA), 2 et 8 reunies (SCL) :\ndecision D3, un seul bus pour l'ESP32.\n"
      "Convertisseur du pH en 0x48, de l'ORP en 0x49,\nhorloge en 0x51.", X + g(30), Y + g(26), 1.1)

# 8. ALIMENTATIONS 5 V ET 3,3 V
X, Y = g(154), g(150)
cadre(X, Y, X + g(64), Y + g(40), "ALIMENTATIONS 5 V et 3,3 V")
comp(charge("Regulator_Linear", "AMS1117-3.3"), "U", "AMS1117-3.3", X + g(40), Y + g(16), {"3": "+5V", "1": "GND", "2": "+3V3"}, "Package_TO_SOT_SMD:SOT-223-3_TabPin2", "C6186", refpos=(-3.81, -6.35), valpos=(-3.81, -4.445))
C("10u", X + g(28), Y + g(20), "+5V", "GND"); C("22u", X + g(52), Y + g(20), "+3V3", "GND")
drapeau("+5V", X + g(12), Y + g(22))
texte("PROVISOIRE : le regulateur 12 V vers 5 V\nn'est PAS dessine. Il attend l'essai\nd'alimentation M3 (feature-215, partie B).", X + g(2), Y + g(14), 1.2, True)
texte("Reference : kicad/v3-principale/README.md", X + g(2), Y + g(39), 1.1)

lib_symbols = "\n".join(SYMS.values())
sch = """(kicad_sch (version 20250114) (generator "eeschema") (generator_version "9.0") (uuid "%s") (paper "A1")
(title_block (title "Carte principale v3 - ESP32, USB, horloge, sorties 12 V") (date "2026-11-25") (rev "0.1")
 (company "ESP32 Pool Controller") (comment 1 "Reference : kicad/v3-principale/README.md") (comment 2 "Bloc 12 V vers 5 V non dessine : attend l'essai M3"))
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
    '(sym_lib_table\n  (version 7)\n  (lib (name "%s")(type "KiCad")(uri "${KIPRJMOD}/%s.kicad_sym")(options "")(descr "Symboles propres a la carte principale v3"))\n)\n' % (PRJ, PRJ))
pro = os.path.join(OUT, PRJ + ".kicad_pro")
if not os.path.exists(pro):
    json.dump({"meta": {"filename": PRJ + ".kicad_pro", "version": 3}, "sheets": [[ROOT, "Root"]], "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []}},
              open(pro, "w", encoding="utf-8"), indent=2)
if len(sys.argv) > 2:
    json.dump({"nets": {k: sorted(v) for k, v in ATTENDU.items()}, "en_l_air": sorted(NC)}, open(sys.argv[2], "w"), indent=1)
print("ok", len(E), "elements,", len(ATTENDU), "nets attendus,", len(NC), "broches en l'air")
