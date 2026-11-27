# Commander une carte v3 chez JLCPCB — étapes et options

Fiche écrite le 2026-11-26, à partir de la commande de la carte principale faite ce jour-là
sur le site (Safari). ✅ = vu à l'écran pendant cette commande. ⏳ = pas encore vu.
⏳ Le déroulé dans l'application JLCONE n'a pas été vu : les noms d'options sont ceux du site.

## 1. Préparer les trois fichiers

```bash
./scripts/build-jlcpcb.sh v3-principale     # ou v3-mesure-std
```

Les fichiers sortent dans `kicad/<carte>/fabrication/` (jamais commités) :

| Fichier | Où le déposer |
|---|---|
| `gerbers-<carte>.zip` | au tout début : « Add gerber file » |
| `bom-<carte>.csv` | après avoir activé « PCB Assembly » : « Add BOM File » |
| `cpl-<carte>.csv` | au même écran : « Add CPL File » |

Puis contrôler le fichier de pose contre les empreintes de JLCPCB :

```bash
python3 scripts/verifier_pose_jlcpcb.py v3-principale     # ou v3-mesure-std
```

- ⛔ en colonne « position » = les trous de la carte ne sont pas ceux de la pièce : ne pas
  commander. Exception connue : U6 (0,82 mm), les pastilles de JLCPCB sont seulement plus
  écartées que celles de KiCad, pour la même pièce.
- ⚠️ en colonne « numéro » : attendu pour D9, SW1 à SW3, J7, U7 et les borniers (JLCPCB ne
  numérote pas leurs pattes comme KiCad). Sur toute autre pièce = pièce tournée.

✅ Carte de mesure : le défaut des prises BNC (pattes percées à 10 mm au lieu de 8) est
corrigé depuis le 2026-11-26 ; le contrôle ne signale plus de ⛔ sur cette carte.

⚠️ Toujours relancer le script avant une commande : les fichiers d'une ancienne session
peuvent dater d'avant une correction.

## 2. Options de la carte nue ✅

| Option | Choix |
|---|---|
| Base Material | FR-4 |
| Layers | **4** |
| Dimensions | lues dans le fichier : 119 × 100 mm (principale), 73 × 54 mm (mesure) |
| PCB Qty | 5 |
| Delivery Format | Single PCB |
| PCB Thickness | 1,6 mm |
| PCB Color | vert |
| Surface Finish | **ENIG**, épaisseur d'or 1 U" |
| Outer Copper Weight | 1 oz |
| Inner Copper Weight | 0,5 oz |
| Via Covering | Plugged |
| Min via hole size/diameter | 0,3 mm |
| Mark on PCB | Remove Mark |
| Confirm Production file | **Yes** (l'atelier envoie ses fichiers à valider avant de graver) |
| Le reste | laissé à la valeur proposée |

## 3. Options d'assemblage (PCB Assembly) ✅

| Option | Choix |
|---|---|
| PCBA Type | **Standard** |
| Assembly Side | Top Side |
| PCBA Qty | 5 |
| Confirm Parts Placement | **Yes**, et cocher « Do not confirm automatically » : rien ne part tant que la pose n'est pas validée à la main |
| Depanel boards & edge rails before delivery | Yes |
| Board Cleaning | voir ci-dessous — **différent selon la carte** |
| Conformal Coating | No |
| Solder Paste | High temp |

**Board Cleaning**

- **Carte principale** : « Caution for Special Parts », avec la remarque :
  `Sensitive parts: U5 (DS18B20), U4 (DS3231), U1 (ESP32 module), SW1-SW3 (tactile switches).`
  (les pièces sont celles signalées le 2026-11-26 ; la phrase exacte saisie ce jour-là n'a pas été gardée)
- **Carte de mesure** : « Solvent-based Cleaning », sans remarque. Le nettoyage du flux y est
  obligatoire (entrées des sondes à très haute impédance).

## 4. Liste des pièces ✅

Chaque ligne doit être cochée, avec la bonne quantité : 39 lignes (principale), 18 (mesure).

- ⚠️ Une ligne à quantité 0, décochée, avec un point d'exclamation = fichier périmé. Relancer
  le script de l'étape 1.
- Le site peut proposer une autre référence pour la même pièce (vu : `C7464026` à la place de
  `C84681` pour le CH340C, même pièce, même boîtier). À accepter seulement si le fabricant et
  le boîtier sont les mêmes.
- Pièces volontairement absentes de la carte principale : H1 à H4 (trous de fixation).
  À mettre à la main : la pile CR2032, et le **capuchon du cavalier JP1** (à acheter à
  part ; sans lui la carte n'a ni 5 V ni 3,3 V).

## 5. Écran de pose — ⚠️ l'étape à ne pas survoler

Le site montre chaque pièce posée sur la carte. M'envoyer une capture avant de valider.

À regarder sur la **carte principale** :

- U6 (régulateur 5 V) : pattes sur leurs deux pastilles, corps dans son cadre (✅ revu après
  la correction de sa position) ;
- J2, J3, J10 (petits borniers verts) : l'écran les montre en boîte sombre à contour rose, sans
  l'image verte des grands borniers. Ils sont bien dans le fichier de pose ; vérifier que leur
  ligne est cochée dans la liste des pièces (référence C8396, 3 par carte) ;
- JP1 : deux broches côte à côte, à l'horizontale, dans leur cadre (✅ vu le 2026-11-26) ;
- sens des diodes D1 à D7 et D9, du condensateur C1, des transistors, de U2, U3, U4, U7 ;
- borniers, J9, J11, J7 et module ESP32 centrés sur leurs trous ou pastilles (✅ revu le
  2026-11-26 après correction du sens des borniers et de J9) ;
- ⚠️ la vue de dessus est en perspective : le dessus d'une pièce haute (borniers de 12 mm,
  condensateur C1) paraît repoussé vers le bord de l'image. Pour juger un bornier, incliner
  la vue 3D et regarder ses pattes dans leurs trous ;
- BT1 (support de pile) : la polarité ne se lit pas à l'écran, d'où la remarque ci-dessous.

À regarder sur la **carte de mesure** — ⏳ jamais vue à cet écran :

- les condensateurs au tantale C104 et C204 : la barre de l'empreinte du côté du « + » ;
- les mots « pH » et « ORP » imprimés à droite de chaque prise (ajoutés le 2026-11-27) ;
- les deux prises BNC : elles doivent pointer hors de la carte, par le bord du bas ;
- J1, le convertisseur et les deux modules d'alimentation isolée (PS101, PS201).

**Support de pile (BT1)** : la patte 1 de l'empreinte de JLCPCB tombe sur la pastille 1 de
la carte (à gauche, « + » de la pile) ; que leur patte 1 soit le « + » est l'usage, pas une
chose lue. Commande du 2026-11-26 passée sans remarque (champ non trouvé, carte d'essai).
⇒ À la réception, **avant de mettre la pile** : vérifier que la lame du dessus est soudée à
la pastille de gauche. Sinon, ne pas mettre de pile : l'heure vient d'internet.

**Remarque d'assemblage à joindre si le champ existe (carte principale)** :

```
BT1 (CR2032 holder): the positive (+) terminal must be soldered on the LEFT pad (net VBAT),
the negative (-) terminal on the RIGHT pad (GND). Please check polarity.
```

## 6. Devis et envoi

- Repère du 2026-11-26, carte principale, 5 pièces : carte nue 46,60 € + assemblage 168,51 €
  = 215,11 €, hors port et taxes.
- Coupons : liés à l'application JLCONE (vus : 5 $ carte 4 couches, 5 $ carte, 20 $
  assemblage au-dessus de 91,02 €). ⏳ Leur cumul n'a pas été vérifié.
- Description du produit (douane) : **« Irrigation Controller – HS Code 903290 »**. À défaut,
  « Others » avec : `Swimming pool controller board (pH and chlorine regulation)`.
- ⏳ Mode de livraison (taxes payées à la commande ou à la réception) : pas tranché.

## 7. Après la commande

1. L'atelier envoie ses fichiers de production : vérifier l'ordre des quatre couches.
2. Il envoie ensuite la pose proposée par le technicien : la relire comme à l'étape 5, puis
   valider.

## 8. À acheter en plus (rien de tout cela n'est dans la commande JLCPCB)

JLCPCB ne soude que la moitié fixée à la carte. Le reste s'achète à part, par exemple chez
LCSC (panier et paiement séparés). Stocks relevés au catalogue le 2026-11-26.

| Pour | Pièce | Référence LCSC | Par carte principale |
|---|---|---|---|
| J2, J3, J10 | fiche à vis 2 points, pas de 3,81 mm — Kangnex WJ15EDGK-3.81-02P, **même marque que l'embase** | `C8466` (112 646) | 3 |
| J1, J4, J5, J6, J12 | fiche à vis 2 points, pas de 5,08 mm — Kefa KF2EDGK-5.08-2P | `C440880` (36 521) | 5 |
| J8 | fiche à vis 3 points, pas de 5,08 mm — Kefa KF2EDGK-5.08-3P | `C440881` (20 921) | 1 |
| JP1 | capuchon de cavalier 2,54 mm, **doré** (la barrette l'est : un capuchon étamé se corrode en air humide et chloré) | `C5305` (981 442) — ⏳ sa finition n'est pas vérifiée, la lire avant d'acheter | 1 |
| J9 | câble JST XH 8 fils, une prise à chaque bout, fils droits (1 sur 1) | tout fait ; ou boîtier `C144407` × 2 + contacts `C140573` × 16 et une pince à sertir | 1 |
| BT1 | pile CR2032 | n'importe où | 1 |
| J11 | nappe 16 fils 2 × 8 au pas de 2,54 mm | seulement avec une sonde de débogage | 0 |

⚠️ **Fiches de 5,08 mm : marque différente de l'embase** (embase Xinya XY2500V, fiche Kefa).
La fiche Xinya au même pas n'a pas été trouvée au catalogue — leur « XY2500F-A-5.0 » est au
pas de 5,0 mm, ⛔ pas la bonne. L'emboîtement Kefa sur Xinya n'est pas vérifié : en prendre
quelques-unes d'abord, ou les essayer à la réception des cartes.
⏳ Non vérifié : le regroupement du port entre LCSC et une commande JLCPCB en cours.

Voir aussi les fiches de chaque carte : [principale](v3-principale/README.md),
[mesure](v3-mesure-std/README.md).
