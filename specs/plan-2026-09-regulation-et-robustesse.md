# Plan d'ensemble — régulation pH/ORP et robustesse (septembre 2026)

**Statut : ACTIF — validé au point du 04/09/2026 (décisions en §5, suivi en §3, journal en §6)**
(avec l'enregistrement `/data` 60 s du 04/09 06:50 → 15:00).

Ce document est le plan de suivi demandé le 04/09 : il regroupe les features et
bugs déjà identifiés, en ajoute, et les ordonne autour de quatre objectifs
mesurables. Il ne remplace pas les specs (`specs/features/`), il les ordonne.
Le tableau de suivi (§3) est la seule partie à tenir à jour ; le reste est
l'analyse qui le justifie.

---

## 1. Les quatre objectifs (formulation mesurable)

| # | Objectif (demande utilisateur) | Cible mesurable proposée |
|---|---|---|
| **O1** | Un démarrage ou un plantage n'a pas d'impact sur le délai d'atteinte des cibles pH et ORP | Après un reboot pendant la filtration : intégrale et FF retrouvés à leur valeur d'avant (aucune ré-estimation) ; coupure ≤ 10 s → **aucune purge**, dosage repris dès les filtres amorcés (~65 s) ; coupure plus longue → purge courte seulement (plancher 10 min + amorçage ≈ **11 min de retard au total** si l'eau est stable — cible initiale « 4-5 min » révisée le 04/09 : le fond en V du rinçage impose une fenêtre de 10 médianes, cf. journal 19:10 ; jamais plus que le temps réel de stabilisation, plafond 90 min avec alerte) ; aucune alerte fausse, aucun terme remis à zéro. Il n'y a plus de « 15 min » nulle part. |
| **O2** | Le délai de rinçage (ni mesure, ni injection) est le plus court possible ; la valeur mesurée et filtrée est juste en 1-2 min | Fin de purge décidée par la **stabilité de la mesure**, jamais par une durée choisie : le matin, la porte s'ouvre dès que le brut est stable au-delà du plancher (10 min, révisé le 04/09 : fenêtre de 10 médianes imposée par le fond en V du rinçage — la cible initiale ~3 min / ≤ 5 min ne tenait pas) — donc ≈ 11 min si la sonde est saine ; le transitoire de 35-45 min observé chaque matin est **expliqué et supprimé** (matériel) ou, à défaut, traversé sans alerte ni dégât sur la régulation |
| **O3** | **Démarrer sur l'acquis, ajuster ensuite.** Ré-apprendre chaque jour est acceptable, à condition que l'apprentissage ne retarde jamais la correction en début de filtration : la régulation part de ce qu'elle sait déjà (veille, jours précédents) et corrige avec ce qu'elle apprend dans la journée | À la reprise de filtration, chaque terme démarre à sa dernière valeur connue : FF ✅, intégrale (093 après reboot, 091 contre l'eau de rinçage), dose de rattrapage matinale calculée sur la sensibilité déjà apprise (N2) puis affinée avec la journée. Aucun terme ne repart de zéro sauf changement de contexte (consigne, produit, vitesse) |
| **O4** | Atteindre la cible pH ne prend pas 4 h pour 0,1 d'écart | Écart initial 0,10 → \|erreur\| < 0,05 tenue (30 min), en **≤ 2 h de filtration** avec 094 seule (décision du 04/09 : on observe 094 avant d'envisager N2 ; l'actionneur permettrait ≤ 1 h avec une dose de rattrapage) |

**Indicateur commun (N5)** : « temps d'atteinte de cible » calculé et journalisé
chaque jour par le firmware (reprise de filtration → première tenue de
\|erreur\| < 0,05 pendant 30 min), pour pH et ORP. Sans cet indicateur, aucun
des quatre objectifs n'est vérifiable autrement qu'en relisant des CSV à la main.

---

## 2. Plan clair par objectif

Chaque tableau répond aux mêmes questions : qu'est-ce qui ne va pas, existe-t-il
déjà une réponse, que faut-il créer, quelle est la suite immédiate.

### O1 — Un reboot ne doit rien coûter

| Ce qui ne va pas aujourd'hui | Réponse | Existe ? | Suite |
|---|---|---|---|
| L'intégrale pH est perdue au reboot (jugée « périmée » alors qu'elle a 1 s) | Fraîcheur mesurée en temps de filtration, écriture garantie | **feature-093, spec écrite** (todo) | Cycle complet en v2.34, après le 06/09 |
| Le boot déclenche une purge de 15 min comme après une nuit | Purge terminée sur stabilité ; durée de coupure connue → pas de purge après 7 s | **feature-091, spec écrite** (todo), cas reboot ajouté le 03/09 | Même cycle que 093 (v2.34) |
| Le firmware redémarre (PANIC OOM, 3 fois) | Cause racine | **bug-080, en cours** (lot A déployé, lot B à faire) | `debugger` sur le lot B dès la clôture de la fenêtre lot A (~06/09) |
| Rien n'est perdu côté FF, compteurs journaliers, ORP (P pur) | — | ✅ déjà correct | Rien |

**Rien à créer.** Trois items existent ; la suite est un cycle v2.34 = 091 + 093, puis bug-080 lot B.

### O2 — Mesure juste en 1-2 min après la reprise

| Ce qui ne va pas aujourd'hui | Réponse | Existe ? | Suite |
|---|---|---|---|
| Le **brut** plonge de 0,8 pH pendant 30-35 min à chaque reprise (3 matins sur 3), l'ORP aussi. Le filtre firmware n'y est pour rien (5-10 min de retard seulement) | Trouver la cause physique (chambre de sonde vidée la nuit ? bulles ? effet électrique de la pompe ?) et la corriger côté matériel | **Non — à créer : N1** (protocole de mesure de référence 2 matins + inspection chambre/bypass + trace brute 5 s). feature-069 existe mais ne cible pas ce transitoire | **Demain matin** : bandelette au refoulement à t+2, t+15, t+35 min, notée en regard du brut (`/data` ou trace). Je prépare le protocole aujourd'hui |
| La purge ferme la porte sur un chronomètre (15 min), trop court le matin, trop long après une micro-coupure | Porte fermée tant que la mesure bouge, plancher 10 min (révisé le 04/09, fond en V du rinçage), plafond fail-safe 90 min | **feature-091, spec écrite** | v2.34 |
| L'eau de rinçage alimente la régulation : intégrale remise à zéro puis chargée à −191, 3 alertes à tort | Aucun terme (P, I, FF, alertes) ne consomme les mesures tant qu'elles ne sont pas stables | **feature-091, critère ajouté le 04/09** | v2.34 |

**À créer : N1** (investigation, humain + firmware léger). Ce que le firmware peut garantir seul : porte ≈ 11 min *quand la sonde est stable* (révision du 04/09) et zéro dégât en attendant. La cible « juste en 1-2 min » dépend du résultat de N1.

### O3 — Démarrer sur l'acquis, ajuster ensuite

Principe (précisé le 04/09) : ré-apprendre chaque jour est acceptable **si ça ne
retarde pas la correction en début de filtration**. À la reprise, chaque terme
démarre à sa dernière valeur connue et s'ajuste avec la journée. Passage en
revue de tout ce que la régulation « sait » :

| Ce que la régulation apprend | Démarre-t-elle sur l'acquis aujourd'hui ? | Réponse | Existe ? | Suite |
|---|---|---|---|---|
| Dose de base (charge du bassin, mL/min) | ✅ Oui — médiane des 3-7 derniers jours, persistée, servie dès la 1re session (1,37-1,41), ré-estimée chaque soir | — | **feature-085, en production** | Rien |
| Intégrale (complément de charge du jour) | ⚠️ Oui la nuit (gelée), **non après un reboot** (perdue) | Restaurer la valeur d'avant coupure | **feature-093** | v2.34 |
| Intégrale (suite) | ❌ **Non le matin** : l'eau de rinçage la remet à zéro et la charge à l'envers (−191 le 04/09) → la journée part plus bas que la veille au soir | Aucun terme ne consomme les mesures de rinçage | **feature-091** (critère ajouté) | v2.34 |
| Intégrale (suite) | Dégonflement l'après-midi sous la cible (1 968 → 440) : voulu, anti-emballement ; la valeur du soir est celle du matin suivant | — | ✅ conception (ADR-0052) | Ne pas toucher |
| **Stock d'erreur nocturne** (le bassin dérive de +0,06 à +0,2 chaque nuit) | ❌ **Non** : rien n'est appris sur « combien de mL pour 0,1 pH dans ce bassin ». Chaque matin, le terme P le redécouvre à 0,8 mL/min : c'est *ça* qui retarde la correction en début de filtration | Sensibilité apprise (mL / 0,1 pH) à partir des journées passées → dose de rattrapage calculée à la reprise, bornée, vérifiée après mélange ; la journée affine ensuite la sensibilité | **Non — à créer : N2** (nouveau chemin d'injection : pool-chemistry + code-reviewer) | 1) accord de principe à 15 h ; 2) spec après N4 ; 3) cycle v2.36 |
| Débit de pompe, rendement des impulsions | ✅ Calibrés, persistés | — | ✅ (feature-074/075) | Rien |

**Conclusion O3 : la seule chose que la régulation ré-apprend chaque matin en
retardant la correction, c'est le stock d'erreur nocturne (N2). L'intégrale
« sait » mais on lui fait perdre son acquis par deux chemins (093, 091).**

### O4 — 0,1 d'écart ne doit pas prendre 4 h

| Pourquoi c'est lent aujourd'hui | Réponse | Existe ? | Suite |
|---|---|---|---|
| Près de la cible, le bruit (±0,08) ouvre et ferme les sessions (bande 0,05/0,01) ; chaque fermeture arme 15 min de pause → rapport cyclique 1/3, FF servie un tiers du temps. **2 h deviennent 4 h** | Porte de session insensible au bruit (signal de décision lent, bande, durées minimales, FF non retenue) | **feature-094, spec écrite** | v2.35, après intégration de l'enregistrement du 04/09 (période du bruit) |
| À e = 0,1 le surplus est kp·e = 0,8 mL/min pour ~80-100 mL à injecter → **2 h** à plein régime, alors que l'actionneur permet 4,33 mL/min | Doser le stock d'erreur d'un coup au lieu de le laisser au terme P | **N2** (même item que O3) | v2.36 |
| Gains kp = 8 (normal) | `regulation_speed = fast` (kp 12) existe ; ne pas y toucher avant 094 (amplifierait le bruit) | ✅ réglage existant — **N3** = décision, pas de code | Évaluer après 094 ; peut-être inutile si N2 suffit |
| Impossible de mesurer le progrès sans enregistreur externe | Filtrée historisée + indicateur « temps d'atteinte de cible » calculé par le firmware | **Non — à créer : N4, N5** (petites specs `specs/modele.md`) | v2.35, **avant** 094 pour pouvoir la juger |

**À créer : N4, N5** (petits). 094 existe. N2 partagé avec O3.

### Synthèse : ce qui existe, ce qui manque

| | Existe (spec ou code) | À créer |
|---|---|---|
| O1 | 093, 091, bug-080 | — |
| O2 | 091 | **N1** (investigation matérielle, dès demain) |
| O3 | 085 ✅, 093, 091 | **N2** (sensibilité apprise) |
| O4 | 094, réglage `fast` | **N4** (filtrée historisée), **N5** (indicateur), N2 |

Ordre : **v2.34** 091+093 → **v2.35** N4+N5 puis 094 → **v2.36** N2. N1 en parallèle, humain, dès demain.

---

## 2 bis. Diagnostic détaillé par objectif

### O1 — Reboot / plantage

**Faits** (03/09 08:59:47, PANIC bug-080, coupure 7 s, filtration active) :
- Intégrale pH **rejetée « périmée »** (R3 en heures murales + R5 n'écrit que si
  la valeur bouge ≥ 810 pH·s ; l'intégrale est gelée la nuit ⇒ tout reboot du
  matin la perd, par construction) → repart de 0, régime retrouvé à 14:30.
- Purge de stabilisation fixe de **15 min** appliquée au boot comme après une
  nuit de stagnation → 16 min sans dosage.
- FF (1,41 mL/min) et compteurs journaliers correctement restaurés ✅.
- Coût estimé : ~1 h de régulation. Le reboot lui-même (bug-080) n'est pas
  élucidé : 3 occurrences de la même classe (13/08, 29/08, 03/09), compteur
  d'échecs d'allocation DMA (`caps=0x080C`) toujours en hausse (x8 le 04/09 07:28).

**Réponse** : feature-093 (fraîcheur de l'intégrale en temps de filtration),
feature-091 (purge adaptative, cas reboot), bug-080 lot B (cause racine).
Côté ORP : régulation P pure, rien à restaurer hors la purge → 091 suffit.

### O2 — Rinçage et justesse de la mesure à la reprise

**Faits** — trois matins consécutifs, même signature, indépendante de la durée de purge :

| Matin | Purge | Brut à la reprise | Brut en fin de purge | Δ | Retour du brut à la normale | Retour de la filtrée |
|---|---|---|---|---|---|---|
| 02/09 | 5 min | 6,85 | 5,60 | −1,25 | ~30 min | ~30 min |
| 03/09 | 15 min | 6,72 | 6,04 | −0,69 | — (reboot 08:59) | — |
| 04/09 | 15 min | 6,78 | 5,97 | −0,81 | 07:30 (**33 min**) | 07:40-07:50 (43-53 min) |

L'ORP suit le même motif (−56 à −74 mV en fin de purge, retour en ~30 min).

**Ce que ces chiffres disent — et qui contredit la prémisse** : l'eau stagnante
lue à la reprise est *normale* (6,78 brut = 7,28 corrigé, la valeur du bassin
la veille au soir). C'est **pendant la circulation** que la lecture brute
plonge de 0,7-0,8 puis remet 30-35 min à revenir. Ce transitoire est dans le
**brut**, en amont de tout filtre firmware ; la chaîne médiane (65 s) + EMA
(τ ≈ 50 s) n'y ajoute que 5-10 min de retard. **Aucune modification du filtre
ne rendra la mesure juste en 1-2 min si la sonde elle-même lit 5,97.**

Hypothèses physiques (à départager, feature-069 + N1) :
1. **Chambre de sonde partiellement vidée la nuit** (bypass, siphon) : jonction
   de référence à l'air → ré-équilibrage lent (30-60 min est l'ordre de grandeur
   d'une jonction qui se re-mouille). Explique aussi l'ORP. Test : inspecter la
   chambre avant la reprise ; clapet ou remontée du point de mesure.
2. **Bulles d'air chassées au démarrage** passant sur les électrodes : effet
   plus court en général, mais la chambre en bypass peut en piéger longtemps.
3. **Courants de fuite de la pompe de filtration** au démarrage (moteur froid,
   alimentation) : effet électrique qui s'atténue — testable en comparant le
   transitoire avec la pompe alimentée par une autre phase / masse déportée.
4. Bouchon d'eau morte réel : peu compatible avec « lecture normale à la
   reprise, plongeon ensuite » et avec la vitesse de circulation — à écarter
   sauf preuve.

**Test décisif (humain, 2 matins)** : mesure de référence (bandelette ou
photomètre) au refoulement à t+2 min, t+15 min, t+35 min après la reprise, en
regard du brut. Si la référence dit 7,3 quand la sonde dit 6,3, la sonde est
en cause (1-3) ; sinon c'est l'eau (4).

**Ce que le firmware peut faire quoi qu'il en soit** (091, renforcé) :
- fermer la porte **sur la stabilité de la mesure**, pas sur un chronomètre
  (plancher court, plafond fail-safe) ;
- **ne laisser aucun terme** (P, I, FF, alertes) consommer l'eau de rinçage :
  le 04/09 l'intégrale a été remise à zéro puis chargée à −191 pH·s par 30 min
  de mesures « représentatives » qui ne l'étaient pas ; trois alertes à tort ;
- journaliser le transitoire (durée, amplitude) chaque matin → indicateur pour
  juger la correction matérielle.

### O3 — Apprentissage

**Ce qui est déjà acquis** : la dose de base (FF) est apprise sur 3-7 jours et
persistée (1,37-1,41 mL/min retrouvés à chaque reprise) ✅. Les compteurs
journaliers survivent au reboot ✅.

**Ce qui est ré-appris chaque jour** :
- L'**intégrale** : gelée la nuit, mais (a) perdue au reboot du matin (093),
  (b) détruite par l'eau de rinçage (091), (c) dégonflée par la décroissance
  sous la cible (demi-vie 1 h : 1 968 → 440 pH·s hier après-midi). (c) est un
  choix de conception (anti-emballement) et la valeur qui reste (440) est
  faible ; ce n'est pas l'intégrale qui doit porter la mémoire du bassin.
- Le **rattrapage matinal** : chaque matin le bassin a dérivé de +0,06 à +0,2
  pendant la nuit ; ce stock d'erreur est résorbé par le terme P (kp = 8 →
  0,8 mL/min pour 0,1 d'écart) et une intégrale à reconstruire. Rien n'est
  appris de la veille sur « combien de mL pour 0,1 pH dans ce bassin ». C'est
  la seule vraie lacune d'apprentissage → **N2 : sensibilité apprise**
  (mL par 0,1 pH, estimée à partir des journées passées : volume surplus vs
  variation obtenue) et dose de rattrapage calculée, pas découverte.

### O4 — Vitesse d'atteinte à petit écart

**Pourquoi 4 h pour 0,1** (arithmétique de la v2.33.0, vitesse `normal`) :
- Demande = FF 1,41 (couvre la charge) + kp·e = 8 × 0,1 = **0,8 mL/min de
  surplus** + I (faible le matin). Ramener 0,1 pH dans ce bassin demande ~80-100
  mL de surplus (ordre de grandeur du 03/09) → **~2 h** à plein régime.
- Près de la cible, les sessions sont ouvertes/fermées par le **bruit** (±0,08
  contre une bande 0,05/0,01), chaque fermeture arme 15 min de pause : rapport
  cyclique ~1/3, FF servie un tiers du temps → **~4 h**. (feature-094)
- L'actionneur, lui, permet 4,33 mL/min soutenables : le régulateur n'utilise
  que ~50 % de son autorité à e = 0,1.

**Leviers, dans l'ordre** :
1. **094** — stabiliser la porte de session : rend au régulateur ses 2/3 de
   temps perdus. Sans ça, tout gain supplémentaire amplifie le bruit en dosage.
2. **N2** — rattrapage par sensibilité apprise : dose le stock d'erreur d'un
   coup (à débit soutenable, borné, une fois par reprise, vérifié après
   mélange) au lieu de le laisser au terme P. C'est ce qui fait passer de 2 h à
   ~30-45 min à écart 0,1. pool-chemistry obligatoire.
3. **N3** — gains : `regulation_speed = fast` (kp 12, Ti 4 h) existe déjà ;
   à n'évaluer **qu'après** 094 (sinon 12 × 0,08 = 1 mL/min de bruit dosé).

---

## 2 ter. Résultats de l'enregistrement du 04/09 (06:50 → 14:20, 1 point/min)

Source : `builds/2.33.0/esp32dev_core3/ph_filtered_20260904_0650-1550.csv`
(analyse `ph_analyse.py` ; trous 14:43-15:43 dus à la mise en veille du Mac,
sans incidence sur la matinée). Ce que la journée confirme ou corrige :

| Point | Mesure du 04/09 | Conséquence pour le plan |
|---|---|---|
| Transitoire de reprise (O2) | Reprise 06:57, fin de purge 07:12 sur brut **5,71** ; brut revenu à ~6,85 vers 07:45 ; filtrée à 7,30 à **07:47** (t+50 min). Identique aux 02 et 03/09. | 3/3 matins : le transitoire est dans le BRUT, dure ~45-50 min, insensible au filtre. 091 (porte fermée) + N1 (cause physique) confirmés. |
| Intégrale (O3) | 440 au réveil → **−193** à 07:45 (règle de reset sur l'eau de rinçage) → 1614 à 14:20 ; la veille au soir : 1968. | L'acquis est détruit chaque matin et reconstruit en ~5 h : c'est ~0,6 mL/min de terme I absents toute la matinée. 091 suffit à le garder (pas de reboot aujourd'hui, 093 non sollicitée). |
| Temps d'atteinte (O4, KPI N5) | Filtrée à 7,35-7,51 toute la matinée malgré 214 mL d'acide à 10:00 ; durablement dans 7,30 ± 0,05 seulement à partir de **~11:40** (t+4 h 45). ORP 485 mV à 07:34 → 530 à **12:50** (t+5 h 50), Javel 870 mL bloquée dès 10:50. | Ordre de grandeur « 4 h pour 0,1 » confirmé une 2e fois. Les deux régulations mettent ~5 h à atteindre leur cible après une nuit. |
| Bruit de la filtrée (O4) | ±0,08 confirmé sur 7 h ; pointe isolée 7,51 → 7,38 en 10 min à 10:00 ; brut à ±0,18 sur 10 min (6,74-7,10). | Dimensionnement de 094 : amplitude ±0,08 à 10 min, valeurs d'entrée figées. |
| Rebond en pause | 8/11 pauses positives (+0,04 à +0,16), 1 négative, 1 nulle. **Ne pas y lire un effet d'injection** : les pauses s'ouvrent quand le bruit est bas (fermeture sous 7,29) et se ferment quand il est haut → régression vers la moyenne, biais de sélection. | Confirme que la porte est pilotée par le bruit, pas par la chimie (094). |
| Montée matinale du pH sous acide | 7,33 (07:50) → 7,51 (10:00) pendant que la **Javel** passe de 0 à 789 mL (07:34-09:30). NaOCl 36° est alcaline : 0,8 L ≈ 1 mol OH⁻ dans ~40 m³ ≈ 25 µeq/L → ~+0,02-0,03 pH et ~90-100 mL d'acide à neutraliser. Ne suffit pas seul (l'écart est de 0,18), s'ajoute au stock nocturne et au bruit. | Nouvelle observation **N9** : couplage chlore → pH le matin. À quantifier avec N4 avant toute idée de compensation croisée. |

Lecture d'ensemble : **rien dans cette journée ne contredit le plan** ; elle
lève la condition « enregistrement 04/09 » posée sur 094 et N6, et ajoute N9.

---

## 3. Tableau de suivi

Statuts : `idée` → `spec` (écrite, todo) → `doing` → `test` (déployé, en observation) → `done` / `abandonné`.
Conditions : ce qui doit être vrai avant de démarrer l'item.

| ID | Sujet | Objectif | Type | Priorité | Conditions / dépendances | Statut | Version | Notes |
|---|---|---|---|---|---|---|---|---|
| bug-080 | Redémarrages PANIC, famine heap 8 bits / DMA (lot B : instrumentation dans le hook alloc-failed, fragmentation, PSRAM) | O1 | bug | **critique** | Fin fenêtre d'observation lot A (~06/09) ; `debugger` avant tout fix | doing (lot A déployé v2.33.0) | 2.34+ | 3 occurrences ; compteur DMA 0x080C x8 le 04/09 |
| bug-famine-8bits | Parachute mort + gardes aveugles (métrique heap fausse) | O1 | bug | haute | Lié à bug-080 lot B | doing | 2.34+ | Post-mortem RTC à faire |
| feature-091 | Purge adaptative : fin sur stabilité, plancher court, plafond fail-safe ; **aucun terme (P/I/FF/alertes) ne consomme l'eau de rinçage** ; cas reboot | O1, O2 | feature | **haute** | pool-chemistry ; données [STAB] des 3 matins (acquises) | **done — revue Approved 04/09, déploiement en attente du go** | 2.34.0 | Fenêtre 10 médianes / plancher 10 min (deux re-arbitrages pool-chemistry le 04/09, fond en V) ; suivi feature-097 |
| feature-093 | Régulation transparente au reboot : fraîcheur de l'intégrale en temps de filtration, écriture garantie, durée de coupure | O1, O3 | feature | **haute** | pool-chemistry (longue coupure) ; changement de schéma NVS | **done — revue Approved 04/09, déploiement en attente du go** | 2.34.0 | Schéma NVS 2, compteur monotone de filtration |
| feature-094 | Décision de session robuste au bruit (signal de décision lent / bande / durées minimales / rétention FF) | O4 | feature | haute | ~~Enregistrement 04/09~~ (fait : ±0,08 à 10 min) ; pool-chemistry | **DÉPLOYÉE 05/09 (v2.36.0)** | 2.36.0 | Écrite le 03/09 ; condition levée le 04/09 |
| N1 = **feature-069** (refondue le 04/09) | **Transitoire de reprise** : protocole bandelettes bassin + refoulement (t+2/15/35 min), inspection chambre de sonde / bypass, courbe brute 10 s de la première heure + `sensor_trace` (enregistreur `record_morning.sh` armé pour le 05/09) | O2 | investigation (humain + firmware léger) | **haute** | Utilisateur au bassin le 05 ou le 06/09 à 06:50 | spec (todo) | — | Hypothèses H1-H5 et grille de lecture dans la spec ; l'ORP (N6) se lit sur les mêmes relevés |
| N2 | **Rattrapage matinal par sensibilité apprise** (mL / 0,1 pH appris ; bolus borné à la reprise, vérifié après mélange) | O3, O4 | feature | basse (en réserve) | **Décision du 04/09 : pas de spec pour l'instant** — on observe 094 (cible ≤ 2 h) ; à rouvrir seulement si 094 ne suffit pas. Alors : N4, pool-chemistry, ≥ 5 jours sans reboot | en attente | — | Nouveau chemin vers pump_controller : pool-chemistry + code-reviewer |
| N3 | Évaluation `regulation_speed = fast` (kp 12) | O4 | réglage | basse (en réserve) | Après observation de 094 ; décision pool-chemistry | en attente | — | Pas de code ; ne s'ouvre que si 094 ne donne pas ≤ 2 h |
| N4 = **feature-095** | **Historiser la filtrée** (et le signal de décision de 094) à côté du brut dans `/get-history` | tous (mesure) | feature | haute | Aucune ; budget partition history (64 Ko) à vérifier (≈ +7 Ko) | **DÉPLOYÉE 05/09 (v2.38.0)** | 2.38.0 | Spec écrite le 04/09 ; sans ça chaque analyse repasse par un enregistreur externe |
| N5 = **feature-096** | **Indicateur « temps d'atteinte de cible »** pH et ORP (minutes de filtration depuis la 1re mesure valide du jour, tenue 30 min), mémoire 7 j, `/data` + WS (parité), `/get-kpi` | tous (KPI) | feature | haute | 091 (porte = t0), 093 (compteur de filtration) | **DÉPLOYÉE 05/09 (v2.37.0)** | 2.37.0 | Spec écrite le 04/09 ; c'est la mesure de succès du plan entier ; ADR « définition normative » attendu |
| N6 | Transitoire ORP à la reprise et chute nocturne (537 → 485 mV) : normal ou artefact ? Limite horaire ORP atteinte dès 07:53 | O2, O4 (ORP) | observation | normale | Enregistrement 04/09 + N1 (la référence ORP est plus difficile) | idée | — | Le plan est pH-centré : ne pas oublier l'ORP |
| N7 | Dérive nocturne du pH : mesurer (fin de soirée vs reprise) sur 7 jours, décider si une dose « hors filtration » est justifiée ou si N2 suffit | O3, O4 | observation | normale | N4 ou N5 (sinon relevés manuels) | idée | — | 03→04/09 : +0,06 seulement (fin 7,29) ; nuits précédentes +0,2 (fin 7,3 tard) |
| feature-092 | Carte « Débit d'injection » pH/ORP (tiret muet) | visibilité | feature UI | basse | ux-ui déjà consulté | todo | plus tard | Indépendante du plan |
| feature-097 | **Seuil de stabilité sur données terrain** : ajuster le détecteur 091 (médianes 2 min ou statistique robuste) sur les durées de purge réelles | O2 | feature | moyenne | ≥ 5 matins de télémétrie `measures_purge_last_*` (au plus tôt 10/09) ; pool-chemistry | todo (spec écrite) | 2.36+ | Condition 10 du gate 091 ; seuil 0,20 refusé (marge V nulle) |
| bug-089 | Parité WS/config, champs manquants | hygiène | bug | basse | — | **DÉPLOYÉ 05/09 (v2.39.0)** | 2.39.0 | Récidive documentée : à grouper avec N4/N5 (qui ajoutent des champs) |
| 084/085/086 | Fenêtre d'observation intégrale / FF / limite actionneur | — | clôture | — | ~06/09 ; question R3 tranchée par 093 | doing | 2.33.0 | Clore avec le rapport de synthèse |
| N8 | Mesure de référence de l'offset pH (+0,5) après le changement d'alimentation | O2 (justesse) | humain | normale | Bandelette / photomètre, eau du bassin (pas du refoulement à la reprise) | idée | — | L'offset n'est pas une suspicion de sonde ; il faut juste le re-vérifier après tout changement électrique |
| N9 | Couplage chlore → pH le matin : la Javel (alcaline, ~0,8 L avant 09:30) pousse le pH pendant que l'acide le tire ; quantifier l'acide « dû à la Javel » | O3, O4 | observation | basse | N4 (filtrée + volumes historisés) ; 5 matins | idée | — | Vu le 04/09 (§2 ter) ; ordre de grandeur ~90-100 mL d'acide/jour ; aucune compensation croisée sans pool-chemistry |

---

## 4. Séquencement proposé

| Phase | Fenêtre | Contenu | Livrable de sortie |
|---|---|---|---|
| **0 — Observer** | 04/09 → 06/09 | Aucun déploiement. Enregistrement 04/09 ; N1 protocole de référence les 05 et 06 au matin ; N8 ; clôture 84/85/86 | Point de 15 h : plan validé, conditions levées ou datées |
| **1 — Robustesse** | v2.34 (semaine du 07/09) | 091 + 093 ensemble (même zone, même gate pool-chemistry, un seul cycle) ; bug-080 lot B en parallèle par `debugger` | Un reboot ou une reprise ne coûte plus rien à la régulation |
| **2 — Voir** | v2.35 | N4 + N5 (+ bug-089 dans le même cycle : mêmes fichiers) puis 094 | Les objectifs deviennent mesurables sans CSV ; la porte de session ne bat plus |
| **3 — Aller vite** (conditionnelle) | v2.36 | Seulement si l'observation de 094 ne donne pas ≤ 2 h : N2 (sensibilité apprise) et/ou N3 | O4 renforcé : 0,1 d'écart en ≤ 1 h |
| Transverse | — | N6, N7 alimentés par N4/N5 ; 092 ; correction matérielle issue de N1 | — |

Règles du plan :
- Une spec à la fois en `doing/` (règle projet) ; 091+093 sont l'exception
  assumée parce qu'elles touchent la même séquence de reprise et le même gate.
- Tout item qui modifie la régulation passe par pool-chemistry ; N2 ouvre un
  **nouveau chemin d'injection** → pool-chemistry + code-reviewer sans exception.
- Chaque phase se termine par une période d'observation avec N5 comme juge
  (à partir de la phase 2 ; avant, enregistreur externe).

---

## 5. Décisions du point du 04/09 (plan validé — statut : ACTIF)

1. **Cibles** : O1 et O2 validées (reboot ≤ 10 s = zéro coût, sinon ≤ 4-5 min —
   **révisé à ≈ 11 min le 04/09 19:10** par pool-chemistry, fenêtre 10 min
   imposée par le fond en V du rinçage ; fin de purge sur stabilité, ≤ 5 min sonde
   saine → **≥ 10 min de plancher**, même révision). O4 validée à **≤ 2 h avec
   094 seule** ; on observe avant d'aller plus loin.
2. **N1** : l'utilisateur fait les relevés bandelettes (bassin + refoulement,
   t+2/15/35, air à la buse de refoulement les 5 premières minutes — la chambre
   de sonde n'est pas observable) le **samedi 05/09** à la reprise de 06:50 ;
   protocole dans feature-069 refondue ; enregistreur `record_morning.sh` armé
   pour le 05/09.
3. **091 + 093 dans le même cycle** (v2.34) : accepté.
4. **N2** : non pour l'instant — on attend le résultat de **091 + 093 + 094**
   avant de décider ; reste en réserve.
5. **Ordre** : phase 1 (091+093) d'abord, bug-080 lot B juste derrière ; le
   `debugger` peut instruire lot B en parallèle sans déployer.
6. **Ménage** : feature-069 refondue en N1 (transitoire de reprise, hypothèse
   « stagnation acide » enterrée) ; N3 gardé en réserve, ne s'ouvre que si 094
   ne suffit pas.

---

## 6. Journal du plan

- 2026-09-04 — brouillon initial (analyse des 4 objectifs, 18 items, 4 phases).
- 2026-09-04 — §2 « plan clair » ajouté ; O3 reformulé (« démarrer sur l'acquis,
  ajuster ensuite ») ; feature-091 amendée : plancher = constante firmware,
  réglage `stabilization_delay_min` retiré de l'UI, coupure ≤ 10 s sans purge
  (mémoire RTC), piège de la stabilité à t+0.
- 2026-09-04 14:30 — §2 ter : résultats de l'enregistrement du jour (transitoire
  3/3, intégrale détruite puis reconstruite en 5 h, cible atteinte à t+4 h 45 pH
  / t+5 h 50 ORP, bruit ±0,08 confirmé, rebond en pause = biais de sélection) ;
  condition levée sur 094 ; N9 ajouté (couplage Javel → pH).
- 2026-09-04 15:50 — point avec l'utilisateur : cibles **O1, O2 validées** ; O1
  reformulée sans ambiguïté (reboot ≤ 10 s = zéro coût, sinon ≤ 4-5 min, plus
  de « 15 min » nulle part) ; **O4 validée à ≤ 2 h avec 094 seule**, N2 mis en
  réserve (phase 3 devient conditionnelle). CSV du jour archivé.
- 2026-09-04 16:10 — **plan validé** (§5 : les six questions tranchées) ;
  feature-069 refondue en N1 (protocole bandelettes, hypothèses H1-H5, grille de
  lecture) ; enregistreur matinal 10 s + `sensor_trace` armé pour le 05/09 ;
  N2 et N3 en réserve jusqu'au résultat de 091+093+094.
- 2026-09-04 16:20 — **décision utilisateur : lancer tout le programme et
  déployer dès que possible aujourd'hui**, y compris pendant la filtration
  (« tant pis pour les mesures »). La fenêtre d'observation 084/085/086 est
  close sur 3 jours (02 → 04/09). Exécution en déploiements successifs pour
  garder un gate pool-chemistry par lot : v2.34 = 091 + 093 ; v2.35 = 094 + N4 +
  N5 (+ bug-089) ; bug-080 lot B instruit par `debugger` en parallèle. N1 reste
  valide demain (mesure du brut, que 091 ne touche pas) mais sur firmware neuf.
- 2026-09-04 17:15 — lot 1 (v2.34.0) : plan architecte livré, **gate
  pool-chemistry = GO sous conditions** (C1 refermeture par hystérésis 2× seuil
  — le plancher 4 min ne couvre pas le 02/09 dont l'onset est inconnu ; C2
  plafond porté à 90 min, ouvrant + alerte `measures_purge_timeout` ; C3
  dispense reboot durcie : whitelist reset SW/PANIC/WDT, `waterPresent`
  mémorisé, FNV-1a, ≤ 1 dispense/h glissante tant que bug-080 n'est pas résolu ;
  C4 le mode Programmée garde un sas de 4 min de circulation ; C5 static_assert
  093 réécrits, écriture dès le 1er tick éligible après rejet ; C6 test natif
  « porte fermée = tous les termes gelés »). Valeurs figées : fenêtre 4 min,
  plage 0,15 pH / 15 mV, fraîcheur 6 h de filtration ET 72 h murales, échéance
  d'écriture 3 h, schéma 2. Implémentation lancée (firmware + frontend en
  parallèle).
- 2026-09-04 17:15 — **bug-080 lot B : rapport `debugger` reçu** (analyse seule,
  `bug080_lotB_analyse.md` dans le scratchpad). Deux corrections au brief : la
  carte A 4 Mo de PSRAM (`psram_size 4194304`) avec `SPIRAM_USE_MALLOC`
  (allocs > 4 Ko → PSRAM, échec → retombée PSRAM) donc le parachute
  `new_handler` est structurellement inerte et sa réserve de 12 Ko d'interne est
  du poids mort ; la métrique 8 bits du lot A est déjà correcte. Cause racine
  retenue : **rafales réseau sub-seconde** (chargement UI 190 Ko d'assets sans
  `Cache-Control`, 55 discovery MQTT) → famine interne < 2 s → `fopen` newlib
  (`xSemaphoreCreateRecursiveMutex` ~92 o) échoue → abort. Le verdict
  « SUSPICION CORRUPTION » du post-mortem est un faux positif (compare au ring
  vieux de 2 s). Livrables : B1 `heap_caps_malloc_extmem_enable(256-512)` ;
  B2 ne pas armer la réserve parachute sur carte PSRAM (gate pool-chemistry,
  ADR-0058) ; B3 `Cache-Control`/`Last-Modified` sur le handler statique,
  étaler les discovery, garde `heapFree8Bit()` → 503 avant `fopen` ; B4
  instrumentation + re-tentative différée pour un verdict pur famine/corruption ;
  B5 seuils non recalibrés. **Exécution après le lot 1** (zones `main.cpp`,
  `constants.h`, `oom_guard*`, `web_server.cpp`, `mqtt_manager.cpp`).
- 2026-09-04 18:40 — **les tests natifs invalident la fenêtre 4 min** : le rejeu
  de la courbe du 04/09 (fond 5,54 à t+9, remontée 0,043-0,047 pH/min) ouvre la
  porte à t+7-8 à pH ≈ 5,6 — exactement l'incident à supprimer. Cause : W
  médianes ne couvrent que W−1 intervalles, pente détectable = 0,15/3 = 0,05
  pH/min > pente du rinçage. Pool-chemistry re-tranche **W = 8, plancher 8 min**
  (+ conditions : rejeu 04/09 fermé tant que pente ≥ 0,03 ; test « pH plat, ORP
  3 mV/min → fermé » ; dérive lente de sonde jugée stable = limite connue à
  consigner en ADR). Recommandation 5 (dispenser aussi les micro-coupures de
  filtration < 10 s sans reboot) **différée** hors périmètre — à ouvrir en spec
  de suivi.
- 2026-09-04 19:10 — **W = 8 encore à la limite, deuxième re-arbitrage → W = 10,
  plancher 10 min**. Rejeu sans bruit de la même courbe avec W = 8 : ouverture à
  t+13 à pH 5,71 (étendue 0,145, 5 millièmes sous le seuil) puis re-fermeture
  C1 à t+17 (0,303, 3 millièmes au-dessus). Physique ratée au premier
  arbitrage : le rinçage est un **fond en V** (descente −0,047 puis remontée
  +0,043) qui occupe une demi-fenêtre de chaque côté, la pente détectable est
  donc seuil/((W−1)/2) = 0,043 pH/min pour W = 8. Avec W = 10 (capacité de
  l'anneau) : 0,20 > 0,15 sur le V, re-fermeture dès 0,033 pH/min, ouverture à
  t+46-53 sur le plateau (pH 6,88) dans les rejeux bruités. **Conséquence sur
  O1** : le retard après coupure longue passe de « ≤ 4-5 min » à **≈ 11 min**
  (10 min de plancher + 65 s d'amorçage), contre 15 min de minuteur fixe avant ;
  coupure ≤ 10 s toujours à zéro. Conditions ajoutées : (6) le test de rejeu
  fige W = 10 par mutation W → 8 ; (7) l'ADR consigne le V et la limite « dérive
  ≤ 0,017 pH/min jugée stable » ; (8) test 5 graines bruitées, ouverture ou
  plafond 90 min garanti (1 tirage sur 5 n'ouvre pas en 60 min : le plafond
  `measures_purge_timeout` n'est pas théorique).
- 2026-09-04 20:50 — **tests verts (29 suites, 1088 cas), mutation 10→8 tuée,
  flash 98,6 %.** Limite mesurée par le test-engineer : sur eau plate avec bruit
  ±0,18, l'étendue de 10 médianes ≈ 0,16 = le seuil → ouverture au plancher 12
  graines sur 21, queue jusqu'à t+29 ; rejeu 04/09 bruité t+47-62 (plateau
  t+39), 0 fausse ouverture avec ORP. Pool-chemistry : **GO (a) — accepter pour
  v2.34** (fail-closed, coût en temps seulement, plafond + alerte), seuil 0,20
  refusé (marge V nulle), médianes 2 min = piste à trancher sur données réelles.
  Conditions 9-11 : limite consignée en ADR ; **feature-097** créée (todo,
  s'ouvre après ≥ 5 matins de télémétrie `measures_purge_last_*`) ; parité
  /data ↔ WS des champs de purge vérifiée par grep (5 champs des deux côtés).
  Revue de code lancée.
- 2026-09-04 21:15 — **Revue Approved** après correctifs des 3 majeurs : (1) garde
  `candidatePending` — aucune écriture NVS tant qu'une restauration R4 attend sa
  confirmation (sans elle, `kDelta` écrivait 0 par-dessus l'instantané valide et
  annulait 093 au second reboot) + test natif tueur de mutant ; (2) branche UI
  fausse « Stabilisation non atteinte (90 min) » retirée (le plafond OUVRE la
  porte) ; (3) `-Os` restauré dans `[env:esp32dev]`. Mineur : log info au 1er
  boot 2.34 (« aucun instantané NVS exploitable — départ de zéro »). État :
  29 suites / 1089 cas verts, core3 Flash 98,6 % (1 808 697 o), FS 320 K
  reconstruit. Specs 091 et 093 → `done/` avec notes d'implémentation.
  Doc-writer en cours (CHANGELOG 2.34.0, ADR-0059/0060, guides PDF).
  **Déploiement `./deploy.sh ota-all` : en attente du go explicite.**
- 2026-09-04 23:00 — **Doc v2.34.0 livrée** (ADR-0059 fenêtre de médianes / fond en V,
  ADR-0060 fraîcheur en temps de filtration + garde `candidatePending`, CHANGELOG,
  UPDATE_GUIDE, API/MQTT/WS/sous-systèmes/pages, guides utilisateur PDF v2.34,
  PDF v2.33 supprimés). Trois documents périmés hors périmètre des deux agents ont
  été corrigés à la main : `history.md` (purge non prévisible), 
  `regulation-vue-d-ensemble.md` (ligne de tempo 10-90 min non fixe),
  `UPDATE_GUIDE.md` (la recommandation « configurer stabilizationDelayMin ≥ 5 min »
  visait un réglage qui n’existe plus — le passage de minuit arme désormais des
  durées GRAVÉES 5 min pH / 3 min ORP).

  ⚠️ **CONTRAINTE DÉCOUVERTE AU RECOMPTAGE — à traiter dans le lot 2** : le gabarit
  de `/data` (`StaticJson<2304>`, web_routes_data.cpp) n’a plus que **68 octets de
  marge** (charge pire cas 2 236 o après les +97 o de la feature-091). Côté WS la
  marge reste confortable (742 o sous `kWsFrameBufBytes` 3584). Or **feature-095
  (historiser la filtrée) et feature-096 (KPI temps d’atteinte) ajoutent toutes deux
  des champs à `/data` ET au WS (parité)** : le gabarit `/data` devra être relevé
  AVANT, sous peine de sérialisation tronquée silencieuse. À inscrire en condition
  des deux specs.

- 2026-09-05 08:20 — **PREMIER MATIN EN v2.35.0 : la purge adaptative fait
  exactement ce pour quoi elle a été écrite — mais le matin est CONTAMINÉ.**

  | | 04/09 (v2.33.0) | 05/09 (v2.35.0) |
  |---|---|---|
  | Fin de purge | 07:12 sur brut **5,97** (fond du V) | 07:40 sur brut **6,77** (plateau) |
  | Durée | 15 min fixes | **≈ 43 min**, ajustée seule |
  | Intégrale au réveil | **−191 pH·s** | **0** — intacte |
  | Alertes pH à tort | **3** | **0** |

  Fond du V touché à **5,45 brut à 07:01**, porte restée fermée. Dispense de purge
  correctement refusée : « Coupure 12 s (eau absente avant la coupure) → purge
  complète » (12 s > seuil de 10 s, et eau absente ⇒ fail-closed). **Zéro warning,
  zéro erreur, zéro critical depuis le boot.**

  ⚠️ **CONTAMINATION — condition NOUVELLE, propre à ce matin** : le niveau d'eau
  du bassin était trop bas, de l'air entrait par les skimmers et ressortait aux
  buses de refoulement. L'utilisateur a rempli dans la matinée. **La piscine
  tournait le 04/09 après-midi SANS air** (observation directe) : l'air n'explique
  donc **PAS** les transitoires des 02, 03 et 04/09, qui restent inexpliqués — N1
  garde tout son objet. Ne PAS agréger les chiffres du 05/09 aux autres matins.

  **Mesure de référence utilisateur (appareil jaune), 07:50-08:00 : ~7,35**, contre
  un pH filtré appareil de **7,239** en moyenne sur la même fenêtre — écart
  **+0,11, l'appareil lit bas**. Cet écart **ne dit rien sur l'offset** : mesure
  prise juste après le rinçage ET avec de l'air dans le circuit, soit les deux
  conditions que le protocole N8 dit d'éviter. À refaire en régime stable, eau du
  bassin. Conséquence à surveiller : à 08:11 l'appareil lisait 7,266 (sous la
  consigne) et l'intégrale partait en négatif (−117 pH·s, terme −0,043) — si la
  réalité est 7,35, la régulation accumule dans le mauvais sens.

  **Limite de l'ADR-0059 observée dès le premier matin** : sur 07:50-08:01 le pH
  filtré montait encore à **0,0078 pH/min**, sous le seuil de détection de
  **0,017 pH/min** du détecteur. La porte s'est donc ouverte, légitimement au
  regard de son critère, sur un signal encore en dérive lente. **Premier point de
  données terrain pour la feature-097.**

  Chlore : **424 mL à 08:14** contre 470 mL à la même heure le 04/09 — l'air n'a
  PAS emballé le dosage (plafond journalier 1 500 mL).

  **Prédiction falsifiable de bug-080 lot B-1 : NON TESTÉE, et c'est une erreur de
  conception d'expérience de ma part.** `maxBloc8` est descendu à 21 492 o (contre
  ~30 000 attendus) et `psram_free` n'a bougé que de 184 o sur l'heure — mais la
  consigne était de **ne pas ouvrir l'interface web** pendant la fenêtre, pour ne
  pas déclencher un PANIC. Or l'hypothèse porte précisément sur les chargements
  d'UI : le mécanisme corrigé n'a pas été sollicité. Les deux objectifs étaient
  contradictoires. 21 492 correspond au « nominal fragmenté » de l'ADR-0058, donc
  au régime de fond inchangé — cohérent avec un fragmenteur piloté par l'UI.
  **À trancher par une mesure délibérée** : plusieurs chargements d'UI d'affilée
  en surveillant `psram_free` et `maxBloc8`. Sans risque désormais, le parachute
  étant réparé et prouvé.

  **Test décisif du transitoire : le matin suivant le remplissage.** Si le
  transitoire s'effondre, la cause est mécanique (niveau d'eau) ; s'il tient
  45-50 min, l'air n'était qu'un facteur du 05/09.

- 2026-09-05 11:40 — **QUATRE VERSIONS DÉPLOYÉES D'UN COUP** (l'appareil était
  resté en 2.35.0 toute la journée pendant que la file se construisait).

  | Version | Objet | Nature |
  |---|---|---|
  | **2.36.0** (feature-094) | La décision de session cesse d'obéir au bruit | **change le comportement du bassin** |
  | **2.37.0** (feature-096) | Indicateur « temps d'atteinte de cible », mémoire 7 j, `/get-kpi` | mesure |
  | **2.38.0** (feature-095) | La mesure lissée entre dans l'historique et les courbes | mesure |
  | **2.39.0** (bug-089) | Parité de la trame WS `config` + sentinelle anti-récidive | affichage |

  **Une seule des quatre touche la régulation** : la 2.36.0. Si quelque chose se
  comporte mal, c'est vers elle qu'il faut regarder en premier ; le retour arrière
  vers la 2.35.0 est disponible (binaires archivés dans `builds/`).

  Vérifié sur l'appareil après redémarrage : les 8 champs du KPI répondent,
  `/get-kpi` sert `{"days":[]}` (mémoire vierge, normal), l'historique a conservé
  ses **247 points** avec les 2 nouvelles clés présentes partout (`null` sur les
  points horaires, conforme au choix « RAW seul »), les 15 clés de config
  ajoutées répondent, et le **parachute mémoire est armé en RAM interne**
  (`maxBloc8` 49 140 o au boot).

  **État du bassin au déploiement** : pH filtré **7,52** pour une consigne de
  7,30, filtration en marche. La purge de stabilisation repart au plancher de
  10 min — donc une dizaine de minutes sans dosage après le redémarrage, coût
  assumé du choix de la feature-091.

  **La matinée du 05/09 en chiffres** (enregistreur 60 s, 600 points, 06:50 →
  11:53, archivé dans `builds/2.35.0/`) : le pH filtré est passé de **7,169 à
  7,489** malgré **128 points en session de dosage active**. Le bassin a monté
  pendant qu'on le corrigeait. Cohérent avec la mesure de référence de
  l'utilisateur (~7,35 à l'appareil jaune vers 08:00, contre 7,239 lus par
  l'appareil) et avec l'air dans le circuit : la mesure était **basse**, donc la
  régulation se croyait plus près de la consigne qu'elle ne l'était.
  ⚠️ **Journée contaminée**, à ne pas agréger aux autres matins.

  **Deux specs de suivi ouvertes** par les cycles du jour :
  **bug-098** (le drapeau de dosage ORP est perdu à l'ÉCRITURE de l'historique —
  format persisté, donc irrécupérable quelle que soit la relecture) et
  **bug-099** (le `.tmp` de rotation n'est jamais nettoyé au démarrage — avec un
  tampon plein, la marge de croissance tombe à 90 octets).
  Une troisième piste a été **tranchée sans spec** : l'absence de garde d'âge sur
  l'accesseur de la mesure lissée est **conservée** (une divergence entre
  l'historique et `/data` serait pire), et la nuance est documentée.

  **Marge flash après les quatre : 13 195 o** (99,3 % occupés). C'est désormais
  la contrainte dominante du projet — les lots B-2 et B-3 de bug-080 (~700 o)
  y tiennent, mais plus grand-chose au-delà sans arbitrage.
