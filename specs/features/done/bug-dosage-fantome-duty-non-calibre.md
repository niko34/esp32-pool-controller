# Bug — dosage fantôme : le duty commandé ne fait pas tourner la pompe, mais le volume est compté

- **Type** : bug critique (sécurité chimique / justesse de la régulation)
- **Priorité** : critique — le contrôleur compte de l'acide qu'il n'injecte pas, et le pH ne descend pas
- **Statut** : **livré (3 itérations)** — défaut n°2 corrigé en **v2.20.5**, défaut n°1 corrigé en **v2.21.0** (proportionnel de temps, [ADR-0043](../../../docs/adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)/[ADR-0044](../../../docs/adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md)), défaut n°3 corrigé en **v2.21.1** (UI « commandé » vs « effectif », [ADR-0045](../../../docs/adr/0045-etat-impulsion-expose-en-calendrier.md)). **CLÔTURÉE le 2026-08-28** : la confirmation journal du défaut n°2 est acquise on-target (voir critères), et la concordance bidon 3 jours (P12) a été **transférée vers `feature-075`** — elle valide la justesse des débits, pas la correction du dosage fantôme, et doit suivre la mesure réelle plutôt que la précéder.
- **Version cible** : 2.20.5 (défaut n°2) ✅, 2.21.0 (défaut n°1) ✅, 2.21.1 (défaut n°3) ✅
- **Créée** : 2026-08-22, après signalement utilisateur et diagnostic on-target
- **Domaine** : Régulation pH/ORP, `pump_controller`, comptabilité produit, UI
- **Specs liées** : feature-025 (P temporisée), feature-067 (offset pH)

## Symptôme rapporté

La carte pH affiche « injection en cours » et la pompe **ne tourne pas**. Actionnée
manuellement depuis les paramètres avancés (duty 255), elle tourne normalement — donc
le matériel, le canal LEDC et le câblage sont sains.

L'utilisateur observe par ailleurs que le pH est **quand même** passé de 8,2 à 7,8 dans
la journée : de l'acide entre donc **par intermittence**. Le régime n'est pas « pompe
bloquée » mais « pompe marginale ».

## Défaut n°1 — le duty commandé est au ras du plancher de rotation (PROUVÉ)

`regulation_speed = normal` → `phPID.kp = 8.0`, `ki = kd = 0` (`pump_controller.cpp:62-66`).
Avec une erreur de 0,80 pH : débit calculé = **6,4 ml/min** sur 90 possibles.

`flowToDuty` (`pump_controller.cpp:756-769`) mappe [`kPumpMinFlowMlPerMin` = 5,2 ;
`kPumpMaxFlowMlPerMin` = 90] sur [`MIN_ACTIVE_DUTY` = 80 ; 255]. Soit :

```
duty = 80 + round((6,4 − 5,2) / (90 − 5,2) × 175) = 82 / 255  ≈ 32 %
```

Deux unités au-dessus du plancher. Le test manuel applique 255.

**Vérification arithmétique on-target** : `ph_daily_ml` est passé de 816 (valeur restaurée
au boot de 16:39:20) à 1295,26, soit **479 ml comptés en 75 minutes actives = 6,4 ml/min**
— exactement `dutyToFlow(82)`. La comptabilité intègre donc le **débit commandé**
(`pump_controller.cpp:1704-1706`), jamais un débit mesuré ni un débit plausible.

Conséquence : **le compteur monte sur de l'acide qui ne sort pas du bidon.** Le niveau
affiché, les limites journalières et les alertes de niveau bas reposent sur une fiction.

## Défaut n°2 — double comptage de cycle à chaque démarrage (PROUVÉ)

`computePID` retourne 0 quand `dt > 10 s` (`pump_controller.cpp:732-735`). Or il s'écoule
~29 min entre deux cycles de dosage. Le **premier tick de chaque cycle rend donc toujours
un débit nul** → `active` n'est pas latché (`:1197-1204`, `:1753`) → le tick suivant
repasse par `shouldStartDosing`, incrémente `cyclesToday` une seconde fois et réémet la
ligne de journal (`:1150-1158`).

Trace on-target :
```
[17:39:21] Démarrage dosage pH (auto): pH=8.05 cible=7.20 erreur=0.854 (cycle 3/48)
[17:39:21] Démarrage dosage pH (auto): pH=8.05 cible=7.20 erreur=0.854 (cycle 4/48)
```

Conséquence : le budget de 48 cycles/jour n'en vaut réellement que **24**. (Le « ×4 »
observé à 16:45:19 était 2 pH + 2 ORP, pas une imbrication.)

Hypothèse écartée en cours de diagnostic : ce doublon **ne** consomme **pas** deux fois le
budget horaire — `usedMs` s'accumule sur un drapeau unique, une seule fois par tour
(`:178-182`). 16:45:19 → 17:10:19 fait exactement 25 min.

## Défaut n°3 — conflation dans l'interface

`ph_dosing` = `phDosingState.active` (auto, `web_routes_data.cpp:133`) reflète l'**intention**
(un duty est commandé), pas la sortie réelle. `ph_inject_remaining_s` =
`manualInjectRemainingS` (`:139`) ne concerne que l'injection **manuelle minutée**, donc
nul en automatique par construction. `data/app.js:4434` fait un OU des deux.

D'où l'affichage « injection en cours » alors que rien ne coule visiblement. Il n'y a pas
de contradiction interne, mais l'UI ne distingue pas « commandé » de « effectif ».

## Faux positifs écartés

- `ph_limit_reached = False` est **correct** : la limite journalière en vigueur est
  2000 ml (réglée par l'utilisateur), pas 1000. `checkSafetyLimits` fonctionne.
- Le rebouclage de la limite horaire (25 min/h puis 35 min de suspension) est le
  **comportement nominal** d'une fenêtre glissante ancrée au boot, pas un bug. Il est la
  *conséquence* du fait que le pH ne descend pas.

## LA QUESTION DE CONCEPTION À TRANCHER PAR `pool-chemistry`

Une pompe doseuse péristaltique n'a **pas** un débit proportionnel au rapport cyclique en
bas de plage : sous un certain seuil elle cale, et autour de ce seuil son débit est
erratique. Le modèle linéaire de `flowToDuty` suppose l'inverse.

Trois options, à arbitrer :

**(A) Recalibrer `MIN_ACTIVE_DUTY` et `kPumpMinFlowMlPerMin`** sur le matériel réel.
Simple, mais ne supprime pas la zone erratique juste au-dessus du seuil, et exige une
mesure au banc (volume recueilli à duty fixe, plusieurs points).

**(B) Passer en proportionnel de TEMPS** : la pompe tourne à **pleine vitesse** pendant une
fraction de la période, au lieu d'un duty analogique réduit. C'est la commande normale
d'une doseuse, et elle rend le débit réellement proportionnel à la consigne. Coût : refonte
de `flowToDuty` et de la comptabilité, et interaction à revoir avec la limite horaire et
`MIN_ACTIVE_DUTY`.

**(C) Plancher de débit efficace** : refuser d'injecter sous un débit calculé jugé non
réalisable, et attendre que l'erreur justifie un duty franc. Conservateur, mais retarde la
correction quand l'erreur est faible.

⚠️ Aucune de ces options ne peut être validée sans **mesure au banc** du seuil de rotation
réel et du débit à quelques duty. Le correctif logiciel seul risquerait de remplacer un
modèle faux par un autre.

## Correctif attendu

1. **Défaut n°2** (indépendant, purement logiciel) : ne pas consommer un cycle quand le
   premier tick rend un débit nul — ou amorcer `pid.lastTime` à l'entrée du cycle pour que
   le premier `dt` soit exploitable. Testable en natif.
2. **Défaut n°1** : appliquer l'option retenue par le gate, après mesure au banc.
3. **Comptabilité** : ne plus créditer de volume sur un duty dont on ne sait pas s'il
   produit un débit. Au minimum, garde de plausibilité et journalisation distincte du
   « volume commandé » et du « volume estimé réalisable ».
4. **Défaut n°3** : distinguer dans `/data` et dans l'UI le duty **commandé** de la sortie
   **effective**, et cesser de mélanger l'injection manuelle minutée avec l'automatique.

## Invariants à préserver

- Limites journalières et horaires, présence d'eau, watchdog : intouchés.
- Aucun chemin ne doit pouvoir augmenter le débit réel au-delà de `pump1_max_duty_pct`.
- Un correctif qui augmenterait le duty **doit** être validé par `pool-chemistry` : passer
  de 32 % à un duty franc multiplie le débit réel par un facteur inconnu, avec un risque
  de **surdosage d'acide**. C'est le risque principal de ce cycle.
- `regulation_speed` reste un réglage utilisateur, pas un contournement du défaut.

## VERDICT DU GATE `pool-chemistry` (2026-08-22)

**NO GO sur toute modification du duty avant la condition C0.** NO GO sur l'option (A)
seule. GO SOUS CONDITIONS sur **(B) + (C)**. GO SOUS CONDITION sur le défaut n°2 livré
séparément.

### C0 — LEVÉE le 2026-08-22 (réponse de l'utilisateur)
Le gate exigeait de vérifier le pH réel au réactif avant tout code, tout le diagnostic
reposant sur l'offset de +0,50 : si le pH réel avait été 7,3-7,5, l'erreur de 0,80 aurait
été fictive et le correctif aurait été un recalibrage, pas un duty plus franc.

**L'offset a été établi empiriquement par l'utilisateur avant l'ajout de la
fonctionnalité, avec TROIS instruments indépendants : PoolLab (photomètre), réactif
colorimétrique et pH-mètre autonome sur pile.** C'est une base plus solide que le contrôle
ponctuel demandé. La condition est donc satisfaite : la valeur corrigée est la valeur vraie,
l'écart de 0,7-0,8 avec la consigne est **réel**, et le bassin a effectivement besoin
d'acide.

**HORS PÉRIMÈTRE, à ne pas rouvrir** : la fonctionnalité d'offset (feature-067, ADR-0039)
et sa valeur. Aucune option de ce correctif ne doit la modifier, la contourner ni la
compenser. C0 portait sur la *fidélité de la mesure*, jamais sur le mécanisme.

Réserve mineure, non bloquante : cette validation précède la fonctionnalité, et une
électrode dérive avec le temps. Un unique point de contrôle au PoolLab confirmerait
l'absence de dérive depuis — deux minutes, à faire à l'occasion, pas un préalable.

### Correction d'arithmétique du brief
Le clamp `pump1_max_duty_pct` est appliqué **en amont** de la comptabilité
(`pump_controller.cpp:1678-1681`) : duty max = 127 → modèle = **28 ml/min**, plafond horaire
**700 ml/h** et non 1,1 L/h.

### P1 — option retenue : (B) + (C)
(A) seule refusée : elle conserve un modèle linéaire faux, et surtout **si le seuil de
rotation mesuré dépasse 127**, alors `MIN_ACTIVE_DUTY > maxDuty` → `dutyToFlow` rend 0 →
**régulation silencieusement morte**. Piège majeur à couvrir.
(B) proportionnel de temps à un duty franc mesuré : seule option où le volume crédité vaut
*débit mesuré constant × temps*, donc traçable. (C) plancher de refus pour les consignes
sous le pouls minimal.
Conditions : **B1** `pump1_max_duty_pct` devient un plafond de *fraction de période*, pas de
PWM, sinon le duty franc est inatteignable. **B2** aucun clamp ne doit ramener le duty sous
le seuil de rotation — ce cas refuse et émet un `critical`. **B3** pouls minimal ≥ 5 s,
période fixe.

### P2 — mesure au banc exigée (à exécuter par l'utilisateur)
Eau claire, pas d'acide. Tubulure et hauteurs réelles. `pump1_max_duty_pct` porté
temporairement à 100, restauré ensuite.
Duty à tester : **80, 96, 112, 127, 160, 192, 224, 255**. Durée 120 s pour duty ≤ 127,
60 s au-delà. Volume **pesé** (1 g = 1 ml, balance au gramme). **3 répétitions** sur
80/96/112/127, une seule au-delà. Noter rotation continue oui/non.
Livrables : `Dmin` = plus petit duty avec 3/3 rotations continues ; `Dfranc` = plus petit
duty avec dispersion < 10 %, **plus 16 counts de marge**.

### P3 — limites abaissées pendant la validation : OUI
`max_ph_ml_per_day` 2000 → **300** ; `ph_limit_minutes` 25 → **6** ; `max_cycles_per_day`
→ **12**. 300 ml borne l'écart à ~0,1-0,15 pH sur 40 m³. Restaurer après 3 jours de
concordance entre le compteur et le niveau réel du bidon.

### P4 — comptabilité
Continuer à créditer le duty commandé (la surestimation est fail-safe), **jamais** créditer
zéro sur une pompe alimentée. Sous `Dmin`, **refuser** l'injection plutôt que compter faux.
Afficher un badge « débit non caractérisé » et graduer le bidon (relevé daté comparé à
`ph_daily_ml`) — seule vérité terrain sans débitmètre.

### P5 — défaut n°2 livrable immédiatement, sous condition
À condition de passer `max_cycles_per_day` **48 → 24 dans le même commit** : l'enveloppe
réellement vécue est 24, la restaurer à 48 doublerait l'exposition sous couvert de correctif.
Incrémenter `cyclesToday` **seulement si `flow > 0`** — ne pas synthétiser un `dt`, ce serait
un piège latent si `ki` devenait non nul.

### P6 — urgence : aucune
À pH vrai 7,3-7,5 : HOCl 52-62 %, confort et chlore corrects. À pH vrai 8,0 : HOCl ~22 %,
chlore 2,5× moins actif, surconsommation ORP et risque d'entartrage. Quelques jours sont
tolérables si le chlore libre tient et l'eau reste claire. **C0 tranche laquelle des deux
situations est réelle.**

## Critères d'acceptation

- [x] Un cycle de dosage ne consomme **qu'un** cycle (défaut n°2), vérifié en natif et au journal
  → **natif ✅** (4 tests dédiés, dont le rejeu du scénario réel) ; **journal on-target ✅ (2026-08-28)** :
  ```
  07:01:29  Démarrage dosage pH (auto): pH=7.35 cible=7.30 erreur=0.051 (cycle 1/48)
  07:04:29  [Dosage pH] Dosage par accumulation : impulsion minimale de 5 s (2.4 mL cumulés, résidu 0.1 mL)
  ```
  Une seule ligne « Démarrage dosage » pour la séance, et l'impulsion suivante
  **n'incrémente pas** le compteur : il reste à `1/48`. C'est précisément le
  défaut n°2 — auparavant chaque impulsion consommait un cycle du quota
  journalier, et l'anti-rafale finissait par bloquer le mécanisme.
- [x] Le seuil de rotation réel de la pompe est **mesuré** et consigné → mesure du 2026-08-23 (D = 82 ; 12,0 ml/min à 82 ; 61,0 ml/min à 255), voir « Mesure au banc »
- [x] Le volume comptabilisé correspond à un débit réellement réalisable → **v2.21.0** : proportionnel de temps, volume = débits **pesés** (kick à débit-255, plateau au débit d'amplitude calibrée, `pulseDeltaVolumeMl`) ; fail-closed sans pesée ; manuel crédité au majorant. Validation terrain (3 jours de concordance bidon/compteur) : voir « Mode opératoire post-déploiement »
- [x] L'UI distingue « commandé » de « effectif » → **v2.21.1** (itération 3) : 7 champs de calendrier d'impulsion identiques `/data`/WS ([ADR-0045](../../../docs/adr/0045-etat-impulsion-expose-en-calendrier.md)), cartes pH/ORP à 6 priorités (« Injection en cours » = pompe réellement alimentée ; « Prochaine impulsion : Ns » pendant les repos ; injection manuelle distincte), tests 625/625, revue Approved, visa `pool-chemistry` GO
- [x] Verdict `pool-chemistry` GO explicite sur le risque de surdosage → GO sous conditions **(B)+(C)** pour le défaut n°1, **re-confirmé en contre-visite le 2026-08-23 (GO)** après mesure et amendement amplitude ; **GO sous condition P5** pour le défaut n°2, condition **remplie** (`maxCyclesPerDay` 48 → 24 dans le même commit)
- [x] `pio run` bi-core verts, `pio test -e native` sans régression (suite `test_native_dosing`)
  → itération 2 : compilation ✅, natif ✅ **625 tests** (dont la nouvelle suite `pulse_dosing_logic`), zéro régression ; revue de code **Approved** (les bloquants portaient sur les docs, levés le 2026-08-23)

## Suivi / itérations

### Itération 1 — défaut n°2 (double comptage) — v2.20.5, 2026-08-22

**Statut : livré — tests natifs verts (609), revue OK, gate `pool-chemistry` P5 satisfaite.**

Ce qui est livré :

1. **Comptabilisation différée du démarrage de cycle** (branches `automatic` pH **et** ORP). Les quatre effets — `cyclesToday++`, ring anti-rafale, ligne `INFO Démarrage dosage`, `lastStartTime` — sont déplacés **après** le calcul du débit et conditionnés au **même prédicat** que le latch de `active`. Invariant désormais **structurel** : *cycle compté ⇔ cycle réellement démarré*. `computePID` et `pid.lastTime` sont **intacts** — **aucun `dt` n'est synthétisé** (prescription P5 : un `dt` inventé serait un piège latent si `ki` devenait non nul).
2. **Logique extraite en pur** : `evaluateCycleStartAccounting(wasActive, startDecided, flowActive)` dans `src/dosing_logic.{h,cpp}` (`countCycleStart = !wasActive && startDecided && flowActive`). **4 tests natifs** ajoutés, dont le rejeu du scénario réel traversant la garde des 10 s.
3. **`maxCyclesPerDay` : défaut 48 → 24** (condition P5, validée explicitement par l'utilisateur). ⚠️ Ce défaut de struct n'est qu'un **repli** : `max_cycles_day` étant persisté en NVS, un appareil déjà provisionné **conserve sa valeur**. Aucune migration n'a été écrite. La valeur a été portée à 24 **sur l'appareil de production via `/save-config`** (vérifié : 48 → 24, autres limites inchangées) ; tout autre appareil provisionné doit être réglé à la main.
4. **Nouveau `WARN` « démarrage décidé mais aucun débit commandé »** (pH et ORP), throttle `kDosingNoFlowWarnIntervalMs = 5 min`. Motif : avant le correctif, une régulation sans effet réémettait une ligne de démarrage à chaque tour — bruit involontaire, mais **c'est lui qui a révélé le bug** ; elle serait désormais silencieuse. Le WARN rend la situation explicite sans réintroduire l'inondation du journal corrigée en 2.20.2.
5. **Nouveau paramètre `?duty=N` sur `POST /pump1/on` et `/pump2/on`** — optionnel, défaut `MAX_PWM_DUTY` (255) donc comportement historique inchangé sans lui, borné `[0, 255]` et **volontairement pas** borné par `MIN_ACTIVE_DUTY` (sonder *sous* le plancher est l'objet de la mesure). `duty=0` ≡ `/off`. Sûreté : ne peut que **réduire** la puissance par rapport au 255 déjà atteignable ; toutes les gardes restent en amont (auth `WRITE`, présence d'eau, inhibition maintenance).

Fichiers modifiés : `src/pump_controller.cpp`, `src/dosing_logic.h`, `src/dosing_logic.cpp`, `src/config.h`, `src/constants.h`, `src/web_routes_control.cpp`, `src/version.h`, `test/test_native_dosing/test_dosing_decision.cpp`.
Docs : `docs/API.md`, `docs/subsystems/pump-controller.md`, `docs/subsystems/logger.md`, `CHANGELOG.md`. **Pas d'ADR** — correction locale, tranchée par le gate dans le cadre existant ; l'arbitrage (B)+(C) fera l'objet d'un ADR **quand il sera implémenté**.

⚠️ **Vocabulaire** : cette itération ne répare **pas** « la pompe qui ne tourne pas ». Ce symptôme relève du **défaut n°1**, non corrigé : le régulateur commande 82/255 (~32 %) pour une erreur de 0,8 pH, valeur à laquelle la pompe ne tourne que par intermittence, alors que le modèle duty↔débit la crédite de 6,4 mL/min — d'où un volume compté sur de l'acide qui ne sort pas.

### Itération 2 — défaut n°1 : proportionnel de temps — v2.21.0, 2026-08-23

**Statut : livré — tests natifs verts (625), revue OK (Approved code ; les Request changes portaient sur les docs, levés), contre-visite `pool-chemistry` GO.**

**Décision de périmètre (2026-08-23)** : le **défaut n°3** (UI « commandé » vs « effectif ») **sort de ce cycle** et devient l'**itération 3**. L'itération 2 couvre le défaut n°1 seul (modèle duty↔débit + comptabilité associée).

**Re-arbitrage post-mesure** : le NO GO sur l'option (A) reposait sur l'hypothèse `Dmin > 127`, invalidée par la mesure (D = 82 < 127) — mais (A)+(C) restait une **régulation morte** : avec `kp = 8`, les demandes typiques (~6,4 ml/min) sont sous le plancher physique mesuré (12-16,5 ml/min). D'où (B)+(C) confirmé, avec l'**amendement utilisateur** : amplitude réglable (plancher 40 %) + kick 400 ms. Arbitrage complet : [ADR-0043](../../../docs/adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md) ; bascule du budget horaire en temps pompe-ON : [ADR-0044](../../../docs/adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md).

Ce qui est livré :

1. **Modèle proportionnel de temps** (`src/pulse_dosing_logic.{h,cpp}`, pur ADR-0017) : période 60 s (`kPulsePeriodMs`), pouls min 5 s (`kPulseMinMs`, B3), **kick 400 ms à duty 255** (`kPulseKickMs`) puis plateau à l'amplitude (= `pumpN_max_duty_pct × 255/100`, sémantique PWM conservée, borné **[40..100] %** serveur + UI, `kPumpAmpMinPct`). `MIN_ACTIVE_DUTY` 80 → **98** (Dmin 82 + 16 counts marge P2). Condition B2 : amplitude sous plancher → refus + `critical`, jamais de clamp.
2. **Calibration fail-closed PAR pompe** : `pumpN_calibrated_duty` (0 = non caractérisé) + `pumpN_pulse_flow_ml_per_min`, NVS `p1_cal_duty`/`p1_pulse_flow`/`p2_cal_duty`/`p2_pulse_flow` ; ancienne clé partagée `pump_max_flow` supprimée **SANS conversion**. Amplitude ≠ duty calibré ou `cal_duty = 0` → dosage auto/programmé **REFUSÉ** + `critical` throttlé 30 min + badge « Débit non caractérisé — pesée requise ». **AUCUNE interpolation** — `dutyToFlow`/`flowToDuty` et `kPumpMin/MaxFlowMlPerMin` **SUPPRIMÉS**. Écart tube +10 % mesuré entre pompes identiques → calibration par pompe justifiée.
3. **Comptabilité** : `V = 0,4 s × flow255 + reste × Famp` (`pulseDeltaVolumeMl`, par timestamps, exacte au jitter près — limite connue : transition kick→plateau vue au premier tick après 400 ms, sous-comptage ~0,1 ml/pouls). Manuel `?duty=N` : crédit au **majorant** flow255 sauf duty == calibré. Injection volumée : duty calibré ou 255 forcé, **409 `flow_uncharacterized`** si incohérent. Constantes de banc : `kPump1Flow255MlPerMin = 61,0` / `kPump2Flow255MlPerMin = 67,0`.
4. **Seuil C (deadband)** : demande < Famp × 5/60 → refus, `info` throttlé 30 min, **ne nourrit pas** le WARN no-flow (ni le refus de gate — chacun a son émetteur dédié).
5. **Limite horaire (`usedMs`) ET temps min d'injection sur le temps POMPE-ON** (impulsions), plus sur la session — validé gate (« minutes où le produit coule »), [ADR-0044](../../../docs/adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md). **Cycle = session** (les impulsions n'incrémentent ni `cyclesToday` ni le ring anti-rafale) ; pause mélange armée en **fin de session**, y compris fin par deadband.
6. **HTTP/UI** : `/save-config` accepte les 4 clés de calibration (validation stricte, 400 explicite — patron `ph_offset`) ; `/get-config` les expose ; `/data` expose `ph/orp_pump_flow_uncharacterized` + `ph/orp_pump_amplitude_duty` ; `pump_max_flow_ml_per_min` entrant **ignoré** (rétrocompat) ; slider puissance max borné 40 % ; formulaire de pesée par pompe dans Réglages → Avancé (duty d'exploitation affiché à côté du %).

Fichiers modifiés : `src/pulse_dosing_logic.h` (**nouveau**), `src/pulse_dosing_logic.cpp` (**nouveau**), `src/pump_controller.cpp`, `src/pump_controller.h`, `src/config.h`, `src/config.cpp`, `src/constants.h`, `src/web_routes_config.cpp`, `src/web_routes_control.cpp`, `src/web_routes_data.cpp`, `data/index.html`, `data/app.js`, `data/app.css`, `src/version.h`, `test/test_native_dosing/test_dosing_decision.cpp`.
Docs : **ADR-0043** + **ADR-0044** (nouveaux), `docs/adr/README.md`, `docs/README.md`, `docs/subsystems/pump-controller.md` (section « Dosage en proportionnel de temps », purge du modèle linéaire), `docs/API.md`, `docs/features/page-settings.md`, `docs/features/page-dosages.md`, `docs/subsystems/logger.md`, `CHANGELOG.md` ([2.21.0]).

#### Mode opératoire post-déploiement (P8-P12) — à exécuter par l'utilisateur

1. **P8** — vérifier `pump1_max_duty_pct = 50` (duty d'exploitation affiché : 127) ; le serveur a re-clampé à 40 minimum le cas échéant.
2. **P9** — **peser 120 s à duty 127** les **deux** pompes (`POST /pumpN/on?duty=127`, eau claire, tubulure réelle, balance au gramme).
3. **P10** — saisir les deux pesées dans Réglages → Avancé (duty 127 + débit mesuré) → les badges « Débit non caractérisé » disparaissent, le dosage auto redevient possible.
4. **P11** — limites abaissées **P3** pendant la validation, pour les **deux** produits : `max_ph/orp_ml_per_day` → **300 mL**, limites horaires → **6 min (pH) / 12 min (ORP)**, `max_cycles_per_day` → **12**.
5. **P12** — **3 jours de concordance** entre le compteur (`ph/orp_daily_ml`) et le niveau réel des bidons (bidon gradué, relevé daté), puis **restaurer** les limites d'exploitation.

### Itération 2 (préparation — archives)

#### Mesure au banc — 2026-08-23

Procédure **minimale** (deux pesées), exécutée par l'utilisateur — PAS la campagne P2 complète (8 duty × 3 répétitions).

- **D = 82** : plus petit duty à rotation **continue et franche** (balayage `?duty=` : 255, 82, 110, 82)
- **Pesée A** : duty 82, 120 s → **24 g** → **12,0 ml/min**
- **Pesée B** : duty 255, 60 s → **61 g** → **61,0 ml/min**
- Conditions : eau claire, tubulure et hauteurs d'aspiration/refoulement **réelles**, balance au gramme, 2 pesées.

**Nuance importante** : au banc, duty 82 tourne franchement en rotation **ENTRETENUE** ; or le symptôme terrain était un **non-démarrage** à 82 — couple de **décollage à froid** possible. La question de la **marge** (les +16 counts de P2) est à trancher par `pool-chemistry`.

**Écarts au modèle actuel** : à duty 82 le modèle crédite 6,4 ml/min, la pompe débite **12,0** (sous-estimation ×1,9) ; à 255 le modèle suppose 90 ml/min, mesuré **61,0** (surestimation ×1,5). Le modèle linéaire actuel est faux aux deux extrémités.

#### Procédure minimale — deux pesées

Elle suffit à fixer les trois constantes du modèle. La campagne complète de P2 (8 duty, 3 répétitions) reste la version rigoureuse ; ces deux pesées en sont le strict nécessaire.

Préparation :

- **Eau claire, jamais d'acide.** Tubulure et hauteurs d'aspiration/refoulement **réelles** (le débit d'une péristaltique en dépend).
- Balance au **gramme** (1 g = 1 mL). Récipient tare comprise.
- `pump1_max_duty_pct` porté temporairement à **100** — sinon le clamp écrête le duty demandé et fausse la pesée. **À restaurer ensuite.**
- Filtration en marche (la garde de présence d'eau refuse le démarrage sinon).

**Pesée A — au duty plancher retenu (`Dmin`)** : le plus petit duty auquel la pompe tourne **en continu** (rotation ininterrompue à l'œil, pas de saccade). Le trouver par tâtonnement décroissant : 255 → 192 → 160 → 127 → 112 → 96 → 80 → 64…, quelques secondes par palier, en notant le premier duty qui **cale ou saccade** ; `Dmin` est le palier juste au-dessus.

```bash
# remplacer 96 par le Dmin candidat
curl -u admin:MDP -X POST "http://poolcontroller.local/pump1/on?duty=96"
sleep 120                     # 120 s pour un duty bas (volume mesurable)
curl -u admin:MDP -X POST http://poolcontroller.local/pump1/off
```

**Pesée B — à 255** :

```bash
curl -u admin:MDP -X POST "http://poolcontroller.local/pump1/on?duty=255"
sleep 60
curl -u admin:MDP -X POST http://poolcontroller.local/pump1/off
```

À consigner : duty, durée, masse recueillie, rotation continue oui/non, hauteurs, date.

#### Ce que les deux pesées fixent

> ⚠️ **Table PÉRIMÉE, conservée pour l'historique** — elle datait d'avant l'arbitrage (B)+(C). Destinations réelles en v2.21.0 : `MIN_ACTIVE_DUTY = 98` (Dmin 82 + 16 counts marge P2, `src/config.h`) ; masse B ÷ durée B → `kPump1/2Flow255MlPerMin` (`src/constants.h`) ; la pesée au duty d'exploitation → `pumpN_calibrated_duty` + `pumpN_pulse_flow_ml_per_min` (config utilisateur). `kPumpMinFlowMlPerMin` et `pump_max_flow_ml_per_min` **n'existent plus**.

| Grandeur | Déduite de | Destination (périmée) |
|---|---|---|
| `MIN_ACTIVE_DUTY` | `Dmin` (pesée A) | `src/config.h` |
| ~~`kPumpMinFlowMlPerMin`~~ | masse A ÷ durée A, ramenée en mL/min | ~~`src/constants.h`~~ (supprimée) |
| ~~`pump_max_flow_ml_per_min`~~ | masse B ÷ durée B, ramenée en mL/min | → `kPumpNFlow255MlPerMin` (constantes) |

#### Pièges identifiés à la préparation — tous traités en v2.21.0

- ✅ **`Dmin` > duty max configuré** → traité par la condition **B2** : `evaluatePulseGate` refuse (`AmplitudeBelowMinActive`) + `critical` throttlé — jamais de dégradation silencieuse (`dutyToFlow` n'existe plus).
- ✅ **Condition B1** — amendée en accord avec le gate : `pumpN_max_duty_pct` reste une **amplitude PWM** (le plateau du pouls), bornée [40..100] % ; c'est la **durée du pouls** qui réalise la fraction de période. Le duty franc est garanti atteignable par le plancher 40 % ≥ `MIN_ACTIVE_DUTY`.
- ✅ **Condition B3** — pouls minimal 5 s (`kPulseMinMs`), période fixe 60 s (`kPulsePeriodMs`).
- ⏳ **P3** — limites abaissées pendant la validation : à exécuter par l'utilisateur (voir « Mode opératoire post-déploiement (P8-P12) » ci-dessus), restaurer après **3 jours** de concordance bidon/compteur.
- ✅ `pump1_max_duty_pct` : plus besoin de le porter à 100 pour peser — la pesée se fait au duty d'exploitation via `?duty=N`.

### Itération 3 — défaut n°3 : UI « commandé » vs « effectif » — v2.21.1, 2026-08-23

**Statut : livré — tests verts (625/625), revue OK (Approved), visa `pool-chemistry` GO (lecture seule, aucun chemin de dosage touché).** Arbitrage d'exposition : [ADR-0045](../../../docs/adr/0045-etat-impulsion-expose-en-calendrier.md) (calendrier plutôt que booléen instantané — aliasing à 5 s de push pour des impulsions de 5-60 s ; `/data` seul à 30 s trop lent).

Ce qui est livré :

1. **3 getters lecture seule `PumpController`** (par chimie, chem 0 = pH / 1 = ORP) : `isPulseSessionActive(chem)`, `getPulseMs(chem)`, `getPulsePhaseMs(chem, nowMs)` (soustraction uint32 wrap-safe, clampée [0..`kPulsePeriodMs`]). Aucun setter, gardes d'index **fail-closed** (`false`/`0`), course bénigne documentée (patron `isAnyPumpEnergized` — écriture loopTask / lecture async_tcp, lectures 8/32 bits atomiques Xtensa).
2. **7 champs JSON identiques `GET /data` et push WS (5 s)** : `pulse_period_ms` (u32, période du train = `kPulsePeriodMs`), `ph/orp_pulse_session` (bool, séance ouverte — auto OU scheduled), `ph/orp_pulse_ms` (u32, durée d'impulsion planifiée, **0 = deadband**), `ph/orp_pulse_phase_ms` (u32, position **RELATIVE** dans la période, clampée [0..période] — jamais un `millis()` absolu). Buffers : `/data` 1536 → 1792 o, pool WS 2176 → 2432 o, **`kWsFrameBufBytes` 3072 → 3584** (trame mesurée ~2,2 Ko / 3584).
3. **Cartes pH/ORP du tableau de bord — 6 priorités d'affichage** (`updateDosingStatus`, app.js) : 1) « Injection manuelle : Ns » (dot animé) ; 2) « Injection en cours » (dot animé — la pompe TOURNE, `phase < pulse_ms`) ; 3) « Prochaine impulsion : Ns » (dot **statique**, compte à rebours local ancré `dosingAnchor`, resynchronisé à chaque trame ; à l'échéance sans donnée → « Prochaine impulsion… », **jamais de bascule optimiste**) ; 4) « Dosage en attente : Ns » (deadband) ; 5) motifs existants (Stabilisation, Pause mélange…) ; 6) rien. Bascule locale **RESTRICTIVE** impulsion→attente quand la phase ancrée dépasse `pulse_ms` (correctif revue) ; le sens inverse exige une donnée serveur. Un seul `setInterval` 1 s partagé pH+ORP. **Rétrocompat** : champs absents → comportement historique.
4. **`_updateInjectButtons` harmonisé restrictif** : la désactivation proactive du bouton « Calibrer la sonde » pH inclut la séance auto (`ph_dosing === true`), comme l'ORP le faisait déjà ; les OU de verrouillage calibration conservés et commentés (verrous fonctionnels voulus, distincts de la conflation d'affichage corrigée).

Fichiers modifiés : `src/pump_controller.h`, `src/pump_controller.cpp`, `src/ws_manager.cpp`, `src/web_routes_data.cpp`, `src/constants.h` (`kWsFrameBufBytes`), `data/app.js`, `data/app.css` (`.is-waiting`, dot statique), `src/version.h`.
Docs : **ADR-0045** (nouveau) + index `docs/adr/README.md` / `docs/README.md`, `docs/subsystems/ws-manager.md` (contrat des 7 champs + buffer 3584), `docs/API.md`, `docs/features/page-dashboard.md` (tableau des 6 états, sémantique du dot — présent = séance ; animé = pompe qui tourne ; statique = silence volontaire —, honnêteté du compte à rebours ±1-2 s), `docs/features/page-ph.md` (verrou Calibrer harmonisé), `docs/subsystems/pump-controller.md` (les 3 getters), `CHANGELOG.md` ([2.21.1]).

**Suivis opérationnels restants (motif du maintien en `doing/`)** : confirmation au journal du défaut n°2 (une seule ligne « Démarrage dosage » par cycle, au prochain cycle auto) ; P12 — 3 jours de concordance bidon/compteur puis restauration des limites d'exploitation.

## Note de clôture (2026-08-28)

Les **trois défauts** sont corrigés, testés, revus et désormais confirmés sur
l'appareil :

| Défaut | Correctif | Confirmation |
|---|---|---|
| n°1 — volume compté sur un débit non réalisable | v2.21.0, proportionnel de temps (ADR-0043/0044) | dosage réel observé le 28/08, impulsions à l'amplitude calibrée |
| n°2 — une impulsion consommait un cycle | v2.20.5 | **journal on-target du 28/08** : impulsion sans incrément, compteur figé à `1/48` |
| n°3 — l'UI annonçait « injection » pendant les silences | v2.21.1, calendrier d'impulsion (ADR-0045) | validé par l'utilisateur à la livraison |

**Ce que cette spec a établi et qui dépasse son périmètre** : le modèle linéaire
duty ↔ débit était faux d'un facteur 1,9 en bas d'échelle et 1,5 en haut (mesure
au banc du 23/08). Sa suppression a entraîné la calibration par pompe, puis la
calibration de type pour la flotte (feature-074), puis l'assistant de mesure
(feature-075). Le fil est parti d'un symptôme d'affichage — « la carte dit
injection, la pompe ne tourne pas » — et a fini par changer le modèle de dosage.

**Leçon de mise en service consignée** : le premier dosage réel a tourné ~35 min
avec un bypass de sondes fermé et les limites hautes. Les mesures n'étaient pas
représentatives et 765/802 mL ont été comptés — bornés, sans danger. La garde de
présence d'eau ne voit pas un bypass fermé. Les limites de validation se posent
**avant** la mise en service, pas après.

**Reste à faire, ailleurs** : P12 (concordance bidon/compteur 3 jours) et la
restauration des limites d'exploitation (P3) sont portées par
`feature-075-assistant-mesure-debit-pompe.md`.
