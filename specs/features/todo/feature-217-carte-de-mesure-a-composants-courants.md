# feature-217 — Carte de mesure à composants courants, sans module Atlas

## Métadonnées
- **ID** : feature-217
- **Slug** : carte-de-mesure-a-composants-courants
- **Type** : feature — ⚠️ **ÉTUDE d'abord** : rien ne se dessine avant la relecture de cette fiche
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : matériel — sans objet tant que l'étude n'a pas conclu
- **Dernière mise à jour** : 2026-11-24
- **Spec(s) liée(s)** : feature-215 (PCB v3, carte de mesure à modules Atlas), ADR-0001 et
  ADR-0003 (ancienne chaîne ADS1115, étalonnage côté ESP32)

## Domaines touchés
- [x] Matériel — carte de mesure
- [x] Capteurs — pilote, étalonnage, correction de température
- [x] Régulation — la mesure alimente le dosage
- [x] Frontend — écrans d'étalonnage
- [ ] Filtration · MQTT · OTA · Authentification · Historique

## Agents requis
- [x] electronics-reviewer — **en premier**, sur cette étude puis sur le schéma
- [x] spec-reader · architect
- [x] pool-chemistry — **obligatoire** : étalonnage et mesure du pH et de l'ORP
- [x] ux-ui-designer — écrans d'étalonnage (avant `web-ui-developer`)
- [x] embedded-developer · web-ui-developer · test-engineer · code-reviewer · doc-writer

## Description

Demande de l'utilisateur (2026-11-24) : *« est-ce qu'il y a la possibilité de remplacer les
modules EZO par un schéma avec des composants plus standards pour qu'ils soient dispo chez
jlcpcb ? Les modules Atlas sont chers. C'est peut-être compliqué […] et cela nécessitera sans
doute du logiciel en plus, mais ça vaut le coup. »*

Chaque module Atlas coûte 45,99 $ et n'est pas au catalogue de JLCPCB : il faut l'acheter
chez Atlas et le leur expédier. L'appareil en porte deux. Cette fiche étudie une carte de
mesure qui fait le même travail avec des composants posés par l'atelier.

## Principe retenu pour l'étude

**Ce que fait un module Atlas** : (1) un amplificateur d'entrée qui lit la sonde sans lui
prendre de courant ; (2) un convertisseur qui en fait un nombre ; (3) un processeur qui
étalonne et corrige en température. **Les deux premiers se refont en composants ; le
troisième passe dans l'ESP32.**

**Une voie, de la sonde au bus** — tout ce qui suit est du côté isolé, derrière le même
ADM3260 que la carte à modules :

```
 âme de la BNC ──[2,2 MΩ]─────► ampli A (suiveur) ──┬──► convertisseur, entrée +
                                                     └──► anneau de garde autour de l'entrée
 corps de la BNC ◄── ampli B (suiveur) ◄── pont de deux résistances sur VISO  (= Vcm)
                                        └──────────────► convertisseur, entrée −
 convertisseur ──I2C──► ADM3260 ──► ESP32
```

- **Vcm** : la sonde délivre une tension positive ou négative (pH : environ ±414 mV ; ORP :
  ±1 020 mV, la plage d'Atlas). Avec une seule alimentation, on décale le tout : le corps de
  la BNC — l'électrode de référence — est tenu à mi-tension par l'ampli B.
- **Mesure différentielle** entre la sortie de l'ampli A et Vcm : la valeur exacte de Vcm
  ne compte pas, donc pas de référence de tension de précision à ajouter.
- **Garde** : un anneau de cuivre autour de l'entrée, tenu au potentiel de la sortie de
  l'ampli A. Toute fuite de surface vers l'entrée part d'un cuivre au même potentiel.

## Composants retenus après relecture (2026-11-24)

Prix et stocks lus chez le distributeur lié à JLCPCB. ⚠️ Le stock de l'atelier
d'assemblage lui-même n'a pas pu être lu. Caractéristiques **lues dans les fiches des
fabricants** par `electronics-reviewer`.

| Rôle | Composant | Référence | Prix à l'unité | Stock | Ce que dit sa fiche |
|---|---|---|---|---|---|
| Entrée de la voie **pH** (ampli A) | LMP7721MAX, TI | C2864387 | 6,69 $ (3,89 $ à l'index JLCPCB) | **224** ⚠️ | courant d'entrée garanti 20 fA à 25 °C, 900 fA à 85 °C ; une patte libre de chaque côté de l'entrée, **à relier à la garde** |
| Suiveur de Vcm (ampli B), et toute la voie **ORP** | LMC6482AIMX, **TI** | **C12620** | 0,58 $ | 54 008 | double ; 20 fA typique, 4 pA garantis en température ; entrée et sortie sur toute la plage d'alimentation |
| Convertisseur | ADS1115IDGSR, TI | C37593 | 1,53 $ | 73 418 | I2C, différentiel, 2 à 5,5 V ; adresses 0x48 / 0x49 |
| Protection de l'entrée | résistance 2,2 MΩ, 0805 | à choisir | < 0,01 $ | — | limite un choc de 8 kV à 3,6 mA ; effet sur la mesure < 1 µV |

**Écartés, avec la raison**
- ⛔ **C2058286 « LMC6482IDT »** : c'est un composant **ST homonyme**, pas celui de TI. Son
  courant d'entrée est garanti à 50 pA (200 pA en température) : 0,085 pH d'erreur avec une
  électrode de 100 MΩ, **0,42 pH** avec une électrode vieillie à 500 MΩ. Cette référence
  figurait dans la première version de cette fiche : **erreur corrigée à la relecture.**
- ⛔ **LMC6062** : demande au moins 4,5 V, la carte fournit 3,85 V.
- ADA4530-1 : 22 $ pour moins de 0,03 pH gagnés. AD8603, LMP91200 : absents du catalogue.
- MCP3421 : une seule adresse I2C.
- ⛔ **Le condensateur sur l'entrée** du premier schéma de principe est retiré : sa fuite
  fausserait la pente (ordre de grandeur de mémoire), et la capacité du câble de la sonde
  fait déjà ce filtrage.
- ⛔ Diodes de protection ajoutées : leur fuite fausse la mesure.

**Erreur de pH due au courant d'entrée** (59 mV par unité de pH) :

| Amplificateur | Électrode de 100 MΩ | Électrode vieillie, 500 MΩ |
|---|---|---|
| LMP7721 | négligeable | 0,0002 pH |
| LMC6482 (TI) | 0,007 pH | 0,034 pH |

**Pourquoi le LMP7721 sur la voie pH, alors que le LMC6482 suffirait sur le papier.** Sur un
amplificateur double ordinaire, la patte d'entrée touche une patte voisine portée à environ
1,9 V de plus ou de moins. Aucun anneau de garde ne passe entre deux pattes : la fuite se
fait sur le boîtier. Pour rester sous 0,01 pH avec 500 MΩ, il faudrait plus de 1,6 TΩ entre
ces deux pattes, en local humide. Le LMP7721 est dessiné pour ce cas. ⇒ Surcoût ≈ 3 à 6 $
par appareil.

**Calibre du convertisseur** : ±1,024 V pour le pH (⛔ pas ±0,512 V : une sonde dont le zéro
a dérivé de 128 mV, ce que celle du bassin a déjà montré, saturerait aux pH extrêmes) ;
±2,048 V pour l'ORP.

| Voie | Bruit du convertisseur | En unités de mesure | Résolution annoncée par Atlas |
|---|---|---|---|
| pH | 31 µV | 0,0005 pH | 0,001 pH |
| ORP | 62 µV | 0,06 mV | 0,1 mV |

⇒ Sur le papier, au moins aussi fin que le module Atlas. Dans les deux cas, c'est la sonde
qui limite.

**Coût, hors isolateur** : voie pH ≈ 6 à 9 $ (LMP7721 + LMC6482 + ADS1115) ; voie ORP ≈ 2,3 $
(LMC6482 + ADS1115). Soit **≈ 8 à 11 $ par appareil, contre 92 $** pour deux modules Atlas.
⇒ **Économie de l'ordre de 80 $ par appareil.**

## Relecture du 2026-11-24 (`electronics-reviewer`) — verdict : **la voie vaut d'être poursuivie**

- ✅ **Le principe est celui que publie Texas Instruments** (note AN-1852, SNOA529A, p. 4) :
  un suiveur tient l'électrode de référence au pont, un second lit l'électrode de verre.
- ✅ Pas de courant à travers l'eau : chaque voie flotte, comme avec les modules Atlas.
- ✅ **L'ORP tient sous 3,85 V** avec le LMC6482 : inutile de monter la tension isolée.
- ✅ Mesure différentielle sans référence de précision : correct.
- ⚠️ **Risque principal : la propreté autour de l'entrée pH**, pas les composants.
- ℹ️ Si l'on filtre Vcm, le condensateur se met sur le pont de résistances, **avant** le
  suiveur B (le LMC6482 n'accepte qu'environ 100 pF en direct sur sa sortie).
- **Ce que le module Atlas fait en plus** : garder l'étalonnage, compenser la température,
  veille, voyant, signal « mesure prête ». ⛔ Aucune détection de sonde absente dans leur
  documentation. Ce qui manquerait vraiment : leur dessin éprouvé de l'entrée.
- **Non vérifié** : le stock de l'atelier ; l'effet du découpage de l'ADM3260 (125 MHz) sur
  ces amplificateurs — **à mesurer sur la première carte** ; la fuite réelle d'un
  condensateur céramique.

## Les questions de départ, et où elles en sont

| # | Question | Pourquoi elle décide |
|---|---|---|
| Q1 ✅ | Courant d'entrée **garanti** de chaque amplificateur, à 25 °C et à chaud | 1 pA dans 100 MΩ = 0,1 mV ≈ 0,002 pH : négligeable. La marge est donc large **si** la carte ne fuit pas |
| Q2 ✅ | Plage d'entrée et de sortie des amplificateurs sous 3,85 V | l'ORP demande Vcm ± 1,02 V : faut-il monter VISO à 5 V ? |
| Q3 ✅ | Adresses I2C | deux ADS1115 sur un bus réuni : 0x48 et 0x49 par la broche ADDR |
| Q4 ✅ | Protection de l'entrée | une résistance série et la diode interne de l'ampli suffisent-elles ? ⛔ pas de diode de protection ajoutée : sa fuite fausserait la mesure (relecture du 2026-11-24) |
| Q5 ✅ | Résolution réelle | 16 bits sur ±512 mV ≈ 15,6 µV ≈ 0,0003 pH : à confirmer contre le bruit du convertisseur |
| Q6 | La garde | comment la dessiner sur quatre couches, et si la BNC métallique la gêne |
| Q7 ✅ | Ce que la version Atlas fait en plus | détection de sonde absente ? filtrage ? à lire dans leur documentation |

## Ce que cela demande au logiciel

Ce que le module Atlas faisait revient dans l'ESP32 :
- pilote du convertisseur (le projet en a eu un pour l'ADS1115, retiré avec la migration) ;
- **étalonnage du pH** en deux ou trois points, pente et zéro, stockés dans l'appareil ;
- **étalonnage de l'ORP** en un point ;
- **correction de température** du pH (loi de Nernst), avec la sonde d'eau déjà présente ;
- filtrage, et détection d'une mesure invalide ;
- écrans d'étalonnage — ceux d'aujourd'hui pilotent le module Atlas.

⚠️ Le firmware doit savoir tourner sur **les deux** cartes : celle à modules Atlas et celle-ci.

## Démarche proposée — ne rien parier

La carte de mesure est séparée de la carte principale (décision D7 de feature-215), et son
connecteur à huit fils est un contrat. ⇒ **Deux versions de la carte de mesure**, même
connecteur, même isolation, même boîtier :

1. la version à modules Atlas (feature-215, déjà dessinée) sert de **référence** ;
2. la version à composants courants se dessine ensuite ;
3. les deux se comparent sur le même bassin, avec la même métrique de bruit que la partie B
   de feature-215.

⚠️ **Ne pas changer deux choses à la fois** : le bruit et le décalage de pH observés
aujourd'hui existent **avec** les modules Atlas. La nouvelle isolation doit d'abord être
jugée seule, sur la carte de référence.

## Livrables — check-list
- [x] E1 — Relecture de cette étude par `electronics-reviewer` : Q1 à Q5 et Q7 tranchées, sources à l'appui ; Q6 (dessin de la garde) reste pour le schéma
- [ ] E2 — Choix des composants arrêté, avec leur disponibilité à l'atelier
- [ ] E3 — Tableau des connexions d'une voie, puis schéma
- [ ] E4 — Carte, sur le modèle de `kicad/v3-mesure/`
- [ ] L1 — Fiche logicielle : pilote, étalonnage, correction de température, écrans
- [ ] M1 — Essai comparé des deux cartes sur le bassin

## Sécurité chimique
- Dosage impliqué : **oui, indirectement** — la mesure du pH et de l'ORP commande le dosage.
- ⚠️ Un étalonnage faux fait doser faux. `pool-chemistry` valide l'étalonnage, ses bornes et
  le comportement en cas de mesure invalide **avant** tout code.

## ADR attendu ?
- **Oui** si l'étude conclut à basculer : c'est un retour sur ADR-0001 (abandon de la chaîne
  analogique au profit des modules Atlas).

## Éléments visibles (pour doc-writer)
- Écrans d'étalonnage du pH et de l'ORP ; documentation utilisateur de l'étalonnage.
- `docs/FABRICATION.md` : le nettoyage après soudure reste obligatoire, pour la même raison.

## Critères d'acceptation
- [ ] Sur le même bassin, la carte à composants courants ne mesure pas plus mal que la carte
      à modules Atlas — seuils à fixer avec la métrique de feature-215
- [ ] Tous ses composants sont posés par l'atelier, sans pièce fournie hors l'ADM3260 si son
      stock l'exige
- [ ] Le firmware tourne sur les deux cartes

## Notes pour agent
- ⛔ Ne pas présenter cette carte comme acquise : c'est une étude. Atlas vend un savoir-faire
  (fuites, garde, étalonnage) que cette carte doit refaire.
- ⚠️ **Leçon de cette fiche** : la première référence relevée pour le LMC6482 était un homonyme
  d'un autre fabricant, dix fois moins bon. ⇒ Toujours lire la fiche technique **de la
  référence exacte du catalogue**, pas celle du nom du composant.
- Contrainte du projet : préférer la solution la plus simple ; chiffrer ce que protège toute
  exigence ajoutée.

## Hors périmètre
- La carte principale et la carte d'alimentation du v3.
- Remplacer l'ADM3260.
