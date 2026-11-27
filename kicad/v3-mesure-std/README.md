# Carte de mesure du PCB v3, à composants courants — composants et connexions

Carte de mesure **retenue pour le v3** (décision D8 de
[feature-215](../../specs/features/todo/feature-215-conception-du-pcb-v3.md), 2026-11-25) :
sans module Atlas, tous les composants posés par l'atelier, chacun avec un équivalent d'un
autre fabricant. L'étude et les relectures sont dans
[feature-217](../../specs/features/todo/feature-217-carte-de-mesure-a-composants-courants.md).

La première étude, à modules Atlas (`kicad/v3-mesure/`), a été **retirée du dépôt le
2026-11-26** : elle reste dans l'historique git (dernier état : commit `5eb6b3d`). Ce qui en
restait valable est recopié plus bas, dans « Repris de l'étude à modules Atlas ».

✅ **État au 2026-11-25 : schéma (rév. 0.2) et carte placée et routée (rév. 0.1) écrits par
script, relus par `electronics-reviewer` — aucune erreur. Pas encore ouverts dans KiCad par
l'utilisateur.**

## Les fichiers de ce dossier

| Fichier | Rôle |
|---|---|
| `generer_schema.py` | écrit le schéma depuis le tableau ci-dessous. ⚠️ Le relancer **réécrit** le schéma en entier |
| `v3-mesure-std.kicad_sch` | le schéma, format A2 : les deux voies à gauche, le connecteur J1 à droite |
| `v3-mesure-std.kicad_sym`, `sym-lib-table` | les symboles propres à la carte |
| `generer_carte.py` | écrit la carte depuis la netlist du schéma : placement, pistes, plans. ⚠️ Le relancer **réécrit** la carte en entier |
| `v3-mesure-std.kicad_pcb` | la carte, 73 × 54 mm, quatre couches |
| `v3-mesure-std.pretty/`, `fp-lib-table` | empreintes propres : module d'alimentation, prise BNC, trou de fixation |
| `v3-mesure-std.kicad_pro`, `v3-mesure-std.kicad_dru` | le projet, ses classes de fils et ses règles d'isolement |

Pour régénérer :
`kicad-cli sch export netlist -o /tmp/n.net v3-mesure-std.kicad_sch` puis
`python3 generer_carte.py /tmp/n.net`.

## La carte (2026-11-25)

Même contour que la carte à modules Atlas : 73 × 54 mm, prises BNC à l'entraxe de 36,3 mm
sur le bord du bas, J1 en haut, quatre trous M3. Couches : composants et signaux dessus,
masse au-dessous, alimentation, puis masse et quelques pistes sur la face du dessous.

**La barrière d'isolation**
- Zone interdite au cuivre de **3,0 mm** sur les quatre couches entre le côté ESP32 et chaque
  voie : c'est la distance entre les deux rangées de pastilles de l'ISO1540.
- Au droit du module d'alimentation elle se réduit à un col : ses broches 2 et 3 sont au pas
  de 2,54 mm, soit **0,94 mm de cuivre à cuivre**, que rien ne peut élargir.
- Entre la voie pH et la voie ORP : 3,0 mm.
- ℹ️ La carte à modules Atlas avait 5 mm partout (imposés par l'ADM3260). Ici la barrière
  est **fonctionnelle** : elle ne voit que quelques volts, la sécurité secteur vient de
  l'alimentation (D4). 1 500 V est l'essai d'une minute du module. Relu et jugé cohérent.

**L'entrée de la sonde**
- pH : anneau de garde sur le dessus, relié aux broches 2 et 7 de U104, qui entoure l'âme de
  la prise, R105 et la broche 1 ; garde en plein sur les trois autres couches, sous l'entrée.
  Piste d'entrée de 2,1 mm, piste de l'âme de 6,1 mm.
- ORP : même anneau, relié à `MESURE_ORP`, refermé sous le boîtier entre la broche 4 (masse)
  et la broche 5 (entrée).
- **Vernis conservé** sur l'anneau et l'entrée (point P6 fermé). Le retirer sert aux mesures
  au femtoampère. Ici la garde est la sortie du suiveur, à 150 µV au plus de
  l'entrée : même avec 1 GΩ de fuite de surface, l'erreur est de 0,0003 pH. Le retirer
  laisserait du cuivre nu dans l'air chloré.
- Le trait d'encre de la prise BNC qui passait sur l'âme et l'anneau est retiré de l'empreinte.
- Le corps de la prise est relié par une zone de cuivre `REF` sur le dessus.

**Deux pistes passent par la face du dessous** : SCL côté isolé, sur 3 mm, pour croiser
SDA ; et, sur la voie ORP, la liaison de R208 au corps de la prise. Sans effet sur la mesure.

### Ce qui est vérifié
- Contrôle des règles (`kicad-cli pcb drc --severity-all --schematic-parity`) : 0 violation,
  0 liaison manquante, 0 écart avec le schéma. Les seuls messages portent sur des
  bibliothèques absentes de la machine de génération.
- Les protections ont été éprouvées par des fautes volontaires : via dans la barrière,
  condensateur non relié, piste qui franchit la barrière, court-circuit masse / entrée du pH
  — toutes détectées. ℹ️ Un plan de cuivre débordant sur la séparation des voies n'est pas
  signalé : la zone interdite le rogne d'elle-même.
- Relecture du tracé : découplages à 1,2 à 2 mm de leur broche ; régulateur à +7 à +13 °C ;
  module d'alimentation à 13,7 mm de l'entrée du pH ; pistes, écarts et vias dans les
  capacités de l'atelier ; rien n'est posé sur la face du dessous.

### Ce qui n'est PAS vérifié
- L'ouverture de la carte dans le KiCad de l'utilisateur.
- **L'empreinte du module d'alimentation face à la pièce réelle** : dessinée d'après le plan
  d'implantation des fiches Evisun et Ylptec (corps de 11,7 × 6 mm d'un seul côté de la
  rangée de broches, broche 1 à 2,03 mm du bout). Le relecteur n'a pas pu afficher le dessin.
  Si le corps était de l'autre côté, il reposerait sur C101.
- L'empreinte de la prise BNC face au plan du fabricant (déjà ouvert sur la carte Atlas).
- L'empilement des couches de l'atelier.

## Repris de l'étude à modules Atlas (2026-11-24)

**Disposition.** La carte est montée à l'horizontale et les deux prises BNC, **coudées**,
sortent par le grand bord du bas. Entraxe de **36,3 mm** : celui des deux perçages que le
boîtier avait déjà. Le connecteur J1 et le côté ESP32 sont à l'opposé, en haut.

**La prise** : `C521219`, dosinconn DOSIN-801-0080, embase BNC femelle coudée, traversante.
Plan du fabricant (révision A) lu le 2026-11-24 :

| Ce que dit le plan | Valeur |
|---|---|
| Corps | **laiton nickelé, donc métallique** ; isolant en téflon |
| Fixation | nez fileté **M12 × 0,75**, avec écrou et joint : la prise se serre sur la paroi |
| Pattes de corps | 4, Ø 1,2 mm, au pas de **8 mm × 8 mm** ; le « 10 » du plan est la largeur du corps carré, pas l'écart des pattes |

⛔ **Défaut ouvert le 2026-11-26 : l'empreinte de la carte a ces pattes à 10 mm en travers**
(lecture fausse du plan le 2026-11-24 : « 8 × 10 »). Relu ce jour sur le plan, à l'échelle
de la cote de 29,5 mm : 7,8 mm dans les deux sens ; l'empreinte de JLCPCB pour `C521219` dit
8 × 8 aussi. Avec des trous de 1,5 mm pour des pattes de 1,2 mm, 1 mm d'erreur par patte ne
passe pas : **la carte est à corriger avant toute commande.**
| Âme | Ø 1 mm, au centre des quatre pattes |
| Hauteur de l'axe au-dessus de la carte | 9,8 mm |
| Longueur totale | 29,5 mm |

⚠️ **La paroi du bas doit être isolante.** Le corps de la prise est en métal et se serre sur
la paroi : sur une paroi métallique reliée à la terre, la référence des sondes serait reliée
à la terre et les deux voies entre elles, l'isolation serait annulée. ✅ Le boîtier est en
ASA, paroi de 3,2 mm, perçages de Ø 13 mm.

✅ **Défaut des pattes fermé le 2026-11-26** : empreinte corrigée à 8 × 8 mm, carte régénérée ;
le contrôle patte par patte contre l'empreinte de JLCPCB donne 0,00 mm d'écart. Les quatre
pattes sont maintenant reliées **en plein** à leur plan (plus la place pour deux rayons).

⚠️ **Mesuré sur le dessin, pas coté — à vérifier pièce en main** : le contour du corps, la
distance de l'âme à la paroi (≈ 6,7 mm) et la longueur du filetage (≈ 7,2 mm, pour
3,2 mm de paroi + 0,8 mm de joint + 2,5 mm d'écrou : le serrage tient, de peu). Perçages de
l'empreinte choisis ici : 1,5 mm pour les pattes, 1,3 mm pour l'âme.

**Pourquoi quatre couches** (utilisateur, 2026-11-24, sur avis du relecteur) : le critère est
la qualité de la mesure, pas le prix. La note d'application AN-0971 d'Analog Devices mesure le
bruit sur l'alimentation derrière un convertisseur isolé : 460 mV sans plan, 104 mV puis
64 mV avec des plans rapprochés (ordres de grandeur, relevés sur une autre famille de
composants). La même note conseille un condensateur entre les deux côtés de la barrière
(140 à 300 pF) : ⛔ écarté, c'est le chemin du bruit que cette carte doit fermer.

**Nettoyage du flux obligatoire après soudure** : voir
[`docs/FABRICATION.md`](../../docs/FABRICATION.md).

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
| U104 | Amplificateur d'entrée **pH** | LMP7721MAX (TI) | SOIC-8, **brochage spécial** | C2864387 | aucun broche à broche. Avant le 2026-11-26 : OPA928DR (C32418779, 24 $) — voir « Changement d'amplificateur » |
| U105, U205 | Amplificateur double : Vcm, et entrée **ORP** sur U205 | LMC6482AIMX (**TI**) | SOIC-8 | C12620 | LMC6482IDT de ST (C2058286) |
| R101, R102, R201, R202 | Rappels du bus, côté ESP32 | 4,7 kΩ 1 % | 0603 | C23162 | — |
| R103, R104, R203, R204 | Rappels du bus, côté isolé | 3,3 kΩ 1 % | 0603 | C22978 | — |
| R105, R205 | Protection de l'entrée, en série avec l'âme | 2,2 MΩ 1 % | 0805 | C26113 | — |
| R106, R206 | Pont de Vcm, haut | pH : **30 kΩ** (C22984) · ORP : **10 kΩ** (C25804) | 0603 | voir valeur | — |
| R108, R208 | Entre le suiveur de Vcm et le corps de la prise | 100 Ω 1 % | 0603 | C22775 | — |
| R107, R207 | Pont de Vcm, bas | 10 kΩ 1 % | 0603 | C25804 | — |
| C101, C201 | Entrée du module | 4,7 µF 25 V | 0805 | C1779 | — |
| C103, C203 | Sortie du module et entrée du régulateur | 10 µF **50 V** | **1206** | C13585 | — |
| C104, C204 | Sortie du régulateur | 22 µF 16 V, **tantale** | boîtier B | C8020 (AVX TAJB226K016RNJ) | CA45-B016K226T (Xiangyee, C122226) |
| C105, C205 | Découplage de l'isolateur, côté ESP32 | 0,1 µF | 0402 | C1525 | — |
| C106, C206 | Découplage de l'isolateur, côté isolé | 0,1 µF | 0402 | C1525 | — |
| C107, C207 | Découplage du convertisseur | 0,1 µF | 0402 | C1525 | — |
| C108 | Découplage de l'amplificateur d'entrée pH | 0,1 µF | 0402 | C1525 | — |
| C109, C209 | Découplage de l'amplificateur double | 0,1 µF | 0402 | C1525 | — |
| C110, C210 | Filtre du pont de Vcm | 0,1 µF | 0402 | C1525 | — |
| J101, J201 | Connecteur de sonde | BNC coudée DOSIN-801-0080 | traversant | C521219 | — (inchangé) |
| J1 | Connecteur vers la carte principale | JST XH 8 voies (B8B-XH-A) | traversant | C157972 | — (inchangé) |

**Tensions**
- Alimentation isolée : **5 V**, choisie pour l'OPA928 d'origine (4,5 V minimum) et la marge
  des niveaux du bus côté isolé ; le LMP7721 l'accepte (5,5 V au plus). ⚠️ Marge de 0,5 V :
  ne pas remplacer le régulateur par un modèle à sortie plus haute.
- Vcm du pH : **1,25 V** (pont 30 kΩ / 10 kΩ). LMP7721 : dans sa plage (−0,3 à 4 V). Les deux
  lignes suivantes datent de l'OPA928 et de son remplaçant d'alors.
  L'entrée balaie 0,7 à 1,8 V, dérive du zéro de 128 mV comprise. OPA928 : dans sa plage
  garantie (0,1 à 2,0 V). ADA4530-1 : sur 0 à 3 V sa fiche garantit décalage ≤ 300 µV,
  réjection 73 dB, écart de garde ≤ 150 µV ; pire cas 0,12 mV, soit **0,002 pH**.
  ⇒ Le remplacement se fait sur la même empreinte, **sans rien changer d'autre**.
- Vcm de l'ORP : **2,5 V** (pont 10 kΩ / 10 kΩ). L'entrée balaie 1,48 à 3,52 V.
- Adresses I2C : convertisseur du pH en **0x48**, de l'ORP en **0x49** — les deux bus sont
  réunis sur la carte principale (D3).

## Tableau des connexions

Signaux : `+5V`, `+3V3`, `GND` viennent de la carte principale. Du côté isolé de la voie pH :
`VBRUT_PH` (sortie du module), `VISO_PH` (5 V régulé), `GNDISO_PH`, `VCM_PH` (sortie du suiveur), `REF_PH` (corps de la prise, après les 100 Ω).
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

### Amplificateur d'entrée du pH (U104, LMP7721 — voie pH seulement)

| Broche | Nom | Relié à | Remarque |
|---|---|---|---|
| 1 | IN+ | `ENTREE_PH` : R105, dont l'autre bout va à l'âme de J101 | ⚠️ **le point sensible** : piste la plus courte possible, entourée de l'anneau de garde |
| 2, 7 | libres dans le boîtier | `MESURE_PH` : l'anneau de garde | la fiche recommande de les relier à la garde ; la garde est tenue par la sortie |
| 3 | V− | `GNDISO_PH` | sort sous le boîtier, via hors du plan de garde |
| 4 | OUT | `MESURE_PH` | vers AIN0 de U103, et vers l'anneau de garde |
| 5 | — | **rien** | libre dans le boîtier |
| 6 | V+ | `VISO_PH` + C108 vers `GNDISO_PH` | |
| 8 | IN− | `MESURE_PH` | montage suiveur |

#### Changement d'amplificateur (2026-11-26, décision de l'utilisateur)

L'OPA928 coûtait 24,36 $ pièce (121 $ pour cinq cartes). Remplacé par le **LMP7721** (6,63 $).
Lu ce jour dans sa fiche (TI SNOSAW6E) :

| Point | Valeur de la fiche | Ce qu'il faut ici |
|---|---|---|
| Courant d'entrée | ±3 fA typique, ±20 fA au plus à 25 °C, ±900 fA de −40 à 85 °C | sonde de 500 MΩ : 0,0002 pH à 25 °C, 0,008 pH au pire à 85 °C |
| Alimentation | 1,8 à 5,5 V (6,0 V à ne pas dépasser) | 5 V isolé |
| Plage d'entrée à 5 V | −0,3 à 4 V | l'entrée balaie 0,7 à 1,8 V |
| Décalage | ±150 µV au plus | absorbé par l'étalonnage |
| Écart entre les deux entrées | ±0,3 V à ne pas dépasser | suiveur : moins de 1 mV ; R105 limite le courant |
| Brochage | 1 IN+, 2 libre, 3 V−, 4 sortie, 5 libre, 6 V+, 7 libre, 8 IN− | carte redessinée autour de U104 |

Ce que l'OPA928 avait en plus : une sortie de garde propre au composant. Ici la garde est la
sortie du suiveur, comme sur la voie ORP. ⇒ Le net `GARDE_PH` n'existe plus.
⛔ L'ADA4530-1 n'est **plus** un remplaçant direct : il a le brochage de l'OPA928.
✅ Relu par `electronics-reviewer` le 2026-11-26 : brochage et limites conformes, bon pour commande.
- ⚠️ **À regarder sur la première carte : la stabilité.** La sortie tient la garde (anneau et
  trois plans), estimée à 15–30 pF ; la fiche (fig. 19) donne alors environ 35° de marge de
  phase, et 4° vers 200 pF. Si une oscillation ou un bruit anormal apparaît : séparer la
  garde de `MESURE_PH` et l'y relier par 100 à 470 Ω (recommandation de TI, p. 20-21).
- La garde suit l'entrée à 150 µV près à 25 °C, 450 µV sur toute la plage de température.
- ⚠️ **Un seul fabricant** (TI), aucune pièce ne se pose sur la même empreinte — écart assumé
  à la règle des deux fabricants. Repli : la carte de l'OPA928, dans l'historique git.
- Les pattes de corps des prises sont maintenant à 0,53 mm de l'anneau de garde (1,4 mm
  avant) : à garder en tête pour le point P7 (décharges sur la prise).
⏳ Non fait : mesure du bruit sur carte réelle.
L'utilisateur fera d'abord ses essais sur l'alimentation (par exemple sur batterie 5 V) ;
si la mesure y est bonne, l'OPA928 n'aura pas lieu de revenir.

### Amplificateur double (U105 pour le pH, U205 pour l'ORP — LMC6482)

| Broche | Nom | Voie pH (U105) | Voie ORP (U205) |
|---|---|---|---|
| 1 | OUT A | `VCM_PH` | `VCM_ORP` |
| 2 | IN− A | `VCM_PH` (suiveur) | `VCM_ORP` (suiveur) |
| 3 | IN+ A | `VCM_REF_PH` : milieu du pont R106 / R107, + C110 vers `GNDISO_PH` | `VCM_REF_ORP` : idem avec R206 / R207, C210 |
| 4 | V− | `GNDISO_PH` | `GNDISO_ORP` |
| 5 | IN+ B | `VCM_REF_PH` (moitié inutilisée, montée en suiveur) | `ENTREE_ORP` : R205, dont l'autre bout va à l'âme de J201 |
| 6 | IN− B | relié à la broche 7 | `MESURE_ORP` (suiveur) |
| 7 | OUT B | relié à la broche 6, rien d'autre | `MESURE_ORP`, vers AIN0 de U203 |
| 8 | V+ | `VISO_PH` + C109 vers `GNDISO_PH` | `VISO_ORP` + C209 vers `GNDISO_ORP` |

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
- [x] P6 — Garde autour de l'entrée : dessinée et relue, vernis conservé (voir « La carte »).
- [ ] P7 — Isolement du corps de la prise BNC, et tenue aux décharges quand on la touche
      (le LMC6482 tient 1,5 kV ; les 100 Ω ne règlent pas ce point).
- [x] P8 — Référence au catalogue du condensateur au tantale (22 µF 16 V, boîtier B) : ✅ C8020, le 2026-11-26. Avant : non
      trouvée. Les autres passifs sont relevés ; ⚠️ leur stock se lit à l'atelier, pas chez
      le distributeur (une résistance courante y figure à 0).
- [ ] P11 — Empreinte du convertisseur : prévoir la pastille centrale de son équivalent
      (SGM58031), à décider au dessin de la carte.
- [ ] P9 — **À mesurer sur la première carte** : tension réelle du module à faible charge,
      et bruit vers 250 kHz (le module découpe à 200–340 kHz, le convertisseur échantillonne
      à 250 kHz ; non chiffrable sur le papier).
- [ ] P10 — Le second fabricant du module n'a que 184 pièces en stock.

## Dossier de fabrication JLCPCB (2026-11-26)

`./scripts/build-jlcpcb.sh v3-mesure-std` écrit `fabrication/` (ignoré par git, régénérable) :

| Fichier | Contenu |
|---|---|
| `gerbers-v3-mesure-std.zip` | gravure : 4 couches de cuivre, vernis, pâte, sérigraphie, contour, perçages |
| `bom-v3-mesure-std.csv` | liste des pièces au format JLCPCB (valeur, repères, empreinte, référence LCSC) |
| `cpl-v3-mesure-std.csv` | position et angle de chaque pièce à poser |

Le script refuse de produire quoi que ce soit si le contrôle des règles signale une erreur.

✅ Vérifié le 2026-11-26 : le dossier sort sans erreur, 47 pièces à poser en 18 lignes (une par référence). C104 et C204 (22 µF 16 V au tantale) : référence trouvée le 2026-11-26 et confirmée par l'utilisateur, C8020 (AVX TAJB226K016RNJ, 55 997 en stock chez JLCPCB), équivalent C122226 (Xiangyee). Le 10 V a été écarté : sur 5 V il serait à la limite de la règle des fabricants (moitié de la tension nominale).
✅ Stock relevé au catalogue de JLCPCB le 2026-11-26 : toutes les références y sont, plus bas relevés : **LMP7721MAX 1 668** (relevé après le changement d'amplificateur), module d'alimentation B0509S 1 493, prise BNC 6 096. Environ la moitié des références sont des pièces « étendues » (frais de mise en place par référence).
⏳ Non vérifié : l'envoi réel des fichiers sur le site de JLCPCB.
⚠️ **Étape humaine à la commande** : JLCPCB ne compte pas les angles comme KiCad pour certains
boîtiers. Son écran de pose montre chaque pièce sur la carte : y vérifier le sens des diodes,
des transistors, des circuits intégrés et du condensateur polarisé avant de valider.

## Audit extérieur du 2026-11-27 (demandé par l'utilisateur)

Verdict : rien de bloquant. Déjà connus : charge minimale des modules isolés (≈ 10 mA tirés
pour 11 à 12 demandés ; résistance de charge écartée à la relecture du 2026-11-25, à
trancher par les essais d'alimentation), stabilité du LMP7721 sur sa garde, tenue des prises
aux décharges.
Nouveau : « pH » et « ORP » n'étaient écrits que sur la couche de commentaires, donc non
imprimés. ✅ Ajoutés le jour même en sérigraphie, à droite de chaque prise (`generer_carte.py`).
Rien d'autre n'a changé sur la carte (comparaison pièce par pièce et piste par piste).
