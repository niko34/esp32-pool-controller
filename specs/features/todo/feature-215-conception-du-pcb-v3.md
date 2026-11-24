# feature-215 — Conception du PCB v3

## Métadonnées
- **ID** : feature-215
- **Slug** : conception-du-pcb-v3
- **Type** : conception matérielle (+ étude d'alimentation qui la conditionne)
- **Statut** : todo
- **Priorité** : haute
- **Version cible** : `pcb-v3` (matériel — aucune version firmware visée par cette fiche)
- **Dernière mise à jour** : 2026-11-24
- **Spec(s) liée(s)** : [feature-137](../done/feature-137-perturbation-de-mode-commun-sur-les-electrodes.md)
  (caractérisation du signal), [feature-069](../done/feature-069-investigation-materielle-sonde-ph.md),
  [bug-135](bug-135-butee-brute-ph-se-declenche-sur-une-lecture-isolee.md)

ℹ️ **Fiche UNIQUE du v3** (demande utilisateur du 2026-11-24). Elle réunit la conception de
la carte et l'étude d'alimentation, d'abord écrite seule sous le titre « L'alimentation
secteur est la cause du bruit de mesure ».

⚠️ **Les sources `kicad/` sont gelées sauf demande explicite** (règle 5 de `CLAUDE.md`).
La demande explicite existe pour le v3 (2026-11-23). ⛔ Elle ne couvre **pas** le v2 : ses
sources restent intactes. ⚠️ **À trancher avant le premier fichier** : où vit le v3 dans le
dépôt (dossier neuf à côté du v2, recommandé — le v2 reste fabricable).

## Domaines touchés
- [x] Matériel — schéma, routage, nomenclature, fabrication
- [x] Capteurs (pH / ORP) — ⚠️ le changement de modules impose un pilote neuf, **dans une
      fiche logicielle distincte**
- [ ] Régulation · Filtration · Web · MQTT · OTA · Authentification · Historique · Frontend

## Agents requis
- [x] spec-reader
- [ ] architect — ⛔ aucune implémentation logicielle dans cette fiche
- [x] pool-chemistry — **lecture seule** : les conclusions ne changent rien aux gardes de
      dosage tant qu'aucune mesure n'a bougé
- [x] doc-writer — ⚠️ `kicad/README.md` affiche une hypothèse que la partie B renverse
- [ ] embedded-developer · web-ui-developer · test-engineer · code-reviewer

## Description

Concevoir la troisième version de la carte, fabriquée **et assemblée** chez JLCPCB, pour un
appareil vendu à des clients. L'état de départ est le v2 : 2 couches, 119,4 × 119,4 mm,
93 empreintes dont 87 traversantes, ESP32 sur carte de développement enfichée, horloge sur
module externe (`J11`), une seule barrière d'isolation partagée par les deux voies de mesure.

# PARTIE A — La carte

## Les besoins, tels que demandés (2026-11-23)

1. **Passer aux composants CMS.**
2. **Intégrer l'ESP32 sur le PCB** — ⚠️ module **avec PSRAM obligatoire** (WROVER-E ou
   équivalent) : la mise à jour en ligne en dépend.
3. **Ajouter un port USB** pour la programmation.
4. **Intégrer l'horloge et un support de pile.**
5. **Utiliser les modules Atlas OEM** — [pH OEM](https://atlas-scientific.com/embedded-solutions/ph-oem-circuit/)
   et [ORP OEM](https://atlas-scientific.com/embedded-solutions/orp-oem-circuit/) (D6).
6. **Une carte d'alimentation séparée, alimentée en 230 V**, pour résoudre les perturbations
   de mesure.
7. **Fabrication et assemblage chez JLCPCB, modules Atlas compris.**

## Les décisions prises

| # | Décision | Date | Origine |
|---|---|---|---|
| D1 | **Une barrière d'isolation PAR VOIE** (pH et ORP séparés), sans condition | 2026-11-24 | utilisateur — *« ça ne coûtera pas tellement plus cher »* |
| D2 | **Modules OEM, en I2C** — ⛔ pas d'UART | 2026-11-24 | étude ci-dessous |
| D3 | **Deux bus I2C en sortie de la carte de mesure, réunis en UN SEUL sur la carte principale.** La carte de mesure garde ses huit voies ; la carte principale relie `SDA_PH`↔`SDA_ORP` et `SCL_PH`↔`SCL_ORP`. ⇒ Un seul bus pour l'ESP32 (comme le v2, horloge comprise), et séparer les voies plus tard ne touche **que** la carte principale. ⚠️ Aucun effet attendu sur le bruit de mesure : le bus est du côté ESP32 de la barrière. ⚠️ Conséquence assumée : une voie qui bloque le bus fait perdre les deux mesures | 2026-11-24 | utilisateur, sur conseil |
| D4 | **Mean Well RPS-30-12 conservée** tant que la partie B n'a pas mesuré mieux | 2026-11-23 | utilisateur |
| D5 | **Isolation telle que préconisée par Atlas** : un **ADM3260** par voie, schéma publié | 2026-11-24 | utilisateur |
| D6 | **Module ORP en version OEM** lui aussi | 2026-11-24 | utilisateur |
| D7 | **Carte de mesure SÉPARÉE** de la carte principale | 2026-11-24 | utilisateur — *« intéressant si on veut faire évoluer la carte principale sans toucher à la carte de mesure. C'est la même idée que pour l'alimentation »* |

⚠️ **D4 et le besoin 6 se regardent** : la carte d'alimentation sur mesure n'est pas
abandonnée, elle est **suspendue à l'essai M3**. Si un transformateur 50 Hz ne fait pas
mieux que la RPS-30-12, la « carte d'alimentation » se réduit à porter cette dernière.

## L'ensemble : trois cartes

| Carte | Porte | Évolue |
|---|---|---|
| **Principale** | ESP32, USB, horloge + pile, puissance (pompes, relais), régulateurs | souvent |
| **Mesure** | les deux modules OEM, leurs deux ADM3260, les deux prises de sonde | rarement — c'est le but |
| **Alimentation** | 230 V → 12 V (⛔ contenu suspendu à M3) | rarement |

⭐ **L'intention de D7 fixe la règle de conception** : chaque carte doit pouvoir être refaite
**sans toucher aux autres**. ⇒ Ce qui compte n'est plus seulement chaque carte, c'est **la
liaison entre elles**, qui devient un contrat. ⛔ Une liaison qu'on modifie à chaque version
annule l'intérêt de la séparation.

### La liaison principale ↔ mesure

⭐ **La barrière d'isolation est SUR la carte de mesure.** La liaison ne transporte donc que
le côté non isolé : alimentation, masse, et les deux fils de données de chaque voie — **six
conducteurs**. Signaux numériques lents (100 kHz au plus). ⛔ **Le signal d'électrode ne
quitte jamais la carte de mesure** : quelques millimètres entre la prise de sonde et le module.

À figer **une fois**, et à écrire dans cette fiche avant le premier schéma :
- le connecteur et son brochage ;
- la tension fournie à la carte de mesure, et le courant maximal ;
- la forme de la liaison ;
- ⚠️ où vivent les résistances de rappel du côté non isolé — d'un seul côté, jamais des deux.

#### Proposition retenue (2026-11-24 — l'utilisateur s'en remet au conseil)

- **Un câble court, pas une carte enfichée.** L'enfichage fige la position des barrettes sur
  les deux cartes : c'est un contrat **mécanique** en plus du contrat électrique, qui gênerait
  justement l'évolution de la carte principale — et le boîtier n'est pas encore dessiné.
- **Connecteur à détrompeur et à maintien, pas de 2,5 mm**, de la famille JST XH : répandu,
  câbles tout faits disponibles. ⚠️ Référence exacte à confirmer avec V1 (catalogue JLCPCB).
- **Huit voies** : `+5V` · `+3V3` · `GND` · `GND` · `SDA_PH` · `SCL_PH` · `SDA_ORP` · `SCL_ORP`.
  ✅ **Confirmé sur la documentation de l'ADM3260** (2026-11-24) : convertisseur en 5 V,
  données en 3,3 V, sur deux broches distinctes. ⚠️ Six voies suffiraient avec un seul bus,
  mais **les huit sont conservées** (D3) : c'est la carte principale qui réunit les deux bus,
  pas la carte de mesure.
- **Les résistances de rappel du côté non isolé sont sur la carte de MESURE**, autour de
  l'ADM3260 comme dans le schéma d'Atlas. ⇒ La carte de mesure est complète à elle seule ;
  la carte principale n'en pose aucune sur ces deux bus.
  ⚠️ **Bus réunis (D3) : les rappels des deux voies se retrouvent en parallèle.** Avec
  4,7 kΩ par voie (schéma d'Atlas), cela fait 2,35 kΩ, soit 1,4 mA sous 3,3 V — sous la
  limite I2C de 3 mA. ⚠️ À recalculer avec les rappels de l'horloge si elle en porte.

⚠️ **Risques propres à cette liaison** (examinés le 2026-11-24) :
- **Les connecteurs** en local de piscine, humide et chloré : un point de panne de plus sur
  un appareil vendu. ⇒ Choisir un connecteur verrouillé, pas une barrette nue.
- **Le rayonnement** : l'ADM3260 contient un convertisseur à très haute fréquence ; un câble
  voisin peut faire antenne. Risque de perturbation radio, ⛔ pas de mesure.
- ⛔ **La séparation ne change rien au bruit mesuré en partie B** : il arrive par
  l'alimentation et traverse la barrière de la même façon.

## ⚠️ Perturbations électromagnétiques — IMPORTANCE ÉLEVÉE

**Exigence posée par l'utilisateur le 2026-11-24** : *« le sujet de EMI doit être traité avec
une importance élevée. On a un capteur à haute impédance avec le pH. »*

### Pourquoi la haute impédance rend le sujet central

Une électrode pH se comporte comme une source de tension derrière une résistance énorme
(de l'ordre de la centaine de mégohms). ⇒ **Un courant parasite infime y produit une tension
visible** : 1 nanoampère dans 100 MΩ donne 100 mV, soit plus d'une unité et demie de pH
(59 mV par unité). Tout ce qui injecte un courant, même minuscule, dans le chemin de
l'électrode fausse donc la mesure. Trois voies :

1. **Le couplage capacitif** : toute piste voisine dont la tension varie vite (découpage,
   commande des pompes, données, radio) pousse un courant à travers la capacité qui la
   sépare du signal. C'est la perturbation électromagnétique au sens propre.
2. **Les fuites de surface** : humidité, résidus de flux, poussière créent des chemins
   résistifs sur le circuit imprimé. ⚠️ Ce n'est pas un rayonnement, mais c'est la **même
   faiblesse** et les mêmes parades.
3. **Le mode commun venu du secteur** : celui que mesure la partie B. ⚠️ Voie distincte des
   deux premières — un routage parfait ne la supprime pas.

### Règles de conception — ⛔ aucune n'est facultative

**Tirées de la documentation Atlas (pH OEM, p. 33 à 37)** :
- **Deux plans de masse par module** : la broche 5 (`PRB GND`) au plan de sonde, la broche 6
  (`GND`) au plan numérique.
- **Le plan de sonde ENTOURE le signal** : autour de la piste, sur la face du dessus **et**
  celle du dessous.
- **Piste de signal de 0,4 mm, la plus courte possible**, prise de sonde au plus près.
- ⛔ **Aucun via sous l'empreinte du module.**
- ⛔ **Les broches non connectées restent en l'air** — jamais à l'alimentation ni à la masse.
  Idem pour `INT` si elle ne sert pas.
- ⚠️ **Nettoyage OBLIGATOIRE après soudure** : ultrasons, produit de nettoyage de flux, ou
  bain d'alcool ~20 min. Atlas : *« Failure to do so will result in poor quality readings »*.
  ⇒ **À exiger de JLCPCB, par écrit** (V14).
- ⚠️ **Humidité** : au-delà de 65 %, la mesure est affectée. Atlas préconise d'enrober le
  module dans de la **résine époxy TRANSPARENTE** — ⛔ jamais colorée ni noire, qui
  l'endommage. Un local de piscine est humide ⇒ **à prévoir** (V15).

**Propres à cette carte** :
- **Sur la carte de mesure, rien d'autre que la mesure** : aucun régulateur à découpage,
  aucune commande de puissance, aucune radio.
- **Les deux ADM3260 sont eux-mêmes des sources** : leur convertisseur interne travaille à
  très haute fréquence. ⇒ Suivre la note d'application du fabricant pour le routage
  (condensateurs au plus près, plans), et les tenir à distance des pistes de sonde.
- **Sur la carte principale** : l'antenne de l'ESP32 dégagée et à l'opposé du connecteur de
  la carte de mesure ; commandes des pompes (modulation de largeur d'impulsion) et relais
  loin de ce connecteur ; diodes de roue libre sur toute charge inductive.
- **Le câble entre cartes** : court, masse à côté de chaque paire de données, éloigné des
  câbles de puissance et des câbles de sonde.
- **Les câbles de sonde** : blindés jusqu'à la prise, jamais dans la même goulotte que le
  230 V ou les pompes — ⚠️ consigne d'**installation** à porter dans la notice.

### Comment on le vérifie

- ⛔ **Aucune de ces règles ne se prouve sur le schéma.** La preuve est la mesure : le delta
  pH brut (métrique de la partie B) sur la carte réelle, sonde dans un verre d'eau isolé puis
  en place, pompes à l'arrêt puis en marche, Wi-Fi au repos puis en émission.
- ⚠️ **La conformité réglementaire** (marquage CE, émissions rayonnées) d'un appareil vendu
  est un sujet distinct et réel. ⛔ Hors de ce que cette fiche sait démontrer — à instruire
  avec un laboratoire.

⭐ **Les contraintes de fabrication et le contrôle à la réception vivent dans
[`docs/FABRICATION.md`](../../../docs/FABRICATION.md)**, document de référence tenu en continu — ⛔ pas
dans cette fiche, qui se fermera un jour.

## Étude du 2026-11-24 — UART ou I2C entre les modules Atlas et l'ESP32

**Question posée** : passer en UART pour réduire le prix (mineur) et pour simplifier et
améliorer l'isolation séparée des deux voies.

⛔ **Les modules OEM ne parlent QUE l'I2C.** Vérifié dans leur documentation (V 4.7) :
« SMBus/I2C », 10 à 100 kHz, et leurs dix broches sont `SDA`, `SCL`, `INT`, `VCC`, `GND`,
`PRB`, `PRB GND` et trois non connectées. Aucune liaison série. L'UART n'existe que sur les
modules **EZO** — ceux du v2.

⇒ Le choix n'est pas entre deux bus, il est entre deux **couples** :

| | OEM + I2C | EZO + UART |
|---|---|---|
| Prix du module (à l'unité) | 45,99 $ | 45,99 $ |
| Montage | CMS, prévu pour la pose automatique | broches traversantes |
| Taille | 12 × 11 mm | 14 × 20 mm |
| Consommation | ~3,5 mA | 14 à 18 mA |
| Isolation des données | isolateur I2C | isolateur numérique ordinaire, moins cher |
| Lecture | par **registres** | par commandes texte |

**Ce que l'UART aurait apporté** : un isolateur banal par voie, et aucune ligne partagée
entre les voies. ⛔ **Ce qu'il coûte** : garder les EZO, donc renoncer au CMS et à la pose
par JLCPCB pour ces modules (besoins 1, 5 et 7). Le prix du module est identique, et il faut
de toute façon une alimentation isolée par voie.

⭐ **Le même bénéfice s'obtient en I2C** : l'ESP32 a deux contrôleurs I2C indépendants.
Un bus par voie ⇒ les deux isolateurs ne sont **jamais en parallèle sur un même bus**, donc
aucun risque qu'ils se renvoient mutuellement leur état bas. Aucune ligne n'est partagée
entre les voies, exactement comme en UART.
⚠️ **Dépassé le même jour** : la documentation de l'ADM3260 écarte ce risque, et D3 réunit
finalement les deux bus sur la carte principale. Ce paragraphe reste comme trace de l'étude.

⭐ **Atlas publie son schéma d'isolation pour l'OEM**, autour d'un seul composant,
l'**ADM3260** : il isole les données **et** fournit l'alimentation isolée (150 mW
disponibles ; le module en consomme ~14). Sortie réglée à 3,9 V par un pont diviseur,
résistances de rappel 4,7 kΩ de chaque côté. ⇒ **Un composant par voie**, là où le v2 en
utilise deux pour les deux voies réunies (`U1` ADuM1250 + `U5` TMA-0505S).

## Les deux modules OEM, côte à côte

Les deux documentations ont été lues le 2026-11-24.

| | pH OEM | ORP OEM |
|---|---|---|
| Adresse I2C par défaut | 0x65 | 0x66 |
| Brochage (10 broches) | `SDA` · `VCC` · `PRB` · `PRB GND` · `GND` · `INT` · `SCL` + 3 non connectées | ⭐ **identique** |
| Taille | 12 × 11 mm | 12 × 11 mm |
| Alimentation | 3,0 à 5,5 V | 3,0 à 5,5 V |
| Consommation | ~3,5 mA | ~3,5 mA |
| Bus | I2C, 10 à 100 kHz | I2C, 10 à 100 kHz |
| Étalonnage | 1, 2 ou 3 points | un seul point |
| Compensation de température | oui | sans objet |
| Schéma d'isolation publié | ADM3260, sortie 3,9 V | ⭐ **le même** |

⇒ **Les deux voies sont le même dessin, posé deux fois** : même empreinte, même isolation,
mêmes valeurs. Seuls changent le module posé et le bus auquel il est relié.

### Les consignes de routage d'Atlas (documentation ORP OEM, p. 25 à 27)

- Le module demande **deux plans de masse séparés** (numérique et sonde).
- Le connecteur de sonde **au plus près** du module ; piste de signal de **0,4 mm**, la plus
  courte possible.
- ⚠️ *« Placing the OEM circuits onto their own board is strongly recommended »*, avec *« its
  own power regulator »*. ⇒ **Atlas recommande une carte de mesure séparée.** Voir V10.

## L'isolateur ADM3260 — ce que dit sa documentation

Lue le 2026-11-24, **révision C** (copie hébergée par un revendeur : le site du fabricant
était inaccessible ; ⚠️ une révision plus récente existe — E d'après une recherche du 2026-11-24, la mention « G » notée d'abord n'est pas confirmée — à relire avant de figer le schéma).

| Point | Valeur | Conséquence |
|---|---|---|
| Alimentation du convertisseur (`VIN`) | 3,0 à 5,5 V | broche **distincte** de celle des données |
| Alimentation des données côté ESP32 (`VDDP`) | 3,0 à 5,5 V | en **3,3 V** ⇒ niveaux compatibles avec l'ESP32 |
| Puissance isolée | **150 mW** depuis 5 V · **66 mW** depuis 3,3 V | le module OEM et le côté isolé de l'isolateur demandent ~25 mW : les deux suffisent, le 5 V laisse de la marge |
| Sortie isolée | réglable de 3,15 à 5,25 V, **au plus 20 % au-dessus de `VIN`** | 3,9 V depuis 3,3 V est à la limite (3,96 V) ⇒ **alimenter `VIN` en 5 V** |
| Capacité entre les deux côtés | ⭐ **2,2 pF** (composant entier) | voir ci-dessous |
| Tenue d'isolation | 2 500 V efficaces, 1 minute | |
| Fréquence du convertisseur | **125 MHz** | source de rayonnement, voir V16 |
| Côté ESP32 (« côté 2 ») | niveaux I2C normaux, jusqu'à 400 pF | ⭐ *« multiple ADM3260 devices connected to a bus by their Side 2 pins communicate with each other »* |
| Côté module (« côté 1 ») | niveau bas entre 0,6 et 0,9 V, **40 pF au plus** | pistes **courtes** entre l'isolateur et le module |
| Boîtier | SSOP 20 broches | pose automatique sans difficulté |
| Découplage | 0,1 µF + 10 µF sur `VIN` et sur la sortie, **à moins de 2 mm** | règle de placement ferme |

**Trois enseignements :**

1. ✅ **Le brochage à huit voies du câble est confirmé** : 5 V pour le convertisseur, 3,3 V
   pour les données.
2. ⚠️ **Correction de la raison de D3.** Le fabricant écrit que plusieurs ADM3260 peuvent
   partager un bus du côté ESP32. ⇒ Le risque de blocage mutuel que D3 voulait écarter
   **n'existe pas avec ce composant** : un seul bus fonctionnerait (adresses 0x65 et 0x66),
   avec deux fils de moins. **D3 reste recommandée pour une autre raison** : avec un bus par
   voie, une voie en panne ne bloque pas l'autre. ✅ **Arbitré le 2026-11-24** (D3) : deux
   bus en sortie de la carte de mesure, réunis en un seul sur la carte principale.
3. ⭐ **La capacité de barrière chute fortement.** Le convertisseur isolé du v2 (TMA-0505S)
   annonce **60 pF typique, 100 pF au plus** ; l'ADM3260 annonce **2,2 pF**. Or la partie B
   suppose que le bruit passe par deux capacités en série, celle de l'alimentation puis celle
   de la barrière. ⇒ **Hypothèse à éprouver, ⛔ pas une promesse** : une barrière près de
   trente fois plus faible devrait laisser passer moins de courant parasite. ⚠️ Le cuivre de
   la carte ajoute sa propre capacité — c'est lui qui peut gâcher ce chiffre.

### ⚠️ Un conflit à arbitrer : rayonnement contre capacité de barrière

Le fabricant renvoie à sa note d'application AN-0971 pour limiter le rayonnement, et
recommande un **boîtier relié à la masse**. ⚠️ **De mémoire, non relu** : cette note
préconise notamment de faire **se recouvrir les plans de cuivre des deux côtés** de la
barrière, pour former un condensateur qui referme les courants à haute fréquence.

⛔ **C'est l'inverse de ce que demande la mesure** : ce recouvrement **augmente** la capacité
de barrière, celle-là même qu'on cherche à réduire (enseignement 3).

⇒ **V16 devient un arbitrage, pas une simple lecture** : moins de rayonnement, ou moins de
couplage vers l'électrode. Le choix par défaut de cette fiche est **la mesure d'abord**
(aucun recouvrement), le rayonnement se traitant par le découplage, la compacité et le
boîtier — à réviser après lecture de la note et essai.

## Ce que « une barrière par voie » impose

**Aujourd'hui** : `U1` et `U5` créent **un** domaine `+5V_ISO` / `GND_ISO` que les deux
voies partagent — pH sur `J12`/`J13`, ORP sur `J3`/`J6`. Les deux électrodes sont reliées
entre elles par le cuivre de ce domaine.

**Demain** : deux domaines, ⛔ **sans aucun cuivre commun** — ni plan de masse, ni
condensateur de découplage partagé — et deux zones de garde distinctes sur la carte.

⛔ **Ce que cette barrière ne fait PAS** : elle ne traite que l'interaction **entre les
voies**. Le chemin de mode commun venu du secteur traverse la capacité parasite de
l'alimentation **puis** celle de la barrière ; doubler les barrières ne supprime ni l'une ni
l'autre. ⇒ La partie B garde tout son sens.

## Possibilité : des cartes à quatre couches (2026-11-24)

**Apport de l'utilisateur** : JLCPCB propose les cartes à quatre couches au même prix que les
cartes à deux couches, ce qui faciliterait le routage. ⚠️ **Prix non vérifié ici** : à
constater sur le devis réel, aux dimensions de **chaque** carte (une offre de ce type est en
général bornée en taille) et **assemblage compris**. Voir V17.

⛔ **Ce n'est pas encore une décision** : les **deux options restent ouvertes**, à trancher
carte par carte au moment du schéma.

⭐ **Le critère, posé par l'utilisateur le 2026-11-24 : ⛔ pas le prix.** Quatre couches si la
carte en est **meilleure et plus simple à concevoir** ; deux couches s'il n'y a **aucun
avantage de facilité ni de qualité**. ⇒ Le choix s'écrit avec sa raison, pour chaque carte.

**Ce que cela apporterait**
- **Un plan de masse continu** en couche interne : les retours de courant sont courts, le
  découplage est meilleur, la carte rayonne moins. C'est le premier levier du sujet EMI.
- **Le routage se simplifie** : le v2 a des plans découpés par les pistes ; avec deux couches
  internes, les pistes n'ont plus à traverser les plans.
- **Carte de mesure** : Atlas demande que le plan de sonde entoure le signal **dessus et
  dessous**. Avec quatre couches, le « dessous » peut être à 0,2 mm au lieu de 1,6 mm : le
  blindage de la piste d'électrode est plus serré.
- **Carte principale** : place pour l'ESP32 intégré et son antenne, les MOSFET et le 12 V,
  sans croiser les signaux lents.

**Ce que cela impose — ⚠️ sur la carte de mesure surtout**
- ⛔ **Aucune couche interne ne traverse la barrière d'isolation.** Chaque couche est coupée
  au même endroit que les autres. Deux plans internes qui se recouvrent de part et d'autre
  de la barrière forment un condensateur, d'autant plus fort que les couches sont proches :
  c'est exactement la capacité qu'on cherche à réduire (2,2 pF annoncés par l'ADM3260).
  ⇒ Quatre couches rendent ce défaut **plus facile à commettre** qu'en deux couches.
- ⛔ **Aucun cuivre interne oublié sous la piste d'électrode** qui ne soit le plan de sonde.
- ⛔ **Toujours aucun via sous les modules Atlas** : la consigne ne dépend pas du nombre de
  couches.
- ⚠️ **Les distances d'isolement** valent pour les quatre couches, pas seulement pour les
  deux faces visibles.
- ⚠️ L'arbitrage V16 (rayonnement contre capacité de barrière) se pose avec plus de force :
  la note du fabricant s'appuie justement sur les couches internes.

⇒ **Lecture proposée** : quatre couches sont un gain net pour la carte principale ; pour la
carte de mesure, le gain existe mais dépend du respect strict de la coupure de barrière.

## Points à vérifier AVANT de dessiner

| # | Point | Pourquoi il décide |
|---|---|---|
| V1 | **ADM3260 chez JLCPCB** : disponibilité, prix | ✅ **relevé le 2026-11-24** : au catalogue (C208558), 8 à 16 $ l'unité. ⚠️ **Stock faible, une soixantaine de pièces** — deux par carte. Détail dans [`docs/FABRICATION.md`](../../../docs/FABRICATION.md) |
| V2 | ~~Capacité de barrière de l'ADM3260 contre celle du TMA-0505S~~ | ✅ **relevé le 2026-11-24 : 2,2 pF contre 60 pF typique** — voir « L'isolateur ADM3260 » |
| V3 | **Modules Atlas en pièces fournies** à JLCPCB | ✅ **relevé le 2026-11-24** : service existant, envoi à Hong Kong, douane à notre charge, ⛔ pièces non testées par l'atelier. ⏳ Reste à valider l'étuvage auprès d'Atlas |
| V4 | **Broche `INT` du module** | Atlas demande de l'isoler si on s'en sert ; le module prévoit de s'en passer ⇒ **ne pas la câbler**, sauf besoin démontré |
| V5 | **Bus I2C et horloge** | ✅ Sans objet tant que D3 réunit les deux bus : un seul bus sur GPIO 21/22, horloge comprise, comme le v2. ⚠️ Reste à vérifier la somme des rappels (voir « Proposition retenue ») et que l'horloge ne partage le cuivre d'aucun domaine isolé. ℹ️ Si les voies sont séparées un jour : GPIO 16/17 pris par la PSRAM, et l'ESP32 n'a que deux contrôleurs I2C |
| V6 | **Pont USB-série et remise à zéro automatique** | l'ESP32 retenu n'a pas d'USB natif |
| V7 | **MOSFET de puissance en CMS** (`Q1`–`Q7`, TO-220 aujourd'hui) | dissipation à redimensionner sans dissipateur vertical |
| V8 | **Régulateurs** (`U3` Pololu, `U4` OKI) : modules enfichés à remplacer | dépend de la partie B |
| V9 | ~~Module OEM-ORP : documentation à lire~~ | ✅ **levé le 2026-11-24** — voir « Les deux modules OEM, côte à côte » |
| V10 | ~~Une carte de mesure séparée ?~~ | ✅ **tranché le 2026-11-24 : oui** (D7) |
| V11 | **La liaison principale ↔ mesure** : connecteur, brochage, tension, câble ou enfichage | c'est le contrat qui permet de refaire une carte sans l'autre |
| V12 | ~~Les sondes de température : sur quelle carte ?~~ | ✅ **tranché le 2026-11-24 : carte principale** (utilisateur) |
| V13 | ~~Le boîtier accueille-t-il trois cartes ?~~ | ✅ **oui, et il sera traité APRÈS les cartes** (utilisateur, 2026-11-24). ⚠️ Conséquence : formes et fixations des cartes ne sont contraintes par aucun boîtier existant |
| V14 | **Nettoyage après soudure chez JLCPCB** | ⚠️ **relevé le 2026-11-24** : flux « sans nettoyage », résidu jugé normal par l'atelier, aucune option de nettoyage trouvée. ⏳ À demander par écrit ; à défaut, nettoyage à la réception sur chaque carte |
| V15 | **Enrobage en résine transparente** des modules | protection contre l'humidité ; ⚠️ irréversible, à faire après essai de la carte |
| V16 | **Note d'application AN-0971** de l'ADM3260 | ✅ **lue le 2026-11-24** (révision B). Le recouvrement de plans préconisé fait 140 à 300 pF : ⛔ écarté. Le fabricant prévoit ce cas : là où la fuite doit rester faible, le rayonnement se traite par un **boîtier métallique relié à la masse**. Reste applicable sans toucher à la barrière : quatre couches, plans rapprochés, charge faible. Détail : [`kicad/v3-mesure/README.md`](../../../kicad/v3-mesure/README.md) |
| V17 | **Deux ou quatre couches**, carte par carte | critère = qualité et facilité de conception, ⛔ pas le prix. ✅ **Carte de mesure : quatre couches** (utilisateur, 2026-11-24, sur avis du relecteur — voir [`kicad/v3-mesure/README.md`](../../../kicad/v3-mesure/README.md)). ⏳ Carte principale et carte d'alimentation : à trancher à leur tour |

## Conséquence logicielle — ⛔ hors de cette fiche

Le module OEM se lit par **registres**, pas par les commandes texte que `src/atlas_ezo.cpp`
(316 lignes) envoie aux EZO, et les adresses diffèrent (pH 0x65 contre 0x63, ORP 0x66 contre 0x62). ⇒ Pilote
neuf, **un seul bus I2C conservé** (D3), et passage obligatoire par `pool-chemistry` (la garde
`kEzoBusFailMaxConsecutive` bloque le dosage sur panne de bus). **Fiche logicielle distincte
à créer** quand le matériel est figé. ⚠️ Le firmware devra rester capable de tourner sur le
v2 tant qu'il en existe en service.

# PARTIE B — L'étude d'alimentation

Douze essais matériels menés par l'utilisateur établissent que **le bruit de mesure du pH
vient de l'alimentation secteur du contrôleur**, et qu'il disparaît largement quand la carte
entière est alimentée par batterie. ⛔ Toutes les tentatives d'assainir **un seul rail** ont
échoué. Un essai décisif reste à faire : alimenter **l'ensemble** de la carte par une
alimentation linéaire à transformateur 50 Hz.

### Ce que le dépôt dit aujourd'hui, et pourquoi c'est faux

⚠️ **`kicad/README.md` et `feature-137` attribuent ce bruit aux courants de fuite de la pompe
de FILTRATION.** C'est l'hypothèse de travail du projet « de longue date ».

⛔ **Les essais ci-dessous la rendent très improbable** : remplacer l'alimentation secteur
par une autre alimentation secteur a fait tomber le bruit de 0,52 à 0,30, et la batterie le
fait tomber à 0,09–0,15. La pompe de filtration, elle, n'a pas changé entre ces essais.

⚠️ **Le dépôt affirme donc quelque chose de faux sur le matériel de l'utilisateur**, et c'est
le document que lit toute nouvelle session de travail.

### La métrique

**Delta pH = max − min sur les 13 dernières mesures BRUTES.** C'est la métrique que le
firmware utilise déjà pour détecter une sonde instable.

⚠️ **Ne jamais comparer un delta brut à un delta lissé.** L'erreur a déjà été commise dans
l'analyse de ces essais : la power bank donne **0,09–0,15 en brut** et **0,04 en lissé**.
Comparer 0,30 (brut secteur) à 0,04 (lissé batterie) annonce un gain de 7× au lieu des **2 à
3,3×** réels.

⚠️ **Les essais n'ont pas tous été faits dans des conditions strictement identiques.** Les
comparaisons portent une incertitude que la fiche assume et ne masque pas.

### Les douze essais, tels que relevés

| # | Configuration | Delta pH | Lecture |
|---|---|---:|---|
| E01 | Mean Well **HDR-30-12** (découpage standard) | ~0,52 | référence historique |
| E02 | ⭐ **Toute la carte sur power bank 5 V**, secteur débranché | **0,09–0,15** | ⭐ **meilleur résultat** |
| E03 | Power bank sur le seul rail 5 V, secteur toujours sur le reste | ~0,52 | aucun gain |
| E04 | Convertisseur isolé médical **MDS03F-05** à la place du TMA | 0,96–1,00 | ⛔ **dégradation** |
| E05 | E04 + condensateur céramique 10 µF | 0,96–1,00 | aucun gain |
| E06 | Mean Well **RPS-30-12** (médicale 2×MOPP) | **0,30–0,31** | gain net |
| E07 | Power bank sur le seul rail 5 V, RPS sur le reste | ~0,29 | aucun gain |
| E08 | Linéaire 5 V **en amont** du TMA, RPS sur le reste | 0,30–0,35 | aucun gain |
| E09 | Linéaire 5 V **en aval** du TMA (domaine isolé) | ~0,56 | ⛔ **dégradation** |
| E10 | Retour RPS + OKI + TMA | ~0,30 | référence rétablie |
| E11 | **Sonde** ORP déconnectée | inchangé | aucun effet |
| **E12** | ⏳ **Toute la carte sur une alimentation linéaire complète** | **non mesuré** | ⏳ **à faire** |

### Ce que ces essais établissent

⭐ **1. L'alimentation secteur est le chemin principal.** E01 → E06 : changer d'alimentation
secteur divise le bruit par 1,7 sans toucher à rien d'autre. E02 : le supprimer le divise
par 3 à 5.

⭐ **2. Assainir UN rail ne sert à rien.** E03, E07, E08 et E09 ont tous essayé de nettoyer
une portion de la carte en laissant le secteur sur le reste. **Aucun n'a gagné.** ⇒ Le
couplage est **distribué sur toute la carte**, ⛔ il n'est pas localisé dans la chaîne de
mesure.

⚠️ **3. Une meilleure classe médicale ne garantit rien.** Le MDS03F-05 est un convertisseur
isolé médical, et il a **dégradé** le signal d'un facteur 3 (E04). ⛔ Ne jamais choisir un
composant d'isolation sur sa classification seule.

⚠️ **4. Le découpage n'est pas disqualifié en soi.** Atlas Scientific utilise lui-même des
convertisseurs isolés à découpage sur ses propres cartes d'isolation (RFM-0505S, SI8600,
ADM3260). ⇒ Ce qui compte est la **topologie des chemins de courant parasite**, ⛔ pas la
nature découpage/linéaire prise isolément.

⚠️ **5. L'interaction entre les deux sondes n'est ni prouvée ni écartée.** E11 n'a débranché
que la **sonde** ORP, ⛔ pas le module EZO-ORP, qui continuait d'alimenter et de charger le
domaine isolé partagé.

### Ce qui reste ouvert

⛔ **Le mécanisme exact n'est pas démontré.** L'hypothèse de travail — couplage capacitif de
mode commun entre le secteur, la carte et l'eau — est cohérente avec tout ce qui précède,
mais **aucune mesure électrique directe** (capacité de couplage, courant de mode commun) n'a
été faite. ⚠️ Elle reste une hypothèse, exactement comme l'était celle de la pompe.

### L'essai décisif (« E12 ») — ce qu'il est, en clair

**Alimenter la carte ENTIÈRE avec l'alimentation linéaire construite, la Mean Well totalement
débranchée**, puis mesurer le pH. C'est la seule façon de savoir si un transformateur 50 Hz
reproduit le résultat de la power bank **tout en restant branché au secteur**.

#### Matériel — ⚠️ EXISTENCE À CONFIRMER AVANT TOUTE AUTRE ÉTAPE

⚠️ **L'investigation décrit cette alimentation comme « construite et vérifiée ».
L'utilisateur n'en a PAS la confirmation** (2026-11-23) : il ne retrouve pas le
transformateur et n'est pas certain de l'avoir. ⛔ **Ne rien planifier sur ce matériel tant
que M0 n'a pas tranché.**

Description telle que consignée dans l'investigation :

| Élément | Valeur |
|---|---|
| Transformateur | Myrra **44266**, EI 48 × 16,8, 230 V → 9 V, **10 VA** |
| Tension à vide | 10,8 V AC · courant nominal 1 111 mA AC |
| Pont de diodes | DF04 |
| Lissage | 3 300 µF / 25 V (Panasonic EEUFR1E332) |
| Régulateur | ST **L7805CV** |
| Découplages | 100 nF entrée · 10 µF + 100 nF sortie |
| Cible | ~400 mA DC |

⭐ **Point favorable vérifié sur le datasheet de la gamme** : le transformateur est à
**« double sections bobbins »** — primaire et secondaire sur deux chambres séparées, et non
en enroulement concentrique. Cette construction **réduit intrinsèquement la capacité de
couplage primaire-secondaire**, qui est précisément la grandeur en cause. ⚠️ Elle n'est pas
chiffrée dans le document ; aucun écran électrostatique n'est mentionné.

#### ⚠️ Deux points à lever AVANT la mise sous tension

1. ⛔ **Contradiction sur la protection.** Le datasheet de la gamme EI48×16,8 10 VA porte
   **« Inherently short-circuit proof »**. Le dessin constructeur du 44266 utilisé pendant les
   essais indiquerait l'inverse, avec une protection primaire par fusible **63 mA**. ⚠️ Les
   deux ne peuvent pas être vrais, et c'est **ce qui décide du fusible** (63 mA contre le
   T100 mA envisagé). ⇒ Trancher auprès du fabricant.
2. ⚠️ **Dissipation sous-évaluée.** Le calcul de 2,2 W prend 10,5 V d'entrée. À vide ou en
   faible charge, le secondaire monte à 10,8 V AC, soit **~14,5 V** après redressement : le
   L7805CV dissipe alors près de **3,8 W**. ⇒ Dimensionner le dissipateur sur ce chiffre.

#### Protocole

- ⛔ Mean Well **totalement** débranchée, **+12 V ET masse**.
- OKI-78SR retiré, VIN/VOUT pontés, 5 V linéaire injecté à sa place.
- ⚠️ **Le pont VIN/VOUT doit être retiré avant toute remise sous tension en 12 V.**
- D24V10F3, TMA0505-S, ADuM1250 et les deux EZO **conservés**.
- ⛔ **Aucune charge 12 V** : pompes, filtration, éclairage et sorties de puissance hors jeu.
- ⛔ **Aucun câble USB vers un ordinateur** — il rétablirait un chemin vers le secteur.
- ⛔ **Ne pas relier la masse du secondaire à la terre** pour cet essai.
- Étalonnage et paramètres de mesure **inchangés**.
- Relever : tension secondaire sous charge, continu après filtrage, sortie du L7805CV, rails
  3,3 V et 5V_ISO, courant consommé, température du transformateur et du régulateur.
- ⚠️ Vérifier que l'entrée du L7805CV reste suffisante **pendant les pointes Wi-Fi de l'ESP32**.
- Mesurer **au moins une heure**, noter l'état de la filtration, calculer médiane, maximum et
  95ᵉ centile du delta glissant.

#### Lecture du résultat

| Delta brut | Conclusion |
|---|---|
| 0,09–0,15 | ⭐ équivalent à la batterie — l'architecture du v3 est tranchée |
| 0,15–0,20 | gain net, à confirmer par répétition |
| 0,20–0,25 | gain modéré — répéter avant de décider |
| 0,28–0,35 | ⛔ aucun gain : le transformateur n'est pas la réponse |
| > 0,35 | ⛔ dégradation — chercher l'erreur de montage avant d'interpréter |

⚠️ **Un résultat négatif n'écarterait PAS le mode commun** : un transformateur 50 Hz porte
lui aussi une capacité parasite. Il dirait seulement que **celui-ci** ne suffit pas.

### Le second essai, à cinq minutes

⭐ **Déconnecter le module EZO-ORP ENTIER**, pas seulement sa sonde — puis l'inverse, le
module EZO-pH entier. C'est le seul essai qui tranche si le **domaine isolé partagé** crée
une interaction entre les deux chaînes. E11 ne l'a pas fait.

✅ **L'isolation par voie est décidée (D1)**, quel que soit le résultat de cet essai.

⚠️ **L'essai reste utile, mais il ne décide plus — il MESURE.** Ce qu'il apporte désormais :
- **de combien** le domaine isolé partagé dégrade la mesure, donc ce que la séparation va
  réellement rapporter ;
- ⛔ et surtout : **s'il ne montre rien**, c'est que le chemin de perturbation passe ailleurs.
  Le v3 ne doit alors **pas se croire réglé** parce qu'il a doublé son isolation —
  l'alimentation reste le seul levier connu.

#### Si le matériel n'existe pas — ce qu'il faut vraiment

⭐ **L'essai ne dépend d'AUCUNE référence précise.** Il demande une source 5 V continue
**sans découpage**, capable d'alimenter la carte sans ses charges de puissance.

Trois façons d'y arriver, par ordre de simplicité :

1. ⭐ **Une alimentation de laboratoire LINÉAIRE**, si tu en as une — ⚠️ vérifier qu'elle
   n'est pas à découpage, beaucoup le sont aujourd'hui. **Rien à construire.**
2. **Un bloc secteur ancien à transformateur** (lourd pour sa taille, bourdonne) suivi d'un
   régulateur. De plus en plus rare.
3. **Un transformateur encapsulé + pont + régulateur**, comme décrit ci-dessus.

⚠️ **Le critère de dimensionnement, si tu dois acheter** : un transformateur **9 V d'au moins
6 VA**, 10 VA étant confortable. ⛔ Le 1 VA utilisé pour l'essai partiel (Myrra 44050) **ne
suffit pas** : la carte entière demande environ 400 mA en 5 V, soit 2 W en sortie, et il faut
compter les pertes du redressement et du régulateur linéaire.

## Ce que la partie B décide pour la carte

⛔ **L'architecture d'alimentation n'est pas décidée avant M3.** Deux issues :

1. **Alimentation linéaire pour le domaine logique et mesure**, découpage conservé pour la
   puissance, avec commandes isolées entre les deux. ⚠️ Une masse commune annulerait
   l'isolation.
2. **RPS-30-12 conservée** (D4) si M3 ne gagne rien : le logiciel continue de composer.

⭐ **Le reste de la carte n'attend pas** : CMS, ESP32 intégré, USB, horloge, modules OEM et
barrières par voie sont indépendants de ce choix. Seule la partie alimentation du schéma se
fige après M3.

## Livrables — check-list

**Vérifications avant schéma**
- [ ] V1 à V17 — tableau « Points à vérifier AVANT de dessiner »
- [x] P0 — Où vit le v3 dans le dépôt : ✅ **un dossier neuf à côté du v2** (utilisateur, 2026-11-24). Le v2 (`kicad/esp32-pool-controller/`) reste intact et gelé. Premier dossier créé : `kicad/v3-mesure/` (carte de mesure — composants et connexions, avant schéma)

**Conception**
- [ ] P1 — Schéma : ESP32 intégré, USB, horloge + pile
- [ ] P2 — **Carte de mesure** : deux voies, une barrière et un bus I2C chacune — ⛔ après V11
- [ ] P3 — Schéma : puissance en CMS
- [ ] P4 — Schéma : alimentation — ⛔ **après M3**
- [ ] P5 — Placement et routage, contrôles de règles sans erreur
- [ ] P6 — Dossier de fabrication JLCPCB : gerbers, nomenclature, positions
- [ ] P7 — Carte d'alimentation séparée — ⛔ **après M3**

**Mesures (partie B)**
- [ ] M0 — ⚠️ **Confirmer ce qui existe réellement** : transformateur, pont, condensateur,
      régulateur, dissipateur. ⛔ Préalable à M1, M2 et M3.
- [ ] M1 — Trancher la contradiction « short-circuit proof » et fixer le fusible primaire
- [ ] M2 — Redimensionner le dissipateur sur 3,8 W au pire cas
- [ ] M3 — Essai décisif : carte entière sur alimentation linéaire, ≥ 1 h de mesure
- [ ] M4 — Essai module EZO-ORP entier déconnecté, puis module EZO-pH entier. ⚠️ **Mesure le
      gain attendu de l'isolation par voie — ⛔ il ne la conditionne plus** (D1).
- [ ] M5 — Répéter les trois configurations (RPS · linéaire · power bank) dans des conditions
      comparables

**Documentation**
- [ ] C1 — `kicad/README.md` : remplacer l'hypothèse « pompe de filtration » par l'état réel
- [ ] C2 — `specs/features/done/feature-137-…` : note de renvoi vers cette fiche
- [ ] C3 — Verser les relevés bruts dans `test/fixtures/` comme pour feature-137
- [ ] C4 — Fiche logicielle du pilote OEM créée

## Critères d'acceptation
- [ ] Les points V1 à V17 sont tranchés **par écrit**, avec leur source
- [ ] Les deux voies de mesure n'ont **aucun cuivre commun** côté isolé — vérifié sur la liste
      des connexions, pas à l'œil
- [ ] La liaison principale ↔ mesure est décrite dans cette fiche : connecteur, brochage,
      tension, courant
- [ ] Chaque règle de la section « Perturbations électromagnétiques » est **cochée sur le
      routage réel**, une par une
- [ ] Le delta pH brut est mesuré pompes en marche et Wi-Fi en émission, et consigné
- [ ] Contrôles de règles du schéma et du routage sans erreur, **pour chaque carte**
- [ ] Le dossier de fabrication est accepté par JLCPCB, modules Atlas compris
- [ ] Le delta brut de la configuration linéaire complète est mesuré sur ≥ 1 h et consigné
- [ ] Les configurations sont comparées **en delta BRUT**, jamais brut contre lissé
- [ ] `kicad/README.md` ne présente plus la pompe de filtration comme hypothèse principale
- [ ] L'architecture d'alimentation retenue est écrite, avec la mesure qui la justifie

## Notes pour agent

- ⛔ **Aucune modification de code n'est attendue de cette fiche.** Elle produit une carte,
  des mesures et des décisions matérielles.
- ⚠️ **Sécurité** : toute manipulation touchant le 230 V, la terre ou l'eau se fait avec les
  protections adaptées. ⛔ Ne jamais relier expérimentalement l'eau à la terre ou à la masse
  sans analyse préalable.
- ⚠️ **Régulation pH en MANUEL** pendant toute manipulation de sonde (même préalable que
  feature-069).
- ⚠️ **Une mesure stable n'est pas une mesure exacte.** Le bruit et l'erreur absolue sont deux
  grandeurs distinctes : le 2026-10-16, l'eau était à pH 8 et la sonde lisait 6,9. ⛔ Cette
  fiche traite le **bruit**, elle ne dit rien de l'exactitude.
- ⭐ **Gains attendus côté logiciel si le bruit tombe** : feature-097 pourrait revenir à un
  plancher de purge de 10 min au lieu de 16 min 40 s, et [bug-135](bug-135-butee-brute-ph-se-declenche-sur-une-lecture-isolee.md)
  deviendrait largement sans objet.

## Hors périmètre
- Le pilote logiciel des modules OEM (fiche distincte, livrable C4).
- Le boîtier (sources FreeCAD hors dépôt).
- L'exactitude de la mesure et la correction de décalage pH.
- Le transitoire de reprise de filtration (feature-069), qui reste ouvert et distinct.

## Références
- Atlas Scientific — [carte d'isolation EZO Gen 2](https://atlas-scientific.com/carrier-boards/electrically-isolated-ezo-carrier-board-gen-2/) · [principes d'isolation](https://atlas-scientific.com/electrical-isolation/)
- Analog Devices — [CN0326, mesure pH isolée](https://www.analog.com/en/resources/reference-designs/circuits-from-the-lab/CN0326.html)
- Emerson — [Theory and Practice of pH Measurement](https://www.emerson.com/is/content/emerson/en/measurement-instrumentation/technical/products/liquid-analysis/documents/manual-theory-and-practice-of-ph-measurement.pdf)
- Yokogawa — [sondes pH sans masse de solution](https://www.yokogawa.com/us/library/resources/faqs/can-i-connect-ph-sensors-without-solution-ground-if-yes-how/)
- Mean Well — [RPS-30 (fiche constructeur)](https://www.meanwell.com/Upload/PDF/RPS-30/RPS-30-SPEC.PDF)
- Myrra — [gamme EI48×16,8 10 VA](https://myrra.com/wp-content/uploads/2024/03/44265-44276-EI48-x-16.8-230-V-10-VA-Datasheet.pdf)
- Atlas Scientific — [documentation du pH OEM](https://files.atlas-scientific.com/oem_pH_datasheet.pdf) (V 4.7, lue le 2026-11-24) · [documentation de l'EZO-pH](https://files.atlas-scientific.com/pH_EZO_Datasheet.pdf)
- Atlas Scientific — [documentation de l'ORP OEM](https://files.atlas-scientific.com/ORP_oem_datasheet.pdf) (lue le 2026-11-24)
- Analog Devices — documentation de l'ADM3260, révision C ([copie](https://download.mikroe.com/documents/datasheets/adm3260arsz.pdf), lue le 2026-11-24)
- Traco Power — [gamme TMA](https://tracopower.com/tma-datasheet) (capacité d'isolation 60 pF typique, 100 pF au plus)
