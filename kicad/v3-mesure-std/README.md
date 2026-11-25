# Carte de mesure du PCB v3, à composants courants — composants et connexions

Carte de mesure **retenue pour le v3** (décision D8 de
[feature-215](../../specs/features/todo/feature-215-conception-du-pcb-v3.md), 2026-11-25) :
sans module Atlas, tous les composants posés par l'atelier, chacun avec un équivalent d'un
autre fabricant. L'étude et les relectures sont dans
[feature-217](../../specs/features/todo/feature-217-carte-de-mesure-a-composants-courants.md).

La carte à modules Atlas reste dans [`../v3-mesure/`](../v3-mesure/README.md) comme étude.
Ce qui n'est pas redit ici en vient et **reste valable** : disposition dans le boîtier, prises
BNC coudées sur le bord du bas, connecteur J1 et son ordre, quatre couches, relevé du boîtier.

✅ **État au 2026-11-25 : liste et tableau relus par `electronics-reviewer` — aucune erreur,
corrections portées (voir « Relecture » en bas). **Schéma écrit (rév. 0.1), pas encore ouvert
dans KiCad par l'utilisateur. Aucune carte.**

## Les fichiers de ce dossier

| Fichier | Rôle |
|---|---|
| `generer_schema.py` | écrit le schéma depuis le tableau ci-dessous. ⚠️ Le relancer **réécrit** le schéma en entier |
| `v3-mesure-std.kicad_sch` | le schéma, format A2 : les deux voies à gauche, le connecteur J1 à droite |
| `v3-mesure-std.kicad_sym`, `sym-lib-table` | les symboles propres à la carte |
| `v3-mesure-std.kicad_pro` | le projet KiCad |

**Vérifié sur le schéma (2026-11-25)** : la liste des connexions que KiCad en extrait est
identique à celle du tableau (31 fils nommés, 7 broches en l'air) ; contrôle des règles
électriques en ligne de commande : aucune erreur ni avertissement sur le schéma lui-même.
**Non vérifié** : l'ouverture dans le KiCad de l'utilisateur ; les empreintes du module
d'alimentation et de la prise BNC ne sont pas encore affectées (étape de la carte).

## Ce que fait une voie

```
 âme de la BNC ──[2,2 MΩ]──► ampli d'entrée (suiveur) ──► convertisseur, entrée +
                                └─ sa sortie de garde ──► anneau de garde autour de l'entrée
 corps de la BNC ◄─[100 Ω]─ ampli Vcm (suiveur) ◄── pont de deux résistances sur l'alimentation isolée
        └──────────────────────────────────────────────────► convertisseur, entrée −
 convertisseur ──I2C──► isolateur de signaux ──► J1 ──► ESP32
 +5 V de J1 ──► module d'alimentation isolée 9 V ──► régulateur 5 V ──► alimentation isolée
```

Deux voies, **isolées séparément** (D1). Repères en `1xx` pour le pH, `2xx` pour l'ORP.
⚠️ Les deux voies **ne sont plus identiques** : l'entrée du pH a son amplificateur dédié,
l'entrée de l'ORP utilise la seconde moitié de l'amplificateur double.

## Liste des composants

Stocks et prix lus chez le distributeur le 2026-11-25 (voir feature-217). ⚠️ Stock de
l'atelier lui-même non lu.

| Repère | Rôle | Composant | Boîtier | Référence | Équivalent d'un autre fabricant |
|---|---|---|---|---|---|
| PS101, PS201 | Alimentation isolée | module 5 V → 9 V, 1 W, non régulé (B0509S-1WR3) | SIP-4 traversant | C5369463 (Ylptec, stock 1 503) | C7500906 (Evisun, stock 184), même boîtier, même brochage |
| U101, U201 | Isolation des signaux I2C | ISO1540DR | SOIC-8 | C179739 | ADuM1250ARZ (C13839) |
| U102, U202 | Régulateur 5 V du côté isolé | AMS1117-5.0 | SOT-223 | C6187 | LD1117-5.0 UMW (C347230), même brochage, entrée ≤ 12 V conseillée |
| U103, U203 | Convertisseur | ADS1115IDGSR | VSSOP-10 | C37593 | SGM58031XMS10 (C700177), pastille centrale à la masse |
| U104 | Amplificateur d'entrée **pH** | OPA928DR | SOIC-8 | C32418779 | ADA4530-1ARZ (C77380), même brochage, rien d'autre à changer |
| U105, U205 | Amplificateur double : Vcm, et entrée **ORP** sur U205 | LMC6482AIMX (**TI**) | SOIC-8 | C12620 | LMC6482IDT de ST (C2058286) |
| R101, R102, R201, R202 | Rappels du bus, côté ESP32 | 4,7 kΩ 1 % | 0603 | C23162 | — |
| R103, R104, R203, R204 | Rappels du bus, côté isolé | 3,3 kΩ 1 % | 0603 | C22978 | — |
| R105, R205 | Protection de l'entrée, en série avec l'âme | 2,2 MΩ 1 % | 0805 | C26113 | — |
| R106, R206 | Pont de Vcm, haut | pH : **30 kΩ** (C22984) · ORP : **10 kΩ** (C25804) | 0603 | voir valeur | — |
| R108, R208 | Entre le suiveur de Vcm et le corps de la prise | 100 Ω 1 % | 0603 | C22775 | — |
| R107, R207 | Pont de Vcm, bas | 10 kΩ 1 % | 0603 | C25804 | — |
| C101, C201 | Entrée du module | 4,7 µF 25 V | 0805 | C1779 | — |
| C103, C203 | Sortie du module et entrée du régulateur | 10 µF **50 V** | **1206** | C13585 | — |
| C104, C204 | Sortie du régulateur | 22 µF 16 V, **tantale** | boîtier B | à relever | — |
| C105, C205 | Découplage de l'isolateur, côté ESP32 | 0,1 µF | 0402 | C1525 | — |
| C106, C206 | Découplage de l'isolateur, côté isolé | 0,1 µF | 0402 | C1525 | — |
| C107, C207 | Découplage du convertisseur | 0,1 µF | 0402 | C1525 | — |
| C108 | Découplage de l'amplificateur d'entrée pH | 0,1 µF | 0402 | C1525 | — |
| C109, C209 | Découplage de l'amplificateur double | 0,1 µF | 0402 | C1525 | — |
| C110, C210 | Filtre du pont de Vcm | 0,1 µF | 0402 | C1525 | — |
| J101, J201 | Connecteur de sonde | BNC coudée DOSIN-801-0080 | traversant | C521219 | — (inchangé) |
| J1 | Connecteur vers la carte principale | JST XH 8 voies (B8B-XH-A) | traversant | C157972 | — (inchangé) |

**Tensions**
- Alimentation isolée : **5 V** (au lieu de 3,85 V), demandée par l'amplificateur d'entrée
  (4,5 V minimum) et par la marge des niveaux du bus côté isolé.
- Vcm du pH : **1,25 V** (pont 30 kΩ / 10 kΩ), **pour les deux amplificateurs possibles**.
  L'entrée balaie 0,7 à 1,8 V, dérive du zéro de 128 mV comprise. OPA928 : dans sa plage
  garantie (0,1 à 2,0 V). ADA4530-1 : sur 0 à 3 V sa fiche garantit décalage ≤ 300 µV,
  réjection 73 dB, écart de garde ≤ 150 µV ; pire cas 0,12 mV, soit **0,002 pH**.
  ⇒ Le remplacement se fait sur la même empreinte, **sans rien changer d'autre**.
- Vcm de l'ORP : **2,5 V** (pont 10 kΩ / 10 kΩ). L'entrée balaie 1,48 à 3,52 V.
- Adresses I2C : convertisseur du pH en **0x48**, de l'ORP en **0x49** — les deux bus sont
  réunis sur la carte principale (D3).

## Tableau des connexions

Signaux : `+5V`, `+3V3`, `GND` viennent de la carte principale. Du côté isolé de la voie pH :
`VBRUT_PH` (sortie du module), `VISO_PH` (5 V régulé), `GNDISO_PH`, `VCM_PH` (sortie du suiveur), `REF_PH` (corps de la prise, après les 100 Ω),
`GARDE_PH`.
Pour l'ORP, remplacer `_PH` par `_ORP` et `1xx` par `2xx`, sauf mention.

### Module d'alimentation isolée (PS101)

| Broche | Nom | Relié à |
|---|---|---|
| 1 | −Vin | `GND` |
| 2 | +Vin | `+5V` + C101 vers `GND` |
| 3 | 0V | `GNDISO_PH` |
| 4 | +Vo | `VBRUT_PH` + C103 vers `GNDISO_PH` |

Brochage lu sur le dessin des deux fiches (Ylptec et Evisun, p. 4) : identique, pas de
2,54 mm, trous de 1,0 mm.

### Régulateur (U102, AMS1117-5.0)

| Broche | Nom | Relié à |
|---|---|---|
| 1 | GND | `GNDISO_PH` |
| 2 et languette | OUT | `VISO_PH` + C104 vers `GNDISO_PH` |
| 3 | IN | `VBRUT_PH` (C103 est sur ce fil) |

### Isolateur de signaux (U101, ISO1540)

Le **côté 2** regarde l'ESP32, le **côté 1** regarde le convertisseur.

| Broche | Nom | Relié à | Remarque |
|---|---|---|---|
| 1 | VCC1 | `VISO_PH` + C106 vers `GNDISO_PH` | côté isolé |
| 2 | SDA1 | `SDA_ISO_PH` + R103 vers `VISO_PH` | niveau bas 0,65 à 0,8 V, par conception |
| 3 | SCL1 | `SCL_ISO_PH` + R104 vers `VISO_PH` | |
| 4 | GND1 | `GNDISO_PH` | |
| 5 | GND2 | `GND` | côté ESP32 |
| 6 | SCL2 | `SCL_PH` + R102 vers `+3V3` | |
| 7 | SDA2 | `SDA_PH` + R101 vers `+3V3` | niveau bas 0,4 V au plus |
| 8 | VCC2 | `+3V3` + C105 vers `GND` | |

Pourquoi ce sens : le côté 1 sort un niveau bas de 0,8 V au plus. L'ESP32 sous 3,3 V
n'accepte un « 0 » que sous 0,825 V : 25 mV de marge. Le convertisseur sous 5 V l'accepte
jusqu'à 1,5 V : 0,7 V de marge. Rappels de 3,3 kΩ : 1,3 mA, dans la plage 0,5 à 3,5 mA que
la fiche impose au côté 1. Dans l'autre sens, le convertisseur tire sa ligne à 0,4 V au plus
et le côté 1 lit un « 0 » jusqu'à 0,5 V : 100 mV garantis. C'est aussi le sens que donne la
fiche de TI (p. 19) : côté 1 vers un nœud court et peu chargé, côté 2 vers le bus.

### Convertisseur (U103, ADS1115)

| Broche | Nom | Relié à | Remarque |
|---|---|---|---|
| 1 | ADDR | pH : `GNDISO_PH` · ORP : `VISO_ORP` | adresse 0x48 · 0x49 |
| 2 | ALERT/RDY | **rien** | non utilisée |
| 3 | GND | `GNDISO_PH` | |
| 4 | AIN0 | `MESURE_PH` (sortie de l'amplificateur d'entrée) | entrée + |
| 5 | AIN1 | `REF_PH` | entrée − : prise **au corps de la prise**, après R108 |
| 6, 7 | AIN2, AIN3 | **rien** | admis par la fiche (note 1) |
| 8 | VDD | `VISO_PH` + C107 vers `GNDISO_PH` | |
| 9 | SDA | `SDA_ISO_PH` | |
| 10 | SCL | `SCL_ISO_PH` | |

### Amplificateur d'entrée du pH (U104, OPA928 — voie pH seulement)

| Broche | Nom | Relié à | Remarque |
|---|---|---|---|
| 1 | IN+ | `ENTREE_PH` : R105, dont l'autre bout va à l'âme de J101 | ⚠️ **le point sensible** : piste la plus courte possible, entourée de l'anneau de garde |
| 2, 7 | GRD | `GARDE_PH` : l'anneau de garde | le composant pilote lui-même la garde |
| 3 | — | **rien** | à laisser en l'air sur les deux amplificateurs possibles |
| 4 | V− | `GNDISO_PH` | |
| 5 | V+ | `VISO_PH` + C108 vers `GNDISO_PH` | |
| 6 | OUT | `MESURE_PH` | vers AIN0 de U103 |
| 8 | IN− | `MESURE_PH` | montage suiveur |

### Amplificateur double (U105 pour le pH, U205 pour l'ORP — LMC6482)

| Broche | Nom | Voie pH (U105) | Voie ORP (U205) |
|---|---|---|---|
| 1 | OUT A | `VCM_PH` | `MESURE_ORP`, vers AIN0 de U203 |
| 2 | IN− A | `VCM_PH` (suiveur) | `MESURE_ORP` (suiveur) |
| 3 | IN+ A | `VCM_REF_PH` : milieu du pont R106 / R107, + C110 vers `GNDISO_PH` | `ENTREE_ORP` : R205, dont l'autre bout va à l'âme de J201 |
| 4 | V− | `GNDISO_PH` | `GNDISO_ORP` |
| 5 | IN+ B | `VCM_REF_PH` (moitié inutilisée, montée en suiveur) | `VCM_REF_ORP` : milieu du pont R206 / R207, + C210 |
| 6 | IN− B | relié à la broche 7 | `VCM_ORP` (suiveur) |
| 7 | OUT B | relié à la broche 6, rien d'autre | `VCM_ORP` |
| 8 | V+ | `VISO_PH` + C109 vers `GNDISO_PH` | `VISO_ORP` + C209 vers `GNDISO_ORP` |

ℹ️ Sur la voie ORP, les deux moitiés ont été **échangées au dessin du schéma** (entrée sur
la moitié A, Vcm sur la moitié B) pour qu'aucun fil ne se croise. Les deux moitiés sont
identiques ; la relecture avait porté sur la disposition inverse (fuite entre la broche
d'entrée et la broche 4 : inchangée, la broche 3 en est voisine comme la broche 5).

Le pont : R106 entre `VISO_PH` et `VCM_REF_PH`, R107 entre `VCM_REF_PH` et `GNDISO_PH`.
R108 entre `VCM_PH` (broche 1) et `REF_PH`.

### Connecteurs de sonde (J101, J201)

| Borne | Relié à |
|---|---|
| âme | R105 (R205) — rien d'autre |
| corps (4 pattes) | `REF_PH` (`REF_ORP`) |

⛔ Le corps de la BNC n'est **pas** à la masse : il est tenu à Vcm, à travers 100 Ω
(schéma pH publié par TI, fiche OPA928, fig. 7-7). Le boîtier est en
plastique ; rien de métallique ne doit relier les deux prises entre elles ni à la terre.

### Connecteur vers la carte principale (J1) — inchangé

| Voie | Signal | Va vers |
|---|---|---|
| 1 | `SDA_PH` | broche 7 de U101 |
| 2 | `SCL_PH` | broche 6 de U101 |
| 3 | `GND` | masse côté ESP32 |
| 4 | `+5V` | entrée de PS101 et PS201 |
| 5 | `+3V3` | broche 8 de U101 et U201, rappels R101, R102, R201, R202 |
| 6 | `GND` | masse côté ESP32 |
| 7 | `SDA_ORP` | broche 7 de U201 |
| 8 | `SCL_ORP` | broche 6 de U201 |

## Ce qui est volontairement absent

- Tout condensateur sur l'entrée de la sonde, et toute diode de protection : leur fuite
  fausserait la mesure (feature-217).
- Tout condensateur reliant les deux côtés de la barrière, y compris les 270 pF que
  conseille la fiche du module d'alimentation.
- Toute référence de tension de précision : la mesure est différentielle, Vcm s'annule.
- La liaison de la broche « mesure prête » du convertisseur.

## Relecture du 2026-11-25 (`electronics-reviewer`) — aucune erreur, on peut passer au schéma

Chaque broche vérifiée contre la fiche de son composant. Corrections portées ci-dessus :
- **C104 au tantale** : la seule condition de stabilité que donne la fiche de l'AMS1117
  (p. 4) est 22 µF au tantale ; elle ne dit rien de la céramique.
- **R108 / R208, 100 Ω**, entre le suiveur de Vcm et le corps de la prise, et l'entrée − du
  convertisseur prise côté prise : c'est le schéma de TI ; le LMC6482 ne supporte qu'environ
  100 pF en direct. Coût : 20 µV, soit 0,0003 pH, absorbés par l'étalonnage.
- **C102 retiré** : même fil que C103.
- **Un seul Vcm** pour les deux amplificateurs d'entrée possibles.

**Pourquoi l'AMS1117 est gardé** : ses 5 mA de consommation propre servent de charge au
module, qui en demande 10 % (la carte en tire ≈ 9 %, soit ≈ 10 mA). Un régulateur plus
sobre ferait tomber la charge vers 4 % et monter la tension. Bruit 150 µV efficaces, qui
arrive sur les deux entrées du convertisseur à la fois : effet attendu sous 0,001 pH.

**Voie ORP** : électrode d'environ 20 kΩ ; erreur dominée par R205 — 9 µV avec le LMC6482
de TI, 0,44 mV avec celui de ST, 0,8 mV de fuite sur une carte sale. Négligeable.

**Bus réuni sur la carte principale** : rappels en parallèle 2,35 kΩ (1,4 mA), 2,1 mA avec
ceux d'une horloge — sous les 3 mA de l'I2C.

**Envisagé et écarté** : résistance de charge sur le module, régulateur plus sobre, filtre
sur les entrées du convertisseur (seulement si la mesure le demande), grosse résistance sur
le corps de la prise (10 kΩ ferait passer le 50 Hz capté de 70 µV à 7 mV).

## Points ouverts avant le schéma

- [x] P1 — Brochage du module d'alimentation : lu, identique chez les deux fabricants.
- [x] P2 — Régulateur : AMS1117 gardé, C104 au tantale.
- [x] P3 — Condensateur d'entrée du module : 4,7 µF (les deux fiches).
- [x] P4 — AIN2 et AIN3 en l'air.
- [x] P5 — Un seul Vcm de 1,25 V pour le pH.
- [ ] P6 — Dessin de la garde autour de l'entrée du pH, selon la fiche de l'OPA928
      (vernis retiré, anneau sur les deux faces, plan de garde dessous). Voie ORP : un
      anneau relié à `MESURE_ORP` autour de la broche 3 de U205 et de R205.
- [ ] P7 — Isolement du corps de la prise BNC, et tenue aux décharges quand on la touche
      (le LMC6482 tient 1,5 kV ; les 100 Ω ne règlent pas ce point).
- [ ] P8 — Référence au catalogue du condensateur au tantale (22 µF 16 V, boîtier B) : non
      trouvée. Les autres passifs sont relevés ; ⚠️ leur stock se lit à l'atelier, pas chez
      le distributeur (une résistance courante y figure à 0).
- [ ] P11 — Empreinte du convertisseur : prévoir la pastille centrale de son équivalent
      (SGM58031), à décider au dessin de la carte.
- [ ] P9 — **À mesurer sur la première carte** : tension réelle du module à faible charge,
      et bruit vers 250 kHz (le module découpe à 200–340 kHz, le convertisseur échantillonne
      à 250 kHz ; non chiffrable sur le papier).
- [ ] P10 — Le second fabricant du module n'a que 184 pièces en stock.
