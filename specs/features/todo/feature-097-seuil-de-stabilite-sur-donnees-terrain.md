## Métadonnées
- **ID** : feature-097
- **Slug** : seuil-de-stabilite-sur-donnees-terrain
- **Type** : feature (réglage sur données — condition 10 du gate pool-chemistry v2.34.0)
- **Statut** : todo — **ne pas ouvrir avant 5 matins de télémétrie** (au plus tôt le 10/09/2026)
- **Priorité** : moyenne
- **Version cible** : 2.36+
- **Dernière mise à jour** : 2026-09-04
- **Spec(s) liée(s)** : feature-091 (purge adaptative, done en v2.34.0), plan `specs/plan-2026-09-regulation-et-robustesse.md` (O1/O2, journal 19:10 et 20:50), ADR-0059

## Domaines touchés
- [x] Capteurs (pH / ORP / température) — détecteur de stabilité `purge_stability_logic`
- [x] Régulation (pump_controller, PID) — indirect : la porte de mesure conditionne le dosage
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **obligatoire** : le détecteur conditionne le premier dosage du matin
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer — rejeux natifs + mutation, comme pour 091
- [x] code-reviewer
- [x] doc-writer

## Description
Le détecteur de stabilité de la purge (feature-091) juge l'eau stable quand
l'étendue (max − min) de 10 médianes par minute du pH brut est ≤ 0,15 (et
≤ 15 mV en ORP). Sur bruit synthétique uniforme ±0,18 sur les lectures 5 s,
l'étendue moyenne de 10 médianes vaut ≈ 0,16 : **le seuil est au niveau du
bruit**. Sur une eau parfaitement plate, la porte s'ouvre donc au plancher
(10 min) une fois sur deux, et jusqu'à ~30 min dans la queue de distribution
(12/21 graines à t+10, 5 à t+13-19, 4 à t+22-29). Rejeu du 04/09 bruité :
ouverture t+47 à t+62 pour un plateau atteint à t+39 (une queue à t+83).

Ce n'est **pas** un risque chimique (erreur fail-closed : perte de temps de
régulation, jamais de dosage sur eau de rinçage ; plafond 90 min + alerte), mais
c'est du temps de régulation perdu le matin — l'objectif O2 du plan.

Cette spec ajuste le détecteur **sur les durées de purge réelles** mesurées par
la télémétrie livrée en v2.34.0, pas sur du bruit synthétique.

## Contexte
- Le bruit réel des lectures pH est probablement moins pessimiste qu'un uniforme
  ±0,18 (médiane de 12 lectures gaussiennes plus resserrée) — à vérifier sur
  `/debug/sensor_trace` (anneau 5 s, 25 min).
- Options déjà évaluées le 04/09 (pool-chemistry) :
  - **(b) seuil 0,20 — REFUSÉE** : la marge sur le fond en V du rinçage retombe
    à zéro (4,5 × 0,045 = 0,20), on rouvrirait la faille qui a imposé W = 10.
  - **(c) médianes sur 2 min** (24 lectures, W = 5 blocs, même fenêtre 10 min) :
    σ de la médiane /√2 → étendue ≈ 0,085 ≪ 0,15, mais marge V réduite (2 blocs
    de chaque côté = 4 min × 0,045 = 0,18 pour 0,15). **Piste privilégiée**, à
    décider sur données réelles.
  - Autres pistes ouvertes à l'architect : statistique plus robuste que l'étendue
    (écart interquartile des médianes, régression linéaire pente + résidu),
    seuil pH dérivé du bruit mesuré en direct (dispersion feature-071).

## Comportement attendu
- Sur une eau plate avec le bruit réel du bassin, la porte s'ouvre au plancher
  dans **≥ 90 % des matins** (au lieu de ~50 % estimés) ; queue ≤ plancher + 5 min.
- Le fond en V du rinçage (descente −0,047 / remontée +0,043 pH/min le 04/09)
  reste détecté avec une marge ≥ 0,03 pH sur le seuil (rejeu natif 04/09 : aucune
  ouverture avant le plateau, sans bruit ET avec bruit sur 20 graines).
- La re-fermeture C1 (2× seuil) garde une pente de déclenchement ≤ 0,035 pH/min.
- Aucun changement d'interface (mêmes champs `/data`/WS, mêmes libellés).

### Cas limites
- Sonde bruyante (dispersion > 0,5) : la porte ne doit pas s'ouvrir plus tôt
  qu'aujourd'hui — un assouplissement du seuil ne doit jamais rendre stable une
  mesure que 091 jugeait instable sur le fond du V.
- ORP : critère secondaire, ne pas le durcir (1,5 mV/min au rinçage → 13,5 mV).

## Comportement actuel
`kPurgeWindowMin = 10`, `kPurgeSamplesPerMin = 12`, `kPurgeStablePhRange = 0,15`,
`kPurgeStableOrpRange = 15`, re-fermeture 0,30 / 30, plancher 10 min, plafond
90 min (`src/constants.h` §feature-091, `src/purge_stability_logic.{h,cpp}`).

## Pré-requis (données à collecter AVANT d'ouvrir la spec)
Relever chaque matin, depuis `/data` après l'ouverture de la porte, pendant
**au moins 5 matins** (05 → 09/09 minimum) :
- `measures_purge_last_duration_s` — durée réelle de la purge ;
- `measures_purge_last_end` — 1 stabilité / 2 plafond / 3 dispensée / 4 re-purge ;
- `measures_purge_count` — nombre de re-purges C1 dans la journée ;
- une `sensor_trace` (5 s) couvrant l'ouverture, pour mesurer la dispersion
  réelle des médianes minute sur plateau (σ, étendue de 10).
L'enregistreur `record_morning.sh` (scratchpad) fait déjà `sensor_trace` à 07:17
et 07:42 le 05/09. Consigner les relevés dans le §6 du plan 2026-09.

**Critère de décision** : si la médiane des durées réelles (fin 1) est ≤ plancher
+ 3 min et qu'aucune fin 2 n'est observée → **ne rien changer**, clore la spec
en « sans objet ». Sinon, option (c) ou statistique robuste, sur rejeu des
traces réelles.

## Fichiers potentiellement concernés
- `src/purge_stability_logic.{h,cpp}` — statistique / taille de bloc
- `src/constants.h` — constantes kPurge*, static_asserts
- `test/test_native_purge_stability/` — rejeux 04/09 + traces réelles, mutation

## Livrables — check-list
- [ ] A1 — Relevés 5 matins consignés (durées, fins, re-purges, σ médiane sur plateau)
- [ ] A2 — Décision architect + pool-chemistry : rien / (c) / statistique robuste
- [ ] A3 — Implémentation dans le module pur, constantes, static_asserts
- [ ] A4 — Tests natifs : rejeu 04/09 (sans et avec bruit, 20 graines) toujours fermé avant le plateau ; rejeu des traces réelles ouvre au plancher ≥ 90 % ; mutation (revenir à l'ancien paramètre → le rejeu terrain meurt)
- [ ] A5 — ADR (amendement de l'ADR-0059) + `docs/subsystems/measure-validity.md` + CHANGELOG

## Sécurité chimique
- Dosage impliqué : indirectement (ouverture de la porte de mesure = autorisation du premier dosage)
- Algorithme PID touché : non
- Risque identifié : ouvrir à tort dans le fond du V du rinçage → intégrale chargée à l'envers, doses sur eau non représentative — c'est précisément ce que 091 a supprimé ; toute modification passe par pool-chemistry avec les mêmes rejeux.

## ADR attendu ?
- **Oui** si le détecteur change (amendement ADR-0059) ; non si clôture « sans objet »

## Éléments visibles (pour doc-writer)
- Aucun endpoint, champ WS, topic MQTT ou libellé nouveau ; `docs/subsystems/measure-validity.md` (paramètres) et CHANGELOG si changement.

## Critères d'acceptation
- [ ] Relevés de ≥ 5 matins consignés avant toute modification de code
- [ ] Rejeu 04/09 : aucune ouverture avant le plateau, sans et avec bruit
- [ ] Rejeu traces réelles : ouverture ≤ plancher + 5 min dans ≥ 90 % des cas
- [ ] `pio run` sans nouveau warning ; `pio test -e native` vert ; mutation tuée

## Hors périmètre
- Dispense de purge pour les micro-coupures de filtration < 10 s **sans reboot**
  (recommandation 5 du gate 091, différée) → spec distincte si le besoin se
  confirme sur les compteurs `measures_purge_count`.
- Cause physique du fond en V (N1, feature-069).
