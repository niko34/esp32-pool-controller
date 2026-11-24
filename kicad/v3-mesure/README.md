# Carte de mesure du PCB v3 — composants et connexions

> **État : schéma écrit (révision 0.4, vrais fils), ⚠️ pas encore relu par le relecteur.** Aucune carte n'est dessinée. Ce document est la référence contre
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
| `v3-mesure.pretty/` + `fp-lib-table` | l'empreinte propre à cette carte : `Atlas_OEM` |
| `generer_schema.py` | le script qui a écrit le schéma depuis le tableau des connexions |

**Le schéma (révision 0.4) est tracé avec de vrais fils** — demande de l'utilisateur du
2026-11-24, la révision 0.1 reliait tout par étiquettes. Les alimentations et les masses
passent par les symboles habituels (flèche, masse), comme dans le schéma du v2. Seuls les
quatre signaux du bus rejoignent le connecteur J1 par étiquette, parce que J1 est commun aux
deux voies.

**Ce qui a été vérifié**
- **Dans le KiCad de l'utilisateur** (révision 0.1, 2026-11-24) : contrôle électrique à
  **0 erreur, 0 avertissement**. ⇒ Les noms d'empreintes standard (0402, 0805, 0603,
  SSOP-20, JST XH) existent bien dans ses bibliothèques.
- **En ligne de commande** (révision 0.4, KiCad 9.0.2) : 0 erreur ; la liste des connexions
  que KiCad extrait du schéma est **identique** à celle du tableau de ce document — 21 nets,
  12 broches volontairement en l'air ; rendu regardé en image, sans chevauchement.
  Sur ce poste, le contrôle rend des avertissements d'un seul type : les bibliothèques
  standard de KiCad n'y sont pas installées.

**Ce qui n'est PAS vérifié**
- ✅ Révision 0.2 ouverte dans le KiCad de l'utilisateur le 2026-11-24 : contrôle électrique sans problème. ⚠️ Les révisions 0.3 (C108/C208 retirés) et 0.4 (empreinte du module attribuée) n'y ont pas encore été ouvertes.
- ⛔ **L'empreinte de la prise BNC n'est pas attribuée** : elle doit être dessinée d'après
  la fiche de la prise retenue (voir « Disposition dans le boîtier »).
- ⚠️ **L'empreinte `Atlas_OEM` n'a pas été confrontée à un module réel** : elle suit les
  cotes publiées, pas une pièce mesurée.

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

**Prise pressentie** : `C521219` — dosinconn DOSIN-801-0080, BNC coudée 50 Ω pour carte,
traversante, pièce étendue (lue sur la fiche JLCPCB le 2026-11-24). ⚠️ **Non vérifié** : si
c'est bien une embase femelle, la matière de son corps, son stock, et surtout **ses cotes
de perçage** — l'empreinte ne se dessine que d'après sa fiche technique.

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
| J101, J201 | Connecteur de sonde | **BNC coudée**, pour carte | traversant | C521219 (dosinconn DOSIN-801-0080, étendue) — ⚠️ fiche technique à lire | utilisateur (2026-11-24) : les sondes en place sont en BNC |
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
| O10 | Empreinte de la BNC coudée `C521219` : à dessiner d'après sa fiche technique ; dire quelles pattes sont le corps | à faire |

## Règles de routage à ne pas oublier (rappel, détail dans la fiche)

- ⛔ Aucun via sous les modules Atlas.
- ⛔ Aucun cuivre, sur aucune couche, ne franchit la barrière sous un ADM3260.
- **Quatre couches** : zone interdite au cuivre de 5,5 mm, sur les quatre couches, entre le côté
  ESP32 et chaque voie et entre les deux voies — déclarée comme règle dans KiCad, pas tenue à l'œil.
- Condensateurs de découplage à moins de 2 mm des broches de l'ADM3260.
- Connecteur de sonde au plus près du module ; piste `PRB` de 0,4 mm, entourée de son plan.
- Nettoyage du flux **obligatoire** après soudure : voir `docs/FABRICATION.md`.
