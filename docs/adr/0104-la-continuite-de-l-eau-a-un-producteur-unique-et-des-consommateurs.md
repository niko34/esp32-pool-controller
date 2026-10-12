# ADR-0104 — La continuité de l'eau a un producteur unique et des consommateurs

- **Statut** : Accepté
- **Date** : 2026-10-12
- **Décideurs** : `pool-chemistry` (GO sous 13 conditions, gate de conception), `architect`, `code-reviewer`
- **Spec(s) liée(s)** : [`feature-158`](../../specs/features/done/feature-158-duree-de-filtration-survit-au-redemarrage.md), feature-139 (chemin de dispense unique), feature-155 (garde des 10 min sur la correction pH), feature-157 (lot où le défaut a été constaté en production)

## Contexte

`filtration_elapsed_s`, servi par `/data` et la trame WebSocket, repartait de
**zéro** à chaque redémarrage logiciel — alors même que la pompe de
filtration n'avait jamais cessé de tourner. Relevé en production le
2026-10-12 : 1 619 s de durée de filtration annoncée contre 1 644 s
d'uptime, pour une circulation réelle d'environ 5 h. **`filtration_elapsed_s`
mesurait l'appareil, pas la filtration.**

Conséquence chimique directe : la garde des 10 minutes de la carte
« Correction manuelle de la mesure » pH (`kPhTuneMinFiltrationMs`,
feature-155) se réarmait à chaque déploiement OTA, y compris quand la
filtration tournait depuis des heures — une garde qui protège un **régime
hydraulique établi** n'a aucune raison physique de redémarrer sur un
redémarrage logiciel de quelques secondes.

Le projet avait **déjà** résolu un problème jumeau : `measure_validity`
(feature-139, [ADR-0089](0089-ce-qu-un-redemarrage-a-le-droit-doublier.md))
porte l'horodatage d'un arrêt d'eau à travers un reboot, en mémoire RTC
scellée, et calcule un **verdict de continuité** (`OutageContinuityVerdict`)
qui dit si l'arrêt est assez court pour dispenser la purge de 10 min des
mesures pH/ORP. La question structurante : la filtration doit-elle
**consommer** ce verdict, ou doit-elle produire le sien ?

Une lecture rapide de la spec affirmait initialement « le remède existe
déjà, c'est un raccordement » — **vérification faite**, c'était à moitié
faux : le verdict de continuité dit « cet arrêt a duré N secondes et il est
dispensé », il ne dit **pas** « la filtration tournait depuis 5 heures ».
Cette seconde grandeur — une **durée mesurée** de cycle, pas un **verdict**
de continuité — n'existait nulle part et ne survivait à aucun redémarrage.

## Décision

**`measure_validity` reste le producteur unique du verdict de continuité de
l'eau. `filtration` en est un consommateur.**

La frontière, formulée pour être opposable en revue (verdict `pool-chemistry`,
reprise mot pour mot de la spec feature-158) :

> `measure_validity` est le **producteur unique** du verdict de continuité de
> l'eau. `filtration` en est un **consommateur**. Elle persiste une
> **donnée** (des secondes mesurées) ; elle ne persiste aucun **verdict** et
> n'en recalcule aucun. Aucune comparaison de durée de coupure ne doit
> apparaître dans `filtration.*`.

Concrètement :

1. **Un verdict, un seul calcul.** `measureValidity.begin()` calcule
   `OutageContinuityVerdict` (durée de la coupure, dispense accordée ou non)
   exactement comme avant ce lot. `MeasureValidityManager::bootContinuityVerdict()`
   expose ce verdict **déjà calculé**, en lecture seule, à `setup()` — aucune
   seconde comparaison « durée de coupure < grâce » n'existe où que ce soit
   dans `filtration.*`. Test de non-régression explicite : `grep` de
   `kWaterStopGraceS`, `outageS`, `graceS` dans `src/filtration.*` doit rendre
   zéro occurrence hors commentaire.
2. **Une donnée nouvelle, propre à la filtration, persistée séparément.**
   `filtration.cpp` porte son propre enregistrement RTC scellé
   (`FiltrationRtcRecord`, magic `"FLTR"` — distinct de `"PURG"`,
   `measure_validity.cpp`) qui ne contient que des faits que la filtration
   seule peut attester : `carryS` (secondes **réellement mesurées**, pompe
   commandée ON et appareil vivant — jamais la durée de la coupure),
   `filtrationRunning`, `installMode`. **Persister une donnée n'est pas
   produire un verdict** : la filtration ne décide jamais seule si une
   coupure est « courte » ; elle se contente de savoir, pour sa part, combien
   de temps elle tournait avant.
3. **La décision de recréditer reste une fonction pure dédiée**
   (`evaluateFiltrationResume()`, `src/filtration_resume_logic.{h,cpp}`), qui
   **lit** le verdict de `measure_validity` (`in.verdict->waive`) sans jamais
   le recalculer, et qui refuse tout recrédit si ce verdict n'est pas
   disponible (condition 13 du gate — drapeau « verdict produit » explicite,
   `critical` bruyant en cas d'ordre d'initialisation inversé plutôt qu'un
   report deviné en silence).

## Alternatives considérées

- **Option A (rejetée) — un second verdict de continuité, propre à la
  filtration.** Violerait directement l'arbitrage de feature-139 (« un seul
  chemin de dispense ») : deux calculs indépendants de la même notion physique
  (l'eau a-t-elle cessé de circuler) peuvent diverger, et c'est exactement
  cette divergence qui avait produit l'incident du 2026-10-08 documenté par
  l'ADR-0089.
- **Option B (rejetée) — fusionner la durée de filtration dans
  `OutageRtcRecord` (measure_validity).** Couplerait deux cycles de vie sans
  rapport fonctionnel : la continuité de l'eau est une notion **globale**
  (vraie quel que soit le mode d'installation), la durée de filtration est
  **propre au mode managé** (condition 2 du gate — `filtrationRunning`/
  `installMode` ne doivent **jamais** être déduits de `waterPresent`, présumé
  en modes externes). Même travers que l'option D écartée par l'ADR-0100 :
  faire porter une grandeur neuve par le format persisté d'un mécanisme
  existant, au lieu de lui donner son propre enregistrement.
- **Option C (retenue) — enregistrement RTC propre à la filtration, même
  patron que `OutageRtcRecord`, consommant le verdict de `measure_validity`
  sans jamais le recalculer.** Préserve le chemin de dispense unique,
  n'alourdit aucun format existant, et rend la frontière vérifiable par un
  grep de non-régression plutôt que par une seule relecture humaine.

## Conséquences

### Positives
- La garde des 10 min de la correction manuelle pH ne se réarme plus après
  un redémarrage logiciel court pendant lequel l'eau n'a pas cessé de
  circuler — le cas le plus fréquent de la flotte (OTA hebdomadaire).
- Le chemin de dispense reste **unique** : aucune branche de code ne compare
  une durée de coupure à une grâce en dehors de `measure_validity_logic`.
- La frontière producteur/consommateur est **vérifiable mécaniquement** (grep
  CI-able), pas seulement documentée en prose.

### Négatives / dette assumée
- **Tout changement de disposition d'un enregistrement RAM RTC scellé coûte
  un redémarrage de non-bénéfice** après l'OTA qui le livre : au premier
  redémarrage suivant le déploiement de ce lot, la zone `FiltrationRtcRecord`
  est neuve (magic/CRC invalides) ⇒ fail-closed, « Reprise à zéro (mémoire de
  filtration absente ou corrompue) ». Ce n'est pas un défaut — c'est la
  preuve que le fail-closed fonctionne — mais la vérification de terrain ne
  peut se faire qu'au **second** redémarrage.
- **Risque résiduel instruit et assumé** (le gate a refusé de le fermer) :
  une boucle de redémarrages rapprochés fragmente la marche réelle tout en
  accumulant le report, chaque intervalle restant sous la grâce. Un plafond
  du nombre de reports consécutifs a été **examiné et refusé** : il
  bloquerait le cas légitime (plusieurs OTA dans le même cycle de
  filtration) — le même travers que le quota « une dispense par heure »
  retiré par feature-139. La contrepartie est l'observabilité : chaque
  report est journalisé avec les deux nombres (secondes recréditées, durée
  de la coupure), et une succession de lignes « Reprise après redémarrage »
  au journal est un signal à regarder, pas une garde automatique.
- Le refus n°1 de `evaluatePhTune()` (`isnan(phValueRaw)`, mesure indisponible)
  devient, depuis ce lot, la **seule** protection des ~45 premières secondes
  après un boot pour la garde de correction pH — le refus n°2 (filtration)
  peut désormais être franchi dès les premières secondes si un report est
  accordé. L'ordre des deux refus est donc **load-bearing** et ne doit
  jamais être inversé (commenté explicitement dans `ph_tune_logic.h`).

### Ce que ça verrouille
- **Un mécanisme neuf = un enregistrement neuf.** Toute future grandeur qui a
  besoin de survivre à un redémarrage logiciel, propre à un composant donné,
  obtient son propre enregistrement RTC scellé plutôt que d'étendre le format
  d'un mécanisme existant — pour ne pas faire payer le redémarrage de
  non-bénéfice à un mécanisme qui n'a pourtant pas changé.
- **La règle « persister une donnée n'est pas produire un verdict »** devient
  le critère de décision pour toute future extension d'un module qui consomme
  un verdict déjà calculé ailleurs : un consommateur peut porter son propre
  état à travers un reboot, à condition de rester fail-closed et de ne
  jamais recalculer la décision dont il dépend.
- **Un grep de non-régression** (jetons `kWaterStopGraceS`/`outageS`/`graceS`
  absents de `src/filtration.*`) devient le patron de vérification mécanique
  de cette frontière pour toute évolution future de `filtration.cpp`.

## Références

- Code : [`src/filtration_resume_logic.h`](../../src/filtration_resume_logic.h) /
  [`.cpp`](../../src/filtration_resume_logic.cpp) (module pur),
  [`src/filtration.h`](../../src/filtration.h) / [`.cpp`](../../src/filtration.cpp)
  (coquille, `FiltrationRtcRecord`, `refreshFiltrationRtcRecord()`),
  [`src/measure_validity.h`](../../src/measure_validity.h) / [`.cpp`](../../src/measure_validity.cpp)
  (`bootContinuityVerdict()`), [`src/ph_tune_logic.h`](../../src/ph_tune_logic.h)
  (ordre des refus *load-bearing*), [`src/main.cpp`](../../src/main.cpp)
  (ordre `measureValidity.begin()` avant `filtration.begin()`)
- Tests : [`test/test_native_filtration_resume/`](../../test/test_native_filtration_resume/)
- Spec : `specs/features/done/feature-158-duree-de-filtration-survit-au-redemarrage.md` (déplacée vers `done/` à la clôture)
- Doc : [subsystems/filtration.md](../subsystems/filtration.md),
  [subsystems/measure-validity.md](../subsystems/measure-validity.md)
- ADR liés : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)
  (logique pure testable, patron repris ici), [ADR-0089](0089-ce-qu-un-redemarrage-a-le-droit-doublier.md)
  (budget horaire de dosage survit au redémarrage — trois étages de
  persistance, principe « au plus défavorable » dont ce lot ne hérite pas
  directement mais dont il partage la question structurante), [ADR-0092](0092-retrait-analyse-stabilite-porte-mesures.md)
  (retrait de l'analyse de stabilité — measure_validity, contexte du verdict
  consommé ici), [ADR-0100](0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md)
  (un verdict qui engage le dosage est servi, jamais recalculé — même
  principe appliqué ici au verdict de continuité), [ADR-0102](0102-ordre-des-gestes-de-justesse-par-compteur.md)
  (autre cas où l'ordre de deux événements à travers un reboot ne peut pas se
  décider par une estampille naïve)
