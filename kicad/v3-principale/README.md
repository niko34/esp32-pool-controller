# Carte principale du PCB v3 — composants et connexions

Troisième carte du v3 ([feature-215](../../specs/features/todo/feature-215-conception-du-pcb-v3.md)),
à côté de la carte de mesure ([`../v3-mesure-std/`](../v3-mesure-std/README.md)).
Elle porte l'ESP32, le port USB, l'horloge et sa pile, les sorties de puissance, les
régulateurs et la sonde de température.

⏳ **État au 2026-11-25 : liste des composants et tableau des connexions, version 0.2 —
relus une fois par `electronics-reviewer`, quatre erreurs corrigées (voir « Relecture »).
Schéma rév. 0.1 écrit par script, relu par `electronics-reviewer` : aucune erreur, deux remarques portées.
⏳ Pas encore ouvert dans KiCad par l'utilisateur.
Pas de carte.** Horloge choisie : PCF8563.

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
à celle voulue — 48 fils, 29 broches en l'air (boutons RESET et BOOT compris).

**Le contrôle électrique de KiCad rend un message attendu** : « deux sorties d'alimentation
reliées », entre la broche V3 du pont USB et la sortie du régulateur 3,3 V. C'est le câblage
que demande le fabricant du pont quand il est alimenté en 3,3 V. Les autres messages
viennent des bibliothèques absentes de la machine qui a généré le schéma.

**Ce que le schéma ne contient pas encore**
- ⛔ le régulateur 12 V → 5 V : il attend l'essai M3. Le fil `+5V` n'a donc aucune source ;
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

### Horloge — ✅ **option B retenue : PCF8563** (utilisateur, 2026-11-25)

| | Option A — garder la DS3231 | Option B — PCF8563 |
|---|---|---|
| Composant | DS3231SN (Maxim), SOIC-16 large, C9866 | PCF8563T (NXP), SO-8, C7440 |
| Prix relevé | **7,99 $** | **0,58 $** (0,25 $ pour l'équivalent) |
| Stock | 3 463 | 69 780 |
| Autre fabricant, même empreinte | ⛔ aucun ⇒ exception | ✅ BM8563 (C269877), HYM8563 |
| Précision | ± 2 ppm, soit 5 s par mois | celle du quartz, ± 20 ppm, soit 1 min par mois |
| Pièces en plus | aucune | un quartz 32,768 kHz (C271654), deux diodes pour la pile (BAT54C, C37704) |
| Firmware | inchangé | ⚠️ à adapter : reconnaître la puce présente, pour rester compatible avec le v2 |

Dans les deux cas : support de pile CR2032 CMS (C70377, relevé par le relecteur à l'atelier :
52 896 pièces). La pile n'est pas posée par l'atelier.

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
| Bornier | à vis, pas de 5,0 mm, 2 voies | traversant | C474881 | DG301, KF301 de plusieurs fabricants |

Un condensateur de 10 µF est posé entre `+12V` et la masse au pied de chaque sortie, comme
sur le v2 : sans lui, une pompe à 20 kHz fait onduler le 12 V de 0,6 V.

Charges connues : pompe doseuse 12 V (moteur à courant continu, commandé en impulsions à
20 kHz) et bobine de contacteur 12 V. ⏳ **Courant réel de la pompe à mesurer** : il fixe
la marge du transistor et de la diode.

### Entrée 12 V

| Rôle | Composant | Boîtier | Réf. atelier | Équivalent même empreinte |
|---|---|---|---|---|
| Bornier d'arrivée | à vis, pas de 5,0 mm, 2 voies | traversant | C474881 | plusieurs |
| Fusible réarmable | 2 A, 24 V | 1812 | C434887 | LUTE 1812L200/24GR (C18198341), BHFUSE BSMD1812-200-24V (C883157). ⚠️ Calibre à revoir avec Q4 |
| Anti-inversion | AO4407A, canal P, 30 V | SO-8 | C16072 (AOS) | C2841482 (UMW), SI4435 |
| Protection de grille | Zener 12 V BZT52C12 | SOD-123 | C173429 | plusieurs |
| Écrêteur | SMBJ13A : au repos jusqu'à 13 V, limite à 21,5 V | SMB | C404130 (MDD) | C135042 (Diodes Inc.) |
| Réserve d'énergie | condensateur 220 à 470 µF, 25 V | ⏳ à choisir | ⏳ | — |

### Régulateurs — ⚠️ PROVISOIRE, ⛔ ne se fige qu'après l'essai M3

| Rôle | Composant | Boîtier | Réf. atelier | Équivalent même empreinte |
|---|---|---|---|---|
| 12 V → 5 V | TPS54331DR (TI), à découpage | SOIC-8 | C9865 | ⛔ aucun — exception E4. ⚠️ 3 A, trop pour le câble vers la carte de mesure : viser 1 à 1,5 A au moment de figer |
| 5 V → 3,3 V | AMS1117-3.3 | SOT-223 | C6186 | LD1117 (UMW) |

Hypothèse retenue pour avancer : l'alimentation 12 V actuelle est conservée (D4). Si M3
conduit à une alimentation linéaire séparée pour la logique, ce bloc et l'entrée 12 V
changent ; le reste de la carte ne bouge pas.

### Température et liaison

| Rôle | Composant | Boîtier | Réf. atelier | Équivalent même empreinte |
|---|---|---|---|---|
| Sonde sur la carte | DS18B20+ (Maxim) | TO-92 traversant | C9753 | DS18B20 UMW (C376006) |
| Prise de la sonde d'eau | bornier à vis 3 voies, pas de 5,0 mm | traversant | C474882 | DG301, KF301 de plusieurs fabricants. Comme le v2 : une sonde du commerce arrive en fils nus |
| Prise vers la carte de mesure | JST XH 8 voies | traversant | C157972 | compatibles XH |

## Les exceptions à la règle D8 — à accepter par l'utilisateur

| # | Composant | Pourquoi il n'a pas d'équivalent | Ce que ça risque | Ce qui limite le risque |
|---|---|---|---|---|
| E1 | Module ESP32 | Espressif est le seul fabricant, et le firmware est écrit pour cette puce | rupture ⇒ plus de carte | 5 100 pièces à l'atelier, vendu par tous les grands distributeurs ; ⏳ durée de vie annoncée par Espressif à vérifier |
| E2 | Pont USB CH340C | aucun pont USB n'a de second fabricant sur la même empreinte | rupture ⇒ plus de programmation par USB | 75 000 en stock ; quatre pastilles d'essai (TX, RX, EN, IO0) permettent de programmer sans lui ; les mises à jour passent par le Wi-Fi |
| ~~E3~~ | ~~DS3231~~ — **sans objet, option B retenue** | Maxim seul | 8 $ et 3 400 pièces | l'option B la supprime ; repli DS3231M (C37663), même empreinte, même fabricant |
| E4 | Régulateur 5 V TPS54331 — **provisoire** | TI seul | rupture ⇒ carte à redessiner sur ce bloc | bloc refait de toute façon après M3 |
| E5 | Pile CR2032 | l'atelier ne la pose pas | une pile à insérer à la main sur chaque carte | aucune soudure, pile courante |

## Tableau des connexions — version 0.2

### ESP32 (U1) — broches identiques au v2

| Broche ESP32 | Signal | Va vers |
|---|---|---|
| 3V3 | `+3V3` | sortie du régulateur 3,3 V ; 22 µF + 100 nF au plus près (schéma d'application du module) |
| EN | `EN` | 10 kΩ vers `+3V3`, 1 µF vers la masse, transistor de remise à zéro, bouton RESET vers la masse, pastille d'essai |
| GND et pastille centrale | masse | plan de masse ; une pastille d'essai de masse à côté des quatre autres |
| IO0 | `IO0` | 10 kΩ vers `+3V3`, transistor de remise à zéro, bouton BOOT vers la masse, pastille d'essai |
| TXD0 (IO1) | `TX` | 499 Ω en série (recommandation d'Espressif), puis entrée RXD du pont USB, pastille d'essai |
| RXD0 (IO3) | `RX` | sortie TXD du pont USB, pastille d'essai |
| IO2 | `LED` | 1 kΩ puis LED vers la masse |
| IO5 | `TEMP` | sonde de la carte, prise de la sonde d'eau, 4,7 kΩ vers `+3V3` |
| IO21 | `SDA` | horloge ; voies 1 et 7 de la prise de mesure |
| IO22 | `SCL` | horloge ; voies 2 et 8 de la prise de mesure |
| IO23 | — | en l'air. ℹ️ Sur le v2 elle recevait le signal carré de l'horloge, jamais utilisé par le firmware |
| IO25 | `POMPE_PH` | 100 Ω puis grille du transistor de la pompe pH |
| IO26 | `FILTRATION` | idem, sortie filtration |
| IO27 | `ECLAIRAGE` | idem, sortie éclairage |
| IO32 | `AUX` | idem, sortie auxiliaire |
| IO33 | `POMPE_ORP` | idem, pompe chlore |
| IO35 | `BOUTON` | **10 kΩ vers la masse, bouton vers `+3V3`** — ⚠️ appuyé = niveau haut, c'est ce que lit le firmware (`src/main.cpp:622`). ⛔ ADR-0012 dit l'inverse, à tort |
| IO36 | `RTC_INT` | broche INT de l'horloge, 10 kΩ vers `+3V3` (réservée : le firmware ne la lit pas) |
| IO12 | — | ⛔ en l'air : cette broche fixe la tension de la mémoire au démarrage |
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

### Horloge (PCF8563, SO-8)

| Broche | Signal |
|---|---|
| 1 OSCI, 2 OSCO | quartz 32,768 kHz, 12,5 pF ; condensateur d'accord de 22 pF entre OSCI et `VRTC` (calcul relu : 22 pF + piste, en série avec les 25 pF internes) |
| 3 INT | `RTC_INT` vers IO36, 10 kΩ vers `+3V3` |
| 4 VSS | masse |
| 5 SDA, 6 SCL | bus I2C |
| 7 CLKOUT | en l'air |
| 8 VDD | `VRTC`, 100 nF ; alimenté par `+3V3` **ou** par la pile, à travers les deux diodes du BAT54C. 1 kΩ en série avec la pile : limite le courant si une diode claque (précaution des fabricants de piles) |

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
| Q1 | ~~Horloge~~ ✅ **PCF8563** (utilisateur, 2026-11-25). ⚠️ Conséquence logicielle : le firmware doit reconnaître la puce présente (DS3231 à l'adresse 0x68 sur le v2, PCF8563 à 0x51 sur le v3) — à inscrire dans la fiche logicielle du v3 |
| Q2 | **Résistances de rappel du bus I2C** : la règle actuelle les met toutes sur la carte de mesure. ⚠️ Carte de mesure débranchée, l'horloge ne répond donc plus. Proposition, **approuvée par le relecteur** : ajouter 10 kΩ sur la carte principale (total 1,9 kΩ, soit 1,7 mA, sous la limite de 3 mA). ✅ **Accepté par l'utilisateur le 2026-11-25**, règle corrigée dans feature-215 |
| Q3 | Deux ou quatre couches (V17) — avis du relecteur : **quatre**, même empilement que la carte de mesure |
| Q4 | Courant réel d'une pompe doseuse, et de la bobine du contacteur |
| Q5 | ~~Référence de l'écrêteur et du support de pile~~ ✅ trouvées. Reste le condensateur de réserve du 12 V |
| Q6 | ~~Durée de vie du module~~ ✅ relevé par le relecteur : variante N8R8 active (fiche v2.4) ; Espressif s'engage sur la puce jusqu'en 2031 au moins, ⚠️ pas sur le module |
| Q7 | Bloc régulateurs : attend M3 |
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
3,3 V, 499 Ω sur la ligne d'émission, pastille de masse, exceptions E4 et E5, point Q10.

Non vérifié par le relecteur : fiche du contacteur, déclassement du fusible avec la
température, empreintes compatibles de la prise USB-C et du support de pile.

## Relecture du schéma, 2026-11-25 (`electronics-reviewer`)

Verdict : aucune erreur de brochage ni de câblage, on peut dessiner la carte. Brochages
contrôlés contre les fiches : AO4407A, AO3400A, SS8050, USBLC6, CH340C, PCF8563T, module
ESP32, prise USB-C. Remise à zéro automatique : même montage que la carte de développement.

Deux remarques, portées dans le schéma :
- 1 kΩ en série avec la pile ;
- bornier à vis pour la sonde d'eau, à la place de la prise JST.

Non vérifié par le relecteur : fiche du DS18B20 (site inaccessible), figure de NXP sur le
condensateur d'accord, empreintes du module ESP32 et du support de pile.
ℹ️ Les boutons RESET et BOOT ont été ajoutés pendant cette relecture : ils ne sont pas relus.
