#!/usr/bin/env python3
"""PoolController V15 Dosita MP1 - PCB v3 : carte principale 119 x 100 et carte de mesure 73 x 54.

Derive du V14 le 2026-11-26 : +20 mm de hauteur, +10 mm de profondeur, deux cartes
au lieu d'une. Le dessin general et la position des pompes ne changent pas.

Toutes les dimensions sont en millimetres. Le script est autonome et doit etre
execute avec FreeCADCmd. Les volumes de couleur sont exportes avec la meme
origine afin de former des objets multi-materiaux dans le trancheur.

Les composants achetes (PCB, RPS-30-12, Dosita MP1, BNC et PG7) sont uniquement des
references de controle dans l'assemblage et ne sont jamais exportes en STL.
"""

from pathlib import Path
import json
import math

import FreeCAD as App
import Part
import Mesh
import MeshPart


ROOT = Path(__file__).resolve().parent
FC_DIR = ROOT / "freecad"
STL_DIR = ROOT / "stl"
STEP_DIR = ROOT / "step"
REPORT_DIR = ROOT / "reports"
for directory in (FC_DIR, STL_DIR, STEP_DIR, REPORT_DIR):
    directory.mkdir(parents=True, exist_ok=True)


# ---------------------------------------------------------------------------
# Dimensions maitresses
# ---------------------------------------------------------------------------

BOX_W = 210.0
BOX_H = 195.0   # V15 : +20 (V14 : 175)
# V15 : 114 (V14 : 94). +10 pour eloigner l'arriere des pompes des cartes, puis +10 pour
# loger sous la pompe de gauche un transformateur de 48 mm (feature-218, 2026-11-26).
# Les cotes qui suivent la facade sont DERIVEES de BODY_D : une cloison oubliee a 70 mm
# a deja laisse 12,8 mm d'ouverture lors du premier agrandissement.
BODY_D = 114.0
WALL = 3.0
REAR = 3.2
TOP_CHAMFER = 10.0
BOTTOM_CHAMFER = 8.0

# La platine est fixee au mur et ferme toute la silhouette arriere. Le capot
# commence exactement sur sa face avant : les deux contours sont affleurants
# et le capot n'a plus a envelopper le chant de la platine.
COVER_ASSEMBLY_Z = REAR
COVER_D = BODY_D - COVER_ASSEMBLY_Z
COVER_FRONT_START = BODY_D - 9.0
FRONT_CORE_T = COVER_D - COVER_FRONT_START
SHELL_FRONT_INSET = 11.0
SHELL_BOTTOM_INSET = 6.0

# Plan de joint sur la platine et cadre de compression dans le capot.
SEAL_FACE_Z = 9.0
# Le cadre fixe est plus large que la gorge afin de laisser 1,6 mm de matiere
# imprimable de chaque cote. La levre mobile conserve son contour propre,
# centre sur le cordon, et ne depend donc plus du contour du cadre fixe.
SEAL_FRAME_OUTER = (8.3, 4.0, 193.4, 187.0, 9.0)
SEAL_FRAME_INNER = (14.3, 10.0, 181.4, 175.0, 3.0)
GASKET_CORD_D = 2.0
GASKET_W = 2.8
GASKET_DEPTH = 1.4
GASKET_COMPRESSION = (GASKET_CORD_D - GASKET_DEPTH) / GASKET_CORD_D
GASKET_PATH_W = 187.4
GASKET_PATH_H = 181.0
GASKET_PATH_R = 6.0
GASKET_PATH_X = 11.3
GASKET_PATH_Y = 7.0
GASKET_PATH_LENGTH = (
    2.0 * (GASKET_PATH_W + GASKET_PATH_H - 4.0 * GASKET_PATH_R)
    + 2.0 * math.pi * GASKET_PATH_R
)
GASKET_PURCHASE_LENGTH = 1000.0
COMPRESSION_RING_Z = SEAL_FACE_Z - COVER_ASSEMBLY_Z
COMPRESSION_RING_T = 2.5
COMPRESSION_RING_W = 2.8
COMPRESSION_RING_OUTER = (9.9, 5.6, 190.2, 183.8, 7.4)
GASKET_SUPPORT_WALL_MIN = 1.2
GASKET_SUPPORT_WALL = (
    GASKET_PATH_X - GASKET_W / 2.0 - SEAL_FRAME_OUTER[0])
# Une bride large relie la zone de compression aux parois du capot. Sa face
# arriere est coplanaire avec la zone centree sur le cordon : les deux rives
# du cadre fixe servent ainsi de butee mecanique a 30 % de compression. Le
# bord interieur est identique a celui de la zone de compression afin de ne
# pas reduire le jeu avec le capot RPS.
COMPRESSION_FLANGE_RECESS = 0.0
COMPRESSION_FLANGE_Z = COMPRESSION_RING_Z + COMPRESSION_FLANGE_RECESS
COMPRESSION_FLANGE_T = COMPRESSION_RING_T - COMPRESSION_FLANGE_RECESS
# Une nervure continue sur le haut et les deux cotes, placee du cote interieur
# du capot, epaissit la bride vers les parois. Son profil nominal est a
# 45 degres : 5,5 mm de course pour 5,5 mm de montee. Elle ne descend jamais
# derriere le plan de contact du joint. En bas, la grande traverse est supprimee
# et remplacee par une interface sur la tablette porte-connecteurs.
COMPRESSION_GUSSET_RUN = (
    COMPRESSION_RING_OUTER[0] + COMPRESSION_RING_W - WALL)
COMPRESSION_GUSSET_RISE = COMPRESSION_GUSSET_RUN
COMPRESSION_GUSSET_ANGLE_DEG = math.degrees(math.atan2(
    COMPRESSION_GUSSET_RISE, COMPRESSION_GUSSET_RUN))
COMPRESSION_GUSSET_OVERLAP = 0.1
COMPRESSION_GUSSET_Z = (
    COMPRESSION_FLANGE_Z + COMPRESSION_FLANGE_T
    - COMPRESSION_GUSSET_OVERLAP)
# L'enveloppe de passage PG7 s'etend jusqu'a Z75 pendant la pose.
PG7_CABLE_INSERTION_KEEP_Z1 = 75.0
BOTTOM_FLANGE_RELIEF_Y1 = (
    COMPRESSION_RING_OUTER[1] + COMPRESSION_RING_W + 0.2)
COVER_INTERNAL_CLEARANCE_MIN = 1.0

# Ruban graphite superficiel. Le noyau clair reste continu derriere lui.
RIBBON_X = SHELL_FRONT_INSET
RIBBON_Y = 47.5
RIBBON_W = BOX_W - 2.0 * SHELL_FRONT_INSET
RIBBON_H = 80.0
RIBBON_TIP = 14.0
RIBBON_SKIN_T = 1.0
RIBBON_RETURN_W = 4.0
RIBBON_Z = COVER_D - RIBBON_SKIN_T

# Signature turquoise captive. La partie visible reste une incrustation
# affleurante de 0,8 mm. Un pied cache plus large l'emprisonne dans le noyau
# clair et evite de faire reposer sa tenue sur la seule adhesion ASA/ABS.
ACCENT_X = 23.0
ACCENT_Y = 31.0
ACCENT_W = 164.0
ACCENT_H = 1.8
ACCENT_TIP = 6.0
ACCENT_VISIBLE_T = 0.8
ACCENT_ANCHOR_T = 0.8
ACCENT_TOTAL_DEPTH = ACCENT_VISIBLE_T + ACCENT_ANCHOR_T
ACCENT_ANCHOR_SPREAD = 0.4
ACCENT_JOIN_OVERLAP = 0.05
ACCENT_VISIBLE_Z = COVER_D - ACCENT_VISIBLE_T
ACCENT_ANCHOR_Z = COVER_D - ACCENT_TOTAL_DEPTH

# Interface Dosita MP1 nominale conservee depuis le modele MP1 precedent.
# Montage et dimensions a confirmer sur les pompes reelles, non mesures utilisateur.
PUMP_MOTOR_D = 25.5
PUMP_MOTOR_CUTOUT_D = 27.5
PUMP_MOUNT_DX = 38.0
PUMP_MOUNT_DY = 26.0
PUMP_SCREW_NOMINAL_D = 4.0  # Reservation M4 seulement ; vis reelle MP1 a confirmer.
PUMP_SCREW_CLEAR_D = 4.5
PUMP_MOTOR_REAR_D = 44.9
PUMP_REAR_D = 44.9
PUMP_TERMINAL_DEPTH = PUMP_REAR_D - PUMP_MOTOR_REAR_D
# Enveloppe axiale de reservation, pas dessin des bornes. Leur encombrement
# radial reel et les cosses/fils branches restent a mesurer sur le prototype.
PUMP_TERMINAL_ENVELOPE_D = PUMP_MOTOR_D
# Seulement pour la representation visuelle historique, non cotes contractuelles.
PUMP_TOTAL_W = 58.0
PUMP_FRONT_D = 25.5
PUMP_PAD_CLEARANCE = 0.4
PUMP_PAD_W = 49.2
PUMP_PAD_H = 58.0
PUMP_CENTRES = [(65.0, 91.0), (145.0, 91.0)]

# Trois crochets superieurs rigides et deux guides lateraux continus. Chaque crochet
# possede une levre fixe sur la platine et une languette du capot placee
# derriere cette levre. Les vis basses interdisent le relevage necessaire au
# desengagement. Aucun clip flexible n'est utilise.
TOP_HOOK_X = (32.0, 96.0, 160.0)
TOP_HOOK_W = 18.0
BACKPLATE_CLEAT_Y = 168.0
BACKPLATE_CLEAT_H = 4.0
BACKPLATE_CLEAT_Z0 = REAR
BACKPLATE_CLEAT_D = 8.8
BACKPLATE_HOOK_NOSE_Y = 172.0
BACKPLATE_HOOK_NOSE_H = 4.0
BACKPLATE_HOOK_NOSE_Z0 = 9.2
BACKPLATE_HOOK_NOSE_D = 2.8
COVER_HOOK_Y = 172.6
COVER_HOOK_H = 4.2
COVER_HOOK_Z0 = 6.7 - COVER_ASSEMBLY_Z
COVER_HOOK_D = 2.0
COVER_HOOK_SPINE_Y = 176.4
COVER_HOOK_SPINE_H = 2.4
COVER_HOOK_SPINE_Z0 = COVER_HOOK_Z0
COVER_HOOK_SPINE_D = 13.7
HOOK_PULL_TEST = 1.0
HOOK_RELEASE_LIFT = 4.0
HOOK_PULL_CLEARANCE = (
    BACKPLATE_HOOK_NOSE_Z0
    - (COVER_ASSEMBLY_Z + COVER_HOOK_Z0 + COVER_HOOK_D)
)
SIDE_GUIDE_Y = (163.5,)  # V15 : au-dessus de la carte principale (bord haut a Y 162,05), 1,45 mm de jeu
SIDE_GUIDE_W = 5.0
SIDE_GUIDE_LEFT_X = 13.0
SIDE_GUIDE_RIGHT_X = 192.0
SIDE_GUIDE_H = 17.0  # V15 : s'arrete a Y 180,5, avant l'arrondi du cadre de compression (Y 182)
# V15 (2026-11-26, constat de l'utilisateur) : ces rails ne depassaient le cadre du joint
# que de 1,2 mm et le capot portait une tablette DEVANT eux : ni guidage, ni retenue.
# Ils montent maintenant a Z 16, 7 mm au-dessus du cadre, avec un chanfrein d'entree ;
# leur face exterieure (X 13 et X 197) centre le capot par le bord interieur de sa bride
# (X 12,7 et X 197,3), soit 0,3 mm de jeu de chaque cote. La tablette du capot est retiree.
SIDE_GUIDE_D = 12.8
SIDE_GUIDE_CHAMFER = 2.0
SIDE_RETAINER_GAP = 0.5
SIDE_RETAINER_LOCAL_Z = (
    REAR + SIDE_GUIDE_D - COVER_ASSEMBLY_Z + SIDE_RETAINER_GAP)
SIDE_RETAINER_D = 3.0

# Sous-face technique solidaire de la platine murale. La peau noire est
# co-imprimee avec le noyau clair; les connecteurs ne bougent pas au retrait
# du capot.
TECH_X = BOTTOM_CHAMFER
TECH_W = BOX_W - 2.0 * BOTTOM_CHAMFER
# La sous-face fixe rejoint exactement le debut global de la facette avant.
# L'ouverture correspondante du capot est exprimee dans le repere local du
# capot, translate de COVER_ASSEMBLY_Z dans l'assemblage.
TECH_FRONT_Z = COVER_ASSEMBLY_Z + COVER_FRONT_START
TECH_D = TECH_FRONT_Z
TECH_OPENING_LOCAL_D = TECH_D - COVER_ASSEMBLY_Z + 0.2
TECH_TOTAL_T = 3.2
TECH_SKIN_T = 0.8
# La tablette inferieure porte les connecteurs et forme un porte-a-faux de
# 88,2 mm. Un gousset triangulaire continu transforme sa liaison avec la
# platine en section en L rigide. Les recouvrements de 0,2 mm evitent les
# contacts simplement tangents lors des fusions booleennes.
TECH_ROOT_GUSSET_LEG = 8.0
TECH_ROOT_GUSSET_OVERLAP = 0.2

PG7_D = 13.0
PG7_BODY_D = 18.0
PG7_ROW_Z = 49.0
PG7_LAYOUT = ((44.0,PG7_ROW_Z),(68.0,PG7_ROW_Z),(88.0,PG7_ROW_Z))
PG7_X = tuple(x for x,z in PG7_LAYOUT)
PG7_FUNCTIONS = ('secteur_RPS','commande_filtration','commande_eclairage')
PG7_NUT_LENGTH = 8.0  # Enveloppe interne conservative, dimension reelle a verifier.

# V15 : deux cartes. PCB_* designe la carte PRINCIPALE (kicad/v3-principale), contour
# 119,05 x 100,05. La carte de MESURE (kicad/v3-mesure-std), 73,05 x 54,05, est dessous,
# ses deux prises BNC sur les axes X du V14. Les trous sont releves sur les fichiers
# KiCad, depuis le coin HAUT-gauche de chaque carte (Y KiCad vers le bas).
PCB_W = 119.05
PCB_H = 100.05
PCB_T = 1.6
PCB_X = 76.95  # bord droit a X 196,0 comme le V14 : 1 mm du capot
PCB_Y = 62.0
MAIN_HOLES_KICAD = ((4.03, 4.03), (90.03, 4.03), (4.03, 96.03), (115.03, 96.03))
MEAS_W = 73.05
MEAS_H = 54.05
MEAS_BNC_KICAD_X = (11.03, 47.33)
MEAS_BNC_PIN_FROM_BOTTOM = 6.03
MEAS_X = 123.102 - MEAS_BNC_KICAD_X[0]
# Plan Dosin 801-0080 : face interieure de la paroi a ~6,7 mm de l'ame (mesure sur le
# dessin, pas cotee : a verifier piece en main).
MEAS_Y = 3.2 + 6.7 - MEAS_BNC_PIN_FROM_BOTTOM
MEAS_HOLES_KICAD = ((3.52, 3.52), (69.53, 3.52), (3.52, 25.52), (69.53, 25.52))
BOARDS = (("principale", PCB_X, PCB_Y, PCB_W, PCB_H, MAIN_HOLES_KICAD),
          ("mesure", MEAS_X, MEAS_Y, MEAS_W, MEAS_H, MEAS_HOLES_KICAD))
PCB_HOLE_EDGE = 4.03
PCB_STANDOFF_H = 8.8
PCB_POST_OD = 7.5
PCB_HOLE_D = 3.2
PCB_COMPONENT_ENVELOPE = 25.0
PCB_UNDERSIDE_CLEARANCE = 3.0
# La reserve de 22 mm passe devant le bord gauche du PCB, pas dans l'ecart
# lateral de 7.02 mm avec la carte RPS. Fiches verticales a confirmer.
PCB_LEFT_CONNECTOR_CLEARANCE_MIN = 22.0
PCB_SERVICE_BAY_X = PCB_X
PCB_SERVICE_BAY_W = PCB_LEFT_CONNECTOR_CLEARANCE_MIN
PCB_SERVICE_BAY_Y = 72.0   # V15 : colonne de borniers du bord gauche (Y 74 a 150)
PCB_SERVICE_BAY_H = 80.0
PCB_SERVICE_BAY_Z = 42.0
PCB_SERVICE_BAY_D = 6.0
PCB_GUIDE_CLEARANCE_MIN = 2.0

PSU_W = 50.8
PSU_H = 76.2
PSU_Z = 24.0
PSU_X = 18.8
PSU_Y = 39.94  # Conserver le RPS en place, decouple de la mauvaise cote PCB V12.
PSU_STANDOFF_H = 6.0
PSU_POST_OD = 7.5
PSU_HOLE_D = 3.2
PSU_HOLE_EDGE_X = 3.175
PSU_HOLE_EDGE_Y = 3.175
# V15 : SECOND FORMAT de carte d'alimentation (feature-218, etude du 2026-11-26) :
# 52 x 120 mm, meme axe que la RPS. Fixations : les deux plots du bas de la RPS, et deux
# plots neufs en haut. Les plots du haut de la RPS restent : la carte s'y appuie sans vis.
ALT_PSU_W = 52.0
ALT_PSU_H = 120.0
ALT_PSU_X = PSU_X + PSU_W / 2.0 - ALT_PSU_W / 2.0
ALT_PSU_Y = 39.0
ALT_PSU_COMPONENT_H = 50.0   # transformateur de 48 mm, radiateur du regulateur
ALT_PSU_TOP_POST_Y = 155.0
ALT_PSU_NEW_POSTS = ((PSU_X + PSU_HOLE_EDGE_X, ALT_PSU_TOP_POST_Y),
                     (PSU_X + PSU_W - PSU_HOLE_EDGE_X, ALT_PSU_TOP_POST_Y))

# V15 : prise Dosin 801-0080 (nez filete M12 x 0,75, axe a 9,8 mm au-dessus de la carte).
# Percage rond de 12,5 (le meplat du V14 etait celui de la prise Amphenol).
BNC_X = tuple(MEAS_X + x for x in MEAS_BNC_KICAD_X)
BNC_AXIS_Z = REAR + PCB_STANDOFF_H + PCB_T + 9.8
BNC_PIN_Y = MEAS_Y + MEAS_BNC_PIN_FROM_BOTTOM
BNC_REFERENCE_Y_SHIFT = BNC_PIN_Y - 5.9  # epaulement de la prise
# Percage de 13,0 pour un nez M12 : 0,5 mm de jeu au rayon. La chaine qui place l'axe (fond,
# plot imprime, epaisseur de carte, prise a +/-0,3 selon son plan, brasure) peut s'ecarter
# d'environ 0,7 mm : 12,5 ne laissait que 0,25. La collerette de la prise fait 14.
BNC_CUTOUT_D = 13.0
BNC_FLAT_OFFSET = 7.0
BNC_BODY_W = 14.0
BNC_BODY_H = 12.5
BNC_REAR_LENGTH = 10.9
BNC_FRONT_LENGTH = 18.6

# Trois vis M4 accessibles uniquement sous le boitier. Elles traversent la
# sous-face fixe et se vissent dans des inserts thermofixes places dans le
# capot. Les axes sont dans une zone libre entre PG7 et BNC.
LOWER_LOCK_X = (18.0, 105.0, 192.0)
LOWER_LOCK_GLOBAL_Z = BODY_D - 10.6
LOWER_LOCK_LOCAL_Z = LOWER_LOCK_GLOBAL_Z - COVER_ASSEMBLY_Z
LOWER_CLEAR_D = 4.5
LOWER_HEAD_D = 8.5
LOWER_COUNTERSINK_DEPTH = (LOWER_HEAD_D - LOWER_CLEAR_D) / 2.0
LOWER_BOSS_OD = 13.0
LOWER_BOSS_LENGTH = 13.0
LOWER_INSERT_OD = 6.3
LOWER_INSERT_HOLE_D = 5.6
LOWER_INSERT_LENGTH = 4.0
LOWER_INSERT_SEAT_DEPTH = 5.0
LOWER_SCREW_LENGTH = 16.0
LOWER_SCREW_SHAFT_D = 4.0
LOWER_SCREW_HEAD_DEPTH = (LOWER_HEAD_D - LOWER_SCREW_SHAFT_D) / 2.0
LOWER_DRIVER_D = 8.0
LOWER_DRIVER_APPROACH = 40.0
LOWER_HEAD_EDGE_MARGIN_MIN = 0.5
LOWER_SEAL_WEB_MIN = 1.2

# V6 : joint spatial continu sur la veritable interface platine/capot.
# Le joint arriere est un U. Ses extremites descendent par deux coudes
# courbes sur les rives de la tablette, puis se rejoignent cote facade.
# Il n'existe plus de traverse fixe separant deux interfaces non etanches.
SEAL_REAR_END_Y = 20.0
SEAL_ELBOW_R = SEAL_REAR_END_Y - TECH_TOTAL_T
SEAL_DECK_START_Z = SEAL_FACE_Z + SEAL_ELBOW_R
SEAL_DECK_X = 11.3
SEAL_DECK_FRONT_Z = BODY_D - 16.2
SEAL_DECK_FRONT_R = 4.0
SEAL_LAND_W = 5.4
# La contre-face plane fait 3,6 mm. Les supports fixes des coudes restent
# a 5,4 mm : ne pas amincir leurs rives pour gagner le passage des cables.
SEAL_DECK_LAND_W = 3.6
SEAL_CABLE_CLEARANCE_MIN = 1.0
GASKET_SUPPORT_WALL = min(GASKET_SUPPORT_WALL, (SEAL_LAND_W - GASKET_W)/2)
SEAL_ELBOW_STEPS = 32  # echantillonnage du controle, PAS la geometrie
LOWER_LOCATOR_X = ()  # V6 : aucun ergot ou logement de cle inferieur
# Rattrapage du jeu des retenues, sans clips flexibles.
BACKPLATE_HOOK_NOSE_Z0 = 8.8
HOOK_PULL_CLEARANCE = BACKPLATE_HOOK_NOSE_Z0 - (
    COVER_ASSEMBLY_Z + COVER_HOOK_Z0 + COVER_HOOK_D)
SIDE_RETAINER_GAP = 0.15
SIDE_RETAINER_LOCAL_Z = REAR + SIDE_GUIDE_D - COVER_ASSEMBLY_Z + SIDE_RETAINER_GAP

# V14 : fixations murales conservees, marge gagnee par translation PCB en Y.
WALL_ACCESS_MOUNT = (191.0, 7.7)  # V15 : +1, pour 2 mm entre la tete de vis et la carte de mesure
WALL_LEFT_LOWER_MOUNT = (24.0, 7.7)
WALL_LOWER_MOUNTS = (WALL_LEFT_LOWER_MOUNT, WALL_ACCESS_MOUNT)
WALL_MOUNTS = (WALL_LEFT_LOWER_MOUNT, WALL_ACCESS_MOUNT,
               (25.0, 178.0), (185.0, 178.0))  # V15 : au-dessus du capot d'alimentation allonge
WALL_MOUNT_D = 4.5
WALL_MOUNT_HEAD_D = 9.0
WALL_MOUNT_BOSS_D = 13.0
WALL_MOUNT_BOSS_H = 5.0
WALL_ACCESS_HEAD_MAX_D = 7.5
WALL_ACCESS_RECESS_D = 8.5
WALL_HEAD_MAX_H = 4.0
WALL_HEAD_SEAT_Z = REAR + WALL_MOUNT_BOSS_H - 2.2
WALL_DRIVER_D = 6.0
WALL_ACCESS_END_Z = BODY_D + 40.0
WALL_ACCESS_CLEARANCE_MIN = 2.0
WALL_ACCESS_RELIEF_Z0 = REAR + WALL_MOUNT_BOSS_H - 0.1

def wall_head_d(x, y):
    return WALL_ACCESS_HEAD_MAX_D if (x, y) in WALL_LOWER_MOUNTS else WALL_MOUNT_HEAD_D

def wall_recess_d(x, y):
    return WALL_ACCESS_RECESS_D if (x, y) in WALL_LOWER_MOUNTS else WALL_MOUNT_HEAD_D

def build_wall_head(x, y):
    # Enveloppe conservatrice de tete, sans empreinte de tournevis.
    return Part.makeCylinder(wall_head_d(x, y)/2, WALL_HEAD_MAX_H,
                             vec(x, y, WALL_HEAD_SEAT_Z))

def build_wall_screw_reference(x, y):
    head = build_wall_head(x, y)
    shaft = Part.makeCylinder(4.0/2, WALL_HEAD_SEAT_Z+20.0, vec(x, y, -20.0))
    return head.fuse(shaft)

def build_wall_access_relief():
    # Seulement le gousset AU-DESSUS de l'assise du bossage : le fond plein
    # de 3,2 mm et la tablette ne sont pas entailles pour liberer l'outil.
    return Part.makeCompound([Part.makeCylinder(WALL_ACCESS_RECESS_D/2, 12.0,
                             vec(x, y, WALL_ACCESS_RELIEF_Z0))
                             for x,y in WALL_LOWER_MOUNTS])

# Capot de securite de l'alimentation.
GUARD_CLEARANCE = 5.0
GUARD_OFFSET_X = 1.0  # Supprimer le chevauchement ancien de la paroi sur le cadre fixe.
GUARD_T = 2.2
GUARD_H = 58.0  # V15 : couvre 50 mm de composants du second format (V14 : 34, pour la RPS)
GUARD_W = PSU_W + 2.0 * GUARD_CLEARANCE
GUARD_L = (ALT_PSU_Y + ALT_PSU_H + 1.0 + GUARD_T) - (PSU_Y - GUARD_CLEARANCE)  # V15 : un seul capot pour les trois alimentations
GUARD_TAB_W = 10.0
GUARD_TAB_D = 8.0
GUARD_MOUNT_X = (10.0, GUARD_W - 10.0)
GUARD_SCREW_D = 3.4
GUARD_PILOT_D = 2.8
GUARD_BOSS_OD = 9.0
GUARD_BOSS_H = 3.0
MAINS_BARRIER_X = 54.5
MAINS_BARRIER_T = 2.2
# V15 : la cloison suit la profondeur du boitier (V14 : 70, extremite Z73,2). Oubliee lors du
# premier agrandissement : elle laissait 12,8 mm d'ouverture devant le capot au lieu de 2,8.
MAINS_BARRIER_CAP_GAP_MAX = 3.0
MAINS_BARRIER_H = BODY_D - 24.0  # 2,8 mm avant la contre-face mobile.
MAINS_BARRIER_START_Y = TECH_TOTAL_T
# Les pattes sont ecartees : prolonger jusqu'a 0,5 mm du cache, sans contact rigide.
GUARD_BARRIER_GAP = .5
GUARD_BAFFLE_T = 2.2
GUARD_BAFFLE_OVERLAP = 3.0
GUARD_BAFFLE_BACK_GAP = .5
MAINS_BARRIER_END_Y = PSU_Y - GUARD_CLEARANCE - GUARD_BARRIER_GAP
# Hypothese de montage explicite : CN1 en bas, CN2 en haut. Le plan officiel
# cote les cartes, pas les positions exactes des faisceaux : verifier le reel.
GUARD_PORT_X = 44.0 - (PSU_X - GUARD_CLEARANCE + GUARD_OFFSET_X)
GUARD_PORT_W = 14.0
GUARD_PORT_H = 18.0
GUARD_CABLE_D = 6.0

def build_mains_barrier():
    barrier = Part.makeBox(MAINS_BARRIER_T,
        MAINS_BARRIER_END_Y-MAINS_BARRIER_START_Y, MAINS_BARRIER_H,
        vec(MAINS_BARRIER_X,MAINS_BARRIER_START_Y,REAR))
    return safe_refine(barrier)


def build_guard_barrier_baffle():
    """Retour solidaire du cache, cote secteur; coordonnees locales du cache.

    Recouvre lateralement la fin de cloison sans croisement des volumes.
    Sa jupe arriere reste a 0,5 mm de la platine pour permettre le retrait Z+.
    Ce labyrinthe mecanique ne certifie aucune distance d'isolation electrique.
    """
    gx=PSU_X-GUARD_CLEARANCE+GUARD_OFFSET_X
    gy=PSU_Y-GUARD_CLEARANCE
    gz=REAR+GUARD_BOSS_H
    x1=MAINS_BARRIER_X-GUARD_BARRIER_GAP
    y0=MAINS_BARRIER_END_Y-GUARD_BAFFLE_OVERLAP
    z0=REAR+GUARD_BAFFLE_BACK_GAP
    return Part.makeBox(GUARD_BAFFLE_T,gy+1.0-y0,gz+GUARD_H-z0,
        vec(x1-GUARD_BAFFLE_T-gx,y0-gy,z0-gz))

def build_pg7_body(x,z,internal=False):
    return Part.makeCylinder(PG7_BODY_D/2,PG7_NUT_LENGTH if internal else 20,
        vec(x,TECH_TOTAL_T if internal else 0,z),vec(0,1 if internal else -1,0))

def build_pg7_corridors(layout=PG7_LAYOUT):
    return Part.makeCompound([Part.makeBox(PG7_BODY_D,20,
        PG7_CABLE_INSERTION_KEEP_Z1-(z-PG7_BODY_D/2),
        vec(x-PG7_BODY_D/2,0,z-PG7_BODY_D/2)) for x,z in layout])

def guard_port_cutter(y):
    radius=GUARD_PORT_W/2
    centre_z=GUARD_PORT_H-radius
    box=Part.makeBox(GUARD_PORT_W,GUARD_T+.4,centre_z+.2,
        vec(GUARD_PORT_X-radius,y,-.2))
    arch=Part.makeCylinder(radius,GUARD_T+.4,
        vec(GUARD_PORT_X,y,centre_z),vec(0,1,0))
    return box.fuse(arch)

def guard_port_probe(y):
    # Faisceau nominal de controle, a l'axe du sommet arrondi ; traverse
    # uniquement la paroi, sans pretendre modeliser la fiche JST complete.
    return Part.makeCylinder(GUARD_CABLE_D/2,GUARD_T+2,
        vec(GUARD_PORT_X,y-.8,GUARD_PORT_H-GUARD_PORT_W/2),vec(0,1,0))

def build_wiring_routes():
    gx=PSU_X-GUARD_CLEARANCE+GUARD_OFFSET_X
    gy=PSU_Y-GUARD_CLEARANCE
    gz=REAR+GUARD_BOSS_H
    px=gx+GUARD_PORT_X
    pz=gz+GUARD_PORT_H-GUARD_PORT_W/2
    turn_y=gy-12
    fx,fz=PG7_LAYOUT[1]
    lx,lz=PG7_LAYOUT[2]
    routes={
      'secteur':[(PG7_LAYOUT[0][0],0,PG7_LAYOUT[0][1]),
                 (px,15,PG7_LAYOUT[0][1]),(px,turn_y,pz),
                 (px,PSU_Y-GUARD_CABLE_D/2-.4,pz)],
      # V15 : sort par le haut du capot allonge, monte, puis redescend sur le bornier 12 V
      # (il contourne par le haut l'acces au tournevis des vis du capot d'alimentation)
      'basse_tension':[(px,gy+GUARD_L-2.0,pz),(px,gy+GUARD_L+4.8,pz),(px,gy+GUARD_L+4.8,45),
                       (56,gy+GUARD_L+4.8,45),(56,gy+GUARD_L+13.3,45),
                       (84,gy+GUARD_L+13.3,45),(84,150,45)],  # passe entre les pattes du capot d'alimentation, puis a droite du crochet du capot
      'commande_filtration':[(fx,0,fz),(fx,20,fz),(82,20,fz),
                              (82,44,fz),(82,54,45),(82,131,45)],
      'commande_eclairage':[(lx,0,lz),(lx,33,lz),(94,33,lz),
                            (94,62,lz),(94,72,45),(94,120,45)]}
    shapes={}
    for name,points in routes.items():
        spheres=[Part.makeSphere(GUARD_CABLE_D/2,vec(*p)) for p in points]
        cylinders=[]
        for a,b in zip(points,points[1:]):
            a,b=vec(*a),vec(*b)
            direction=b-a
            cylinders.append(Part.makeCylinder(GUARD_CABLE_D/2,direction.Length,a,direction))
        shape=spheres[0]
        for other in spheres[1:]+cylinders:
            shape=shape.fuse(other)
        shapes[name]=safe_refine(shape)
    return shapes


# ---------------------------------------------------------------------------
# Outils geometriques
# ---------------------------------------------------------------------------

def vec(x=0.0, y=0.0, z=0.0):
    return App.Vector(float(x), float(y), float(z))


def safe_refine(shape):
    refined = shape.copy().removeSplitter()
    # OCC peut invalider les lofts lors de l'unification des faces.
    # Conserver alors le BRep booleen valide, jamais un solide invalide.
    return refined if refined.isValid() else shape.copy()


def polygon_wire_xy(points, z=0.0):
    vertices = [vec(x, y, z) for x, y in points]
    return Part.makePolygon(vertices + [vertices[0]])


def polygon_prism_xy(points, z, depth):
    return Part.Face(polygon_wire_xy(points, z)).extrude(vec(0, 0, depth))


def polygon_wire_xz(points, y=0.0):
    vertices = [vec(x, y, z) for x, z in points]
    return Part.makePolygon(vertices + [vertices[0]])


def polygon_prism_xz(points, y, depth):
    return Part.Face(polygon_wire_xz(points, y)).extrude(vec(0, depth, 0))


def polygon_wire_yz(points, x=0.0):
    vertices = [vec(x, y, z) for y, z in points]
    return Part.makePolygon(vertices + [vertices[0]])


def polygon_prism_yz(points, x, depth):
    return Part.Face(polygon_wire_yz(points, x)).extrude(vec(depth, 0, 0))


def faceted_points(w, h, top, bottom, x=0.0, y=0.0):
    return [
        (x + bottom, y),
        (x + w - bottom, y),
        (x + w, y + bottom),
        (x + w, y + h - top),
        (x + w - top, y + h),
        (x + top, y + h),
        (x, y + h - top),
        (x, y + bottom),
    ]


def rectangle_points_xz(w, h, x=0.0, z=0.0):
    return [(x, z), (x + w, z), (x + w, z + h), (x, z + h)]


def ribbon_points(x=RIBBON_X, y=RIBBON_Y, w=RIBBON_W,
                  h=RIBBON_H, tip=RIBBON_TIP):
    return [
        (x + tip, y), (x + w - tip, y),
        (x + w, y + tip), (x + w, y + h - tip),
        (x + w - tip, y + h), (x + tip, y + h),
        (x, y + h - tip), (x, y + tip),
    ]


def accent_points(x=ACCENT_X, y=ACCENT_Y, w=ACCENT_W,
                  h=ACCENT_H, tip=ACCENT_TIP):
    return [
        (x + tip, y),
        (x + w - tip, y),
        (x + w, y + h / 2.0),
        (x + w - tip, y + h),
        (x + tip, y + h),
        (x, y + h / 2.0),
    ]


def rounded_prism(w, h, d, r, x=0.0, y=0.0, z=0.0):
    r = max(0.01, min(float(r), float(w) / 2.0, float(h) / 2.0))
    pieces = [
        Part.makeBox(w - 2.0 * r, h, d, vec(x + r, y, z)),
        Part.makeBox(w, h - 2.0 * r, d, vec(x, y + r, z)),
    ]
    for cx, cy in ((x + r, y + r), (x + w - r, y + r),
                   (x + r, y + h - r), (x + w - r, y + h - r)):
        pieces.append(Part.makeCylinder(r, d, vec(cx, cy, z)))
    return pieces[0].multiFuse(pieces[1:]).removeSplitter()


def rounded_ring(w, h, width, depth, radius, x, y, z):
    outer = rounded_prism(w, h, depth, radius, x, y, z)
    inner = rounded_prism(
        w - 2.0 * width, h - 2.0 * width, depth + 0.4,
        max(0.5, radius - width), x + width, y + width, z - 0.2)
    return outer.cut(inner)


def ring_post_z(outer_d, inner_d, height, x, y, z):
    outer = Part.makeCylinder(outer_d / 2.0, height, vec(x, y, z))
    inner = Part.makeCylinder(inner_d / 2.0, height + 0.4,
                              vec(x, y, z - 0.2))
    return outer.cut(inner)


def ring_post_y(outer_d, inner_d, height, x, y, z):
    outer = Part.makeCylinder(outer_d / 2.0, height,
                              vec(x, y, z), vec(0, 1, 0))
    inner = Part.makeCylinder(inner_d / 2.0, height + 0.4,
                              vec(x, y - 0.2, z), vec(0, 1, 0))
    return outer.cut(inner)


def build_lower_screw_head(x, axis_z=None):
    """Tete M4 a 90 degres, affleurante dessous ; diametre nominal 8,5."""
    z = LOWER_LOCK_GLOBAL_Z if axis_z is None else axis_z
    return Part.makeCone(LOWER_HEAD_D/2,LOWER_SCREW_SHAFT_D/2,
                         LOWER_SCREW_HEAD_DEPTH,vec(x,0,z),vec(0,1,0))


def build_lower_screw(x):
    return build_lower_screw_head(x).fuse(Part.makeCylinder(
        LOWER_SCREW_SHAFT_D/2,LOWER_SCREW_LENGTH-LOWER_SCREW_HEAD_DEPTH,
        vec(x,LOWER_SCREW_HEAD_DEPTH,LOWER_LOCK_GLOBAL_Z),vec(0,1,0)))


def build_backplate_top_hook(x):
    """Crochet fixe en L, avec levre orientee vers la facade."""
    base = Part.makeBox(
        TOP_HOOK_W, BACKPLATE_CLEAT_H, BACKPLATE_CLEAT_D,
        vec(x, BACKPLATE_CLEAT_Y, BACKPLATE_CLEAT_Z0))
    nose = Part.makeBox(
        TOP_HOOK_W, BACKPLATE_HOOK_NOSE_H, BACKPLATE_HOOK_NOSE_D,
        vec(x, BACKPLATE_HOOK_NOSE_Y, BACKPLATE_HOOK_NOSE_Z0))
    return base.fuse(nose)


def build_cover_top_catch(x):
    """Languette captive du capot et liaison au cadre de compression."""
    tongue = Part.makeBox(
        TOP_HOOK_W, COVER_HOOK_H, COVER_HOOK_D,
        vec(x, COVER_HOOK_Y, COVER_HOOK_Z0))
    spine = Part.makeBox(
        TOP_HOOK_W, COVER_HOOK_SPINE_H, COVER_HOOK_SPINE_D,
        vec(x, COVER_HOOK_SPINE_Y, COVER_HOOK_SPINE_Z0))
    bridge = Part.makeBox(
        TOP_HOOK_W, 18.2, 5.0,
        vec(x, COVER_HOOK_SPINE_Y - 2.4, 12.4))  # V15 : suit les crochets (V14 : 154,0)
    return tongue.fuse(spine).fuse(bridge)


def d_cutout_y(diameter, flat_offset, length, x, y, z):
    radius = diameter / 2.0
    circle = Part.makeCylinder(radius, length, vec(x, y, z), vec(0, 1, 0))
    keep = Part.makeBox(diameter + 2.0, length, radius + flat_offset + 1.0,
                        vec(x - radius - 1.0, y, z - radius - 1.0))
    return circle.common(keep)


def loft_solid_xy(points_a, z_a, points_b, z_b):
    return Part.makeLoft([
        polygon_wire_xy(points_a, z_a),
        polygon_wire_xy(points_b, z_b),
    ], True, False)


def cover_front_points():
    """Contour de la face avant, avec facette basse discrete."""
    return [
        (14.0, SHELL_BOTTOM_INSET),
        (BOX_W - 14.0, SHELL_BOTTOM_INSET),
        (BOX_W - SHELL_FRONT_INSET, 9.0),
        (BOX_W - SHELL_FRONT_INSET,
         BOX_H - SHELL_FRONT_INSET - (TOP_CHAMFER - 1.0)),
        (BOX_W - SHELL_FRONT_INSET - (TOP_CHAMFER - 1.0),
         BOX_H - SHELL_FRONT_INSET),
        (SHELL_FRONT_INSET + (TOP_CHAMFER - 1.0),
         BOX_H - SHELL_FRONT_INSET),
        (SHELL_FRONT_INSET,
         BOX_H - SHELL_FRONT_INSET - (TOP_CHAMFER - 1.0)),
        (SHELL_FRONT_INSET, 9.0),
    ]


def build_cover_outer_envelope():
    """Volume plein limite par toutes les faces exterieures du capot."""
    outer_pts = faceted_points(
        BOX_W, BOX_H, TOP_CHAMFER, BOTTOM_CHAMFER)
    rear = polygon_prism_xy(outer_pts, 0.0, COVER_FRONT_START + 0.2)
    front = loft_solid_xy(
        outer_pts, COVER_FRONT_START, cover_front_points(), COVER_D)
    return rear.fuse(front).removeSplitter()


def ribbon_side_returns(z0=RIBBON_Z):
    """Peaux conformes aux deux facettes laterales du capot."""
    facet_length = math.hypot(SHELL_FRONT_INSET, FRONT_CORE_T)
    inward_dx = FRONT_CORE_T / facet_length * RIBBON_SKIN_T
    inward_dz = -SHELL_FRONT_INSET / facet_length * RIBBON_SKIN_T
    outer_z = RIBBON_SKIN_T - FRONT_CORE_T * RIBBON_RETURN_W / SHELL_FRONT_INSET
    y0 = RIBBON_Y + RIBBON_TIP
    span = RIBBON_H - 2.0 * RIBBON_TIP

    left_outer_x = RIBBON_X - RIBBON_RETURN_W
    left = [
        (RIBBON_X, RIBBON_SKIN_T),
        (left_outer_x, outer_z),
        (left_outer_x + inward_dx, outer_z + inward_dz),
        (RIBBON_X + inward_dx, RIBBON_SKIN_T + inward_dz),
    ]
    right_seam_x = RIBBON_X + RIBBON_W
    right_outer_x = right_seam_x + RIBBON_RETURN_W
    right = [
        (right_seam_x, RIBBON_SKIN_T),
        (right_seam_x - inward_dx, RIBBON_SKIN_T + inward_dz),
        (right_outer_x - inward_dx, outer_z + inward_dz),
        (right_outer_x, outer_z),
    ]
    shapes = [polygon_prism_xz(left, y0, span),
              polygon_prism_xz(right, y0, span)]
    for shape in shapes:
        shape.translate(vec(0, 0, z0))
    return shapes


def parameter_object(doc, entries):
    obj = doc.addObject("App::FeaturePython", "Parameters")
    obj.Label = "Dimensions maitresses (mm)"
    for name, label, value in entries:
        obj.addProperty("App::PropertyLength", name, "Dimensions", label)
        setattr(obj, name, float(value))
    return obj


def save_part(doc_name, object_name, label, shape, color, params,
              export_mesh=True):
    cleaned = safe_refine(shape)
    doc = App.newDocument(doc_name)
    parameter_object(doc, params)
    feature = doc.addObject("PartDesign::Feature", object_name)
    feature.Label = label
    feature.Shape = cleaned
    feature.addProperty("App::PropertyString", "Material", "Fabrication")
    feature.addProperty("App::PropertyString", "ValidationNote", "Fabrication")
    feature.Material = "Voir FABRICATION.md"
    feature.ValidationNote = (
        "Prototype: valider les AliExpress reelles, les inserts, le joint et la "
        "securite secteur avant fabrication definitive."
    )
    if feature.ViewObject:
        feature.ViewObject.ShapeColor = color
        feature.ViewObject.LineColor = (0.12, 0.12, 0.12)
    doc.recompute()

    fc_path = FC_DIR / f"{doc_name}.FCStd"
    step_path = STEP_DIR / f"{doc_name}.step"
    doc.saveAs(str(fc_path))
    backup = Path(str(fc_path) + "1")
    if backup.exists():
        backup.unlink()
    Part.export([feature], str(step_path))
    mesh_closed = True
    if export_mesh:
        mesh = MeshPart.meshFromShape(Shape=cleaned.copy(), LinearDeflection=0.05,
                                     AngularDeflection=0.15, Relative=False)
        mesh_closed = mesh.isSolid() and mesh.countComponents() == 1
        mesh.write(str(STL_DIR / f"{doc_name}.stl"))

    valid = cleaned.cleaned().isValid()
    solids = len(cleaned.Solids)
    volume = cleaned.Volume
    bbox = cleaned.BoundBox
    print(f"{doc_name}: valid={valid} solids={solids} volume={volume:.1f} mm3")
    App.closeDocument(doc_name)
    for auto_backup in FC_DIR.glob(f"{doc_name}*.FCBak"):
        auto_backup.unlink()
    if not valid or not mesh_closed or solids != 1 or volume <= 0:
        raise RuntimeError(f"Piece non imprimable: {doc_name}")
    return {
        "name": doc_name,
        "volume_mm3": round(volume, 2),
        "bbox_mm": [round(bbox.XLength, 2), round(bbox.YLength, 2),
                    round(bbox.ZLength, 2)],
        "mesh_exported": export_mesh,
        "mesh_closed_connected": mesh_closed,
    }


# ---------------------------------------------------------------------------
# Platine murale
# ---------------------------------------------------------------------------

def seal_elbow_station(theta, right=False):
    x = GASKET_PATH_X
    if right:
        x = BOX_W - x
    p = vec(x, SEAL_REAR_END_Y - SEAL_ELBOW_R * math.sin(theta),
            SEAL_DECK_START_Z - SEAL_ELBOW_R * math.cos(theta))
    normal = vec(0, math.sin(theta), math.cos(theta))
    return p, normal


def seal_profile(p, normal, x0, x1, d0, d1):
    corners = [vec(x, p.y, p.z) + normal * d
               for x, d in [(x0, d0), (x1, d0), (x1, d1), (x0, d1)]]
    return Part.makePolygon(corners + [corners[0]])


def seal_elbow_solid(mode, right=False):
    """Secteur annulaire EXACT extrude selon X : aucun loft ni decalage en S."""
    p, _ = seal_elbow_station(0, right)
    if mode == "groove":
        x0, x1 = p.x - GASKET_W/2, p.x + GASKET_W/2
        d0, d1 = -GASKET_DEPTH, 0.2
    elif mode == "fixed":
        x0, x1 = p.x - SEAL_LAND_W/2, p.x + SEAL_LAND_W/2
        d0, d1 = -(SEAL_FACE_Z - REAR), 0.0
    elif mode == "moving":
        x0, x1 = ((p.x - 1.6, BOX_W) if right else (0.0, p.x + 1.6))
        d0, d1 = 0.0, 2.5
    else:
        raise ValueError(mode)
    def point(theta, d):
        return vec(x0, SEAL_REAR_END_Y-(SEAL_ELBOW_R-d)*math.sin(theta),
                   SEAL_DECK_START_Z-(SEAL_ELBOW_R-d)*math.cos(theta))
    a, b = point(0,d0), point(math.pi/2,d0)
    c, d = point(math.pi/2,d1), point(0,d1)
    wire = Part.Wire([
        Part.Arc(a,point(math.pi/4,d0),b).toShape(),
        Part.makeLine(b,c),
        Part.Arc(c,point(math.pi/4,d1),d).toShape(),
        Part.makeLine(d,a)])
    solid = Part.Face(wire).extrude(vec(x1-x0,0,0))
    if mode == "fixed":
        solid = solid.common(Part.makeBox(BOX_W, BOX_H, BODY_D, vec(0, TECH_SKIN_T, 0)))
    return solid


def seal_deck_u(width, normal_start, normal_end):
    """U dans le plan XZ, face de compression horizontale Y=3,2 mm."""
    path_w = BOX_W - 2*SEAL_DECK_X
    back_z = SEAL_DECK_START_Z - 2*SEAL_DECK_FRONT_R
    ring = rounded_ring(path_w + width, SEAL_DECK_FRONT_Z - back_z + width,
                        width, normal_end - normal_start,
                        SEAL_DECK_FRONT_R + width/2,
                        SEAL_DECK_X - width/2, back_z - width/2,
                        -TECH_TOTAL_T - normal_end)
    ring.rotate(vec(0, 0, 0), vec(1, 0, 0), 90)
    return ring.common(Part.makeBox(BOX_W, BOX_H, BODY_D - SEAL_DECK_START_Z,
                                  vec(0, 0, SEAL_DECK_START_Z)))


def build_spatial_groove():
    rear = rounded_ring(GASKET_PATH_W + GASKET_W, GASKET_PATH_H + GASKET_W,
                        GASKET_W, GASKET_DEPTH + 0.2,
                        GASKET_PATH_R + GASKET_W/2,
                        GASKET_PATH_X - GASKET_W/2,
                        GASKET_PATH_Y - GASKET_W/2, SEAL_FACE_Z - GASKET_DEPTH)
    rear = rear.common(Part.makeBox(BOX_W, BOX_H, BODY_D,
                                   vec(0, SEAL_REAR_END_Y, 0)))
    return rear.multiFuse([seal_elbow_solid("groove", False),
                          seal_elbow_solid("groove", True),
                          seal_deck_u(GASKET_W, -GASKET_DEPTH, 0.2)])


def build_lower_cap_sealing_land():
    land = seal_deck_u(SEAL_DECK_LAND_W, 0, 3.8)
    for x0 in (0.0, BOX_W - SEAL_DECK_X - SEAL_LAND_W/2):
        land = land.fuse(Part.makeBox(SEAL_DECK_X + SEAL_LAND_W/2, 3.8,
                          SEAL_DECK_FRONT_Z - SEAL_DECK_START_Z,
                          vec(x0, TECH_TOTAL_T, SEAL_DECK_START_Z)))
    # Continuite vers la facade : aucun pont horizontal suspendu a imprimer.
    land = land.fuse(Part.makeBox(BOX_W, 3.8,
                     BODY_D - SEAL_DECK_FRONT_Z + SEAL_DECK_LAND_W/2,
                     vec(0, TECH_TOTAL_T, SEAL_DECK_FRONT_Z - SEAL_DECK_LAND_W/2)))
    land = land.multiFuse([seal_elbow_solid("moving", False),
                          seal_elbow_solid("moving", True)])
    land.translate(vec(0, 0, -COVER_ASSEMBLY_Z))
    return land.common(build_cover_outer_envelope())

def build_backplate_technical_skin():
    skin = Part.makeBox(TECH_W, TECH_SKIN_T, TECH_D,
                        vec(TECH_X, 0.0, 0.0))
    for x,z in PG7_LAYOUT:
        skin = skin.cut(Part.makeCylinder(
            PG7_D / 2.0, TECH_SKIN_T + 0.4,
            vec(x, -0.2, z), vec(0, 1, 0)))
    for x in BNC_X:
        skin = skin.cut(d_cutout_y(
            BNC_CUTOUT_D, BNC_FLAT_OFFSET, TECH_SKIN_T + 0.4,
            x, -0.2, BNC_AXIS_Z))
    for x in LOWER_LOCK_X:
        skin = skin.cut(Part.makeCylinder(
            LOWER_CLEAR_D / 2.0, TECH_SKIN_T + 0.4,
            vec(x, -0.2, LOWER_LOCK_GLOBAL_Z), vec(0, 1, 0)))
        skin = skin.cut(Part.makeCone(
            LOWER_HEAD_D / 2.0, LOWER_CLEAR_D / 2.0,
            LOWER_COUNTERSINK_DEPTH, vec(x, 0.0, LOWER_LOCK_GLOBAL_Z),
            vec(0, 1, 0)))
    return skin.removeSplitter()


def build_wall_backplate():
    # Platine pleine grandeur : meme contour exterieur que le capot. Son chant
    # arriere ferme donc toute la silhouette sans retrait peripherique.
    plate_pts = faceted_points(
        BOX_W, BOX_H, TOP_CHAMFER, BOTTOM_CHAMFER)
    plate = polygon_prism_xy(plate_pts, 0.0, REAR)

    # Sous-face porte-connecteurs. On retire la peau noire de 0,8 mm afin que
    # les deux volumes multi-materiaux soient jointifs sans recouvrement.
    deck = Part.makeBox(TECH_W, TECH_TOTAL_T, TECH_D,
                        vec(TECH_X, 0.0, 0.0))
    plate = plate.fuse(deck)
    plate = plate.cut(Part.makeBox(
        TECH_W, TECH_SKIN_T, TECH_D,
        vec(TECH_X, 0.0, 0.0)))

    # Renfort continu de la racine de la sous-face. Sa section YZ est un
    # triangle rectangle de 8 x 8 mm. Il recouvre de 0,2 mm la face avant de
    # la platine et la face interieure de la tablette afin de produire une
    # fusion robuste, sans monter dans l'enveloppe des BNC ni du PCB.
    plate = plate.fuse(build_technical_deck_root_gusset())

    # Cadre arriere en U, face de joint Z=9 mm.
    ox, oy, ow, oh, radius = SEAL_FRAME_OUTER
    ix, iy, iw, ih, iradius = SEAL_FRAME_INNER
    frame_outer = rounded_prism(
        ow, oh, SEAL_FACE_Z - REAR, radius, ox, oy, REAR)
    frame_inner = rounded_prism(
        iw, ih, SEAL_FACE_Z - REAR + 0.4, iradius,
        ix, iy, REAR - 0.2)
    rear_frame = frame_outer.cut(frame_inner).common(Part.makeBox(
        BOX_W, BOX_H, BODY_D, vec(0, SEAL_REAR_END_Y, 0)))
    plate = plate.fuse(rear_frame)
    plate = plate.fuse(seal_elbow_solid("fixed", False)).fuse(
        seal_elbow_solid("fixed", True))

    # Gorge pour cordon EPDM rond de 2 mm, pose a longueur sans etirement.
    groove_outer = rounded_prism(
        GASKET_PATH_W + GASKET_W,
        GASKET_PATH_H + GASKET_W,
        GASKET_DEPTH + 0.2,
        GASKET_PATH_R + GASKET_W / 2.0,
        GASKET_PATH_X - GASKET_W / 2.0,
        GASKET_PATH_Y - GASKET_W / 2.0,
        SEAL_FACE_Z - GASKET_DEPTH)
    groove_inner = rounded_prism(
        GASKET_PATH_W - GASKET_W,
        GASKET_PATH_H - GASKET_W,
        GASKET_DEPTH + 0.5,
        GASKET_PATH_R - GASKET_W / 2.0,
        GASKET_PATH_X + GASKET_W / 2.0,
        GASKET_PATH_Y + GASKET_W / 2.0,
        SEAL_FACE_Z - GASKET_DEPTH - 0.2)
    plate = plate.cut(build_spatial_groove())
    # Crochets superieurs en L. Leur levre vient devant la languette captive
    # du capot et bloque donc une traction vers l'avant.
    for x in TOP_HOOK_X:
        plate = plate.fuse(build_backplate_top_hook(x))

    # Deux rails lateraux continus de guidage, sans fonction de clipsage.
    for y in SIDE_GUIDE_Y:
        z0, z1, c = REAR, REAR + SIDE_GUIDE_D, SIDE_GUIDE_CHAMFER
        xl, xr = SIDE_GUIDE_LEFT_X, SIDE_GUIDE_RIGHT_X
        # profil XZ, chanfrein du cote exterieur (celui qui recoit la bride du capot)
        left = polygon_prism_xz([(xl, z0), (xl + SIDE_GUIDE_W, z0), (xl + SIDE_GUIDE_W, z1),
                                 (xl + c, z1), (xl, z1 - c)], y, SIDE_GUIDE_H)
        right = polygon_prism_xz([(xr, z0), (xr + SIDE_GUIDE_W, z0), (xr + SIDE_GUIDE_W, z1 - c),
                                  (xr + SIDE_GUIDE_W - c, z1), (xr, z1)], y, SIDE_GUIDE_H)
        plate = plate.fuse(left).fuse(right)

    # Entretoises PCB a droite.
    for x, y in pcb_hole_centres():
        plate = plate.fuse(ring_post_z(
            PCB_POST_OD, PCB_HOLE_D, PCB_STANDOFF_H, x, y, REAR))

    # Entretoises Mean Well a gauche, alimentation tournee dans le plan.
    for x in (PSU_X + PSU_HOLE_EDGE_X, PSU_X + PSU_W - PSU_HOLE_EDGE_X):
        for y in (PSU_Y + PSU_HOLE_EDGE_Y, PSU_Y + PSU_H - PSU_HOLE_EDGE_Y):
            plate = plate.fuse(ring_post_z(
                PSU_POST_OD, PSU_HOLE_D, PSU_STANDOFF_H, x, y, REAR))

    # V15 : deux plots neufs pour le second format de carte d'alimentation.
    for x, y in ALT_PSU_NEW_POSTS:
        plate = plate.fuse(ring_post_z(
            PSU_POST_OD, PSU_HOLE_D, PSU_STANDOFF_H, x, y, REAR))

    # Points de fixation du capot de securite de l'alimentation.
    guard_x0 = PSU_X - GUARD_CLEARANCE + GUARD_OFFSET_X
    guard_y0 = PSU_Y - GUARD_CLEARANCE
    for local_x in GUARD_MOUNT_X:
        for local_y in (-GUARD_TAB_D / 2.0,
                        GUARD_L + GUARD_TAB_D / 2.0):
            plate = plate.fuse(ring_post_z(
                GUARD_BOSS_OD, GUARD_PILOT_D, GUARD_BOSS_H,
                guard_x0 + local_x, guard_y0 + local_y, REAR))

    # Cloison prolongee jusqu'a la tablette, avec acces mural arrondi et
    # espace de virage avant l'entree CN1 ; pas de traverse devant les PG7.
    plate = plate.fuse(build_mains_barrier())

    # Fixations murales accessibles lorsque le capot est depose.
    for x, y in WALL_MOUNTS:
        plate = plate.fuse(Part.makeCylinder(
            WALL_MOUNT_BOSS_D / 2.0, WALL_MOUNT_BOSS_H,
            vec(x, y, REAR)))
        plate = plate.cut(Part.makeCylinder(
            WALL_MOUNT_D / 2.0, REAR + WALL_MOUNT_BOSS_H + 0.4,
            vec(x, y, -0.2)))
        plate = plate.cut(Part.makeCylinder(
            wall_recess_d(x, y) / 2.0, 2.4,
            vec(x, y, WALL_HEAD_SEAT_Z)))

    # Perforations de la sous-face dans le noyau clair.
    for x,z in PG7_LAYOUT:
        plate = plate.cut(Part.makeCylinder(
            PG7_D / 2.0, TECH_TOTAL_T + 0.4,
            vec(x, -0.2, z), vec(0, 1, 0)))
    for x in BNC_X:
        plate = plate.cut(d_cutout_y(
            BNC_CUTOUT_D, BNC_FLAT_OFFSET, TECH_TOTAL_T + 0.4,
            x, -0.2, BNC_AXIS_Z))
    for x in LOWER_LOCK_X:
        plate = plate.cut(Part.makeCylinder(
            LOWER_CLEAR_D / 2.0, TECH_TOTAL_T + 0.4,
            vec(x, -0.2, LOWER_LOCK_GLOBAL_Z), vec(0, 1, 0)))
        plate = plate.cut(Part.makeCone(
            LOWER_HEAD_D / 2.0, LOWER_CLEAR_D / 2.0,
            LOWER_COUNTERSINK_DEPTH, vec(x, 0.0, LOWER_LOCK_GLOBAL_Z),
            vec(0, 1, 0)))

    return plate.removeSplitter()


def build_technical_deck_root_gusset(with_access_relief=True):
    root_y = TECH_TOTAL_T - TECH_ROOT_GUSSET_OVERLAP
    root_z = REAR - TECH_ROOT_GUSSET_OVERLAP
    section = [
        (root_y, root_z),
        (root_y + TECH_ROOT_GUSSET_LEG, root_z),
        (root_y, root_z + TECH_ROOT_GUSSET_LEG),
    ]
    gusset = polygon_prism_yz(section, TECH_X, TECH_W)
    return gusset.cut(build_wall_access_relief()) if with_access_relief else gusset


# ---------------------------------------------------------------------------
# Capot cloche multi-materiaux
# ---------------------------------------------------------------------------

def build_cover_graphite_skin():
    skin = polygon_prism_xy(ribbon_points(), RIBBON_Z, RIBBON_SKIN_T)
    for side in ribbon_side_returns(RIBBON_Z):
        skin = skin.fuse(side)

    # Les ilots gris affleurants portent directement les AliExpress.
    for cx, cy in PUMP_CENTRES:
        opening = rounded_prism(
            PUMP_PAD_W + PUMP_PAD_CLEARANCE,
            PUMP_PAD_H + PUMP_PAD_CLEARANCE,
            RIBBON_SKIN_T + 0.6, 2.2,
            cx - (PUMP_PAD_W + PUMP_PAD_CLEARANCE) / 2.0,
            cy - (PUMP_PAD_H + PUMP_PAD_CLEARANCE) / 2.0,
            RIBBON_Z - 0.3)
        skin = skin.cut(opening)
    return skin.removeSplitter()


def build_cover_turquoise_inlay():
    visible = polygon_prism_xy(
        accent_points(), ACCENT_VISIBLE_Z, ACCENT_VISIBLE_T)
    anchor = polygon_prism_xy(
        accent_points(
            ACCENT_X - ACCENT_ANCHOR_SPREAD,
            ACCENT_Y - ACCENT_ANCHOR_SPREAD,
            ACCENT_W + 2.0 * ACCENT_ANCHOR_SPREAD,
            ACCENT_H + 2.0 * ACCENT_ANCHOR_SPREAD,
            ACCENT_TIP + ACCENT_ANCHOR_SPREAD),
        ACCENT_ANCHOR_Z, ACCENT_ANCHOR_T + ACCENT_JOIN_OVERLAP)
    return visible.fuse(anchor).removeSplitter()


def build_compression_gusset():
    """Nervure peripherique a 45 degres, cote interieur du capot."""
    outer_pts = faceted_points(BOX_W, BOX_H, TOP_CHAMFER, BOTTOM_CHAMFER)
    compression_inner = (
        COMPRESSION_RING_OUTER[0] + COMPRESSION_RING_W)
    gusset_base_z = COMPRESSION_GUSSET_Z
    gusset_nominal_base_z = COMPRESSION_FLANGE_Z + COMPRESSION_FLANGE_T
    gusset_top_z = gusset_nominal_base_z + COMPRESSION_GUSSET_RISE
    left_gusset = polygon_prism_xz([
        (0.0, gusset_base_z),
        (compression_inner, gusset_base_z),
        (compression_inner, gusset_nominal_base_z),
        (WALL, gusset_top_z),
        (0.0, gusset_top_z),
    ], 0.0, BOX_H)
    right_gusset = polygon_prism_xz([
        (BOX_W - compression_inner, gusset_base_z),
        (BOX_W, gusset_base_z),
        (BOX_W, gusset_top_z),
        (BOX_W - WALL, gusset_top_z),
        (BOX_W - compression_inner, gusset_nominal_base_z),
    ], 0.0, BOX_H)
    bottom_gusset = polygon_prism_yz([
        (0.0, gusset_base_z),
        (compression_inner, gusset_base_z),
        (compression_inner, gusset_nominal_base_z),
        (WALL, gusset_top_z),
        (0.0, gusset_top_z),
    ], 0.0, BOX_W)
    top_gusset = polygon_prism_yz([
        (BOX_H - compression_inner, gusset_base_z),
        (BOX_H, gusset_base_z),
        (BOX_H, gusset_top_z),
        (BOX_H - WALL, gusset_top_z),
        (BOX_H - compression_inner, gusset_nominal_base_z),
    ], 0.0, BOX_W)
    gusset_clip = polygon_prism_xy(
        outer_pts, gusset_base_z - 0.1,
        gusset_top_z - gusset_base_z + 0.2)
    bottom_opening = Part.makeBox(
        TECH_W + 0.4, 4.0, TECH_OPENING_LOCAL_D,
        vec(TECH_X - 0.2, -0.2, -0.1))
    bottom_suspended_relief = Part.makeBox(
        TECH_W + 0.4, BOTTOM_FLANGE_RELIEF_Y1 - 3.8,
        gusset_top_z - gusset_base_z + 0.4,
        vec(TECH_X - 0.2, 3.8, gusset_base_z - 0.2))
    return left_gusset.multiFuse([
        right_gusset, bottom_gusset, top_gusset,
    ]).common(gusset_clip).cut(
        bottom_opening.fuse(bottom_suspended_relief)).removeSplitter()






def build_cover_light_core(graphite_skin, turquoise_inlay):
    outer_pts = faceted_points(BOX_W, BOX_H, TOP_CHAMFER, BOTTOM_CHAMFER)
    inner_pts = faceted_points(
        BOX_W - 2.0 * WALL, BOX_H - 2.0 * WALL,
        TOP_CHAMFER - WALL, BOTTOM_CHAMFER - WALL, WALL, WALL)

    # Parois ouvertes a l'arriere.
    side_outer = polygon_prism_xy(outer_pts, 0.0, COVER_FRONT_START + 0.2)
    side_inner = polygon_prism_xy(
        inner_pts, -0.2, COVER_FRONT_START + 0.4)
    cover = side_outer.cut(side_inner)

    # Façade facettee. La facette basse reste plus discrete que les autres.
    front_pts = cover_front_points()
    front = loft_solid_xy(outer_pts, COVER_FRONT_START,
                          front_pts, COVER_D)
    cover = cover.fuse(front)

    # Ouverture inferieure: la sous-face porte-connecteurs reste sur la
    # platine. Le capot peut donc etre depose sans solliciter les cables.
    bottom_opening = Part.makeBox(
        TECH_W + 0.4, 4.0, TECH_OPENING_LOCAL_D,
        vec(TECH_X - 0.2, -0.2, -0.1))
    cover = cover.cut(bottom_opening)

    # Volumes decoratifs retires du noyau clair.
    cover = cover.cut(graphite_skin)
    cover = cover.cut(turquoise_inlay)

    # Deux ilots gris affleurants, continus avec le noyau structurel.
    for cx, cy in PUMP_CENTRES:
        pad = rounded_prism(
            PUMP_PAD_W, PUMP_PAD_H, RIBBON_SKIN_T + 0.1, 2.0,
            cx - PUMP_PAD_W / 2.0, cy - PUMP_PAD_H / 2.0,
            RIBBON_Z - 0.1)
        cover = cover.fuse(pad)

    # Passages des deux pompes. Les vis sont introduites depuis l'interieur.
    for cx, cy in PUMP_CENTRES:
        cover = cover.cut(Part.makeCylinder(
            PUMP_MOTOR_CUTOUT_D / 2.0, FRONT_CORE_T + 1.0,
            vec(cx, cy, COVER_FRONT_START - 0.5)))
        for dx in (-PUMP_MOUNT_DX / 2.0, PUMP_MOUNT_DX / 2.0):
            cover = cover.cut(Part.makeCylinder(
                PUMP_SCREW_CLEAR_D / 2.0, FRONT_CORE_T + 1.0,
                vec(cx + dx, cy + PUMP_MOUNT_DY,
                    COVER_FRONT_START - 0.5)))

    # Bride structurelle continue jusqu'aux parois. Sa face d'appui est
    # coplanaire avec la zone centree sur le cordon. Les rives du cadre fixe
    # deviennent donc une butee mecanique correspondant a 30 % de compression.
    # L'ouverture interieure reste celle de la zone de compression, donc le
    # jeu avec le capot RPS est conserve.
    seal_x, seal_y, seal_w, seal_h, seal_radius = COMPRESSION_RING_OUTER
    compression_inner_x = seal_x + COMPRESSION_RING_W
    compression_inner_y = seal_y + COMPRESSION_RING_W
    compression_inner_w = seal_w - 2.0 * COMPRESSION_RING_W
    compression_inner_h = seal_h - 2.0 * COMPRESSION_RING_W
    compression_inner_radius = seal_radius - COMPRESSION_RING_W
    flange_outer = polygon_prism_xy(
        outer_pts, COMPRESSION_FLANGE_Z, COMPRESSION_FLANGE_T)
    flange_inner = rounded_prism(
        compression_inner_w, compression_inner_h,
        COMPRESSION_FLANGE_T + 0.4, compression_inner_radius,
        compression_inner_x, compression_inner_y,
        COMPRESSION_FLANGE_Z - 0.2)
    bottom_flange_relief = Part.makeBox(
        TECH_W + 0.4, BOTTOM_FLANGE_RELIEF_Y1 - 3.8,
        COMPRESSION_FLANGE_T + 0.4,
        vec(TECH_X - 0.2, 3.8, COMPRESSION_FLANGE_Z - 0.2))
    structural_flange = flange_outer.cut(flange_inner).cut(
        bottom_opening.fuse(bottom_flange_relief))
    rear_u_keep = Part.makeBox(BOX_W, BOX_H, COVER_D,
                              vec(0, SEAL_REAR_END_Y, 0))
    cover = cover.fuse(structural_flange.common(rear_u_keep))

    # Nervure oblique continue sous la bride lorsque le capot multi-matiere est
    # imprime face avant sur le plateau. Quatre profils trapezoidaux se
    # recouvrent aux angles puis sont limites par le contour facette externe.
    # A leur sommet ils retrouvent exactement l'epaisseur de paroi ; a leur
    # base ils rejoignent le bord interieur de la bride. La face arriere plane
    # de la bride reste donc inchangee et continue de regler la compression.
    cover = cover.fuse(build_compression_gusset().common(rear_u_keep))

    # Levre continue et saillante qui comprime effectivement le cordon. Le
    # cadre mobile reprend exactement l'enveloppe du cadre fixe. L'ancienne
    # version (205 x 168,9 mm) debordait dans les facettes superieures.
    compression = rounded_ring(
        seal_w, seal_h, COMPRESSION_RING_W, COMPRESSION_RING_T, seal_radius,
        seal_x, seal_y, COMPRESSION_RING_Z)
    cover = cover.fuse(compression.common(rear_u_keep))

    # La contre-face basse appartient directement au capot et suit le joint
    # spatial sur les rives et l'avant de la tablette.
    # V6 : les raccords sont en retrait a X11,3/198,7 et ne traversent
    # plus les parois exterieures. Aucun degagement externe aux coins.
    cover = cover.fuse(build_lower_cap_sealing_land())

    # Trois languettes captives derriere les levres de la platine. Elles
    # s'engagent par le haut et ne peuvent sortir vers l'avant tant que les
    # trois vis inferieures interdisent de relever le capot.
    for x in TOP_HOOK_X:
        cover = cover.fuse(build_cover_top_catch(x))

    # V15 : les contre-languettes laterales du capot sont retirees (elles ne guidaient ni ne
    # retenaient rien). Le guidage se fait par les rails de la platine, voir SIDE_GUIDE_D.

    # Bossages d'inserts des trois vis inferieures. Ils sont relies a la
    # façade par une nervure, au-dessus de la sous-face porte-connecteurs.
    for x in LOWER_LOCK_X:
        boss = Part.makeCylinder(
            LOWER_BOSS_OD / 2.0, LOWER_BOSS_LENGTH,
            vec(x, TECH_TOTAL_T, LOWER_LOCK_LOCAL_Z), vec(0, 1, 0))
        rib = Part.makeBox(
            10.0, LOWER_BOSS_LENGTH, COVER_FRONT_START - LOWER_LOCK_LOCAL_Z,
            vec(x - 5.0, TECH_TOTAL_T, LOWER_LOCK_LOCAL_Z))
        # Les bossages avances atteignent la facette basse. Limiter chacun
        # avant fusion, pour ne pas creer un ergot sous la coque exterieure.
        outer_limit = build_cover_outer_envelope()
        boss = boss.common(outer_limit)
        rib = rib.common(outer_limit)
        cover = cover.fuse(boss).fuse(rib)

        seat = Part.makeCylinder(
            LOWER_INSERT_HOLE_D / 2.0, LOWER_INSERT_SEAT_DEPTH + 0.2,
            vec(x, TECH_TOTAL_T - 0.2, LOWER_LOCK_LOCAL_Z), vec(0, 1, 0))
        relief = Part.makeCylinder(
            LOWER_CLEAR_D / 2.0, LOWER_BOSS_LENGTH + 0.4,
            vec(x, TECH_TOTAL_T - 0.2, LOWER_LOCK_LOCAL_Z), vec(0, 1, 0))
        cover = cover.cut(seat.fuse(relief))

    return safe_refine(cover)


def build_cover_monobloc(core, graphite, turquoise):
    return safe_refine(core.copy().fuse(graphite.copy()).fuse(turquoise.copy()))




def build_backplate_monobloc(core, technical_skin):
    return core.fuse(technical_skin).removeSplitter()


# ---------------------------------------------------------------------------
# Pieces annexes et coupons
# ---------------------------------------------------------------------------

def build_psu_guard(with_cable_ports=True,with_barrier_baffle=True):
    outer = Part.makeBox(GUARD_W, GUARD_L, GUARD_H)
    inner = Part.makeBox(
        GUARD_W - 2.0 * GUARD_T, GUARD_L - 2.0 * GUARD_T,
        GUARD_H - GUARD_T + 0.2, vec(GUARD_T, GUARD_T, -0.2))
    guard = outer.cut(inner)
    for x in GUARD_MOUNT_X:
        guard = guard.fuse(Part.makeBox(
            GUARD_TAB_W, GUARD_TAB_D, GUARD_T,
            vec(x - GUARD_TAB_W / 2.0, -GUARD_TAB_D, 0.0)))
        guard = guard.fuse(Part.makeBox(
            GUARD_TAB_W, GUARD_TAB_D, GUARD_T,
            vec(x - GUARD_TAB_W / 2.0, GUARD_L, 0.0)))
    for x in [9.0 + i * 8.0 for i in range(6)]:
        if x + 3.0 < GUARD_W - 8.0:
            guard = guard.cut(Part.makeBox(
                3.0, GUARD_L - 16.0, GUARD_T + 0.4,
                vec(x, 8.0, GUARD_H - GUARD_T - 0.2)))
    for x in GUARD_MOUNT_X:
        for y in (-GUARD_TAB_D / 2.0,
                  GUARD_L + GUARD_TAB_D / 2.0):
            guard = guard.cut(Part.makeCylinder(
                GUARD_SCREW_D / 2.0, GUARD_T + 0.4,
                vec(x, y, -0.2)))
    if with_cable_ports:
        for y in (-.2,GUARD_L-GUARD_T-.2):
            guard=guard.cut(guard_port_cutter(y))
    if with_barrier_baffle:
        guard=guard.fuse(build_guard_barrier_baffle())
    return guard.removeSplitter()


def build_pump_fit_coupon():
    coupon = Part.makeBox(70.0, 65.0, FRONT_CORE_T)
    cx, cy = 35.0, 25.0
    coupon = coupon.cut(Part.makeCylinder(
        PUMP_MOTOR_CUTOUT_D / 2.0, FRONT_CORE_T + 0.4,
        vec(cx, cy, -0.2)))
    for dx in (-PUMP_MOUNT_DX / 2.0, PUMP_MOUNT_DX / 2.0):
        coupon = coupon.cut(Part.makeCylinder(
            PUMP_SCREW_CLEAR_D / 2.0, FRONT_CORE_T + 0.4,
            vec(cx + dx, cy + PUMP_MOUNT_DY, -0.2)))
    return coupon.removeSplitter()


def build_connector_fit_coupon():
    coupon = Part.makeBox(70.0, TECH_TOTAL_T, 32.0)
    coupon = coupon.cut(Part.makeCylinder(
        PG7_D / 2.0, TECH_TOTAL_T + 0.4,
        vec(17.0, -0.2, 16.0), vec(0, 1, 0)))
    coupon = coupon.cut(d_cutout_y(
        BNC_CUTOUT_D, BNC_FLAT_OFFSET, TECH_TOTAL_T + 0.4,
        52.0, -0.2, 16.0))
    return coupon.removeSplitter()


def build_m4_insert_fit_coupon():
    coupon = Part.makeBox(60.0, 22.0, 8.0)
    for x, diameter in zip((15.0, 30.0, 45.0), (5.4, 5.6, 5.8)):
        coupon = coupon.cut(Part.makeCylinder(
            LOWER_CLEAR_D / 2.0, 7.2, vec(x, 11.0, 1.0)))
        coupon = coupon.cut(Part.makeCylinder(
            diameter / 2.0, LOWER_INSERT_SEAT_DEPTH + 0.2,
            vec(x, 11.0, 8.0 - LOWER_INSERT_SEAT_DEPTH)))
    return coupon.removeSplitter()




def build_gasket_fit_coupon():
    coupon = Part.makeBox(42.0, 24.0, 6.0)
    groove = Part.makeBox(GASKET_W, 24.4, GASKET_DEPTH + 0.2,
                          vec(19.6, -0.2, 6.0 - GASKET_DEPTH))
    return coupon.cut(groove).removeSplitter()


# ---------------------------------------------------------------------------
# Assemblage de controle
# ---------------------------------------------------------------------------

def add_feature(doc, name, label, shape, color, transparency=0):
    obj = doc.addObject("PartDesign::Feature", name)
    obj.Label = label
    obj.Shape = shape
    if obj.ViewObject:
        obj.ViewObject.ShapeColor = color
        obj.ViewObject.Transparency = transparency
    return obj


def build_pump_terminal_envelope(cx, cy, mount_z):
    return Part.makeCylinder(PUMP_TERMINAL_ENVELOPE_D / 2.0,
        PUMP_TERMINAL_DEPTH, vec(cx, cy, mount_z-PUMP_MOTOR_REAR_D), vec(0,0,-1))


def build_pump_fastener_envelope(cx, cy, mount_z):
    """Reservation M4 provisoire : la visserie de la MP1 reste a mesurer."""
    shapes = []
    inner_z = COVER_ASSEMBLY_Z + COVER_FRONT_START
    for dx in (-PUMP_MOUNT_DX/2, PUMP_MOUNT_DX/2):
        x, y = cx+dx, cy+PUMP_MOUNT_DY
        shapes.append(Part.makeCylinder(4.5, 1.0, vec(x,y,inner_z-1.0)))
        shapes.append(Part.makeCylinder(4.0, 3.2, vec(x,y,inner_z-4.2)))
        shapes.append(Part.makeCylinder(2.0, mount_z+4-inner_z+4.2,
            vec(x,y,inner_z-4.2)))
    return Part.makeCompound(shapes)


def build_pump_reference(cx, cy, mount_z):
    """Reference Dosita MP1 conservee depuis V12 ; raccords orientés vers le bas."""
    rear=Part.makeBox(47.6,58.0,8.0,vec(cx-23.8,cy-29,mount_z-8))
    motor = Part.makeCylinder(
        PUMP_MOTOR_D / 2.0, PUMP_MOTOR_REAR_D,
        vec(cx, cy, mount_z), vec(0, 0, -1))
    front=Part.makeBox(PUMP_TOTAL_W,62.0,PUMP_FRONT_D,vec(cx-PUMP_TOTAL_W/2,cy-22,mount_z))
    shapes=[rear,motor,front]
    for dx in (-15.0,15.0):
        shapes.append(Part.makeCylinder(6,24,vec(cx+dx,cy-22,mount_z+PUMP_FRONT_D/2),vec(0,-1,0)))
        shapes.append(Part.makeCylinder(3.2,82,vec(cx+dx,cy-46,mount_z+PUMP_FRONT_D/2),vec(0,-1,0)))
    return shapes[0].multiFuse(shapes[1:]).removeSplitter()


def build_gasket_reference():
    """Enveloppe de la gorge occupee par le cordon comprime, pas un STL."""
    return build_spatial_groove()

def build_assembly(parts):
    doc = App.newDocument("PoolControllerV15MP1_Assembly")
    parameter_object(doc, [
        ("Width", "Largeur hors tout", BOX_W),
        ("Height", "Hauteur hors tout", BOX_H),
        ("BodyDepth", "Profondeur corps hors pompes", BODY_D),
        ("OverallPumpDepth", "Profondeur avec pompes",
         BODY_D + PUMP_FRONT_D),
        ("PumpRearClearance", "Jeu moteur / PCB", pump_rear_clearance()),
        ("PumpMotorRearDepth", "Profondeur moteur MP1 nominale", PUMP_MOTOR_REAR_D),
        ("PumpRearDepthWithTerminals", "Profondeur arriere MP1 nominale", PUMP_REAR_D),
        ("PumpMotorDiameter", "Diametre moteur MP1 nominal", PUMP_MOTOR_D),
        ("PumpMountSpacing", "Entraxe horizontal MP1 nominal", PUMP_MOUNT_DX),
        ("PumpScrewClearance", "Passage M4 provisoire a qualifier", PUMP_SCREW_CLEAR_D),
        ("SideGuideLength", "Longueur de chaque guide lateral",
         SIDE_GUIDE_H),
        ("CompressionLipWidth", "Largeur levre de compression",
         COMPRESSION_RING_W),
        ("StructuralFlangeThickness", "Epaisseur bride peripherique",
         COMPRESSION_FLANGE_T),
        ("StructuralFlangeRecess", "Decalage bride / zone joint",
         COMPRESSION_FLANGE_RECESS),
        ("CompressionGussetRun", "Course nervure oblique",
         COMPRESSION_GUSSET_RUN),
        ("CompressionGussetRise", "Montee nervure oblique",
         COMPRESSION_GUSSET_RISE),
        ("CompressionGussetAngle", "Angle nervure oblique",
         COMPRESSION_GUSSET_ANGLE_DEG),
        ("LowerLockSpacing", "Entraxe des vis M4 exterieures", LOWER_LOCK_X[-1] - LOWER_LOCK_X[0]),
        ("PG7CableInsertionKeepoutEnd",
         "Fin enveloppe insertion cables PG7",
         PG7_CABLE_INSERTION_KEEP_Z1),
        ("GasketGrooveWidth", "Largeur gorge joint", GASKET_W),
        ("GasketGrooveDepth", "Profondeur gorge joint", GASKET_DEPTH),
        ("GasketSupportWall", "Paroi de chaque cote de la gorge",
         GASKET_SUPPORT_WALL),
    ])

    add_feature(doc, "WallBackplate",
                "01 - Platine murale ASA clair",
                parts["backplate_core"], (0.82, 0.80, 0.74), 12)
    add_feature(doc, "TechnicalSkin",
                "02 - Peau technique noire co-imprimee",
                parts["technical_skin"], (0.07, 0.08, 0.09), 0)

    core = parts["cover_core"].copy()
    core.translate(vec(0, 0, COVER_ASSEMBLY_Z))
    add_feature(doc, "CoverLightCore",
                "03 - Capot cloche structurel ASA clair",
                core, (0.91, 0.89, 0.83), 0)

    graphite = parts["graphite_skin"].copy()
    graphite.translate(vec(0, 0, COVER_ASSEMBLY_Z))
    add_feature(doc, "GraphiteSkin",
                "04 - Ruban graphite ASA-CF co-imprime",
                graphite, (0.09, 0.10, 0.11), 0)

    turquoise = parts["turquoise_inlay"].copy()
    turquoise.translate(vec(0, 0, COVER_ASSEMBLY_Z))
    add_feature(doc, "TurquoiseInlay",
                "05 - Signature turquoise co-imprimee",
                turquoise, (0.04, 0.62, 0.66), 0)

    guard = placed_psu_guard(parts["psu_guard"])
    add_feature(doc, "PSUGuard", "06 - Capot RPS-30-12",
                guard, (0.90, 0.52, 0.13), 25)

    # Inserts et vis inferieures.
    inserts = []
    screws = []
    for x in LOWER_LOCK_X:
        insert = Part.makeCylinder(
            LOWER_INSERT_OD / 2.0, LOWER_INSERT_LENGTH,
            vec(x, TECH_TOTAL_T + 0.3, LOWER_LOCK_GLOBAL_Z), vec(0, 1, 0))
        insert = insert.cut(Part.makeCylinder(
            4.0 / 2.0, LOWER_INSERT_LENGTH + 0.2,
            vec(x, TECH_TOTAL_T + 0.2, LOWER_LOCK_GLOBAL_Z), vec(0, 1, 0)))
        inserts.append(insert)
        screws.append(build_lower_screw(x))
    add_feature(doc, "LowerM4Inserts",
                "REFERENCE - Inserts M4 courts x3",
                Part.makeCompound(inserts), (0.78, 0.55, 0.16), 0)
    add_feature(doc, "LowerM4Screws",
                "REFERENCE - Vis M4 inferieures x3",
                Part.makeCompound(screws), (0.52, 0.53, 0.55), 0)
    add_feature(doc, "GasketReference",
                "REFERENCE - Enveloppe gorge du cordon comprime (non imprimable)",
                build_gasket_reference(), (0.05, 0.05, 0.05), 5)

    pcb, comp = build_pcb_reference_shapes()
    add_feature(doc, "PCBReference", "REFERENCE - PCB a droite",
                pcb, (0.05, 0.42, 0.20), 15)
    add_feature(doc, "PCBEnvelope",
                "REFERENCE - Enveloppe composants PCB",
                comp, (0.80, 0.70, 0.15), 82)
    add_feature(doc, "PCBServiceBay",
                "REFERENCE - Reserve cables devant PCB, fiches a qualifier",
                build_pcb_service_bay(), (0.30,0.65,0.95), 88)
    add_feature(doc, "WallScrewReferences",
                "REFERENCE - Vis murales, deux tetes basses limitees a 7.5 mm",
                Part.makeCompound([build_wall_screw_reference(x,y) for x,y in WALL_MOUNTS]),
                (0.52, 0.53, 0.55), 15)

    psu = build_psu_reference_shape()
    add_feature(doc, "PSUReference",
                "REFERENCE - Mean Well RPS-30-12 a gauche",
                psu, (0.55, 0.57, 0.60), 35)
    for name,shape in build_wiring_routes().items():
        add_feature(doc,"RPSWiring_"+name,
            "REFERENCE - Faisceau "+name+" nominal Ø6, non imprimable",
            shape,(.35,.12,.08) if name=='secteur' else (.10,.25,.60),35)

    for index, (cx, cy) in enumerate(PUMP_CENTRES, 1):
        add_feature(doc, f"PUMP_{index}",
                    f"REFERENCE - Pompe Dosita MP1 {index}",
                    build_pump_reference(cx, cy, BODY_D),
                    (0.10, 0.35, 0.85), 12)
        add_feature(doc, f"PumpM4Fasteners_{index}",
                    "REFERENCE - Enveloppes M4 provisoires, visserie MP1 a confirmer",
                    build_pump_fastener_envelope(cx,cy,BODY_D),(.55,.55,.55),20)

    for index, x in enumerate(BNC_X, 1):
        barrel = Part.makeCylinder(
            BNC_CUTOUT_D / 2.0, BNC_FRONT_LENGTH,
            vec(x, BNC_REFERENCE_Y_SHIFT, BNC_AXIS_Z), vec(0, -1, 0))
        body = Part.makeBox(
            BNC_BODY_W, BNC_REAR_LENGTH, BNC_BODY_H,
            vec(x - BNC_BODY_W / 2.0, BNC_REFERENCE_Y_SHIFT,
                BNC_AXIS_Z - BNC_BODY_H / 2.0))
        add_feature(doc, f"BNC_{index}",
                    f"REFERENCE - BNC-123 {index}",
                    barrel.fuse(body), (0.72, 0.73, 0.75), 10)

    for index, (x,z) in enumerate(PG7_LAYOUT, 1):
        gland = build_pg7_body(x,z)
        add_feature(doc, f"PG7_{index}", f"REFERENCE - PG7 {index} "+PG7_FUNCTIONS[index-1],
                    gland, (0.05, 0.05, 0.05), 5)
    add_feature(doc, "PG7InternalNutReferences", "REFERENCE - Ecrous PG7 enveloppes Ø18 x 8",
                Part.makeCompound([build_pg7_body(x,z,True) for x,z in PG7_LAYOUT]),
                (.15,.15,.15), 35)

    doc.recompute()
    path = FC_DIR / "PoolControllerV15MP1_Assembly.FCStd"
    doc.saveAs(str(path))
    backup = Path(str(path) + "1")
    if backup.exists():
        backup.unlink()
    App.closeDocument(doc.Name)
    for auto_backup in FC_DIR.glob("PoolControllerV15MP1_Assembly*.FCBak"):
        auto_backup.unlink()
    print(f"PoolControllerV15MP1_Assembly: {path}")


# ---------------------------------------------------------------------------
# Validations et rapports
# ---------------------------------------------------------------------------

def pcb_hole_centres():
    return [(bx+kx, by+bh-ky) for _n,bx,by,bw,bh,holes in BOARDS for kx,ky in holes]


def build_pcb_service_bay():
    return Part.makeBox(PCB_SERVICE_BAY_W,PCB_SERVICE_BAY_H,PCB_SERVICE_BAY_D,
        vec(PCB_SERVICE_BAY_X,PCB_SERVICE_BAY_Y,PCB_SERVICE_BAY_Z))


def build_pcb_reference_shapes():
    pcb_z = REAR + PCB_STANDOFF_H
    pcb = Part.makeCompound([Part.makeBox(bw, bh, PCB_T, vec(bx, by, pcb_z))
                             for _n,bx,by,bw,bh,_h in BOARDS])
    for x,y in pcb_hole_centres():
        pcb=pcb.cut(Part.makeCylinder(PCB_HOLE_D/2,PCB_T+.2,vec(x,y,pcb_z-.1)))
    components = Part.makeCompound([Part.makeBox(
        bw, bh, PCB_COMPONENT_ENVELOPE, vec(bx, by, pcb_z + PCB_T))
        for _n,bx,by,bw,bh,_h in BOARDS])
    return pcb, components


def pcb_interface_checks(parts,cap):
    plate=parts['backplate_monobloc']
    pcb,components=build_pcb_reference_shapes()
    bay=build_pcb_service_bay()
    guard=placed_psu_guard(parts['psu_guard'])
    psu=build_psu_reference_shape()
    posts=Part.makeCompound([ring_post_z(PCB_POST_OD,PCB_HOLE_D,PCB_STANDOFF_H,x,y,REAR)
                            for x,y in pcb_hole_centres()])
    holes=[]
    for x,y in pcb_hole_centres():
        bore=Part.makeCylinder(PCB_HOLE_D/2-.05,PCB_STANDOFF_H-.1,vec(x,y,REAR+.05))
        post=ring_post_z(PCB_POST_OD,PCB_HOLE_D,PCB_STANDOFF_H-.1,x,y,REAR+.05)
        board_bore=Part.makeCylinder(PCB_HOLE_D/2-.05,PCB_T+.2,vec(x,y,REAR+PCB_STANDOFF_H-.1))
        row={'x_mm':x,'y_mm':y,'post_bore_obstruction_mm3':bore.common(plate).Volume,
             'post_material_missing_mm3':post.Volume-post.common(plate).Volume,
             'pcb_bore_obstruction_mm3':board_bore.common(pcb).Volume}
        row['pass']=max(abs(row[k]) for k in ('post_bore_obstruction_mm3',
            'post_material_missing_mm3','pcb_bore_obstruction_mm3'))<=.01
        holes.append(row)
    remaining_plate=plate.cut(posts)
    obstacles={'plate_without_pcb_posts':remaining_plate,'cap':cap,'guard':guard,'RPS':psu}
    hits={name:{key:shape.common(obstacle).Volume for key,obstacle in obstacles.items()}
          for name,shape in {'PCB':pcb,'components':components}.items()}
    bay_hits={key:bay.common(shape).Volume for key,shape in
        {'plate':plate,'cap':cap,'PCB':pcb,'components':components,'guard':guard,'RPS':psu}.items()}
    bay_pump_hits=[bay.common(build_pump_reference(cx,cy,BODY_D)).Volume for cx,cy in PUMP_CENTRES]
    nearest_plate=pcb.distToShape(remaining_plate)[0]
    # Reserve verticale complete de 3 mm, hors les quatre assises autorisees.
    # La distance euclidienne au gousset externe a la projection n'est pas
    # la hauteur disponible sous le PCB.
    under_bay=Part.makeBox(PCB_W,PCB_H,PCB_UNDERSIDE_CLEARANCE,
        vec(PCB_X,PCB_Y,REAR+PCB_STANDOFF_H-PCB_UNDERSIDE_CLEARANCE)).fuse(
        # V15 : la reserve commence 1,6 mm apres le bord bas de la carte de mesure. Ce bord
        # longe la paroi du bas, au-dessus du gousset de la tablette (2,5 a 3 mm de libre).
        Part.makeBox(MEAS_W,MEAS_H-1.6,PCB_UNDERSIDE_CLEARANCE,
        vec(MEAS_X,MEAS_Y+1.6,REAR+PCB_STANDOFF_H-PCB_UNDERSIDE_CLEARANCE)))
    for x,y in pcb_hole_centres():
        under_bay=under_bay.cut(Part.makeCylinder(PCB_POST_OD/2+.05,
            PCB_UNDERSIDE_CLEARANCE+.2,vec(x,y,REAR+PCB_STANDOFF_H-PCB_UNDERSIDE_CLEARANCE-.1)))
    under_hits=under_bay.common(plate).Volume
    # Anciennes quatre entretoises : aucun anneau complet ne doit rester.
    old_centres=[(x,y) for x in (91.8+7.62,91.8+104.14-7.62)
                for y in (12+7.62,12+104.14-7.62)]
    old_present=[]
    for x,y in old_centres:
        probe=ring_post_z(7.5,3.2,1,x,y,REAR+3)
        # Le nouvel axe droit bas est presque au meme X que l'ancien : son
        # support neuf est legitime et doit etre retire de ce temoin d'obsolescence.
        old_present.append(probe.common(remaining_plate).Volume)
    gap_cap=components.distToShape(cap)[0]
    gap_guard=pcb.distToShape(guard)[0]
    out={'size_mm':[PCB_W,PCB_H,PCB_T],'hole_spacing_mm':[PCB_W-2*PCB_HOLE_EDGE,PCB_H-2*PCB_HOLE_EDGE],
        'edge_offset_mm':PCB_HOLE_EDGE,'position_xy_mm':[PCB_X,PCB_Y],
        'board_back_z_mm':REAR+PCB_STANDOFF_H,'standoff_height_mm':PCB_STANDOFF_H,
        'standoff_diameter_mm':PCB_POST_OD,'bore_diameter_mm':PCB_HOLE_D,'mounts':holes,
        'collision_volumes_mm3':hits,'nearest_non_post_plate_distance_mm':nearest_plate,
        'vertical_under_board_reservation_mm':PCB_UNDERSIDE_CLEARANCE,
        'under_board_reservation_obstruction_mm3':under_hits,
        'cap_component_clearance_mm':gap_cap,'guard_board_clearance_mm':gap_guard,
        'old_post_presence_mm3':old_present,
        'service_bay_size_mm':[PCB_SERVICE_BAY_W,PCB_SERVICE_BAY_H,PCB_SERVICE_BAY_D],
        'service_bay_collisions_mm3':bay_hits,'service_bay_pump_collisions_mm3':bay_pump_hits,
        'connector_assumption':'vertical plugs, front cable reservation; actual plugs unmeasured',
        'screw_retention':'existing smooth 3.2 mm bores; insert or nut retention not qualified',
        'pass':all(r['pass'] for r in holes) and max(v for row in hits.values() for v in row.values())<=.01
            and max(bay_hits.values())<=.01 and max(bay_pump_hits)<=.01
            and under_hits<=.01 and gap_cap>=1-1e-6 and gap_guard>=1-1e-6
            and max(old_present)<=.01 and len(holes)==8}
    return out


def alt_psu_checks(parts):
    """Second format d'alimentation : la carte et son enveloppe de composants tiennent-elles ?"""
    plate=parts['backplate_monobloc']; guard=placed_psu_guard(parts['psu_guard'])
    z=REAR+PSU_STANDOFF_H
    board=Part.makeBox(ALT_PSU_W,ALT_PSU_H,1.6,vec(ALT_PSU_X,ALT_PSU_Y,z))
    env=Part.makeBox(ALT_PSU_W,ALT_PSU_H,ALT_PSU_COMPONENT_H,vec(ALT_PSU_X,ALT_PSU_Y,z+1.6))
    posts=[(x,y) for x in (PSU_X+PSU_HOLE_EDGE_X,PSU_X+PSU_W-PSU_HOLE_EDGE_X)
           for y in (PSU_Y+PSU_HOLE_EDGE_Y,PSU_Y+PSU_H-PSU_HOLE_EDGE_Y)]+list(ALT_PSU_NEW_POSTS)
    seats=[]
    for x,y in posts:
        top=Part.makeCylinder(PSU_POST_OD/2-.3,.2,vec(x,y,z-.2)).cut(Part.makeCylinder(PSU_HOLE_D/2+.1,.4,vec(x,y,z-.3)))
        seats.append({'x_mm':x,'y_mm':y,'seat_missing_mm3':top.Volume-top.common(plate).Volume,
                      'under_board':ALT_PSU_X<=x<=ALT_PSU_X+ALT_PSU_W and ALT_PSU_Y<=y<=ALT_PSU_Y+ALT_PSU_H})
    pump_rear_z=BODY_D-PUMP_REAR_D
    out={'board_mm':[ALT_PSU_W,ALT_PSU_H],'position_xy_mm':[ALT_PSU_X,ALT_PSU_Y],
         'component_height_mm':ALT_PSU_COMPONENT_H,'seats':seats,
         'board_plate_collision_mm3':board.common(plate).Volume,
         'board_guard_collision_mm3':board.common(guard).Volume,
         'components_guard_collision_mm3':env.common(guard).Volume,
         'components_to_guard_mm':env.distToShape(guard)[0],
         'guard_top_z_mm':REAR+GUARD_BOSS_H+GUARD_H,
         'guard_to_pump_rear_mm':pump_rear_z-(REAR+GUARD_BOSS_H+GUARD_H),
         'rps_fits_too_mm3':build_psu_reference_shape().common(guard).Volume}
    out['pass']=(all(abs(r['seat_missing_mm3'])<=.01 and r['under_board'] for r in seats)
        and out['board_plate_collision_mm3']<=.01 and out['board_guard_collision_mm3']<=.01
        and out['components_guard_collision_mm3']<=.01 and out['components_to_guard_mm']>=1-1e-6
        and out['guard_to_pump_rear_mm']>=3.0 and out['rps_fits_too_mm3']<=.01)
    return out


def build_psu_reference_shape():
    return Part.makeBox(
        PSU_W, PSU_H, PSU_Z,
        vec(PSU_X, PSU_Y, REAR + PSU_STANDOFF_H))


def placed_psu_guard(guard_shape):
    guard = guard_shape.copy()
    guard.translate(vec(
        PSU_X - GUARD_CLEARANCE + GUARD_OFFSET_X,
        PSU_Y - GUARD_CLEARANCE,
        REAR + GUARD_BOSS_H))
    return guard

def pump_rear_clearance():
    pump_rear_z = BODY_D - PUMP_REAR_D
    pcb_front_z = REAR + PCB_STANDOFF_H + PCB_T + PCB_COMPONENT_ENVELOPE
    return pump_rear_z - pcb_front_z


def gasket_stations():
    """Points du contour REEL ferme et normales de compression (pas rectangle fictif)."""
    result = []
    def line(a, b, normal):
        a, b = vec(*a), vec(*b)
        count = max(1, int((b-a).Length / 2.0))
        result.extend((a + (b-a)*i/count, normal) for i in range(count))
    def arc(cx, cy, radius, a0, a1, plane):
        for i in range(13):
            a = a0 + (a1-a0)*i/12
            if plane == "rear":
                p = vec(cx + radius*math.cos(a), cy + radius*math.sin(a), SEAL_FACE_Z)
                n = vec(0,0,1)
            else:
                p = vec(cx + radius*math.cos(a), TECH_TOTAL_T, cy + radius*math.sin(a))
                n = vec(0,1,0)
            result.append((p,n))
    left, right = GASKET_PATH_X, GASKET_PATH_X + GASKET_PATH_W
    top, r = GASKET_PATH_Y + GASKET_PATH_H, GASKET_PATH_R
    line((left,SEAL_REAR_END_Y,SEAL_FACE_Z),(left,top-r,SEAL_FACE_Z),vec(0,0,1))
    arc(left+r,top-r,r,math.pi,math.pi/2,"rear")
    line((left+r,top,SEAL_FACE_Z),(right-r,top,SEAL_FACE_Z),vec(0,0,1))
    arc(right-r,top-r,r,math.pi/2,0,"rear")
    line((right,top-r,SEAL_FACE_Z),(right,SEAL_REAR_END_Y,SEAL_FACE_Z),vec(0,0,1))
    result.append((vec(right,SEAL_REAR_END_Y,SEAL_FACE_Z),vec(0,0,1)))
    for i in range(SEAL_ELBOW_STEPS+1):
        result.append(seal_elbow_station(i*math.pi/(2*SEAL_ELBOW_STEPS),True))
    dx, dz, dr = SEAL_DECK_X, SEAL_DECK_FRONT_Z, SEAL_DECK_FRONT_R
    line((BOX_W-dx,TECH_TOTAL_T,SEAL_DECK_START_Z),(BOX_W-dx,TECH_TOTAL_T,dz-dr),vec(0,1,0))
    arc(BOX_W-dx-dr,dz-dr,dr,0,math.pi/2,"deck")
    line((BOX_W-dx-dr,TECH_TOTAL_T,dz),(dx+dr,TECH_TOTAL_T,dz),vec(0,1,0))
    arc(dx+dr,dz-dr,dr,math.pi/2,math.pi,"deck")
    line((dx,TECH_TOTAL_T,dz-dr),(dx,TECH_TOTAL_T,SEAL_DECK_START_Z),vec(0,1,0))
    for i in range(SEAL_ELBOW_STEPS,-1,-1):
        result.append(seal_elbow_station(i*math.pi/(2*SEAL_ELBOW_STEPS),False))
    return result


def outer_wall_closure_check(cap_local):
    """Verifie la matiere de paroi, pas seulement la fermeture d'un STL."""
    pts = faceted_points(BOX_W,BOX_H,TOP_CHAMFER,BOTTOM_CHAMFER)
    inner = faceted_points(BOX_W-2*WALL,BOX_H-2*WALL,
                          TOP_CHAMFER-WALL,BOTTOM_CHAMFER-WALL,WALL,WALL)
    expected = polygon_prism_xy(pts,0,COVER_FRONT_START).cut(
        polygon_prism_xy(inner,-.1,COVER_FRONT_START+.2))
    opening = Part.makeBox(TECH_W+.4,4.0,TECH_OPENING_LOCAL_D,
                          vec(TECH_X-.2,-.2,-.1))
    tested, missing = 0, []
    for i,(ax,ay) in enumerate(pts):
        bx,by = pts[(i+1)%len(pts)]
        length = math.hypot(bx-ax,by-ay)
        nx,ny = -(by-ay)/length,(bx-ax)/length
        count = max(1,math.ceil(length/2.0))
        for j in range(count):
            t = (j+.5)/count
            for depth in (.5,1.5,2.5):
                for zi in range(math.ceil(COVER_FRONT_START/2.0)):
                    p = vec(ax+(bx-ax)*t+nx*depth,
                            ay+(by-ay)*t+ny*depth,1+2*zi)
                    if expected.isInside(p,1e-5,False) and not opening.isInside(p,1e-5,True):
                        tested += 1
                        if not cap_local.isInside(p,1e-5,True):
                            missing.append(list(p))
    # Temoin negatif : trou volontaire dans la paroi droite du coin arriere.
    bad = cap_local.cut(Part.makeBox(6,10,12,vec(BOX_W-5,8,1)))
    negative = not bad.isInside(vec(BOX_W-1.5,12,6),1e-5,True)
    return {"sample_count":tested,"grid_step_mm":2.0,
            "wall_depths_mm":[.5,1.5,2.5],"missing_count":len(missing),
            "missing_samples":missing[:200],"negative_control_detected":negative,
            "scope":"parois laterales nominales, ouverture sous-face autorisee exclue",
            "pass":not missing and negative}


def rps_guard_mount_checks(plate,guard,pcb,components,psu,wires):
    """Verifier les quatre nouveaux bossages/percements et acces M3 nominaux.

    Les pilotes 2,8 ne sont pas des inserts. Tete D6/H3 et outil D6 sont des
    limites d'enveloppe provisoires, a confirmer sur la visserie achetee.
    Le filetage de retenue et sa resistance ne sont pas modelises.
    """
    gx=PSU_X-GUARD_CLEARANCE+GUARD_OFFSET_X
    gy=PSU_Y-GUARD_CLEARANCE
    rows=[]
    obstacles={'plate':plate,'guard':guard,'PCB':pcb,'components':components,
               'RPS':psu,**wires}
    for lx in GUARD_MOUNT_X:
        for ly in (-GUARD_TAB_D/2,GUARD_L+GUARD_TAB_D/2):
            x,y=gx+lx,gy+ly
            support=Part.makeCylinder(GUARD_BOSS_OD/2,GUARD_BOSS_H,vec(x,y,REAR)).cut(
                Part.makeCylinder(GUARD_PILOT_D/2,GUARD_BOSS_H,vec(x,y,REAR)))
            pilot=Part.makeCylinder(GUARD_PILOT_D/2-.05,GUARD_BOSS_H,vec(x,y,REAR))
            passage=Part.makeCylinder(GUARD_SCREW_D/2-.05,GUARD_T,
                vec(x,y,REAR+GUARD_BOSS_H))
            z=REAR+GUARD_BOSS_H+GUARD_T
            head=Part.makeCylinder(3,3,vec(x,y,z))
            tool=Part.makeCylinder(3,70,vec(x,y,z+3))
            missing=support.Volume-support.common(plate).Volume
            head_hits={k:head.common(s).Volume for k,s in obstacles.items()}
            tool_hits={k:tool.common(s).Volume for k,s in obstacles.items()}
            row={'x_mm':x,'y_mm':y,'support_missing_mm3':missing,
                 'pilot_obstruction_mm3':pilot.common(plate).Volume,
                 'tab_bore_obstruction_mm3':passage.common(guard).Volume,
                 'head_collisions_mm3':head_hits,'tool_collisions_mm3':tool_hits}
            row['pass']=abs(missing)<=.01 and row['pilot_obstruction_mm3']<=.01 \
                and row['tab_bore_obstruction_mm3']<=.01 \
                and max(head_hits.values())<=.01 and max(tool_hits.values())<=.01
            rows.append(row)
    return {'checks':rows,'head_max_d_mm':6,'head_max_h_mm':3,'tool_max_d_mm':6,
            'tool_approach_length_mm':70,'scope':'enveloppes M3 provisoires; retenue/filetage non qualifies',
            'pass':all(r['pass'] for r in rows)}


def pump_interface_checks(parts):
    """Sondes sur les vraies pieces fournies : nouveaux trous et anciens rebouches."""
    cap = parts['cover_monobloc'].copy()
    cap.translate(vec(0,0,COVER_ASSEMBLY_Z))
    inner_z = COVER_ASSEMBLY_Z+COVER_FRONT_START
    rows = []
    for cx,cy in PUMP_CENTRES:
        motor = Part.makeCylinder(PUMP_MOTOR_D/2,FRONT_CORE_T+.2,
            vec(cx,cy,inner_z-.1))
        screw_blockages, restored, bearing_missing = [], [], []
        for dx in (-PUMP_MOUNT_DX/2,PUMP_MOUNT_DX/2):
            bore = Part.makeCylinder(PUMP_SCREW_CLEAR_D/2-.05,FRONT_CORE_T+.2,
                vec(cx+dx,cy+PUMP_MOUNT_DY,inner_z-.1))
            screw_blockages.append(bore.common(cap).Volume)
            ring = Part.makeCylinder(4.5,.2,vec(cx+dx,cy+PUMP_MOUNT_DY,inner_z+.05)).cut(
                Part.makeCylinder(PUMP_SCREW_CLEAR_D/2+.05,.2,vec(cx+dx,cy+PUMP_MOUNT_DY,inner_z+.05)))
            bearing_missing.append(ring.Volume-ring.common(cap).Volume)
        for dx in (-24.1,24.1):
            restored.append(cap.isInside(vec(cx+dx,cy,inner_z+1),1e-5,False))
        rows.append({'centre_xy_mm':[cx,cy], 'motor_blockage_mm3':motor.common(cap).Volume,
            'pump_bore_blockages_mm3':screw_blockages,'washer_bearing_missing_mm3':bearing_missing,
            'aliexpress_holes_absent':restored,
            'pass':motor.common(cap).Volume<=.01 and max(screw_blockages)<=.01
                and max(bearing_missing)<=.01 and all(restored)})
    # Coupon controle separe : meme empreinte, ancien decalage vertical absent.
    coupon = parts['pump_coupon']
    probes = [Part.makeCylinder(PUMP_MOTOR_D/2,FRONT_CORE_T+.2,vec(35,25,-.1))]
    probes += [Part.makeCylinder(PUMP_SCREW_CLEAR_D/2-.05,FRONT_CORE_T+.2,
        vec(35+dx,25+PUMP_MOUNT_DY,-.1)) for dx in (-PUMP_MOUNT_DX/2,PUMP_MOUNT_DX/2)]
    coupon_blockages = [p.common(coupon).Volume for p in probes]
    return {'motor_nominal_d_mm':PUMP_MOTOR_D,'motor_cutout_d_mm':PUMP_MOTOR_CUTOUT_D,
        'motor_rear_depth_mm':PUMP_MOTOR_REAR_D,'rear_depth_with_terminals_mm':PUMP_REAR_D,
        'terminal_extra_depth_mm':PUMP_TERMINAL_DEPTH,'terminal_envelope_d_mm':PUMP_TERMINAL_ENVELOPE_D,
        'mount_spacing_mm':PUMP_MOUNT_DX,'mount_vertical_offset_mm':PUMP_MOUNT_DY,
        'screw_thread':'M4 provisional envelope, actual pump screws unmeasured','screw_clearance_d_mm':PUMP_SCREW_CLEAR_D,
        'pad_width_mm':PUMP_PAD_W,'checks':rows,'coupon_blockages_mm3':coupon_blockages,
        'scope':'profondeurs mesurees, reservation bornes coaxiale; radial bornes/cosses/fils et bride visuelle a qualifier',
        'pass':all(r['pass'] for r in rows) and max(coupon_blockages)<=.01}


def write_validation_report(part_reports, parts):
    cap = parts["cover_monobloc"].copy()
    cap.translate(vec(0,0,COVER_ASSEMBLY_Z))
    plate = parts["backplate_monobloc"]
    pcb, components = build_pcb_reference_shapes()
    psu = build_psu_reference_shape()
    guard = placed_psu_guard(parts["psu_guard"])
    bnc = Part.makeCompound([Part.makeBox(BNC_BODY_W,BNC_REAR_LENGTH,BNC_BODY_H,
        vec(x-BNC_BODY_W/2,BNC_REFERENCE_Y_SHIFT,BNC_AXIS_Z-BNC_BODY_H/2)) for x in BNC_X])
    cables = build_pg7_corridors()
    nuts = Part.makeCompound([build_pg7_body(x,z,True) for x,z in PG7_LAYOUT])
    wall_screws = Part.makeCompound([build_wall_screw_reference(x,y) for x,y in WALL_MOUNTS])
    rps_wiring=build_wiring_routes()
    fixed = {"platine":plate,"pcb":pcb,"composants":components,"rps":psu,
             "capot_rps":guard,"corps_bnc":bnc,"cables_pg7":cables,
             "vis_murales":wall_screws,"ecrous_pg7":nuts}
    fixed.update({"faisceau_rps_"+k:v for k,v in rps_wiring.items()})
    fixed['reserve_cables_pcb']=build_pcb_service_bay()
    # Ne pas tester un compound de volumes mobiles qui se chevauchent entre eux.
    # OCC inventait 347 mm3 d'intersection avec la platine alors que chaque
    # piece separee avait une intersection nulle (tiges M3/capot/brides).
    # Tester tous les solides mobiles contre les memes obstacles, sans les
    # supprimer ni reduire les seuils. La somme est conservatrice si deux
    # volumes mobiles rencontrent le meme obstacle.
    moving_parts = {'capot':cap}
    for i,(cx,cy) in enumerate(PUMP_CENTRES,1):
        moving_parts['pompe_'+str(i)] = build_pump_reference(cx,cy,BODY_D)
        for j,solid in enumerate(build_pump_fastener_envelope(cx,cy,BODY_D).Solids):
            moving_parts['m3_'+str(i)+'_'+str(j)] = solid
    static = {name:round(cap.common(shape).Volume,5) for name,shape in fixed.items()}
    maxima = {name:0.0 for name in fixed}
    failures = []
    poses = [(HOOK_RELEASE_LIFT,float(z)) for z in range(100,-1,-1)]
    poses += [(HOOK_RELEASE_LIFT-i*.25,0.0) for i in range(1,17)]
    for lift, forward in poses:
        candidates = {key:s.translated(vec(0,lift,forward)) for key,s in moving_parts.items()}
        for name, shape in fixed.items():
            hits = {key:c.common(shape).Volume for key,c in candidates.items()
                    if c.BoundBox.intersect(shape.BoundBox)}
            vol = sum(hits.values())
            maxima[name] = max(maxima[name],vol)
            if vol > .1:
                failures.append({"lift":lift,"forward":forward,"part":name,
                    "mm3":round(vol,4),"moving_parts_mm3":{k:v for k,v in hits.items() if v>.001}})
    stations = gasket_stations()
    missing_cap, missing_floor, blocked_groove = [], [], []
    for i,(p,n) in enumerate(stations):
        if not cap.isInside(p+n*.3,1e-5,True):
            missing_cap.append(i)
        if not plate.isInside(p-n*(GASKET_DEPTH+.2),1e-5,True):
            missing_floor.append(i)
        if plate.isInside(p-n*.7,1e-5,False):
            blocked_groove.append(i)
    path_len = sum((stations[(i+1)%len(stations)][0]-p).Length for i,(p,n) in enumerate(stations))
    groove = build_spatial_groove()
    holes = []
    fastener_checks = []
    for x in LOWER_LOCK_X:
        plate_bore = Part.makeCylinder(LOWER_CLEAR_D/2-.05,TECH_TOTAL_T+.2,
                                       vec(x,-.1,LOWER_LOCK_GLOBAL_Z),vec(0,1,0))
        seat = Part.makeCylinder(LOWER_INSERT_HOLE_D/2-.05,LOWER_INSERT_SEAT_DEPTH-.2,
                                vec(x,TECH_TOTAL_T+.05,LOWER_LOCK_GLOBAL_Z),vec(0,1,0))
        full_bore = Part.makeCylinder(LOWER_CLEAR_D/2,TECH_TOTAL_T+LOWER_BOSS_LENGTH,
                                      vec(x,0,LOWER_LOCK_GLOBAL_Z),vec(0,1,0))
        holes.append({"x":x,"plate_blockage":plate.common(plate_bore).Volume,
                      "insert_seat_blockage":cap.common(seat).Volume,
                      "seal_groove_intersection":groove.common(full_bore).Volume})
        head=build_lower_screw_head(x)
        screw=build_lower_screw(x)
        countersink=Part.makeCone(
            LOWER_HEAD_D/2,LOWER_CLEAR_D/2,LOWER_COUNTERSINK_DEPTH,
            vec(x,0,LOWER_LOCK_GLOBAL_Z),vec(0,1,0))
        driver=Part.makeCylinder(LOWER_DRIVER_D/2,LOWER_DRIVER_APPROACH+.1,
                                vec(x,-LOWER_DRIVER_APPROACH,LOWER_LOCK_GLOBAL_Z),vec(0,1,0))
        fastener_checks.append({
            "x_mm":x,
            "head_cap_collision_mm3":head.common(cap).Volume,
            "screw_cap_collision_mm3":screw.common(cap).Volume,
            "screw_plate_collision_mm3":screw.common(plate).Volume,
            "head_cap_clearance_mm":head.distToShape(cap)[0],
            "countersink_cap_collision_mm3":countersink.common(cap).Volume,
            "countersink_to_groove_mm":countersink.distToShape(groove)[0],
            "bore_to_groove_mm":full_bore.distToShape(groove)[0],
            "driver_cap_collision_mm3":driver.common(cap).Volume,
            "driver_plate_collision_mm3":driver.common(plate).Volume})
    obsolete = []
    for x,z,d in [(98,74,4.5),(116,74,4.5),(125,85,3.4)]:
        p = vec(x,1.5,z)
        obsolete.append({"x":x,"z":z,"material_restored":plate.isInside(p,1e-5,True)})
    wall_checks = []
    wall_obstacles = {"platine": plate, "pcb": pcb, "composants": components,
                      "rps": psu, "capot_rps": guard, "corps_bnc": bnc,
                      "cables_pg7": cables,"ecrous_pg7":nuts}
    wall_obstacles.update({"faisceau_rps_"+k:v for k,v in rps_wiring.items()})
    for x,y in WALL_MOUNTS:
        head = build_wall_head(x,y)
        screw = build_wall_screw_reference(x,y)
        insertion = Part.makeCylinder(wall_head_d(x,y)/2,
                        WALL_ACCESS_END_Z-WALL_HEAD_SEAT_Z,
                        vec(x,y,WALL_HEAD_SEAT_Z))
        driver = Part.makeCylinder(WALL_DRIVER_D/2,
                        WALL_ACCESS_END_Z-(WALL_HEAD_SEAT_Z+WALL_HEAD_MAX_H),
                        vec(x,y,WALL_HEAD_SEAT_Z+WALL_HEAD_MAX_H))
        checks = {"x_mm":x, "y_mm":y, "max_head_d_mm":wall_head_d(x,y),
                  "head_recess_d_mm":wall_recess_d(x,y),
                  "head_pcb_clearance_mm":head.distToShape(pcb)[0],
                  "head_insertion_pcb_clearance_mm":insertion.distToShape(pcb)[0],
                  "head_insertion_bnc_clearance_mm":insertion.distToShape(bnc)[0],
                  "driver_pcb_clearance_mm":driver.distToShape(pcb)[0],
                  "head_insertion_obstructions_mm3":{
                      k:insertion.common(s).Volume for k,s in wall_obstacles.items()},
                  "driver_obstructions_mm3":{
                      k:driver.common(s).Volume for k,s in wall_obstacles.items()},
                  "installed_screw_collisions_mm3":{
                      k:screw.common(s).Volume for k,s in wall_obstacles.items()}}
        checks["pass"] = all(max(checks[k].values())<=.01 for k in (
                    "head_insertion_obstructions_mm3", "driver_obstructions_mm3",
                    "installed_screw_collisions_mm3")) and (
                    checks["head_insertion_pcb_clearance_mm"] >= WALL_ACCESS_CLEARANCE_MIN-1e-6)
        wall_checks.append(checks)
    raw_gusset = build_technical_deck_root_gusset(False)
    removed_gusset = raw_gusset.common(build_wall_access_relief())
    remaining_gusset = build_technical_deck_root_gusset()
    old_access = Part.makeCylinder(WALL_DRIVER_D/2, WALL_ACCESS_END_Z-8.2,
                                  vec(175,18,8.2))
    old_pcb_obstruction = old_access.common(pcb).Volume
    # Presence de la plaque pleine sous l'encoche ; le test des collisions seul
    # ne prouve pas que cette region de la platine n'a pas ete percee accidentellement.
    mx,my = WALL_ACCESS_MOUNT
    base_probes = [vec(mx+dx,my+dy,z) for dx in (-5,0,5)
                   for dy in (-2,0,2) for z in (.5,1.5,2.5)
                   if dx*dx+dy*dy > (WALL_MOUNT_D/2+.1)**2]
    missing_base = [list(p) for p in base_probes if not plate.isInside(p,1e-5,True)]
    old_wall_holes_restored = all(plate.isInside(vec(x,18,z),1e-5,True)
                                 for x in (25,175) for z in (.5,1.5,2.5))
    old_left_access = Part.makeCylinder(WALL_DRIVER_D/2, WALL_ACCESS_END_Z-8.2,
                                       vec(44,7.7,8.2))
    old_left_cable_obstruction = old_left_access.common(cables).Volume
    old_wall_holes_restored = old_wall_holes_restored and all(
        plate.isInside(vec(46,25.5,z),1e-5,True) for z in (.5,1.5,2.5))
    old_wall_holes_restored = old_wall_holes_restored and all(
        plate.isInside(vec(38,7.7,z),1e-5,True) for z in (.5,1.5,2.5))
    # Controles de la nouvelle interface RPS, y compris temoins negatifs.
    port_checks=[]
    blank_guard=placed_psu_guard(build_psu_guard(False))
    for name,y in [('CN1_bas',-.2),('CN2_haut',GUARD_L-GUARD_T-.2)]:
        probe=placed_psu_guard(guard_port_probe(y))
        row={'name':name,'width_mm':GUARD_PORT_W,'height_mm':GUARD_PORT_H,
             'cable_d_mm':GUARD_CABLE_D,'guard_obstruction_mm3':probe.common(guard).Volume,
             'blank_guard_obstruction_mm3':probe.common(blank_guard).Volume,
             'cable_to_guard_mm':probe.distToShape(guard)[0]}
        row['pass']=row['guard_obstruction_mm3']<=.01 and row['blank_guard_obstruction_mm3']>.1
        port_checks.append(row)
    wiring_obstacles={'platine':plate,'protection':guard,'PCB':pcb,'composants':components,
                      'RPS':psu,'vis_murales':wall_screws,'capot':cap}
    wiring_checks={name:{k:route.common(shape).Volume for k,shape in wiring_obstacles.items()}
                   for name,route in rps_wiring.items()}
    guard_removal=[]
    for forward in range(0,61,3):
        candidate=guard.translated(vec(0,0,forward))
        for name,shape in {'platine':plate,'RPS':psu,'PCB':pcb,**rps_wiring}.items():
            volume=candidate.common(shape).Volume
            if volume>.1:
                guard_removal.append({'forward_mm':forward,'obstacle':name,'mm3':volume})
    barrier=build_mains_barrier()
    misplaced_barrier=barrier.translated(vec(44-MAINS_BARRIER_X,0,0))
    pg7_obstacles={'platine':plate,'PCB':pcb,'composants':components,'RPS':psu,
                   'protection_RPS':guard,'capot':cap,'vis_murales':wall_screws}
    nut_checks={name:nuts.common(shape).Volume for name,shape in pg7_obstacles.items()}
    corridor_checks={name:cables.common(shape).Volume for name,shape in
        {'PCB':pcb,'composants':components,'RPS':psu,'protection_RPS':guard,'cloison':barrier}.items()}
    nut_shapes=[build_pg7_body(x,z,True) for x,z in PG7_LAYOUT]
    nut_pair_gaps=[nut_shapes[i].distToShape(nut_shapes[j])[0]
                  for i in range(3) for j in range(i+1,3)]
    pg7_bore_checks=[]
    for x,z in PG7_LAYOUT:
        bore=Part.makeCylinder(PG7_D/2-.05,TECH_TOTAL_T+.2,
            vec(x,-.1,z),vec(0,1,0))
        pg7_bore_checks.append({'x_mm':x,'z_mm':z,'obstruction_mm3':bore.common(plate).Volume})
    wire_pairs=[{'a':a,'b':b,'intersection_mm3':rps_wiring[a].common(rps_wiring[b]).Volume}
                for i,a in enumerate(rps_wiring) for b in list(rps_wiring)[i+1:]]
    barrier_probe_y=(MAINS_BARRIER_START_Y+1,
                     (MAINS_BARRIER_START_Y+MAINS_BARRIER_END_Y)/2,
                     MAINS_BARRIER_END_Y-1)
    barrier_missing=[y for y in barrier_probe_y if not plate.isInside(
        vec(MAINS_BARRIER_X+.3,y,REAR+1),1e-5,True)]
    barrier_extension_probes=[vec(MAINS_BARRIER_X+.3,y,z) for y in barrier_probe_y
        for z in (REAR+1,REAR+MAINS_BARRIER_H/2,REAR+MAINS_BARRIER_H-.5)]
    barrier_extension_missing=[list(p) for p in barrier_extension_probes
        if not plate.isInside(p,1e-5,True)]
    barrier_missing_volume=barrier.Volume-barrier.common(plate).Volume
    barrier_cap_gap=barrier.distToShape(cap)[0]
    baffle=placed_psu_guard(build_guard_barrier_baffle())
    baffle_missing=baffle.Volume-baffle.common(guard).Volume
    baffle_plate_gap=baffle.distToShape(plate)[0]
    baffle_barrier_gap=baffle.distToShape(barrier)[0]
    # Sonde transversale de la fente: le retour doit couper ce passage direct.
    seam_probe=Part.makeCylinder(.1,10,vec(MAINS_BARRIER_X-6,
        MAINS_BARRIER_END_Y+GUARD_BARRIER_GAP/2,REAR+15),vec(1,0,0))
    bare_guard=placed_psu_guard(build_psu_guard(True,False))
    seam_blocked=seam_probe.common(guard).Volume
    seam_bare_blocked=seam_probe.common(bare_guard).Volume
    barrier_guard_gap=barrier.distToShape(guard)[0]
    pull = cap.translated(vec(0,0,HOOK_PULL_TEST)).common(plate).Volume
    lift_free = Part.makeCompound([cap]).translated(vec(0,HOOK_RELEASE_LIFT,0)).common(plate).Volume
    # Classifier la frontiere tessellee : les booleens de volumes presque
    # entierement coincidants echouent parfois dans OCC sur ces raccords.
    outer = build_cover_outer_envelope()
    boundary_points, _ = parts["cover_monobloc"].copy().tessellate(.05)
    outside_points = [list(p) for p in boundary_points if not outer.isInside(p,1e-5,True)]
    colours = [parts["cover_core"].common(parts["graphite_skin"]).Volume,
               parts["cover_core"].common(parts["turquoise_inlay"]).Volume,
               parts["backplate_core"].common(parts["technical_skin"]).Volume]
    report = {
        "revision":"V15 Dosita MP1 - PCB v3 (principale 119x100 + mesure 73x54); corps %gx%gx%g" % (BOX_W,BOX_H,BODY_D),
        "units":"mm",
        "parts":part_reports,
        "pump_interface":pump_interface_checks(parts),
        "second_psu_format":alt_psu_checks(parts),
        "pcb_interface":pcb_interface_checks(parts,cap),
        "printable_solids":{"pass":all(s.isValid() and len(s.Solids)==1 for s in parts.values())},
        "outer_wall_closure":outer_wall_closure_check(parts["cover_monobloc"]),
        "seal_path_geometry":{"left_x_mm":GASKET_PATH_X,
            "deck_left_x_mm":SEAL_DECK_X,"elbow_radius_mm":SEAL_ELBOW_R,
            "construction":"secteurs annulaires circulaires extrudes, aucune facette CAD",
            "pass":abs(GASKET_PATH_X-SEAL_DECK_X)<1e-9},
        "envelope":{"body_mm":[BOX_W,BOX_H,BODY_D],
                    "boundary_sample_count":len(boundary_points),
                    "boundary_mesh_deflection_mm":.05,"outside_boundary_samples":outside_points,
                    "pass":not outside_points and BOX_W<=220 and BOX_H<=220},
        "static_collisions":{"volumes_mm3":static,"pass":max(static.values())<=.1},
        "wired_installation":{"pose_count":len(poses),"approach_step_mm":1,"lowering_step_mm":.25,
            "maxima_mm3":maxima,"failures":failures,
            "moving_part_count":len(moving_parts),
            "collision_method":"somme des intersections de chaque solide mobile avec chaque obstacle fixe, sans compound superpose",
            "cable_envelope":"18 mm large; Y0..20; Z39..75; axes (X,Z) 44/48,68/48,88/48",
            "scope":"capot avec pompes, reserve axiale bornes et enveloppes M3; radial bornes, cosses et fils moteurs reels non qualifies",
            "pass":max(maxima.values())<=.1},
        "actual_sealing_interface":{"station_count":len(stations),"approx_cord_length_mm":round(path_len,1),
            "groove_width_mm":GASKET_W,"groove_depth_mm":GASKET_DEPTH,"cord_diameter_mm":GASKET_CORD_D,
            "nominal_compression_percent":round(100*GASKET_COMPRESSION,1),
            "groove_solid_count":len(groove.Solids),"groove_valid":groove.isValid(),
            "missing_moving_counterface":missing_cap,"missing_fixed_groove_floor":missing_floor,
            "obstructed_groove":blocked_groove,
            "pass":not (missing_cap or missing_floor or blocked_groove) and len(groove.Solids)==1 and groove.isValid()},
        "lower_screws":{"count":len(LOWER_LOCK_X),"x_mm":list(LOWER_LOCK_X),"z_mm":LOWER_LOCK_GLOBAL_Z,
            "outer_spacing_mm":LOWER_LOCK_X[-1]-LOWER_LOCK_X[0],"checks":holes,"obsolete_holes":obsolete,
            "pass":all(max(h["plate_blockage"],h["insert_seat_blockage"],h["seal_groove_intersection"])<=.1 for h in holes)
                and all(h["material_restored"] for h in obsolete)},
        "fastener_envelopes":{
            "head_nominal_d_mm":LOWER_HEAD_D,"head_angle_deg":90,
            "printed_countersink_depth_mm":LOWER_COUNTERSINK_DEPTH,
            "screw_head_depth_mm":LOWER_SCREW_HEAD_DEPTH,
            "front_edge_margin_mm":TECH_FRONT_Z-(LOWER_LOCK_GLOBAL_Z+LOWER_HEAD_D/2),
            "driver_d_mm":LOWER_DRIVER_D,"driver_approach_mm":LOWER_DRIVER_APPROACH,
            "seal_web_min_mm":LOWER_SEAL_WEB_MIN,"checks":fastener_checks,
            "pass":TECH_FRONT_Z-(LOWER_LOCK_GLOBAL_Z+LOWER_HEAD_D/2)>=LOWER_HEAD_EDGE_MARGIN_MIN-1e-6
                and all(max(c[k] for k in (
                    "head_cap_collision_mm3","screw_cap_collision_mm3","screw_plate_collision_mm3",
                    "countersink_cap_collision_mm3","driver_cap_collision_mm3",
                    "driver_plate_collision_mm3"))<=.01
                    and c["head_cap_clearance_mm"]>=LOWER_HEAD_EDGE_MARGIN_MIN-1e-6
                    and min(c["countersink_to_groove_mm"],c["bore_to_groove_mm"])>=LOWER_SEAL_WEB_MIN-1e-6
                    for c in fastener_checks)},
        "front_seal_cable_corridor":{
            "counterface_front_start_z_mm":SEAL_DECK_FRONT_Z-SEAL_DECK_LAND_W/2,
            "cable_corridor_end_z_mm":PG7_CABLE_INSERTION_KEEP_Z1,
            "nominal_gap_mm":SEAL_DECK_FRONT_Z-SEAL_DECK_LAND_W/2-PG7_CABLE_INSERTION_KEEP_Z1,
            "pass":SEAL_DECK_FRONT_Z-SEAL_DECK_LAND_W/2-PG7_CABLE_INSERTION_KEEP_Z1
                >=SEAL_CABLE_CLEARANCE_MIN-1e-6},
        "retention":{"top_hook_normal_play_mm":HOOK_PULL_CLEARANCE,
            "lower_locator_count":len(LOWER_LOCATOR_X),
            "forward_pull_1mm_collision_mm3":pull,"lift_4mm_collision_mm3":lift_free,
            "pass":pull>.1 and lift_free<=.1 and HOOK_PULL_CLEARANCE<=.15},
        "wall_screw_access":{"positions_mm":[list(p) for p in WALL_MOUNTS],
            "driver_d_mm":WALL_DRIVER_D,"access_end_z_mm":WALL_ACCESS_END_Z,
            "head_height_max_mm":WALL_HEAD_MAX_H,"checks":wall_checks,
            "negative_control_old_axis_pcb_collision_mm3":old_pcb_obstruction,
            "negative_control_old_left_axis_cable_collision_mm3":old_left_cable_obstruction,
            "old_wall_holes_material_restored":old_wall_holes_restored,
            "scope":"capot et pompes deposes, PCB/RPS/BNC/PG7 en place; tete et tige incluses",
            "pass":all(c["pass"] for c in wall_checks) and old_pcb_obstruction>.1
                   and old_wall_holes_restored and old_left_cable_obstruction>.1},
        "wall_access_gusset_relief":{
            "removed_volume_mm3":removed_gusset.Volume,
            "original_gusset_volume_mm3":raw_gusset.Volume,
            "remaining_gusset_valid":remaining_gusset.isValid(),
            "remaining_gusset_solids":len(remaining_gusset.Solids),
            "removed_groove_intersection_mm3":removed_gusset.common(groove).Volume,
            "missing_base_probes":missing_base,"base_probe_count":len(base_probes),
            "pass":remaining_gusset.isValid() and len(remaining_gusset.Solids)==1
                   and removed_gusset.common(groove).Volume<=.01 and not missing_base},
        "rps_cable_ports":{'orientation':'CN1 en bas, CN2 en haut',
            'port_checks':port_checks,'nominal_bundle_d_mm':GUARD_CABLE_D,
            'route_collisions_mm3':wiring_checks,
            'guard_removal_pose_count':21,'guard_removal_failures':guard_removal,
            'guard_plate_collision_mm3':guard.common(plate).Volume,
            'negative_old_guard_frame_collision_mm3':guard.translated(vec(-GUARD_OFFSET_X,0,0)).common(plate).Volume,
            'route_scope':'faisceaux nominaux Ø6; fiches JST et rayons reels non modelises',
            'pass':all(c['pass'] for c in port_checks)
                and all(max(v.values())<=.01 for v in wiring_checks.values())
                and not guard_removal and guard.common(plate).Volume<=.01
                and guard.translated(vec(-GUARD_OFFSET_X,0,0)).common(plate).Volume>.1},
        "pg7_layout":{'axes_x_z_mm':[list(p) for p in PG7_LAYOUT],
            'common_row_z_mm':PG7_ROW_Z,
            'row_alignment_spread_mm':max(z for x,z in PG7_LAYOUT)-min(z for x,z in PG7_LAYOUT),
            'functions':list(PG7_FUNCTIONS),'contact_cables':'basse tension / contacts secs, confirme utilisateur',
            'body_envelope_d_mm':PG7_BODY_D,'internal_nut_length_mm':PG7_NUT_LENGTH,
            'bore_checks':pg7_bore_checks,
            'nut_collisions_mm3':nut_checks,'corridor_collisions_mm3':corridor_checks,
            'nut_pair_gaps_mm':nut_pair_gaps,'wiring_pair_checks':wire_pairs,
            'command_corridor_to_components_mm':build_pg7_corridors(PG7_LAYOUT[1:]).distToShape(components)[0],
            'pass':max(nut_checks.values())<=.01 and max(corridor_checks.values())<=.01
                and all(r['obstruction_mm3']<=.01 for r in pg7_bore_checks)
                and max(z for x,z in PG7_LAYOUT)-min(z for x,z in PG7_LAYOUT)<=1e-6
                and build_pg7_corridors(PG7_LAYOUT[1:]).distToShape(components)[0]>=1-1e-6
                and min(nut_pair_gaps)>=1 and all(p['intersection_mm3']<=.01 for p in wire_pairs)},
        "mains_barrier":{'x_mm':MAINS_BARRIER_X,'thickness_mm':MAINS_BARRIER_T,
            'start_y_mm':MAINS_BARRIER_START_Y,'end_y_mm':MAINS_BARRIER_END_Y,
            'height_mm':MAINS_BARRIER_H,'continuous_without_access_notch':True,
            'front_end_z_mm':REAR+MAINS_BARRIER_H,
            'cap_clearance_mm':barrier_cap_gap,
            'extension_probe_count':len(barrier_extension_probes),
            'extension_missing_probes':barrier_extension_missing,
            'expected_barrier_missing_volume_mm3':barrier_missing_volume,
            'supply_corridor_clearance_mm':barrier.distToShape(build_pg7_corridors(PG7_LAYOUT[:1]))[0],
            'command_corridor_clearance_mm':barrier.distToShape(build_pg7_corridors(PG7_LAYOUT[1:]))[0],
            'valid':barrier.isValid(),'solids':len(barrier.Solids),
            'foot_probe_y_mm':list(barrier_probe_y),
            'foot_missing_probes':barrier_missing,
            'groove_intersection_mm3':barrier.common(groove).Volume,
            'negative_misplaced_barrier_cable_collision_mm3':misplaced_barrier.common(cables).Volume,
            'cap_clearance_max_mm':MAINS_BARRIER_CAP_GAP_MAX,
            'pass':barrier.isValid() and len(barrier.Solids)==1 and not barrier_missing
                and 1.0-1e-6 <= barrier_cap_gap <= MAINS_BARRIER_CAP_GAP_MAX+1e-6
                and not barrier_extension_missing and abs(barrier_missing_volume)<=.01
                and barrier_cap_gap>=1-1e-6
                and barrier.common(groove).Volume<=.01 and misplaced_barrier.common(cables).Volume>.1},
        "rps_guard_mounts":rps_guard_mount_checks(plate,guard,pcb,components,psu,rps_wiring),
        "rps_barrier_labyrinth":{
            'mount_local_x_mm':list(GUARD_MOUNT_X),'tab_width_mm':GUARD_TAB_W,
            'barrier_guard_clearance_mm':barrier_guard_gap,
            'lateral_overlap_mm':GUARD_BAFFLE_OVERLAP,
            'baffle_thickness_mm':GUARD_BAFFLE_T,
            'baffle_missing_volume_mm3':baffle_missing,
            'baffle_barrier_clearance_mm':baffle_barrier_gap,
            'baffle_plate_clearance_mm':baffle_plate_gap,
            'straight_seam_probe_blocked_mm3':seam_blocked,
            'negative_without_baffle_blocked_mm3':seam_bare_blocked,
            'scope':'recouvrement local seulement; ni compartiment ferme ni isolation certifiee',
            'pass':abs(baffle_missing)<=.01 and barrier_guard_gap>=.5-1e-6
                and baffle_barrier_gap>=.5-1e-6 and baffle_plate_gap>=.5-1e-6
                and seam_blocked>.01 and seam_bare_blocked<=.01},
        "colour_partition":{"overlap_mm3":colours,"pass":max(colours)<=.1},
        "component_clearance":{"cap_to_pcb_mm":cap.distToShape(pcb)[0],
            "cap_to_components_mm":cap.distToShape(components)[0],
            "cap_to_psu_guard_mm":cap.distToShape(guard)[0],
            "pump_to_pcb_components_mm":pump_rear_clearance(),
            "pcb_to_psu_lateral_gap_mm":PCB_X-(PSU_X+PSU_W),
            "pass":cap.distToShape(components)[0]>=1 and cap.distToShape(guard)[0]>=1
                and pump_rear_clearance()>=1
                and PCB_SERVICE_BAY_W>=PCB_LEFT_CONNECTOR_CLEARANCE_MIN
                and build_pcb_service_bay().common(components).Volume<=.01
                and build_pcb_service_bay().common(guard).Volume<=.01},
        "limits":["Pas de certification IP.","Compression geometrique, pas simulation elastique ni serrage.",
            "Tolerance/retrait ASA, rugosite FDM et raccord du cordon a valider sur prototype.",
            "Montage reel avec cables, ecrous PG7 et connecteurs moteurs obligatoire avant serie."]
    }
    # Temoin negatif explicite, aligne sur la rangee actuelle : traverse fictive.
    # L'ancienne traverse a Z29 ne croise plus les cables avances a Z48.
    old_lip=Part.makeBox(190,3,2.5,vec(10,7+HOOK_RELEASE_LIFT,PG7_ROW_Z-1))
    negative=old_lip.common(cables).Volume
    report["negative_control"]={"hypothetical_lip_z_mm":PG7_ROW_Z-1,
        "hypothetical_lip_cable_collision_mm3":negative,"pass":negative>.1}
    witness = Part.makeBox(2,2,.5,vec(PCB_X+30,PCB_Y+30,REAR+PCB_STANDOFF_H+.4))
    witness_volume = sum(s.common(pcb).Volume for s in [witness])
    report['moving_parts_negative_control']={'deliberate_pcb_intersection_mm3':witness_volume,
        'pass':witness_volume>.1}
    report["pass"]=all(v.get("pass",True) for v in report.values() if isinstance(v,dict))
    (REPORT_DIR/"validation_report.json").write_text(json.dumps(report,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")
    print("VALIDATION",json.dumps(report,ensure_ascii=False))
    return report

def build_wall_access_coupon(backplate):
    # Extrait exact du coin droit bas, sans simplifier le renfort, le bossage
    # ou la gorge voisine. La translation rend le petit coupon facile a trancher.
    shape = backplate.common(Part.makeBox(26.0,28.0,16.0,vec(178.0,0,0)))
    shape.translate(vec(-178.0,0,0))
    return safe_refine(shape)


def build_pcb_fit_coupon():
    """Gabarit d'entraxes reel 119.38 / 104.14, pas une carte fonctionnelle."""
    gauge=Part.makeBox(PCB_W,PCB_H,2.0)
    for kx,ky in MAIN_HOLES_KICAD:
        gauge=gauge.cut(Part.makeCylinder(PCB_HOLE_D/2,2.2,vec(kx,PCB_H-ky,-.1)))
    return safe_refine(gauge)

def build_measure_board_coupon(backplate):
    """Extrait reel du fond : les quatre plots de la carte de mesure et la paroi du bas avec
    ses deux percages BNC. A imprimer AVANT le boitier, pour presenter la vraie carte avec ses
    prises : c'est le seul essai qui verifie la hauteur des percages (plot + carte + prise)."""
    x0=MEAS_X-5.0
    shape=backplate.common(Part.makeBox(MEAS_W+10.0,MEAS_Y+MEAS_H+4.0,BNC_AXIS_Z+BNC_CUTOUT_D/2+6.0,vec(x0,0,0)))
    shape.translate(vec(-x0,0,0))
    return safe_refine(shape)

def build_left_wall_access_coupon(backplate):
    # Extrait reel des trois PG7, cloison et fixation murale gauche.
    shape=backplate.common(Part.makeBox(100,40,75,vec(10,0,0)))
    shape.translate(vec(-10,0,0))
    return safe_refine(shape)

def main():
    technical_skin = build_backplate_technical_skin()
    backplate_core = build_wall_backplate()
    graphite_skin = build_cover_graphite_skin()
    turquoise_inlay = build_cover_turquoise_inlay()
    cover_core = build_cover_light_core(graphite_skin, turquoise_inlay)
    cover_monobloc = build_cover_monobloc(
        cover_core, graphite_skin, turquoise_inlay)
    backplate_monobloc = build_backplate_monobloc(
        backplate_core, technical_skin)

    parts = {
        "backplate_core": backplate_core,
        "technical_skin": technical_skin,
        "backplate_monobloc": backplate_monobloc,
        "cover_core": cover_core,
        "graphite_skin": graphite_skin,
        "turquoise_inlay": turquoise_inlay,
        "cover_monobloc": cover_monobloc,
        "psu_guard": build_psu_guard(),
        "pump_coupon": build_pump_fit_coupon(),
        "connector_coupon": build_connector_fit_coupon(),
        "m4_coupon": build_m4_insert_fit_coupon(),
        "gasket_coupon": build_gasket_fit_coupon(),
        "wall_access_coupon": build_wall_access_coupon(backplate_monobloc),
        "left_wall_access_coupon":build_left_wall_access_coupon(backplate_monobloc),
        "measure_board_coupon":build_measure_board_coupon(backplate_monobloc),
        "pcb_coupon":build_pcb_fit_coupon(),
    }

    reports = []
    reports.append(save_part(
        "PoolControllerV15MP1_WallBackplateLightCore", "WallBackplateLightCore",
        "Platine murale - noyau ASA clair", parts["backplate_core"],
        (0.82, 0.80, 0.74), [
            ("Width", "Largeur", BOX_W),
            ("Height", "Hauteur", BOX_H),
            ("SealFace", "Plan de joint", SEAL_FACE_Z),
            ("GasketGrooveWidth", "Largeur gorge", GASKET_W),
            ("GasketGrooveDepth", "Profondeur gorge", GASKET_DEPTH),
            ("GasketSupportWall", "Paroi mini autour gorge",
             GASKET_SUPPORT_WALL),
            ("DeckRootGussetLeg", "Aile gousset sous-face",
             TECH_ROOT_GUSSET_LEG),
            ("WallAccessAxisX", "Axe fixation murale droite basse X", WALL_ACCESS_MOUNT[0]),
            ("WallAccessAxisY", "Axe fixation murale droite basse Y", WALL_ACCESS_MOUNT[1]),
            ("WallAccessHeadMaxDiameter", "Diametre maxi tete murale droite basse", WALL_ACCESS_HEAD_MAX_D),
            ("WallLeftAccessAxisX", "Axe fixation murale gauche basse X", WALL_LEFT_LOWER_MOUNT[0]),
            ("MainsBarrierX", "Position cloison secteur", MAINS_BARRIER_X),
            ("MainsBarrierStartY", "Pied cloison", MAINS_BARRIER_START_Y),
            ("MainsBarrierEndY", "Extremite haute cloison", MAINS_BARRIER_END_Y),
            ("MainsBarrierDepth", "Profondeur cloison", MAINS_BARRIER_H),
            ("PCBWidth", "Dimension PCB corrigee", PCB_W),
            ("PCBHeight", "Dimension PCB corrigee", PCB_H),
            ("PCBMountSpacing", "Entraxe PCB", PCB_W-2*PCB_HOLE_EDGE),
            ("PCBLeftX", "Bord gauche PCB", PCB_X),
            ("PCBStandoffHeight", "Hauteur entretoises PCB", PCB_STANDOFF_H),
            ("MaterialCode", "ASA standard clair", 1.0),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_WallBackplateTechnicalSkin",
        "WallBackplateTechnicalSkin",
        "Platine murale - peau technique noire", parts["technical_skin"],
        (0.07, 0.08, 0.09), [
            ("Width", "Largeur", TECH_W),
            ("Depth", "Profondeur", TECH_D),
            ("Thickness", "Epaisseur", TECH_SKIN_T),
            ("MaterialCode", "ASA noir standard", 2.0),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_CoverLightCore", "CoverLightCore",
        "Capot cloche - noyau ASA clair", parts["cover_core"],
        (0.91, 0.89, 0.83), [
            ("Width", "Largeur", BOX_W),
            ("Height", "Hauteur", BOX_H),
            ("Depth", "Profondeur locale", COVER_D),
            ("SideGuideLength", "Longueur guide lateral", SIDE_GUIDE_H),
            ("CompressionLipWidth", "Largeur levre joint",
             COMPRESSION_RING_W),
            ("StructuralFlangeThickness", "Epaisseur bride",
             COMPRESSION_FLANGE_T),
            ("StructuralFlangeRecess", "Decalage bride / zone joint",
             COMPRESSION_FLANGE_RECESS),
            ("CompressionGussetRun", "Course nervure oblique",
             COMPRESSION_GUSSET_RUN),
            ("CompressionGussetRise", "Montee nervure oblique",
             COMPRESSION_GUSSET_RISE),
            ("CompressionGussetAngle", "Angle nervure oblique",
             COMPRESSION_GUSSET_ANGLE_DEG),
            ("LowerLockSpacing", "Entraxe des vis M4 exterieures", LOWER_LOCK_X[-1] - LOWER_LOCK_X[0]),
        ("PG7CableInsertionKeepoutEnd",
             "Fin enveloppe insertion cables PG7",
             PG7_CABLE_INSERTION_KEEP_Z1),
            ("MaterialCode", "ASA standard clair", 1.0),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_CoverGraphiteSkin", "CoverGraphiteSkin",
        "Capot - ruban graphite ASA-CF", parts["graphite_skin"],
        (0.09, 0.10, 0.11), [
            ("Width", "Largeur", RIBBON_W),
            ("Height", "Hauteur", RIBBON_H),
            ("Thickness", "Epaisseur", RIBBON_SKIN_T),
            ("SideReturn", "Retour sur facette", RIBBON_RETURN_W),
            ("MaterialCode", "ASA-CF graphite", 3.0),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_CoverTurquoiseInlay", "CoverTurquoiseInlay",
        "Capot - signature turquoise", parts["turquoise_inlay"],
        (0.04, 0.62, 0.66), [
            ("Width", "Longueur", ACCENT_W),
            ("Height", "Hauteur", ACCENT_H),
            ("VisibleThickness", "Epaisseur visible", ACCENT_VISIBLE_T),
            ("TotalDepth", "Profondeur totale", ACCENT_TOTAL_DEPTH),
            ("AnchorSpread", "Elargissement cache par cote",
             ACCENT_ANCHOR_SPREAD),
            ("MaterialCode", "ABS turquoise", 4.0),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_CoverMonoblocReference", "CoverMonoblocReference",
        "REFERENCE - capot monobloc uni", parts["cover_monobloc"],
        (0.80, 0.80, 0.80), [
            ("Width", "Largeur", BOX_W),
            ("Height", "Hauteur", BOX_H),
            ("Depth", "Profondeur", COVER_D),
            ("MaterialCode", "Reference geometrique", 9.0),
        ], export_mesh=False))
    reports.append(save_part(
        "PoolControllerV15MP1_WallBackplateMonoblocReference",
        "WallBackplateMonoblocReference",
        "REFERENCE - platine monobloc unie", parts["backplate_monobloc"],
        (0.80, 0.80, 0.80), [
            ("Width", "Largeur", BOX_W),
            ("Height", "Hauteur", BOX_H),
            ("Depth", "Profondeur max", SEAL_FACE_Z),
            ("MaterialCode", "Reference geometrique", 9.0),
        ], export_mesh=False))
    reports.append(save_part(
        "PoolControllerV15MP1_PSUGuard", "PSUGuard",
        "Capot de securite RPS-30-12", parts["psu_guard"],
        (0.90, 0.52, 0.13), [
            ("Width", "Largeur", GUARD_W),
            ("Length", "Longueur", GUARD_L),
            ("Height", "Hauteur", GUARD_H),
            ("BarrierInterfaceGap", "Jeu nominal interface cloison", GUARD_BARRIER_GAP),
            ("BarrierBaffleOverlap", "Recouvrement lateral chicane", GUARD_BAFFLE_OVERLAP),
            ("MaterialCode", "ASA standard", 1.0),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_MP1FitCoupon", "MP1FitCoupon",
        "Coupon interface pompe Dosita MP1", parts["pump_coupon"],
        (0.55, 0.55, 0.55), [
            ("Thickness", "Epaisseur", FRONT_CORE_T),
            ("MotorCutout", "Passage moteur", PUMP_MOTOR_CUTOUT_D),
            ("MountSpacing", "Entraxe", PUMP_MOUNT_DX),
            ("HoleDiameter", "Passage M4 provisoire a qualifier", PUMP_SCREW_CLEAR_D),
            ("MaterialCode", "PLA de controle ou ASA", 8.0),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_ConnectorFitCoupon", "ConnectorFitCoupon",
        "Coupon PG7 et BNC en D", parts["connector_coupon"],
        (0.55, 0.55, 0.55), [
            ("Thickness", "Epaisseur", TECH_TOTAL_T),
            ("PG7", "Passage PG7", PG7_D),
            ("BNC", "Passage BNC", BNC_CUTOUT_D),
            ("BNCFlat", "Meplat", BNC_FLAT_OFFSET),
            ("MaterialCode", "PLA de controle ou ASA", 8.0),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_M4InsertFitCoupon", "M4InsertFitCoupon",
        "Coupon inserts M4 5.4 / 5.6 / 5.8", parts["m4_coupon"],
        (0.55, 0.55, 0.55), [
            ("Thickness", "Epaisseur", 8.0),
            ("NominalHole", "Alesage nominal", LOWER_INSERT_HOLE_D),
            ("SeatDepth", "Profondeur", LOWER_INSERT_SEAT_DEPTH),
            ("MaterialCode", "Materiau final", 8.0),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_GasketFitCoupon", "GasketFitCoupon",
        "Coupon gorge cordon EPDM", parts["gasket_coupon"],
        (0.55, 0.55, 0.55), [
            ("Cord", "Diametre cordon", GASKET_CORD_D),
            ("GrooveWidth", "Largeur gorge", GASKET_W),
            ("GrooveDepth", "Profondeur gorge", GASKET_DEPTH),
            ("MaterialCode", "Materiau final", 8.0),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_WallAccessFitCoupon", "WallAccessFitCoupon",
        "Coupon fixation murale sous PCB", parts["wall_access_coupon"],
        (0.55,0.55,0.55), [
            ("HeadMaxDiameter", "Diametre maxi tete", WALL_ACCESS_HEAD_MAX_D),
            ("HeadMaxHeight", "Hauteur maxi tete", WALL_HEAD_MAX_H),
            ("DriverDiameter", "Diametre tige outil", WALL_DRIVER_D),
            ("PCBEdgeY", "Bord bas PCB a reproduire pour essai", PCB_Y),
            ("PCBBackZ", "Face arriere PCB a reproduire pour essai", REAR+PCB_STANDOFF_H),
            ("MaterialCode", "Materiau final", 8.0),
        ]))

    reports.append(save_part(
        "PoolControllerV15MP1_MeasureBoardFitCoupon", "MeasureBoardFitCoupon",
        "Coupon carte de mesure : quatre plots et les deux percages BNC",parts["measure_board_coupon"],
        (.55,.55,.55),[
            ("OriginalXOffset", "Decalage X de l'extrait",MEAS_X-5.0),
            ("BncAxisZ", "Hauteur de l'axe BNC depuis le dos du fond",BNC_AXIS_Z),
            ("BncHoleD", "Diametre des percages BNC",BNC_CUTOUT_D),
            ("StandoffH", "Hauteur des plots",PCB_STANDOFF_H),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_LeftWallAccessFitCoupon", "LeftWallAccessFitCoupon",
        "Coupon fixation gauche, cloison et trois PG7",parts["left_wall_access_coupon"],
        (.55,.55,.55),[
            ("OriginalXOffset", "Decalage X de l'extrait",10.0),
            ("ScrewX", "Axe vis dans le coupon",WALL_LEFT_LOWER_MOUNT[0]-10),
            ("ScrewY", "Axe vis dans le coupon",WALL_LEFT_LOWER_MOUNT[1]),
            ("HeadMaxDiameter", "Diametre maxi tete",WALL_ACCESS_HEAD_MAX_D),
            ("DriverDiameter", "Diametre tige outil",WALL_DRIVER_D),
            ("MaterialCode", "Materiau final",8.0),
        ]))
    reports.append(save_part(
        "PoolControllerV15MP1_PCBFitCoupon", "PCBFitCoupon",
        "Gabarit PCB 119.38 / entraxe 104.14", parts['pcb_coupon'], (.15,.55,.25), [
            ("PCBWidth", "Largeur PCB", PCB_W),
            ("PCBHeight", "Hauteur PCB", PCB_H),
            ("HoleSpacing", "Entraxe PCB", PCB_W-2*PCB_HOLE_EDGE),
            ("HoleDiameter", "Diametre provisoire M3", PCB_HOLE_D),
        ]))
    build_assembly(parts)
    report = write_validation_report(reports, parts)
    if not report["pass"]:
        raise RuntimeError("Une validation dimensionnelle V15 a echoue")
    print("Generation V15 terminee et validations principales reussies.")


if __name__ == "__main__":
    main()
