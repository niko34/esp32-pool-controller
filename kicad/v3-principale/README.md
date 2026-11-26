# Carte principale du PCB v3 — composants et connexions

Troisième carte du v3 ([feature-215](../../specs/features/todo/feature-215-conception-du-pcb-v3.md)),
à côté de la carte de mesure ([`../v3-mesure-std/`](../v3-mesure-std/README.md)).
Elle porte l'ESP32, le port USB, l'horloge et sa pile, les sorties de puissance, les
régulateurs et la sonde de température.

⏳ **État au 2026-11-25 : liste des composants et tableau des connexions, version 0.2 —
relus une fois par `electronics-reviewer`, quatre erreurs corrigées (voir « Relecture »).
Schéma rév. 0.2 écrit par script, relu par `electronics-reviewer` : aucune erreur, deux remarques portées.
**Carte rév. 0.1 placée et routée le 2026-11-26** — contrôle des règles : 0 violation, 0 liaison
manquante, 0 écart avec le schéma. Relue par `electronics-reviewer` : aucune erreur, cinq
retouches portées. ⏳ Rien n'est encore ouvert dans KiCad par l'utilisateur.** Horloge : **DS3231SN** depuis le 2026-11-26 (décision de l'utilisateur : la qualité l'emporte sur le nombre de fabricants et sur le prix) ; carte retouchée à la main, contrôle des règles identique.

## Les fichiers de ce dossier

| Fichier | Rôle |
|---|---|
| `generer_schema.py` | écrit le schéma depuis les connexions ci-dessous. ⚠️ Le relancer **réécrit** le schéma en entier. Il lit les symboles standard de KiCad (paquet `kicad-symbols`) |
| `v3-principale.kicad_sch` | le schéma, format A1, un cadre par fonction |
| `v3-principale.kicad_sym`, `sym-lib-table` | le symbole du module ESP32, absent des bibliothèques de KiCad |
| `v3-principale.kicad_pro` | le projet |

**Comment le schéma est fait** : chaque broche porte un bout de fil et une étiquette ; deux
broches sont reliées quand elles portent la même étiquette. Il n'y a pas de long fil à suivre.

**Vérifié le 2026-11-25** : la liste des connexions que KiCad extrait du schéma est identique
à celle voulue — 62 fils, 20 broches en l'air (schéma rév. 0.2).

**Le contrôle électrique de KiCad rend un message attendu** : « deux sorties d'alimentation
reliées », entre la broche V3 du pont USB et la sortie du régulateur 3,3 V. C'est le câblage
que demande le fabricant du pont quand il est alimenté en 3,3 V. Les autres messages
viennent des bibliothèques absentes de la machine qui a généré le schéma.

**Ce que le schéma ne contient pas encore**
- les empreintes du module ESP32 (absente de KiCad, à dessiner d'après la fiche d'Espressif)
  et du support de pile (celle posée est provisoire) ;
- les trous de fixation, qui viendront avec la carte.

Règle suivie : D8 de feature-215 — composants courants, posés par l'atelier, un équivalent
d'un autre fabricant sur la même empreinte, la qualité restant un critère majeur.

## Ce que la carte reprend du v2, et ce qui change

| Fonction | v2 | v3 |
|---|---|---|
| ESP32 | carte de développement enfichée | module soudé, mémoire PSRAM comprise |
| Programmation | par le port de la carte de développement | port USB-C sur la carte |
| Horloge | module externe enfiché (`J11`) | puce et pile sur la carte |
| Sorties 12 V (2 pompes, filtration, éclairage, auxiliaire) | 5 transistors TO-220 traversants | 5 transistors CMS |
| Entrée 12 V : fusible, anti-inversion, écrêteur | traversants | CMS, même principe |
| 3,3 V et 5 V | deux modules enfichés | régulateurs sur la carte — ⚠️ **provisoire, voir plus bas** |
| Température | une sonde sur la carte, une prise pour la sonde d'eau | inchangé |
| Mesure pH et ORP | sur la carte | partie sur la carte de mesure, reliée par câble |

⭐ **Les broches de l'ESP32 ne changent pas** (ADR-0012) : le même firmware pilote les
mêmes sorties sur le v2 et le v3.

## Disposition dans le boîtier — décidée le 2026-11-25 (utilisateur)

Relevé sur le fichier d'assemblage FreeCAD du boîtier V14 (reçu le 2026-11-25, conservé hors
du dépôt). Origine : coin bas-gauche du boîtier vu de face, cotes en mm.

| Élément | Position relevée |
|---|---|
| Boîtier | 210 × 175, profondeur 94 (119,5 avec les pompes) |
| Alimentation RPS-30-12 | X 18,8 à 69,6 · Y 39,9 à 116,1 ; son capot va jusqu'à X = 75,6 et monte à 40 mm du fond |
| Carte v2 | X 76,6 à 196 · Y 13,5 à 132,9, dos à 12 mm du fond |
| Pompes doseuses (sur le capot avant) | moteurs Ø 25,5 centrés en (65 ; 91) et (145 ; 91), profonds de 44,9 mm : leur arrière est à 43 mm du fond, soit 10,5 mm au-dessus des composants |
| Entrées de câbles (paroi du bas) | trois, à X = 44 / 68 / 88, à **49 mm du fond** : les câbles arrivent au-dessus du capot de l'alimentation |
| Prises des sondes (paroi du bas) | X = 123,1 et 159,4, à 19,8 mm du fond |
| Plots de fixation murale | (25 ; 150) et (185 ; 150) : ils bornent la carte en haut |

**Organisation retenue (« option N »)** — le dessin général du boîtier ne change pas, il
gagne **20 mm en hauteur** (210 × 195) **et 10 mm en profondeur** (104 mm, 129,5 mm avec les
pompes) :
- alimentation à gauche, pompes à leur place actuelle ;
- carte de mesure en bas à droite, prises des sondes sur les perçages existants ;
- **carte principale rectangulaire de 119 × 100 mm** au-dessus : X 77 à 196, Y 62 à 162 ;
- le connecteur de liaison est sur le bord du bas de la carte principale, **face à celui de
  la carte de mesure** ;
- **tout se branche par le dessus**, sur des borniers débrochables ;
- **tous les câbles du boîtier sont sur le bord gauche, en une seule colonne** (utilisateur,
  2026-11-26), de haut en bas : arrivée 12 V — en haut à gauche, pour la relier par le haut à
  l'alimentation —, filtration, éclairage, auxiliaire, sonde d'eau, détecteur de débit ;
- les deux pompes, dont les fils viennent du capot, sont sur le bord du haut ;
- le 5 V d'essai est à l'intérieur de la carte : aucun câble du boîtier n'y arrive ;
- ⛔ aucun connecteur haut sous le moteur de la pompe de droite : centre en (68 ; 71) sur la
  carte, Ø 25,5, 29 mm de libre au-dessus de la carte ;
- antenne de l'ESP32 dans le coin haut-droit.

**Connecteurs — décidé le 2026-11-25 (utilisateur) : uniquement des borniers débrochables**
pour tout ce qui reçoit des fils. On visse les fils sur une fiche, puis on l'enfiche par le
dessus : aucune place n'est nécessaire sur le côté.

| Connexion | Bornier |
|---|---|
| Arrivée 12 V, filtration, éclairage, auxiliaire, 5 V d'essai | 5,08 mm, 2 broches |
| Sonde de température de l'eau | 5,08 mm, 3 broches |
| Pompe pH, pompe chlore, détecteur de débit | 3,81 mm, 2 broches |

Restent d'un autre type : la liaison vers la carte de mesure (JST XH 8 voies, câble tout
fait), la prise USB-C et l'embase de débogage.

**Repérage par la couleur** (utilisateur, 2026-11-25) : les prises de même format sont
distinguées par une pastille ou un cerclage de couleur, sur la carte et sur la fiche.
L'idée de borniers à broches vides pour empêcher les erreurs a été écartée : fonctionnelle,
mais pas élégante.
⚠️ Risque assumé, à couvrir par les couleurs : le pas de 3,81 mm sépare les pompes des
sorties de filtration et d'éclairage, mais rien n'empêche mécaniquement d'inverser les deux
pompes, ni d'enficher le détecteur de débit sur une sortie de pompe (court-circuit du 12 V
à travers le transistor dès que la pompe est commandée). ⏳ À faire à la carte : sérigraphie
de couleur ou repère net à côté de chaque prise.

**Pourquoi 10 mm de profondeur en plus** (utilisateur, 2026-11-25) — jeux mesurés sur le
fichier d'assemblage, moteur de 44,9 mm de profondeur (cote « nominale », ⏳ à mesurer sur
une pompe réelle, cosses comprises) :

| Entre | Aujourd'hui | Avec 10 mm de plus |
|---|---|---|
| moteur de la pompe de gauche et capot de l'alimentation | 3,1 mm | 13,1 mm |
| moteur de la pompe de gauche et l'alimentation | 10,1 mm | 20,1 mm |
| moteur de la pompe de droite et la carte principale | 29,7 mm | 39,7 mm |

Le moteur de gauche empiétait aussi sur la zone réservée aux câbles devant la carte.

⏳ À faire avec le boîtier : déplacer les deux plots de fixation du haut, vérifier la
hauteur d'un bornier débrochable avec sa fiche (environ 20 mm), relever la référence des
fiches à vis (une par câble, à fournir avec l'appareil).

## La carte (2026-11-26)

119 × 100 mm, quatre couches, quatre trous M3. ⚠️ **Le coin haut-droit est découpé** sous
l'antenne du module ESP32 (x 97,5 à 119, y 0 à 6) : l'antenne dépasse dans le vide, comme le
demande Espressif. `generer_carte.py` pose encore un contour rectangulaire ; c'est la
retouche 6 qui le découpe. Couches : composants et signaux dessus, masse
continue en couche 2, 3,3 V en couche 3, signaux et masse dessous. Des plans de masse
entourent les pistes sur les deux faces extérieures.

| Fichier | Rôle |
|---|---|
| `generer_carte.py` | pose les composants, les trous et les deux plans internes. ⚠️ Le relancer **réécrit** la carte et **efface le routage** |
| `outils/1_liaisons_aux_plans.py` | relie chaque plot CMS de masse et de 3,3 V à son plan par une piste courte et un via ; 25 vias sous le régulateur 5 V pour sa chaleur |
| `outils/2_importer_le_routage.py` | importe les pistes tracées par l'outil de routage, pose les plans de masse extérieurs |
| `outils/3_…` et `outils/4_…` | retouches : bouts de piste sans suite laissés par l'outil |
| `outils/6_retouches_relecture.py` | les cinq retouches de la relecture (voir plus bas) — à lancer **avant** les inscriptions |
| `outils/7_condensateur_pres_du_module.py` | le condensateur de 100 nF du module rapproché de sa broche 3,3 V (retouche du 2026-11-26) |
| `outils/8_horloge_ds3231.py` | l'horloge PCF8563 et son quartz remplacés par un DS3231SN, pistes refaites à la main (2026-11-26). ⚠️ `generer_carte.py` place encore l'ancienne horloge : la carte `.kicad_pcb` fait référence |
| `outils/9_alarme_hors_de_la_puce.py` | l'alarme de l'horloge sortie de sous la puce, R21 déplacée (retouche de relecture, 2026-11-26) |
| `outils/5_inscriptions.py` | nom de chaque prise, repère du +, consigne « couper JP1 d'abord » |
| `v3-principale.kicad_pcb` | **la carte : c'est elle la référence**, le routage ne se régénère pas à l'identique |
| `v3-principale.pretty/`, `fp-lib-table` | empreinte du module ESP32 (bibliothèque officielle d'Espressif), trou de fixation |

**Comment elle a été tracée** : placement par script, puis routage automatique par
Freerouting 2.0.1 (outil libre, lancé à la main par l'utilisateur sur l'export Specctra de
la carte), puis retouches par les scripts ci-dessus. 816 segments de piste, 157 vias.

**Largeurs** : 0,8 mm pour le 12 V et les cinq sorties, 0,5 mm pour le 5 V, le 3,3 V et la
masse, 0,25 mm pour les signaux. Écart minimal 0,2 mm (0,25 mm autour du 12 V).

**Vérifié le 2026-11-26** (`kicad-cli pcb drc --severity-all --schematic-parity`) :
0 violation, 0 liaison manquante, 0 écart avec le schéma. Les seuls messages restants
viennent des bibliothèques absentes de la machine.

**Messages restants du contrôle** : deux avertissements, attendus — le dessin de l'antenne du
module, sur la sérigraphie, est coupé par le nouveau bord de la carte.

### Relecture de la carte, 2026-11-26 (`electronics-reviewer`)

Verdict : aucune erreur ; cinq retouches locales, sans refaire le routage. Toutes portées.

| # | Constat | Retouche |
|---|---|---|
| 1 | l'antenne du module reposait sur la carte, avec du cuivre à côté sur les quatre couches | coin de la carte découpé sous l'antenne |
| 2 | le condensateur de la pompe pH (C3) rejoignait sa diode par 48 mm de piste | liaison directe de 8 mm par le dessous, deux vias de 0,8 mm |
| 3 | inscriptions de 0,8 mm, sous le minimum lisible de l'atelier | 1,0 mm, trait de 0,15 mm |
| 4 | la sortie éclairage passait entre les plots de la diode D7, avec un détour de 33 mm | détour retiré, liaison directe par le dessous |
| 5 | quatre vias dans les carrés de la pastille de masse du module | retirés |

En plus : le plot de masse du régulateur 5 V est relié au cuivre sans rayons, pour la chaleur.

**Vérifié par le relecteur** : 12 V et sorties en 0,8 mm partout, soit 2 A admissibles pour
+10 °C ; 44 mV de chute à 1 A jusqu'à la pompe ; masse de la couche 2 d'un seul tenant (95 %
de la surface) ; zone de l'antenne vide de cuivre ; USB correct ; aucun signal sensible à
moins de 3 mm d'une piste de puissance ; pistes, écarts et vias au-dessus des minimums de
l'atelier ; la piste à 0,30 mm du bord est acceptable (minimum 0,2 mm).

**Régulateur 5 V** — estimation du relecteur (calcul, pas la courbe du fabricant) : 25 à
35 °C/W, soit 71 à 84 °C dans la puce à 1,25 W et 85 à 103 °C à 1,8 W, boîtier à 40 °C, pour
une limite de 125 °C.

**Conseils non suivis, à reprendre si la carte est retouchée**
- condensateur de réserve du module (C8, 22 µF) à 15 mm de sa broche 3,3 V : il agit en
  dessous du mégahertz, où 15 mm de plans ne comptent pas (par calcul, non mesuré) ;
- un second via de masse par transistor de sortie si une pompe dépasse 1 A.

**Conseil suivi après coup (2026-11-26), à la demande de l'utilisateur** : le condensateur
de découplage C9 (100 nF) était à 19 mm de la broche 3,3 V du module. Un condensateur de
découplage filtre les appels de courant rapides, et chaque millimètre de trajet le rend
moins efficace : le juger sur la chute de tension (« moins de 5 mV ») était le mauvais
critère. Il est maintenant à 2,5 mm des broches 1 et 2, relié aux deux vias qui les
desservent (`outils/7_condensateur_pres_du_module.py` : R12 retournée, piste EN décalée).
Contrôle des règles relancé : même résultat qu'avant.

**Autre conseil suivi après coup (2026-11-26)** : C1 (470 µF, 25 V) passe de la série 2 000 h
(C106735) à une série 10 000 h à 105 °C, même empreinte de 8 mm au pas de 3,5 mm, 16 mm de
haut au lieu de 12 : AISHI ERJ1EM471F16OT (C5213783), équivalent Rubycon 25YXS470MEFC8X16
(C5114719, 5 000 h). Rien ne change sur la carte.

**Non vérifié**
- la courbe thermique du fabricant pour le régulateur, et sa température réelle ;
- le courant réel des pompes ;
- la règle d'Espressif sur les pistes passant sous le module ;
- hauteur des borniers avec leur fiche ; empreinte du support de pile face à la pièce ;
- repères de couleur à côté des prises de 3,81 mm (pompes, débit) ;
- l'ouverture de la carte dans le KiCad de l'utilisateur.

## Liste des composants — version 0.2

Stocks relevés le 2026-11-25 chez le distributeur de l'atelier. ⚠️ Le stock de l'atelier
lui-même se lit au moment du devis.

### Cœur

| Rôle | Composant | Boîtier | Réf. atelier | Équivalent même empreinte | Remarque |
|---|---|---|---|---|---|
| Microcontrôleur | ESP32-WROVER-E-N8R8 (Espressif) | module 31,4 × 18 mm | C2973667 | ⛔ **aucun — exception E1** | 8 Mo de flash, 8 Mo de PSRAM : c'est le module de la cible actuelle |
| Pont USB-série | CH340C (WCH) | SOP-16 | C84681 | ⛔ **aucun — exception E2** | sans quartz ; stock 75 000 |
| Remise à zéro automatique | 2 × SS8050 | SOT-23 | C2150 | tout NPN SOT-23 (MMBT3904…) | montage classique à deux transistors |
| Prise USB | USB-C 16 broches TYPE-C-31-M-12 | CMS | C165948 | empreinte reprise par de nombreux fabricants | USB 2.0 seulement |
| Protection USB | USBLC6-2SC6 (ST) | SOT-23-6 | C7519 | même brochage chez plusieurs fabricants | |
| Bouton de réinitialisation | TS-1187A | CMS 5,1 mm | C318884 | bouton 5,1 mm courant | |
| Boutons RESET et BOOT | 2 × TS-1187A | CMS 5,1 mm | C318884 | bouton 5,1 mm courant | ajoutés le 2026-11-25 à la demande de l'utilisateur : reprendre la main quand le passage automatique en programmation échoue |
| Voyant | LED rouge 0603 | 0603 | C2286 | toute LED 0603 | remplace la LED de la carte de développement |

### Horloge — ✅ **option A retenue : DS3231SN** (utilisateur, 2026-11-26)

⚠️ L'option B (PCF8563) avait été retenue le 2026-11-25, puis abandonnée le lendemain : sans
réseau, elle dérive d'une à deux minutes par mois, contre une minute par an pour la DS3231.
L'utilisateur a tranché : **la qualité l'emporte sur le nombre de fabricants et sur le prix**
(exception E3). Le firmware reste celui du v2 pour l'horloge. Le tableau est gardé pour mémoire.

| | Option A — garder la DS3231 | Option B — PCF8563 |
|---|---|---|
| Composant | DS3231SN (Maxim), SOIC-16 large, C9866 | PCF8563T (NXP), SO-8, C7440 |
| Prix relevé | **7,99 $** | **0,58 $** (0,25 $ pour l'équivalent) |
| Stock | 3 463 | 69 780 |
| Autre fabricant, même empreinte | ⛔ aucun ⇒ exception | ✅ BM8563 (C269877), HYM8563 |
| Précision | ± 2 ppm, soit 5 s par mois | celle du quartz, ± 20 ppm, soit 1 min par mois |
| Pièces en plus | aucune | un quartz 32,768 kHz (C271654), deux diodes pour la pile (BAT54C, C37704) |
| Firmware | inchangé | ⚠️ à adapter : reconnaître la puce présente, pour rester compatible avec le v2 |

Dans les deux cas : support de pile CR2032 traversant MYOUNG BS-07-A1BJ001 (C2979167, 2 125 pièces
relevées), choisi parce que son empreinte existe dans KiCad. La pile n'est pas posée par l'atelier.

ℹ️ L'heure est remise à jour par le réseau dès que le Wi-Fi est là. La précision de la
puce ne compte que pour un appareil qui reste des mois sans réseau.

### Sorties de puissance (5 voies identiques)

| Rôle | Composant | Boîtier | Réf. atelier | Équivalent même empreinte |
|---|---|---|---|---|
| Interrupteur | AO3400A (AOS), 30 V, 5,7 A | SOT-23 | C20917 | IRLML6344 (Infineon, C53550), 30 V, même brochage. ⛔ Pas le SI2302 : il ne tient que 20 V |
| Diode de roue libre | SS34, 40 V, 3 A | SMA | C8678 | SS34 de plusieurs fabricants |
| Résistance de grille | 100 Ω | 0603 | C22775 | — |
| Rappel à la masse | 10 kΩ | 0603 | C25804 | — |
| Condensateur local | 10 µF, 50 V | 1206 | C13585 | — |
| Filtration, éclairage, auxiliaire | **bornier débrochable** 5,08 mm, embase verticale 2 voies | traversant | C505788 | standard « 2EDG » de nombreux fabricants |
| Pompe pH, pompe chlore | **bornier débrochable 3,81 mm**, embase verticale 2 voies | traversant | C8396 | standard « 15EDG » de nombreux fabricants |

Un condensateur de 10 µF est posé entre `+12V` et la masse au pied de chaque sortie, comme
sur le v2 : sans lui, une pompe à 20 kHz fait onduler le 12 V de 0,6 V.

Charges connues : pompe doseuse 12 V (moteur à courant continu, commandé en impulsions à
20 kHz) et bobine de contacteur 12 V. ⏳ **Courant réel de la pompe à mesurer** : il fixe
la marge du transistor et de la diode.

### Entrée 12 V

| Rôle | Composant | Boîtier | Réf. atelier | Équivalent même empreinte |
|---|---|---|---|---|
| Arrivée 12 V | bornier débrochable 5,08 mm, embase verticale 2 voies | traversant | C505788 | standard « 2EDG » |
| Fusible réarmable | 2 A, 24 V | 1812 | C434887 | LUTE 1812L200/24GR (C18198341), BHFUSE BSMD1812-200-24V (C883157). ⚠️ Calibre à revoir avec Q4 |
| Anti-inversion | AO4407A, canal P, 30 V | SO-8 | C16072 (AOS) | C2841482 (UMW), SI4435 |
| Protection de grille | Zener 12 V BZT52C12 | SOD-123 | C173429 | plusieurs |
| Écrêteur | SMBJ13A : au repos jusqu'à 13 V, limite à 21,5 V | SMB | C404130 (MDD) | C135042 (Diodes Inc.) |
| Réserve d'énergie | condensateur 220 à 470 µF, 25 V | ⏳ à choisir | ⏳ | — |

### Alimentations 5 V et 3,3 V — décidé le 2026-11-25

L'utilisateur a décidé de finir la carte sans attendre l'essai d'alimentation M3 : la carte
fabrique son 5 V à partir du 12 V, **et** peut recevoir un 5 V extérieur pour un essai futur.

| Rôle | Composant | Boîtier | Réf. atelier | Équivalent même empreinte |
|---|---|---|---|---|
| 12 V → 5 V | L7805ABD2T-TR (ST), régulateur **linéaire** | D²PAK (TO-263) | C86206 | MC7805BD2TR4G (onsemi), C15833, même brochage |
| Condensateurs du 7805 | 1 µF 50 V en entrée ; 10 µF + 100 nF en sortie | 0805, 0805, 0402 | C28323, C15850, C1525 | — |
| Pont 5 V | pont de soudure **fermé d'origine**, à couper | sur la carte | aucune pièce | — |
| Bornier « 5 V externe » | bornier débrochable 5,08 mm, embase verticale 2 voies | traversant | C505788 | standard « 2EDG » |
| Protection du 5 V externe | AO3401A, canal P, + 10 kΩ | SOT-23 | C15127 | nombreux |
| 5 V → 3,3 V | AMS1117-3.3 | SOT-223 | C6186 | LD1117 (UMW) |

**Pourquoi un régulateur linéaire** (avis du relecteur, chiffré) : trois composants, deux
grands fabricants sur la même empreinte — l'exception E4 disparaît — et aucun découpage sur
la carte. Aucun convertisseur à découpage de 1 à 1,5 A n'a deux fabricants réellement
compatibles. ⚠️ Ce que ça coûte : **1,2 W de chaleur** au courant moyen estimé (0,17 A),
soit environ 90 °C dans la puce pour 40 °C dans le boîtier ; limite 125 °C, atteinte vers
0,3 A de moyenne. ⇒ Il lui faut **25 × 25 mm de cuivre** et des vias vers le plan de masse,
loin du condensateur de 470 µF et de la sonde de température. ⏳ Consommation réelle du 5 V
à mesurer sur la première carte.

**Le pont et le bornier 5 V externe**
- Pont fermé (état d'origine) : fonctionnement normal, tout part du 12 V.
- Pont **coupé**, 5 V sur le bornier : l'ESP32 et la mesure tournent sur l'alimentation
  extérieure. Pour un essai de bruit, **débrancher aussi le 12 V** : la carte n'a qu'une
  masse, et laisser l'alimentation 12 V branchée referait les essais E03 et E07, sans gain.
- Pont fermé **et** 5 V externe, 12 V présent : rien ne casse, la source la plus haute fournit.
- Pont fermé **et** 5 V externe, 12 V absent : le 5 V remonte vers le rail 12 V à travers le
  régulateur. État mal défini ⇒ toujours couper le pont d'abord.
- Le transistor AO3401A protège d'un 5 V externe branché à l'envers. ⛔ Rien ne protège
  d'une tension trop haute sur ce bornier : les modules de la carte de mesure ne tiennent
  que 5,5 V.
- ℹ️ Le pont est un pont de soudure et non un cavalier enfichable : l'atelier n'a rien à
  poser, et aucun contact ne peut s'oxyder en local humide. On le coupe au cutter, on le
  referme d'un point de soudure.

### Température et liaison

| Rôle | Composant | Boîtier | Réf. atelier | Équivalent même empreinte |
|---|---|---|---|---|
| Sonde sur la carte | DS18B20+ (Maxim) | TO-92 traversant | C9753 | DS18B20 UMW (C376006) |
| Prise de la sonde d'eau | bornier débrochable 5,08 mm, embase verticale 3 voies | traversant | C505789 | standard « 2EDG ». Une sonde du commerce arrive en fils nus : ils se vissent sur la fiche |
| Prise vers la carte de mesure | JST XH 8 voies | traversant | C157972 | compatibles XH |

## Les exceptions à la règle D8 — à accepter par l'utilisateur

| # | Composant | Pourquoi il n'a pas d'équivalent | Ce que ça risque | Ce qui limite le risque |
|---|---|---|---|---|
| E1 | Module ESP32 | Espressif est le seul fabricant, et le firmware est écrit pour cette puce | rupture ⇒ plus de carte | 5 100 pièces à l'atelier, vendu par tous les grands distributeurs ; ⏳ durée de vie annoncée par Espressif à vérifier |
| E2 | Pont USB CH340C | aucun pont USB n'a de second fabricant sur la même empreinte | rupture ⇒ plus de programmation par USB | 75 000 en stock ; le connecteur de débogage (TX, RX, EN, IO0, masse) permet de programmer sans lui ; les mises à jour passent par le Wi-Fi |
| E3 | Horloge DS3231SN (C9866) — ✅ **acceptée par l'utilisateur le 2026-11-26** | Maxim (Analog Devices) seul | 8 $ et 3 100 pièces relevées | précision d'une minute par an sans réseau ; repli DS3231M (C37663), même empreinte, même fabricant, ± 5 ppm |
| ~~E4~~ | ~~Régulateur 5 V TPS54331~~ — **sans objet** : remplacé par un 7805 à deux fabricants | | | |
| E5 | Pile CR2032 | l'atelier ne la pose pas | une pile à insérer à la main sur chaque carte | aucune soudure, pile courante |

## Tableau des connexions — version 0.2

### ESP32 (U1) — broches identiques au v2

| Broche ESP32 | Signal | Va vers |
|---|---|---|
| 3V3 | `+3V3` | sortie du régulateur 3,3 V ; 22 µF + 100 nF au plus près (schéma d'application du module) |
| EN | `EN` | 10 kΩ vers `+3V3`, 1 µF vers la masse, transistor de remise à zéro, bouton RESET vers la masse |
| GND et pastille centrale | masse | plan de masse |
| IO0 | `IO0` | 10 kΩ vers `+3V3`, transistor de remise à zéro, bouton BOOT vers la masse |
| TXD0 (IO1) | `TX` | 470 Ω en série (Espressif recommande 499 Ω ; valeur courante retenue), puis entrée RXD du pont USB |
| RXD0 (IO3) | `RX` | sortie TXD du pont USB à travers 470 Ω : un adaptateur série branché sur le connecteur de débogage garde la main |
| IO2 | `LED` | 1 kΩ puis LED vers la masse |
| IO5 | `TEMP` | sonde de la carte, prise de la sonde d'eau, 4,7 kΩ vers `+3V3` |
| IO21 | `SDA` | horloge ; voies 1 et 7 de la prise de mesure |
| IO22 | `SCL` | horloge ; voies 2 et 8 de la prise de mesure |
| IO23 | `IO23` | connecteur de débogage (broche libre). ℹ️ Sur le v2 elle recevait le signal carré de l'horloge, jamais utilisé par le firmware |
| IO25 | `POMPE_PH` | 100 Ω puis grille du transistor de la pompe pH |
| IO26 | `FILTRATION` | idem, sortie filtration |
| IO27 | `ECLAIRAGE` | idem, sortie éclairage |
| IO32 | `AUX` | idem, sortie auxiliaire |
| IO33 | `POMPE_ORP` | idem, pompe chlore |
| IO35 | `BOUTON` | **10 kΩ vers la masse, bouton vers `+3V3`** — ⚠️ appuyé = niveau haut, c'est ce que lit le firmware (`src/main.cpp:622`). ⛔ ADR-0012 dit l'inverse, à tort |
| IO36 | `RTC_INT` | broche INT de l'horloge, 10 kΩ vers `+3V3` (réservée : le firmware ne la lit pas) |
| IO12 | `IO12` | connecteur de débogage seulement. ⛔ Rien ne doit la tirer vers le haut au démarrage : elle fixe la tension de la mémoire |
| IO13, IO14, IO15 | `IO13`, `IO14`, `IO15` | connecteur de débogage (sonde pas à pas, avec IO12) |
| IO4, IO18, IO19 | idem | connecteur de débogage (broches libres) |
| IO16, IO17 | — | ⛔ en l'air : prises par la PSRAM |
| autres | — | en l'air |

### Une sortie de puissance (les cinq sont identiques)

```
+12V ──┬─────────────► borne 1 du bornier (+ de la charge)
       │
      ─┴─ cathode
      SS34
      ─┬─ anode
       ├─────────────► borne 2 du bornier (− de la charge)
       │ drain
   AO3400A   grille ◄── 100 Ω ◄── broche ESP32
       │ source           └── 10 kΩ ── masse
      masse
```

### Entrée 12 V

`borne +` → fusible réarmable → **drain** de l'AO4407A, dont la **source** donne `+12V`.
Grille : 10 kΩ vers la masse, Zener 12 V entre grille et source. Écrêteur et condensateur
de réserve entre `+12V` et la masse.

⚠️ **Le sens compte** : la diode interne du transistor doit laisser passer le courant dans
le sens normal (note TI SLVA139). ⛔ **Le v2 est monté à l'inverse** (source côté fusible) :
branché à l'envers, il laisse passer −11 V vers toute la carte. Ne pas le recopier.

### Horloge (DS3231SN, boîtier large à 16 broches)

| Broche | Signal |
|---|---|
| 1 32kHz | en l'air |
| 2 VCC | `+3V3`, 100 nF (C13) à 2,6 mm |
| 3 INT/SQW | `RTC_INT` vers IO36, 10 kΩ vers `+3V3` |
| 4 RST | en l'air (rappel interne) |
| 5 à 12 | masse (demandé par le fabricant) |
| 13 GND | masse |
| 14 VBAT | pile CR2032, directement : la puce bascule seule entre le 3,3 V et la pile |
| 15 SDA, 16 SCL | bus I2C |

Plus de quartz, de condensateur d'accord, de double diode ni de 1 kΩ : les repères Y1, C12, D8
et R20 restent libres, pour que les autres ne changent pas. Le fabricant demande de ne pas
faire passer de signal sous la puce sans plan de masse entre les deux (fiche p. 18) : seule la
piste de la pile y passe, en tension continue. L'alarme sort par la gauche puis file sur le
dessous, sous le plan de masse ; sa résistance de rappel R21 est près de (64,5 ; 51,3). Le bus
I2C sort par la droite.

**Relu par `electronics-reviewer` le 2026-11-26** : câblage conforme à la fiche (brochage,
broches 5 à 12 à la masse, pile en direct — la puce est homologuée contre la charge de la
pile, p. 9 —, 100 nF, tensions). Une retouche demandée et portée : l'alarme passait sous la
puce sur le dessus (`outils/9_alarme_hors_de_la_puce.py`). Condensateur C9 près du module :
correct. ⏳ Non vérifié : l'épaisseur réelle entre les plans internes ; La retouche de l'alarme a été
relue une seconde fois par l'agent, sur la carte : correcte, plan de masse sans trou sous la
piste (testé tous les 0,05 mm), écarts de 1,0 mm ou plus autour de la nouvelle piste, 2,0 mm
ou plus autour de R21 ; contrôle des règles : 0 violation, 0 liaison manquante, 0 écart avec
le schéma. Hors carte : `src/constants.h` prévoit deux entrées pour
l'horloge, l'alarme sur IO36 (câblée ici) et le signal carré sur IO23 (non câblé sur le v3, la
puce n'a qu'une sortie pour les deux) — inutilisées aujourd'hui, à noter dans la fiche logicielle.

### Pont USB (CH340C, alimenté en 3,3 V)

| Broche | Signal |
|---|---|
| VCC et V3 | `+3V3`, 100 nF |
| UD+, UD− | prise USB-C, à travers l'USBLC6-2SC6 |
| TXD | `RX` de l'ESP32 |
| RXD | `TX` de l'ESP32 |
| DTR | 10 kΩ → base du transistor 1 ; **et** émetteur du transistor 2 |
| RTS | 10 kΩ → base du transistor 2 ; **et** émetteur du transistor 1 |
| — | collecteur du transistor 1 → `EN` ; collecteur du transistor 2 → `IO0` |

⚠️ Le croisement des émetteurs est indispensable : sans lui, l'ESP32 reste bloqué dès qu'un
moniteur série s'ouvre.

Prise USB-C : CC1 et CC2 chacune à la masse par 5,1 kΩ ; A6 relié à B6, A7 à B7 ; broche 5
de l'USBLC6-2SC6 au 5 V de la prise. ⚠️ **Le 5 V de l'USB n'alimente
rien** : la carte se programme alimentation 12 V branchée. C'est le plus simple, et aucun
courant ne peut remonter de l'USB vers le 12 V.

### Entrée du détecteur de débit — demandée par l'utilisateur le 2026-11-25

Pour un détecteur à palette du type de ceux des électrolyseurs (AstralPool et autres) : un
contact qui se ferme quand l'eau circule, deux fils.

| Élément | Valeur | Réf. atelier |
|---|---|---|
| Prise | bornier débrochable 3,81 mm, 2 voies : 1 = contact, 2 = masse | C8396 |
| Rappel | **1 kΩ** entre le contact et `+3V3` (3,3 mA dans le contact ; une fuite d'eau entre les deux fils ne passe pour un débit que sous 330 Ω) | C21190 |
| Protection et filtrage | 1 kΩ en série vers IO34, 100 nF entre IO34 et la masse | C21190, C1525 |

- Contact fermé = niveau **bas** sur IO34 = débit présent.
- Fil coupé ou détecteur absent = niveau haut = pas de débit : le doute va au refus.
- Détecteur visé par l'utilisateur : AstralPool ZX116000092 (détecteur à palette de pompe à
  chaleur), **deux fils**, livré avec un cordon à fils nus qui se visse sur la fiche.
- ⏳ **Non vérifié** : le sens du contact (fermé ou ouvert quand l'eau circule). À mesurer au
  multimètre à réception ; le firmware devra accepter les deux.
- ⛔ **Aucun firmware ne lit cette entrée.** Aujourd'hui l'appareil ne constate jamais
  l'eau, il la déduit (ADR-0026, ADR-0144). S'en servir pour autoriser une injection est une
  fiche logicielle à part, avec passage obligatoire par `pool-chemistry`.

### Connecteur de débogage — demandé par l'utilisateur le 2026-11-25

Embase 2 × 8 avec détrompeur, pas de 2,54 mm, traversante (C221049, Nextron ; même empreinte
chez plusieurs fabricants). **Soudée en usine sur cette version** ; la version commercialisée
sera faite à part, sans elle. Elle remplace les cinq pastilles d'essai.

| Broche | Signal | Broche | Signal |
|---|---|---|---|
| 1 | `+3V3` | 2 | masse |
| 3 | `EN` | 4 | `IO0` |
| 5 | `TX` | 6 | `RX` |
| 7 | `SDA` | 8 | `SCL` |
| 9 | `IO13` (TCK) | 10 | `IO14` (TMS) |
| 11 | `IO12` (TDI) | 12 | `IO15` (TDO) |
| 13 | `IO4` | 14 | `IO18` |
| 15 | `IO19` | 16 | `IO23` |

### Prise vers la carte de mesure — câble droit, même ordre que le J1 de la carte de mesure

| Voie | Signal sur la carte principale |
|---|---|
| 1 | `SDA` |
| 2 | `SCL` |
| 3 | masse |
| 4 | `+5V` |
| 5 | `+3V3` |
| 6 | masse |
| 7 | `SDA` (réuni à la voie 1 — décision D3) |
| 8 | `SCL` (réuni à la voie 2) |

## Points ouverts

| # | Point |
|---|---|
| Q1 | ~~Horloge~~ ✅ **DS3231SN** (utilisateur, 2026-11-26, après un premier choix PCF8563 la veille). Aucune conséquence logicielle : même puce et même adresse (0x68) que le v2 |
| Q2 | **Résistances de rappel du bus I2C** : la règle actuelle les met toutes sur la carte de mesure. ⚠️ Carte de mesure débranchée, l'horloge ne répond donc plus. Proposition, **approuvée par le relecteur** : ajouter 10 kΩ sur la carte principale (total 1,9 kΩ, soit 1,7 mA, sous la limite de 3 mA). ✅ **Accepté par l'utilisateur le 2026-11-25**, règle corrigée dans feature-215 |
| Q3 | Deux ou quatre couches (V17) — avis du relecteur : **quatre**, même empilement que la carte de mesure |
| Q4 | Courant réel d'une pompe doseuse, et de la bobine du contacteur |
| Q5 | ~~Référence de l'écrêteur et du support de pile~~ ✅ trouvées. Reste le condensateur de réserve du 12 V |
| Q6 | ~~Durée de vie du module~~ ✅ relevé par le relecteur : variante N8R8 active (fiche v2.4) ; Espressif s'engage sur la puce jusqu'en 2031 au moins, ⚠️ pas sur le module |
| Q7 | ~~Bloc régulateurs~~ ✅ dessiné (7805 linéaire, pont, bornier 5 V externe). L'essai M3 reste possible sur la carte v3 elle-même |
| Q8 | ~~Alimenter par l'USB~~ ✅ **non** (relecteur) : 500 mA ne suffisent pas, et le courant remonterait dans le régulateur |
| Q10 | ⚠️ **Contacteurs 230 V** : vérifier sur leur fiche que l'isolation entre bobine et contacts est renforcée. Sinon le 12 V, l'USB et les sondes ne sont plus en très basse tension de sécurité. À faire avant de vendre |
| Q9 | Forme de la carte et position des prises : le boîtier vient après (V13) |

## Relecture du 2026-11-25 (`electronics-reviewer`)

Verdict : on peut passer au schéma une fois quatre erreurs d'écriture corrigées — c'est fait
dans cette version 0.2.

| # | Erreur de la version 0.1 | Correction |
|---|---|---|
| 1 | bouton câblé « actif bas », alors que le firmware lit « actif haut » : l'appareil se serait réinitialisé en boucle | 10 kΩ vers la masse, bouton vers `+3V3` |
| 2 | anti-inversion décrite avec la source côté fusible | drain côté fusible |
| 3 | fusible « équivalent » qui ne tient que 8 V | deux équivalents 24 V |
| 4 | remise à zéro automatique sans le croisement des émetteurs | croisement écrit |

Ajouts tirés de ses remarques : condensateurs sur le 12 V, écrêteur SMBJ13A, 22 µF sur le
3,3 V, résistance sur la ligne d'émission, pastille de masse, exceptions E4 et E5, point Q10.

Non vérifié par le relecteur : fiche du contacteur, déclassement du fusible avec la
température, empreintes compatibles de la prise USB-C et du support de pile.

## Relecture du schéma, 2026-11-25 (`electronics-reviewer`)

Verdict : aucune erreur de brochage ni de câblage, on peut dessiner la carte. Brochages
contrôlés contre les fiches : AO4407A, AO3400A, SS8050, USBLC6, CH340C, PCF8563T (remplacé depuis par le DS3231SN, relu à part), module
ESP32, prise USB-C. Remise à zéro automatique : même montage que la carte de développement.

Deux remarques, portées dans le schéma :
- 1 kΩ en série avec la pile ;
- bornier à vis pour la sonde d'eau, à la place de la prise JST.

Non vérifié par le relecteur : fiche du DS18B20 (site inaccessible), figure de NXP sur le
condensateur d'accord, empreintes du module ESP32 et du support de pile.

## Relecture des ajouts de la rév. 0.2, 2026-11-25 (`electronics-reviewer`)

Verdict : aucune erreur dans les ajouts (régulateur 7805, pont, bornier 5 V externe et sa
protection, boutons RESET et BOOT, embase de débogage, 470 Ω, entrée du détecteur de débit).

Porté sur ses remarques : rappel du détecteur de débit passé de 10 kΩ à 1 kΩ ; le pont JP1
n'est plus compté dans la nomenclature.

À faire au moment de la carte :
- écrire « couper JP1 d'abord » en sérigraphie à côté du bornier 5 V externe : la protection
  contre l'inversion ne joue que pont coupé ;
- ⚠️ une sonde de débogage branchée **à la mise sous tension** peut tirer IO12 vers le haut
  et empêcher le démarrage : la brancher après.

Non suivi : le relecteur conseille un bornier à vis pour le détecteur de débit (fils nus,
pince à sertir). L'utilisateur a choisi la prise JST ; elle sera adaptée au détecteur réel.

Non vérifié par le relecteur : fiche ST du 7805 (brochage pris sur l'équivalent onsemi),
détrompeur et corps de l'embase C221049 contre l'empreinte, type réel du détecteur de débit.
