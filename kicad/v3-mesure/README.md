# Carte de mesure du PCB v3 — composants et connexions

> **État : schéma écrit (révision 0.5, vrais fils, toutes les empreintes attribuées), ⚠️ pas encore relu par le relecteur.** Aucune carte n'est dessinée. Ce document est la référence contre
> laquelle le schéma, puis la carte, sont relus.
> Fiche de conception : [`feature-215`](../../specs/features/todo/feature-215-conception-du-pcb-v3.md) ·
> contraintes de fabrication : [`docs/FABRICATION.md`](../../docs/FABRICATION.md).
> Dernière mise à jour : 2026-11-24.

## Les fichiers de ce dossier

| Fichier | Rôle |
|---|---|
| `v3-mesure.kicad_pro` | projet KiCad 9 — c'est lui qu'on ouvre |
| `v3-mesure.kicad_sch` | le schéma, une seule feuille A3 |
| `v3-mesure.kicad_sym` + `sym-lib-table` | les quatre symboles propres à cette carte : ADM3260, module Atlas OEM, prise BNC, connecteur 8 voies |
| `v3-mesure.pretty/` + `fp-lib-table` | les trois empreintes propres à cette carte : `Atlas_OEM`, `BNC_Dosin_801-0080_Coudee`, `Trou_M3_NPTH` |
| `v3-mesure.kicad_pcb` | la carte — **squelette seulement** : contour, quatre couches, zones interdites, trous de fixation, repères |
| `v3-mesure.kicad_dru` | la règle d'isolement entre les trois domaines |
| `generer_schema.py` | le script qui a écrit le schéma depuis le tableau des connexions |

**Le schéma (révision 0.5) est tracé avec de vrais fils** — demande de l'utilisateur du
2026-11-24, la révision 0.1 reliait tout par étiquettes. Les alimentations et les masses
passent par les symboles habituels (flèche, masse), comme dans le schéma du v2. Seuls les
quatre signaux du bus rejoignent le connecteur J1 par étiquette, parce que J1 est commun aux
deux voies.

**Ce qui a été vérifié**
- **Dans le KiCad de l'utilisateur** (révision 0.1, 2026-11-24) : contrôle électrique à
  **0 erreur, 0 avertissement**. ⇒ Les noms d'empreintes standard (0402, 0805, 0603,
  SSOP-20, JST XH) existent bien dans ses bibliothèques.
- **En ligne de commande** (révision 0.5, KiCad 9.0.2) : 0 erreur ; la liste des connexions
  que KiCad extrait du schéma est **identique** à celle du tableau de ce document — 21 nets,
  12 broches volontairement en l'air ; rendu regardé en image, sans chevauchement.
  Sur ce poste, le contrôle rend des avertissements d'un seul type : les bibliothèques
  standard de KiCad n'y sont pas installées.

**Ce qui n'est PAS vérifié**
- ✅ Révision 0.2 ouverte dans le KiCad de l'utilisateur le 2026-11-24 : contrôle électrique sans problème. ⚠️ Les révisions 0.3 (C108/C208 retirés) à 0.5 (empreintes du module et de la BNC attribuées) n'y ont pas encore été ouvertes.
- ⚠️ **Aucune des deux empreintes propres n'a été confrontée à une pièce réelle** : elles
  suivent les plans publiés. Avant de commander la carte, imprimer le routage à l'échelle 1
  et y poser un module Atlas et une prise BNC.

⚠️ **`generer_schema.py` réécrit le schéma en entier.** Dès que le schéma est modifié à la
main dans KiCad, ⛔ ne plus le relancer : il écraserait ces modifications.
ℹ️ Le script contient un piège appris à l'usage : KiCad ne relie un fil à un point que si le
fil **s'arrête** à ce point. Un fil qui passe sur une broche sans s'y arrêter n'est pas
relié, même avec un point de jonction dessiné. Le script coupe donc chaque fil à chaque
point de connexion.

## Disposition dans le boîtier (utilisateur, 2026-11-24)

**La carte est montée à l'horizontale (« paysage ») et les prises BNC sortent par le bas.**
⇒ Les deux BNC sont sur le **grand bord du bas**, et ce sont des prises **coudées**.

Conséquences pour le placement, à tenir au routage :
- chaque module Atlas est tourné pour que ses broches 4 (`PRB`) et 5 (`PRB_GND`) regardent
  **vers le bas**, face à sa BNC : c'est ce qui garde la piste de sonde courte ;
- le plan `GND_SONDE` de chaque voie occupe la zone entre le module et le bord du bas
  (Atlas, p. 33 : le plan de sonde couvre le coin des broches 4 et 5) ;
- le connecteur J1 et le côté ESP32 sont à l'opposé, vers le haut ou le centre ;
- ⚠️ le corps des BNC traverse la paroi du bas : **paroi isolante, ou trou plus large que le
  corps** — il ne doit toucher aucun métal relié à la terre (voir « Relecture »).

**Prise retenue** : `C521219` — dosinconn **DOSIN-801-0080**, « BNC jack for PCB », 50 Ω,
coudée, traversante, pièce étendue chez JLCPCB (2 832 en stock chez le distributeur le
2026-11-24). Plan du fabricant (révision A) lu le 2026-11-24 :

| Ce que dit le plan | Valeur |
|---|---|
| Genre | embase femelle (« jack ») ✅ |
| Corps | **laiton nickelé — donc métallique** ; isolant en téflon |
| Fixation | nez **fileté M12 × 0,75**, avec écrou et joint : la prise se **serre sur la paroi** |
| Pattes de corps | 4, Ø 1,2 mm, au pas de **8 mm** (le long de l'axe) × **10 mm** (en travers) |
| Âme | Ø 1 mm, au centre des quatre pattes |
| Hauteur de l'axe au-dessus de la carte | 9,8 mm |
| Longueur totale | 29,5 mm |

⚠️ **Conséquence directe pour le boîtier** : la prise est faite pour être serrée par son écrou
sur la paroi, et son corps est en métal. ⇒ **La paroi du bas doit être isolante** (plastique).
Sur une paroi métallique reliée à la terre, le serrage relierait `GND_SONDE` à la terre et
les deux voies entre elles : l'isolation serait annulée. Le joint, lui, est une bonne chose
en local humide.

**Empreinte** : `v3-mesure:BNC_Dosin_801-0080_Coudee`, nez vers le bas. Âme = plot 1 ; les
quatre pattes de corps = plot 2 (`GND_SONDE`). ✅ Les positions des cinq trous viennent des
cotes écrites sur le plan. ⚠️ Le contour du corps et la position de la paroi (≈ 6,7 mm de
l'âme) sont **mesurés sur le dessin**, pas cotés : à vérifier pièce en main. ⚠️ Perçages
choisis par moi : 1,5 mm pour les pattes, 1,3 mm pour l'âme.

## Dimensions et zones de la carte (proposition du 2026-11-24)

**Règle donnée par l'utilisateur** : le boîtier actuel sert à l'idée générale de disposition ;
les tailles des cartes se fixent librement ; **le boîtier sera adapté ensuite** aux cartes.

**Carte de mesure : 70 × 50 mm, quatre couches, 1,6 mm.** Grand côté à l'horizontale, les
deux BNC sur le bord du bas.

```
        0        17      35      53       70 mm
      0 +----------------------------------+
        | o   COTE ESP32 : J1 au centre  o |   bande de 12 mm :
        |     C101-C104, R101-R102 (x2)    |   connecteur, découplage, rappels
     12 +- - - -[U101]- - - - -[U201]- - - +
        |       barrière : 5,5 mm sans     |   les deux ADM3260 à cheval dessus
   17,5 +- - - - - - - -+   +- - - - - - - +
        |  ILE pH        | | |  ILE ORP    |   5,5 mm sans cuivre entre les îles
        |  C105-C107     | | |  C205-C207  |
        |  R103-R106     | | |  R203-R206  |
        |  [U102 Atlas]  | | |  [U202]     |
        | o [J101 BNC]   | | |  [J201]   o |
     50 +------|---------+---+----|--------+
              16,85             53,15         axes des BNC : entraxe 36,3 mm
```

| Élément | Valeur | Pourquoi |
|---|---|---|
| Largeur | 70 mm | deux îles de 30,8 mm + 5,5 mm entre elles + 1,45 mm de marge de chaque côté |
| Hauteur | 50 mm | bande ESP32 12 mm + barrière 5,5 mm + îles 32,5 mm |
| Entraxe des BNC | **36,3 mm** | celui des deux perçages existants du boîtier : ils peuvent resservir |
| Axe des BNC | à X = 16,85 et 53,15 mm | centre de chaque île |
| Âme des BNC | à 6,0 mm du bord du bas | la paroi intérieure est à ≈ 6,7 mm de l'âme : 0,7 mm de jeu entre carte et paroi |
| Barrière | bande sans cuivre de 5,5 mm, de Y = 12 à 17,5 mm | fiche ADM3260 rév. F ; les deux isolateurs la chevauchent |
| Séparation des voies | bande sans cuivre de 5,5 mm, de X = 32,25 à 37,75 mm | les deux voies ne partagent rien |
| Fixations | 4 trous Ø 3,2 mm **non métallisés**, à 3,5 mm des bords | reliés à rien (relecture) ; cuivre écarté de 2 mm autour |

**Dans chaque île, de haut en bas** : la moitié isolée de l'ADM3260 et ses sept composants ;
le module Atlas ; la prise BNC. Le module se place pour que son plot 4 (`PRB`) soit dans
l'axe de l'âme de la BNC : la piste de sonde descend alors tout droit entre les deux pattes
de corps, sur **moins de 10 mm**.

⚠️ **Ce que cette proposition n'a pas prouvé** : c'est un plan de zones fait au calcul des
encombrements, ⛔ pas un placement vérifié dans KiCad. La largeur d'île (30,8 mm) pour un
module de 14,5 mm plots compris, une BNC de 14 mm et le plan de sonde reste à éprouver ; si
elle ne suffit pas, on élargit la carte — rien d'autre n'en dépend.

**Ce que cela demandera au boîtier**, le moment venu : abaisser l'appui de la carte de
3,6 mm par rapport à la carte actuelle (axe de la BNC à 9,8 mm au-dessus de la carte, pour
des perçages à 19,8 mm du fond), ou remonter les perçages ; poser la carte tout en bas.

## Le fichier de carte : un squelette à compléter (2026-11-24)

`v3-mesure.kicad_pcb` contient **ce qui ne se discute pas**, et aucun composant électrique :

- le contour de 70 × 50 mm et la déclaration des **quatre couches** ;
- la **barrière** et la **séparation des voies**, déclarées comme zones interdites sur les
  quatre couches (ni piste, ni via, ni plan de cuivre ; les plots y sont admis, puisque les
  ADM3260 chevauchent la barrière) ;
- quatre trous de fixation non métallisés, avec le cuivre écarté autour ;
- des repères sur les couches de dessin : axe de chaque BNC, position de l'âme, emplacement
  indicatif des modules. Ils n'ont aucun effet électrique.

Le projet range les fils en trois classes — `PRIMAIRE`, `ISO_PH`, `ISO_ORP` — et
`v3-mesure.kicad_dru` impose **5,4 mm** entre deux classes différentes. ⚠️ 5,4 et non 5,5 :
la limite est l'écart entre les deux rangées de plots de l'ADM3260 (5,45 mm) ; les bandes
interdites, elles, font bien 5,5 mm. ⚠️ Cette règle compare des cuivres **d'une même
couche** : le recouvrement entre couches est empêché par les zones interdites.

**Pour continuer dans KiCad** : ouvrir la carte, puis *Outils › Mettre à jour le PCB à
partir du schéma*. Les composants arrivent en vrac ; les placer dans leur zone, puis router.

**Vérifié** : KiCad charge la carte et son contrôle des règles rend 0 violation — sur une
carte encore vide. **Non vérifié** : que les zones interdites et la règle d'isolement
réagissent bien dès qu'il y a du cuivre ; l'empilement des couches (épaisseurs de JLCPCB)
n'est pas renseigné.

## Ce que le boîtier actuel impose (fichiers FreeCAD reçus le 2026-11-24)

L'utilisateur a transmis cinq pièces du boîtier « V14 » (platine murale, capot de
l'alimentation, coupon d'essai), datées du 2026-10-07. Les sources FreeCAD restent **hors du
dépôt**. Relevé fait en lisant les cotes maîtresses et la géométrie des fichiers ; ⚠️ non
ouvert dans FreeCAD, aucune vue 3D regardée.

| Relevé | Valeur |
|---|---|
| Platine murale, hors tout | 210 × 175 mm, profondeur 88,2 mm |
| Matière | **ASA** (plastique imprimé) ⇒ ✅ **paroi isolante** : la condition posée pour les BNC est remplie |
| Paroi du bas | épaisseur **3,2 mm** |
| Perçages de la paroi du bas | cinq, tous de **Ø 13 mm** : trois à X = 44 / 68 / 88 mm (à 49 mm du fond), deux à X = 123,1 / 159,4 mm (à 19,8 mm du fond) |
| Entraxe des deux perçages de droite | **36,3 mm** |
| Cloison du compartiment secteur | à X = 54,5 mm ; capot de l'alimentation RPS-30-12 : 60,8 × 86,2 × 34 mm |
| Carte actuelle (v2) | 119,38 × 119,38 mm, bord gauche à X = 76,62 mm, sur entretoises de 8,8 mm, dos de carte à 12 mm du fond |
| Joint | gorge de 2,8 × 1,4 mm : boîtier étanche |

**Ce qui tombe bien**
- La paroi est en plastique : les BNC à corps métallique peuvent y être serrées sans annuler
  l'isolation.
- Les perçages font Ø 13 mm et le nez de la BNC est fileté **M12** : il passe.
- Paroi 3,2 mm + joint 0,8 mm + écrou 2,5 mm = 6,5 mm, pour un filetage d'environ 7,2 mm
  (⚠️ longueur **mesurée sur le plan** de la prise, non cotée) : le serrage tient, de peu.

**Ce qui ne tombe pas juste, et qu'il faudra adapter** — l'utilisateur a prévenu que
l'intérieur serait repris pour les trois cartes :
- **Hauteur des BNC.** Les deux perçages de droite sont à 19,8 mm du fond. L'axe de la BNC
  retenue est à 9,8 mm au-dessus de la carte. ⇒ Pour les réutiliser, le dessus de la carte
  de mesure doit être à 10,0 mm du fond, soit un dos de carte à 8,4 mm (carte de 1,6 mm),
  contre 12 mm pour la carte actuelle. Sinon, on déplace les perçages.
- **Position de la carte de mesure.** L'âme de chaque BNC se trouve à environ 6,7 mm de la
  face intérieure de la paroi du bas : la carte de mesure se place tout en bas du boîtier.
- **Entraxe.** 36,3 mm entre les deux BNC si l'on garde les perçages : c'est la distance
  entre les deux voies. Avec 5,5 mm d'isolement entre voies, cela laisse ~30 mm de large
  par voie au niveau des prises — ⚠️ **à vérifier au placement**, c'est serré pour un
  module de 12 mm, sa prise de 14 mm et le plan de sonde.
- **Place pour trois cartes.** À droite de la cloison secteur, il reste environ 145 mm de
  large sur 160 mm de haut. Une carte de mesure en bas **plus** une carte principale de la
  taille du v2 (119 mm) ne tiennent pas l'une au-dessus de l'autre. ⇒ La carte principale
  du v3 doit être plus petite que celle du v2, ou la disposition change.

## L'empreinte du module Atlas (`v3-mesure:Atlas_OEM`)

Dessinée d'après la documentation du pH OEM V4.7, cotes **lues sur les figures** :

| Cote | Valeur | Page |
|---|---|---|
| Corps du module | 12 × 11 mm, hauteur 2,65 mm | 6 |
| Pas entre plots | 2,3 mm | 6 et 38 |
| Écart entre les deux rangées de plots, centre à centre | 12 mm (11,9 mm entre demi-trous du module) | 38 et 6 |
| Taille d'un plot | 2,5 × 1,4 mm | 38 |
| Numérotation, vue de dessus | 1 à 5 de haut en bas à gauche ; 10 à 6 de haut en bas à droite | 33 |

- L'empreinte porte une **zone interdite aux vias** sous tout le corps du module, sur toutes
  les couches : la consigne d'Atlas (p. 35) est ainsi contrôlée par KiCad, pas tenue à l'œil.
- Vérifié : KiCad lit la bibliothèque et trace l'empreinte ; le rendu a été comparé à la
  figure de la page 38. ⚠️ La zone interdite n'a pas encore été éprouvée par un contrôle de
  règles sur une carte.
- ℹ️ Le module ORP a le même brochage ; ses cotes n'ont pas été relues séparément.

## Ce que fait cette carte

Elle porte les deux voies de mesure, pH et ORP, **chacune isolée séparément**. Chaque voie
est identique : un isolateur ADM3260, un module Atlas Scientific OEM, un connecteur de sonde.
Elle se relie à la carte principale par **un seul câble à huit fils**. Le signal de la sonde
ne quitte jamais cette carte.

Les deux voies ne partagent **rien** du côté isolé : ni alimentation, ni masse, ni cuivre.

## Ce qui a été lu pour établir ce document

| Document | Révision lue | Source |
|---|---|---|
| Atlas pH OEM — documentation | V4.7 | fabricant |
| Atlas ORP OEM — documentation | — | fabricant |
| ADM3260 — fiche technique | **C** | copie d'un revendeur |
| ADM3260 — guide de la carte d'essai (UG-724) | 0 | copie d'un revendeur |
| AN-0971 — rayonnement des convertisseurs isolés | **B** (6/2011) | copie tierce |

⚠️ **Le site du fabricant de l'ADM3260 est inaccessible depuis le poste de travail.** La
révision **F (12/2024)** a été relue le 2026-11-24 par le relecteur sur une copie tierce :
broches, routage et capacité de barrière inchangés par rapport à la révision C.

### Ce que dit AN-0971, et ce qu'on en retient

- La technique principale contre le rayonnement est un **condensateur entre les deux côtés
  de la barrière**, fait de plans internes qui se recouvrent : des exemples de 140 à 300 pF.
  ⛔ **On ne l'applique pas** : c'est 60 à 140 fois les 2,2 pF de l'isolateur, et c'est le
  chemin du bruit que cette carte doit fermer.
- ✅ **Le fabricant prévoit lui-même ce cas** (conclusions, p. 19) : *« Where low ac leakage
  is required, as in some medical applications, stitching capacitance may not be a viable
  solution. In these applications, grounded metallic chassis enclosures may be the most
  practical solution »*. ⇒ Le rayonnement se traite par le **boîtier**, pas par la carte.
- Ce qui reste applicable **sans toucher à la barrière** (p. 10) : au moins quatre couches ;
  plans d'alimentation et de masse rapprochés ; pistes au-dessus d'un seul plan de
  référence ; rien de rapide près des bords ; vias d'alimentation larges.
- **La charge compte beaucoup** (p. 9) : moins le convertisseur débite, moins il rayonne. Le
  module OEM consomme ~3,5 mA, pour un convertisseur prévu pour 150 mW. ⇒ Situation favorable.
- Argument pour les quatre couches (p. 9, figure 12) : bruit sur l'alimentation de 460 mV
  sans plan, 104 mV puis 64 mV avec des plans rapprochés. Le fabricant parle d'un
  *« very quiet environment for precision measurement applications »*.
- ⚠️ Mesures faites sur une autre famille (ADuM540x, 500 mW) : ordres de grandeur, pas des
  valeurs garanties pour l'ADM3260.

## Liste des composants

Références de la voie pH en `1xx`, de la voie ORP en `2xx` : les deux voies sont identiques.

| Repère | Composant | Valeur | Boîtier | Référence JLCPCB | Source de la valeur |
|---|---|---|---|---|---|
| U101, U201 | Isolateur de données et d'alimentation | ADM3260ARSZ | SSOP-20 | C208558 ✅ (stock faible) | schéma Atlas |
| U102 | Module Atlas **pH** OEM | — | 12 × 11 mm, 10 plots | ⛔ pièce fournie | — |
| U202 | Module Atlas **ORP** OEM | — | 12 × 11 mm, 10 plots | ⛔ pièce fournie | — |
| C101, C201 | Découplage de `VIN` | 0,1 µF | 0402 | C1525 (base) | fiche ADM3260, UG-724 |
| C102, C202 | Réserve de `VIN` | 10 µF | 0805 | C15850 (base) | idem |
| C103, C203 | Découplage de `VDDP` | 0,1 µF | 0402 | C1525 (base) | idem |
| C104, C204 | Réserve de `VDDP` | 10 µF | 0805 | C15850 (base) | idem |
| C105, C205 | Découplage de `VISO` | 0,1 µF | 0402 | C1525 (base) | idem |
| C106, C206 | Réserve de `VISO` | 10 µF | 0805 | C15850 (base) | idem |
| C107, C207 | Découplage de `VDDISO` (broche 17) | 0,1 µF | 0402 | C1525 (base) | idem |
| R101, R102, R201, R202 | Rappels du bus, côté ESP32 | 4,7 kΩ 1 % | 0603 | C23162 (base) | schéma Atlas |
| R103, R104, R203, R204 | Rappels du bus, côté isolé | 4,7 kΩ 1 % | 0603 | C23162 (base) | schéma Atlas |
| R105, R205 | Réglage de tension, bas (`RBOTTOM`) | **4,7 kΩ 1 %** | 0603 | C23162 (base) | figure d'Atlas (R6) |
| R106, R206 | Réglage de tension, haut (`RTOP`) | **10 kΩ 1 %** | 0603 | C25804 (base) | figure d'Atlas (R7) |
| J101, J201 | Connecteur de sonde | **BNC coudée**, pour carte | traversant | C521219 (dosinconn DOSIN-801-0080, étendue) | utilisateur (2026-11-24) : les sondes en place sont en BNC |
| J1 | Connecteur vers la carte principale | 8 voies, pas de 2,5 mm, JST XH (B8B-XH-A) | traversant | C157972 (étendue) | fiche feature-215 |

Total par voie, hors module et connecteurs : 1 circuit, **7 condensateurs**, 6 résistances.

**Pas de C108 ni de C208** — décision du 2026-11-24, sur une remarque de l'utilisateur
(« C105 et C107 sont identiques, C106 et C108 aussi »). Les broches 12 (`VISO`) et 17
(`VDDISO`) sont reliées : les condensateurs y sont en parallèle sur le même fil. La fiche de
l'ADM3260 (rév. C, p. 16) exige 0,1 µF **et** 10 µF sur `VISO`, mais **seulement 0,01 à
0,1 µF** sur `VDDISO`. ⇒ Le second 10 µF doublait celui de `VISO`, à 3 mm de lui : retiré.
C105 et C107 restent tous les deux, chacun contre **sa** broche. Les numéros C108 et C208
restent libres.

**C104 et C204 sont conservés** (10 µF sur `+3V3`), bien que la fiche ne les exige pas non
plus : le `+3V3` est un fil **distinct** du `+5V`, il arrive par un câble, et sans eux il ne
resterait aucune réserve sur cette alimentation à l'entrée de la carte. Utilité non
démontrée, absence de risque non démontrée non plus : dans le doute, on garde.

**Tension isolée : 3,85 V**, avec les valeurs **lues sur la figure d'Atlas** (pH OEM V4.7,
p. 30) : 10 kΩ entre `VSEL` et `VISO`, 4,7 kΩ entre `VSEL` et `GNDISO`. Formule de la fiche
ADM3260 : `VISO = 1,23 V × (RBOTTOM + RTOP) / RBOTTOM` = 1,23 × 14,7 / 4,7 = 3,85 V (Atlas
écrit « 3,9 V »). ⇒ Deux valeurs déjà présentes sur la carte : aucune référence de plus.
La fiche limite la sortie à 20 % au-dessus de `VIN` : avec `VIN` = 5 V, cela convient.

ℹ️ Références JLCPCB relevées par le relecteur dans un index public du catalogue ;
« étendue » confirmé sur jlcpcb.com pour l'ADM3260 et la BNC Molex. ⚠️ **Stocks réels non
vérifiés** : à contrôler au moment du devis.

**Ce qui est volontairement absent**
- Le transistor de mise en veille et ses deux résistances de 1 MΩ du schéma d'Atlas : ils
  servent à couper le convertisseur. Ici `PDIS` est relié à la masse, convertisseur toujours
  actif. ⇒ Trois composants en moins par voie.
- Tout condensateur ou plan reliant les deux côtés de la barrière (voir AN-0971).
- Toute liaison de la broche d'alerte `INT` du module : laissée en l'air, comme Atlas le
  demande quand elle ne sert pas.

## Tableau des connexions — une voie

Valable pour les deux voies : remplacer `_PH` par `_ORP`, `1xx` par `2xx`.
Noms des signaux : `+5V`, `+3V3`, `GND` viennent de la carte principale ;
`VISO_PH` et `GNDISO_PH` n'existent que du côté isolé de la voie pH.

### Isolateur ADM3260 (U101)

| Broche | Nom | Relié à | Remarque |
|---|---|---|---|
| 1, 5, 7, 10 | GNDP | `GND` | masse côté ESP32 |
| 2 | SCL2 | `SCL_PH` + R102 vers `+3V3` | côté ESP32 |
| 3 | SDA2 | `SDA_PH` + R101 vers `+3V3` | côté ESP32 |
| 4 | VDDP | `+3V3` + C103, C104 vers `GND` | alimentation des données |
| 6 | NC | **rien** | ne rien connecter |
| 8 | PDIS | `GND` | convertisseur toujours actif |
| 9 | VIN | `+5V` + C101, C102 vers `GND` | alimentation du convertisseur |
| 11, 14, 16, 20 | GNDISO | `GNDISO_PH` | masse isolée |
| 12 | VISO | `VISO_PH` + C105, C106 vers `GNDISO_PH` + R106 | sortie isolée 3,9 V |
| 13 | VSEL | point milieu de R106 (vers `VISO_PH`) et R105 (vers `GNDISO_PH`) | réglage de tension |
| 15 | NC | **rien** | ne rien connecter |
| 17 | VDDISO | `VISO_PH` + C107 vers `GNDISO_PH` | alimentation des données isolées |
| 18 | SDA1 | `SDA_ISO_PH` + R103 vers `VISO_PH` | côté isolé |
| 19 | SCL1 | `SCL_ISO_PH` + R104 vers `VISO_PH` | côté isolé |

### Module Atlas OEM (U102)

| Broche | Nom | Relié à | Remarque |
|---|---|---|---|
| 1 | SDA | `SDA_ISO_PH` | |
| 2 | NC | **rien** | |
| 3 | VCC | `VISO_PH` | 3,0 à 5,5 V admis |
| 4 | PRB | âme du connecteur de sonde J101 | ⚠️ **la piste sensible** : 0,4 mm, la plus courte possible |
| 5 | PRB GND | `GND_SONDE_PH` : corps de J101 + plan de garde | ⚠️ plan **distinct** de `GNDISO_PH` |
| 6 | GND | `GNDISO_PH` | masse isolée |
| 7 | INT | **rien** | laissée en l'air |
| 8 | SCL | `SCL_ISO_PH` | |
| 9, 10 | NC | **rien** | |

⚠️ **Deux masses du côté isolé, par voie** — consigne d'Atlas : `GNDISO_PH` (broche 6) est le
zéro volt de l'alimentation ; `GND_SONDE_PH` (broche 5) est la référence de la sonde, *dont
la broche 5 est la source*. ⛔ **Aucun lien entre les deux n'est tracé sur la carte.**
Le plan `GND_SONDE_PH` entoure la piste `PRB`, dessus et dessous.

### Connecteur vers la carte principale (J1)

| Voie | Signal | Va vers |
|---|---|---|
| 1 | `+5V` | broche 9 (`VIN`) de U101 et U201 |
| 2 | `+3V3` | broche 4 (`VDDP`) de U101 et U201, rappels R101, R102, R201, R202 |
| 3 | `GND` | broches `GNDP` |
| 4 | `GND` | broches `GNDP` |
| 5 | `SDA_PH` | broche 3 de U101 |
| 6 | `SCL_PH` | broche 2 de U101 |
| 7 | `SDA_ORP` | broche 3 de U201 |
| 8 | `SCL_ORP` | broche 2 de U201 |

⚠️ **Ordre des voies non figé** : à arrêter au schéma, en plaçant les masses entre
l'alimentation et les signaux. La carte principale réunit `SDA_PH`/`SDA_ORP` et
`SCL_PH`/`SCL_ORP` en un seul bus (décision D3) ; cette carte les sort séparément.

## Relecture du 2026-11-24 (`electronics-reviewer`)

**Verdict : on peut passer au schéma. Aucune erreur.** Brochage de l'ADM3260 conforme broche
par broche au tableau de la fiche (révisions C **et F**) ; brochage du module conforme pour
le pH et pour l'ORP ; les quatre calculs ci-dessous refaits et confirmés.

- ✅ **Séparer `VIN` (5 V) et `VDDP` (3,3 V) est obligatoire chez nous**, alors qu'Atlas les
  alimente par le même fil : alimenté en 5 V, `VDDP` n'accepterait un niveau haut qu'au-dessus
  de 3,5 V, et le bus à 3,3 V resterait muet.
- ✅ Les deux masses : Atlas (p. 33-34) dessine deux plans séparés par un vide, sans lien.
  **Le corps de la BNC va au plan de sonde** (broche 5).
- ✅ Les trois retraits volontaires ne créent aucun défaut.
- ⚠️ **RISQUE — le corps d'une BNC qui touche un boîtier métallique relié à la terre.** La
  masse de sonde se retrouverait à la terre : l'isolation de la voie tombe, et les deux
  voies sont de nouveau reliées entre elles. Certain si les BNC traversent une paroi
  métallique. ⇒ **Règle, gratuite** : le corps des BNC ne touche aucun métal du boîtier, et
  les trous de fixation de la carte ne sont reliés à rien. À reporter dans l'étude du boîtier.
- ℹ️ Préférence du relecteur, non retenue : retirer C104/C204 (la fiche ne demande que 0,01
  à 0,1 µF sur `VDDP`). Voir « C104 et C204 sont conservés » plus haut. En revanche son
  raisonnement a été appliqué à C108/C208, où il est sans réserve.
- ℹ️ **Envisagé et écarté par le relecteur** : protection contre les décharges sur la BNC
  (une fuite de 200 nA dans 100 MΩ fausserait la mesure de 20 V) ; garder le transistor de
  coupure ; condensateur entre les deux côtés de la barrière ; garde active autour de la
  piste de sonde.
- ⚠️ Non vérifiable : si les broches 5 et 6 du module sont reliées à l'intérieur — Atlas ne
  le dit pas.

## Vérifications déjà faites sur ce tableau

- **Niveau bas du côté isolé** : l'ADM3260 tire la ligne à 0,6–0,9 V (fiche, révision C). Le
  seuil bas d'un bus I2C est 0,3 × l'alimentation, soit 1,17 V à 3,9 V. ⇒ Compatible, avec
  0,27 V de marge. ⚠️ Le seuil propre au module Atlas n'est pas publié dans sa documentation.
- **Rappels réunis sur la carte principale** : 4,7 kΩ par voie en parallèle = 2,35 kΩ, soit
  1,4 mA sous 3,3 V, sous la limite I2C de 3 mA.
- **Adresses** : pH 0x65, ORP 0x66 — distinctes, donc compatibles sur un bus commun.
- **Consommation** : le module demande ~3,5 mA sous 3,9 V, soit ~14 mW, pour 150 mW
  disponibles.

## Points ouverts avant le schéma

| # | Point | Qui tranche |
|---|---|---|
| O1 | ✅ `RTOP` = 10 kΩ, `RBOTTOM` = 4,7 kΩ, lus sur la figure d'Atlas | fermé |
| O2 | ✅ Connecteur de sonde : **BNC coudée**, sur le bord du bas (utilisateur, 2026-11-24) | fermé |
| O3 | ✅ Références proposées (voir liste). Reste : stocks réels au devis, orientation de la BNC | devis |
| O4 | ✅ Révision **F (12/2024)** relue par le relecteur sur une copie tierce : broches, section de routage et 2,2 pF inchangés ; elle donne **5,5 mm** d'isolement dans le plan de la carte | fermé |
| O5 | ✅ **Quatre couches** — décision de l'utilisateur du 2026-11-24, sur avis du relecteur. Condition : une zone interdite au cuivre sur **toutes** les couches, sous chaque ADM3260 et entre les domaines, contrôlée par KiCad. Gain attendu : bruit d'alimentation (460 → 64 mV d'après AN-0971) ; gain sur la piste de sonde faible et non chiffré. Risque écarté par la zone interdite : 1 cm² de plans internes qui se recouvrent ajouterait 3,7 à 18 pF, soit 2 à 8 fois l'isolateur | fermé |
| O6 | Ordre des voies de J1 et référence exacte du connecteur | schéma |
| O7 | ✅ Empreinte du module OEM créée (`v3-mesure:Atlas_OEM`) : pas **2,3 mm**, plots **2,5 × 1,4 mm**, 12 mm entre rangées — la ligne d'origine avait interverti pas et longueur de plot | fermé |
| O8 | ✅ **5,5 mm sur toutes les couches** entre le côté ESP32 et chaque voie, et entre les deux voies (isolation de fonctionnement, tout est en très basse tension) | fermé |
| O9 | Courant sur le `+5V` à inscrire au contrat de liaison : 12 mA au plus par voie à vide, 104 mA à pleine charge (fiche ADM3260, tableau 2) | schéma |
| O10 | ✅ Empreinte de la BNC coudée créée d'après le plan du fabricant : âme au centre, quatre pattes de corps | fermé |

## Règles de routage à ne pas oublier (rappel, détail dans la fiche)

- ⛔ Aucun via sous les modules Atlas.
- ⛔ Aucun cuivre, sur aucune couche, ne franchit la barrière sous un ADM3260.
- **Quatre couches** : zone interdite au cuivre de 5,5 mm, sur les quatre couches, entre le côté
  ESP32 et chaque voie et entre les deux voies — déclarée comme règle dans KiCad, pas tenue à l'œil.
- Condensateurs de découplage à moins de 2 mm des broches de l'ADM3260.
- Connecteur de sonde au plus près du module ; piste `PRB` de 0,4 mm, entourée de son plan.
- Nettoyage du flux **obligatoire** après soudure : voir `docs/FABRICATION.md`.
