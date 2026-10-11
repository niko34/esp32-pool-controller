# ADR-0041 — Autorité des mesures non représentatives et sortie de secours du filtre de tendance centrale

- **Statut** : Partiellement superseded by ADR-0097 (la sortie de secours de ré-amorçage et ses trois gardes cumulatives disparaissent ; le principe de porte d'alimentation SURVIT, simplifié et re-énoncé dans l'ADR-0097 — vider à la fermeture, aucune action à l'ouverture)
- **Date** : 2026-08-21
- **Décideurs** : `architect`, `pool-chemistry` (verdict GO conditionnel), `code-reviewer`, utilisateur
- **Spec(s) liée(s)** : `specs/features/done/bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue.md` — succède fonctionnellement à [ADR-0027](0027-filtre-tendance-centrale.md) (feature-058), qui reste en vigueur

## Contexte

Le filtre pH/ORP ([ADR-0027](0027-filtre-tendance-centrale.md)) écarte comme « spike dur isolé » tout échantillon situé à plus de `spikeReject` (**1,0 pH** / **300 mV**) de la médiane courante, **avant** qu'il n'alimente le buffer. Ce comportement était documenté comme un fail-closed **volontaire**, pensé pour une sonde défaillante.

C'était un **piège sans issue**, et il s'est refermé en production le 2026-08-16 :

```text
ph : raw=6.957  median=5.293(corrigé → 4.793 interne)  ready=false  rejected=255
orp: raw=556    median=561                             ready=true   rejected=0
```

Le raisonnement est déterministe : la médiane ne peut évoluer **que** si des échantillons entrent dans le buffer ; un échantillon trop éloigné de la médiane est écarté **avant** d'y entrer ; donc dès que la grandeur **vraie** s'éloigne durablement, **chaque** échantillon suivant est rejeté à son tour et la médiane est figée **pour toujours**. Les deux échappatoires existantes sont inopérantes par construction : `consecutiveRejects` n'est volontairement pas incrémenté sur ce chemin, et `dispersionCleanStreak` est remis à zéro par tout spike écarté (condition bloquante de feature-058). `rejectedCount` sature à 255 sans rien piloter. **Seul `reset()` en sortait.**

Conséquence : `ready() == false` en permanence ⇒ dosage pH bloqué (garde `FilterNotReady`) **et** dosage chlore inhibé (fail-closed feature-067 sur pH brut indisponible). Le bassin est resté **sans aucune régulation ≥ 3 h**, **sans alarme**, et sans recours depuis l'interface — la seule porte de sortie étant une route de debug non exposée. L'utilisateur ne l'a détecté que parce qu'une valeur affichée lui semblait incohérente.

Le **déclencheur** est distinct du mécanisme, et c'est lui qui rendait le défaut **récurrent** plutôt qu'accidentel. Chronologie relevée à la seconde :

```text
06:07:59  Reprise circulation — stabilisation des mesures pH/ORP 5 min
06:12:59  [Dosage ORP] pH brut indisponible — dosage chlore automatique suspendu
06:13:00  Stabilisation terminée — mesures pH/ORP de nouveau représentatives
```

À l'instant précis où les mesures redevenaient représentatives, le filtre était **déjà** verrouillé sur une médiane construite pendant la **stagnation**. `measure_validity` déclarait bien ces mesures non représentatives et suspendait l'injection — mais rien ne suspendait l'**ingestion**. En une phrase :

> Le filtre ingérait des échantillons que le système lui-même avait déclarés non représentatifs, puis leur donnait autorité sur les échantillons représentatifs suivants.

Il faut trancher maintenant parce que le défaut est **déterministe et récurrent** (une reprise de filtration suffit à l'amorcer), qu'il produit une **perte totale et silencieuse de régulation**, et que le filtre ORP partage exactement la même logique.

Contrainte transverse : `sensor_filter` est un module **pur** ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) — pas de logger, pas de `String`, testable en `env:native`. Toute la mécanique décidée ici doit y tenir, la journalisation restant à la charge de la coquille `SensorManager`.

## Décision

**Une mesure déclarée non représentative n'a aucune autorité sur le filtre, et un filtre ne peut jamais rester verrouillé en silence.** Cela se décline en quatre engagements.

1. **Porte d'alimentation.** `SensorFilter::setContext(waterPresent, measuresValid, nowMs)` est appelée à chaque cycle, **avant** `addSample()`, avec le verdict de `measure_validity`. Porte fermée si `!waterPresent || !measuresValid`, plus un **plancher de gel de 60 s** (`kSensorFilterGateFloorMs`) après tout front montant d'eau — qui couvre le cas `stabilizationDelayMin = 0`. Porte fermée ⇒ `addSample()` sort **en tête** : l'échantillon ne touche ni le buffer, ni le `FrozenDetector`, ni aucun compteur. À la transition **fermé → ouvert**, l'estimateur est **ré-amorcé** : la médiane de stagnation n'a jamais autorité sur les mesures représentatives qui suivent.

2. **Sortie de secours sur rejets cohérents.** `kSensorFilterSpikeRearmCount` = **12** rejets de spike consécutifs contenus dans une bande `≤ rearmCoherence` (**0,20 pH** / **40 mV**) décrivent une **nouvelle valeur**, pas du bruit : le filtre cesse de croire sa médiane et se ré-amorce. Le critère discriminant est la **cohérence mutuelle des rejets** — une série dispersée ré-ancre le compteur et n'atteint jamais 12.

3. **Trois gardes cumulatives** (verdict `pool-chemistry`), sans lesquelles la sortie de secours serait un contournement du fail-closed :
   - **cooldown de 30 min** par capteur entre deux ré-amorçages ;
   - **bande admissible** jugée sur le **centre** de la série — pH `[6,50 ; 8,20]`, ORP `[300 ; 950]` — sur l'échelle **BRUTE** ([ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md)) ;
   - **cohérence** de la série, réévaluée au moment de la décision.

   Un refus rend `rearmDenied()` **collant** ⇒ le filtre **reste** non prêt (dosage fail-closed) et la coquille émet un `critical` + une alerte MQTT. Les 12 échantillons rejetés sont **jetés**, jamais réinjectés : aucune valeur aberrante n'entre par cette porte. Le filtre re-converge uniquement sur les mesures **à venir**, après **warmup complet** (13 échantillons, ~65 s).

4. **`rearm()` est distincte de `reset()`.** `rearm()` vide l'estimateur mais ne touche **NI** le `FrozenDetector` (feature-022) **NI** le latch EMI (feature-058). C'est la garantie explicite qu'on ne masque **jamais** une sonde défaillante ou une ambiance EMI en re-convergeant — d'autant que **l'origine de l'excursion à ~4,8 n'est pas tranchée** à la date de cet ADR.

S'ajoutent deux corollaires non négociables : l'**alarme** (filtre non prêt > 5 min alors que la filtration tourne et que les mesures sont représentatives ⇒ `critical` + alerte MQTT + drapeau UI, répétition 30 min) et la **reprise utilisateur** (`POST /reset-sensor-filter?sensor=ph|orp`, auth CRITICAL, sélective, 1×/10 min). L'absence totale de régulation ne doit **jamais** être silencieuse, et elle doit **toujours** avoir un recours accessible.

**Le rejet de spike n'est pas supprimé** : il protège d'aberrations réelles (feature-058). La condition bloquante de feature-058 sur `dispersionCleanStreak` est **préservée** telle quelle.

### Tolérance aux trous de lecture — condition bloquante de revue

Un **trou** (`NaN` ou hors-plage) pendant une série de rejets cohérents **n'annule plus** la série : au plus **2 trous** par série, **jamais 2 consécutifs**, et un trou **ne fait pas avancer** le compteur (l'évidence doit rester acquise par des rejets réels).

Cette clause est structurante et non cosmétique : sans elle, une sonde ratant **un cycle sur douze** pendant un échelon légitime n'aurait **jamais** ré-amorcé — c'est-à-dire que l'incident se serait reproduit **à l'identique** malgré le correctif.

## Alternatives considérées

- **Option A — geler l'alimentation seule, sans ré-amorcer à la réouverture** (rejetée). Elle paraissait plus « disponible » : le filtre conserve sa médiane pré-arrêt et reste prêt tout de suite après la purge. Elle est en réalité **inopérante** : `ready()` exige une dernière mesure valide d'âge `≤ kSensorFilterMaxAgeMs` (20 s), or la porte fermée ne met pas `lastValid` à jour. Le filtre devient donc non prêt de toute façon — on paie l'indisponibilité **sans** obtenir la justesse, et la médiane pré-arrêt (potentiellement périmée, construite sur de l'eau stagnante) reste **autoritaire** au retour et rejette les mesures représentatives. Aucun bénéfice, tout le risque.
- **Option B — `ready` immédiat après ré-amorçage** (rejetée). Reprendre le dosage sans attendre le warmup complet supprimerait la fenêtre de ~65 s sans régulation. Mais un ré-amorçage peut aussi être provoqué par une **sonde en train de mourir** dont les lectures dérivent de façon cohérente : accorder le `ready` immédiatement reviendrait à **masquer une sonde morte** et à doser sur sa valeur. Le warmup complet fail-closed est le prix de la sûreté.
- **Option C — supprimer ou relever `spikeReject`** (rejetée). Elle ferait disparaître le piège… en supprimant la protection contre les aberrations franches acquise en feature-058 (diagnostic feature-057 : oscillations EMI de niveau). Un seuil relevé déplace le problème sans le traiter — le verrouillage réapparaît pour tout échelon supérieur au nouveau seuil.
- **Option D — incrémenter `consecutiveRejects` sur les spikes** (rejetée). Le garde `maxConsecutiveRejects` se déclencherait enfin, mais il ne produit qu'un drapeau `unstable()` : le filtre resterait **verrouillé** (médiane figée) tout en étant déclaré instable. On gagne un symptôme, pas une issue. Cela casserait en outre l'invariant de feature-058 qui exclut délibérément le spike du signal d'instabilité.
- **Option E — ré-amorcer sur simple comptage de rejets, sans critère de cohérence** (rejetée). Un capteur bruyant crachant des aberrations **dispersées** atteindrait le compte et imposerait sa valeur au filtre : exactement le comportement que feature-058 avait supprimé. La cohérence mutuelle est ce qui distingue « la grandeur a changé » de « le capteur délire ».
- **Option F (retenue) — porte d'alimentation + ré-amorçage à la réouverture + sortie de secours gardée sur cohérence.** Elle traite le **déclencheur** (ingestion de mesures non représentatives) *et* le **mécanisme** (verrouillage sans issue). Corriger un seul des deux ne suffit pas : sans la porte, le filtre serait empoisonné à chaque reprise de filtration et la sortie de secours re-convergerait à répétition **après** avoir produit une médiane fausse entre-temps ; sans la sortie de secours, tout échelon légitime en cours de fonctionnement reverrouillerait le filtre.

## Conséquences

### Positives

- Le verrouillage définitif du filtre — perte totale et silencieuse de régulation pH **et** chlore — n'est plus atteignable par un échelon légitime de la grandeur mesurée.
- La médiane de stagnation n'a plus aucune autorité : elle ne peut plus rejeter les mesures représentatives qui suivent une reprise de circulation.
- Toute perte durable de régulation est **signalée** (`critical`, alerte MQTT, `binary_sensor` HA `device_class: problem`, chip UI, bannière) et **rattrapable** depuis l'interface, par capteur.
- Le filtre **ORP** bénéficie des mêmes correctifs sans code spécifique (logique partagée, donc défaut partagé).
- Un refus de ré-amorçage devient un **signal de diagnostic** exploitable (« sonde suspecte ») au lieu d'un silence.
- Toute la mécanique reste dans le module **pur** ⇒ couverte par les tests natifs, y compris le rejeu de l'incident à partir des valeurs réelles relevées.

### Négatives / dette assumée

- **Indisponibilité accrue du dosage** : chaque reprise de circulation et chaque ré-amorçage coûtent un warmup complet (~65 s) pendant lequel le dosage automatique est bloqué. Arbitrage explicite de `pool-chemistry` : justesse avant disponibilité.
- **Un échelon vrai supérieur à `spikeReject` met ~60 s à être suivi** (12 rejets cohérents à 5 s/cycle), puis ~65 s de warmup — soit ~2 min de latence avant reprise du dosage sur la nouvelle valeur.
- **Le cooldown de 30 min peut refuser un ré-amorçage légitime** (deux échelons vrais rapprochés). Choix assumé : un refus est bruyant (`critical` + alerte) et rattrapable manuellement, l'inverse ne l'est pas.
- **La bande admissible est exprimée en échelle brute** : avec un `ph_offset` non nul, la bande **effective en valeur affichée** est décalée d'autant. C'est une source de confusion à l'exploitation, documentée mais non supprimée (conséquence directe d'[ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md)).
- **`measure_validity` inclut désormais `sensors.h`** pour l'instrumentation des bruts au front montant d'eau — couplage nouveau, borné à deux accesseurs scalaires en lecture, contexte `loopTask`.
- **L'origine de l'excursion à ~4,8 reste non tranchée.** L'hypothèse (eau stagnante très acide au voisinage du point d'injection) est plausible et cohérente avec la chronologie, mais **non vérifiée**. Le correctif pourrait donc **masquer une défaillance matérielle** — c'est précisément pourquoi `rearm()` ne touche pas le détecteur de sonde figée. L'instrumentation des deltas de purge existe pour trancher a posteriori.

### Ce que ça verrouille

- **Un module de mesure ne peut plus ingérer un échantillon sans contexte de représentativité.** Toute nouvelle chaîne de mesure (température, débit, futur capteur) devra soit consommer le verdict `measure_validity`, soit justifier explicitement pourquoi elle en est dispensée.
- **Un état qui supprime toute régulation doit être alarmé et rattrapable.** Aucun nouvel état bloquant ne pourra être introduit sans (a) une alarme edge-triggered, (b) un recours accessible hors routes de debug.
- **Un ré-amorçage d'estimateur ne peut jamais effacer un diagnostic de capteur.** La frontière `rearm()` / `reset()` est un invariant : seul un geste **explicite** de l'utilisateur (ou une calibration réussie) peut effacer une détection de sonde figée ou un latch EMI.
- **Contrat d'écriture mono-contexte de `SensorFilter`** : plus aucun handler asynchrone n'écrit dans un filtre. Toute nouvelle voie de reset devra passer par `requestFilterReset()`, donc être **différée** en `loopTask`.
- **Les trois champs de fin de `SensorFilter::Config`** (`rearmCoherence`, `rearmMin`, `rearmMax`) doivent rester **en fin de struct** : une init agrégat plus courte laisse `rearmCoherence = 0` et désactive la sortie de secours (rétrocompatibilité des appelants et des tests existants).

## Références

- Code : [`src/sensor_filter.h`](../../src/sensor_filter.h), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) (porte, sortie de secours, tolérance aux trous, `rearm()`)
- Code : [`src/sensors.cpp`](../../src/sensors.cpp) (`_updateFilterGates()`, `_checkRearmAndLog()`, `_checkFilterNotReadyAlarm()`, `requestFilterReset()`), [`src/sensors.h`](../../src/sensors.h)
- Code : [`src/measure_validity.cpp`](../../src/measure_validity.cpp) (instrumentation des bruts au front montant / fin de purge)
- Code : [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) (`POST /reset-sensor-filter`), [`src/web_routes_debug.cpp`](../../src/web_routes_debug.cpp) (auth CRITICAL + sélectivité)
- Code : [`src/constants.h`](../../src/constants.h) (bloc `kSensorFilterSpikeRearm*` / `kSensorFilterGateFloorMs` / `kSensorFilterNotReady*` / `kSensorFilterUiResetMinIntervalMs`)
- Tests : [`test/test_native_sensor_filter/`](../../test/test_native_sensor_filter/) — rejeu de l'incident (médiane 4,79 → échelon 6,96), transition purge → valide, non-régression aberration isolée
- Spec : `specs/features/done/bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue.md`
- ADR liés : [ADR-0027](0027-filtre-tendance-centrale.md) (filtre de tendance centrale), [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md) (échelle brute), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable)
- Doc : [`docs/subsystems/sensors.md`](../subsystems/sensors.md#le-piège-du-spike-répété--incident-du-2026-08-16-v2203), [`docs/subsystems/measure-validity.md`](../subsystems/measure-validity.md), [`docs/API.md`](../API.md#post-reset-sensor-filter--critical), [`docs/MQTT.md`](../MQTT.md)
