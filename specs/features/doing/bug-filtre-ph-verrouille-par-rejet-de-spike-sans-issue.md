# Bug — le filtre pH se verrouille définitivement sur un rejet de spike, et coupe TOUTE la régulation

- **Type** : bug critique (mesure / sécurité chimique / disponibilité)
- **Priorité** : critique — a laissé le bassin **sans régulation pH NI chlore** pendant ≥ 3 h
- **Statut** : **livré en 2.20.3** — tests verts (605 natifs), `pio run` bi-core SUCCESS, revue OK, `pool-chemistry` GO. ⚠️ **Aucun scénario rejoué on-target** : le module de production tourne en 2.20.3 depuis peu, les critères marqués ci-dessous restent à confirmer sur l'appareil.
- **Version cible** : 2.20.3
- **Créée** : 2026-08-16, après incident de production constaté et débloqué en direct
- **Domaine** : Capteurs (`sensor_filter`), Régulation pH/ORP, UI
- **Specs liées** : feature-058 (rejet de spike dur), feature-067 (offset pH + filets de sécurité), ADR-0017

## Incident constaté (mesures on-target, firmware 2.20.0)

```
/debug/sensor_filter_state
ph : raw=6.957  median=5.293(corrigé) filtered=5.282(corrigé) ready=false  rejected=255
orp: raw=556    median=561            filtered=568            ready=true   rejected=0
```

La médiane **interne** valait donc `5.293 − 0.50 = 4.793` (l'offset est appliqué en
sortie d'accesseur, ADR-0039). Écart avec le brut : **2,164 pH**.

## Cause racine — un piège sans issue, déterministe

`src/sensor_filter.cpp:98-108` : un échantillon dont `|raw − médiane| > spikeReject`
(`kPhFilterSpikeReject = 1.0`, `src/constants.h:495`) est classé « spike dur » et
**écarté avant d'alimenter le buffer**.

Or la médiane ne peut évoluer QUE si des échantillons entrent dans le buffer. Dès que
la valeur vraie s'éloigne durablement de plus de `spikeReject` de la médiane courante,
**tout échantillon suivant est rejeté à son tour** : la médiane est figée pour toujours.

Les deux échappatoires existantes ne s'appliquent pas :
1. `_consecutiveRejects` n'est **volontairement pas** incrémenté sur ce chemin
   (commentaire l.92-93) → le garde `maxConsecutiveRejects` ne se déclenche jamais.
2. `_dispersionCleanStreak` est remis à 0 sur tout spike écarté (condition
   `pool-chemistry` de feature-058) → l'auto-levée du latch EMI
   (`kSensorFilterLatchClearCycles`) ne se déclenche jamais non plus.

Le compteur `rejectedCount` sature à 255 et n'est qu'un indicateur : il ne pilote rien.
**Seul `reset()` sort de cet état.**

Le comportement est documenté comme un fail-closed volontaire (`sensor_filter.h:49-53` :
« un spike répété rend le filtre non prêt »), pensé pour un capteur défaillant. Il n'a
pas été pensé pour un **échelon légitime** de la grandeur mesurée.

## Conséquence — c'est le point grave

`ready() == false` en permanence, donc :
- dosage **pH** automatique bloqué (garde FilterNotReady) ;
- dosage **chlore** inhibé par le filet de la feature-067, l'appareil l'ayant écrit :
  `[Dosage ORP] pH brut indisponible (capteur muet ou filtre non amorcé) — dosage
  chlore automatique suspendu par sécurité`.

Le bassin n'était donc plus régulé **du tout**, sans alarme, sans notification, et sans
aucun moyen de reprise depuis l'interface : la seule porte de sortie est
`POST /debug/sensor_filter_reset`, une route de debug absente de l'UI.

L'utilisateur n'a détecté l'anomalie que parce que la **valeur affichée** lui semblait
incohérente — pas parce que le système l'a signalée.

## Ce qui a été fait en direct (déblocage, pas correctif)

`POST /debug/sensor_filter_reset` puis `ph_regulation_mode → automatic`. Les deux
filtres sont repartis et prêts en ~60 s ; `ph_dosing` et `orp_dosing` de nouveau actifs.
**Le défaut firmware reste entier.**

Note : la route de reset réinitialise **les deux** filtres (`{"reset":["ph","orp"]}`),
donc elle sacrifie le filtre ORP qui était sain — à revoir aussi.

## Le DÉCLENCHEUR — le filtre est empoisonné par des mesures que le système sait fausses

Le verrouillage décrit ci-dessus est le **mécanisme**. Son **déclencheur** est distinct, et
c'est lui qui rend le défaut RÉCURRENT plutôt qu'accidentel.

Chronologie relevée dans le journal, à la seconde :

```
06:07:59  Reprise circulation — stabilisation des mesures pH/ORP 5 min
06:12:59  [Dosage ORP] pH brut indisponible — dosage chlore automatique suspendu
06:13:00  Stabilisation terminée — mesures pH/ORP de nouveau représentatives
```

À l'instant précis où les mesures redevenaient représentatives, le filtre était **déjà**
verrouillé sur une médiane construite pendant la stagnation.

Le module `measure_validity` (`src/measure_validity.cpp:25-46`) détecte le front montant
d'eau et déclare les mesures **non représentatives** pendant `stabilizationDelayMin`. Il
suspend l'injection — correctement. Mais :

**`resetPhFilter()` / `resetOrpFilter()` ne sont appelés QUE après calibration réussie
(`src/sensors.cpp:656,671,701`) et depuis `POST /debug/sensor_filter_reset`. JAMAIS à la
reprise de circulation.**

Le filtre continue donc d'ingérer les échantillons de stagnation, sa médiane converge vers
la valeur de l'eau stagnante, et cette médiane acquiert ensuite **autorité sur les
échantillons représentatifs** qui la suivent — qu'elle rejette un à un comme aberrants.

En une phrase, le défaut de conception :
> **Le filtre ingère des échantillons que le système lui-même a déclarés non
> représentatifs, puis leur donne autorité sur les échantillons représentatifs suivants.**

Hypothèse sur l'excursion à ~4,8 : eau stagnante dans la chambre de mesure au voisinage du
point d'injection d'acide, donc localement très acide — **plausible et cohérente avec la
chronologie, mais NON VÉRIFIÉE**. Le cycle doit soit la confirmer (relevé du brut pendant un
arrêt de filtration), soit instruire une défaillance de sonde. Tant qu'elle n'est pas
tranchée, on ne sait pas si le correctif filtre suffit ou s'il masque un problème matériel.

Conséquence de périmètre : **corriger uniquement le verrouillage ne suffit pas.** Sans
traiter l'ingestion des mesures non représentatives, le filtre sera empoisonné à chaque
reprise de filtration ; la sortie de secours re-convergerait à répétition, après avoir
produit une médiane fausse entre-temps. Les deux volets sont nécessaires.

Bonne nouvelle : l'information requise **existe déjà** et est publiée
(`MeasureValidityResult.valid`). Le correctif consiste à la consommer, pas à la produire.

## Correctif attendu (pistes, à arbitrer par `architect` + `pool-chemistry`)

0. **Ne pas alimenter le filtre avec des mesures déclarées non représentatives**, ou
   ré-amorcer le filtre à la fin de la stabilisation. À arbitrer : geler l'alimentation
   pendant la purge (le filtre garde alors une médiane pré-arrêt, potentiellement périmée)
   ou réinitialiser à la transition purge → valide (warmup à chaque reprise, donc dosage
   retardé d'autant). Le second est plus sûr, le premier plus disponible — c'est
   `pool-chemistry` qui tranche.

1. **Sortie de secours obligatoire** : au-delà de N spikes consécutifs écartés, cesser de
   croire la médiane et re-converger (ré-amorçage du buffer sur les échantillons récents).
   Un capteur qui crache des aberrations ISOLÉES doit rester filtré ; une grandeur qui a
   franchement changé doit être suivie. Le critère qui distingue les deux est la
   **cohérence entre échantillons rejetés** : N rejets successifs *proches les uns des
   autres* décrivent une nouvelle valeur, pas du bruit.
2. **Alarme** : un filtre non prêt depuis plus de X minutes alors que la filtration tourne
   doit produire un `CRITICAL` et une alerte visible (UI + MQTT), pas un simple INFO noyé.
   L'absence totale de régulation ne doit jamais être silencieuse.
3. **Reprise depuis l'UI** : exposer la réinitialisation du filtre hors des routes de debug,
   et **par capteur** (ne pas sacrifier l'ORP pour réparer le pH).
4. Vérifier le même piège sur le filtre **ORP** (`kOrpFilterSpikeReject = 300 mV`) : la
   logique est partagée, donc le défaut l'est aussi.
5. Réexaminer `kPhFilterSpikeReject = 1.0` : un échelon de 1 pH est-il vraiment une
   aberration, sachant qu'une correction de calibration ou un ajout de produit peut le
   produire légitimement ?

## Points d'attention pour le cycle

- `pool-chemistry` est **obligatoire** : toute modification change ce que consomme la
  régulation, et la sortie de secours doit être fail-closed si le capteur est réellement HS.
- Ne PAS supprimer le rejet de spike : il protège d'aberrations réelles (feature-058).
- Les conditions bloquantes de feature-058 sur `_dispersionCleanStreak` doivent être
  préservées ou explicitement réarbitrées.

## Observations annexes du même incident

- Sonde de température d'eau signalée **figée** :
  `[SENSOR_FROZEN] Sonde T° eau figée : 900 lectures dans une bande < 0.03 °C autour de
  26.6 °C — compensation pH et planning filtration sur valeur figée`. À instruire à part.
- `ph_regulation_mode` était sur `manual` / `ph_enabled=false` sans trace dans le journal
  restant : impossible de dire si c'est l'escalade automatique de feature-067 (3 refus de
  plancher acide en 24 h) ou une action manuelle. **La bascule automatique en manuel doit
  être journalisée en `CRITICAL` et persistée**, sans quoi elle est indiagnosticable.

## Critères d'acceptation

- [x] **Test de non-régression rejouant l'incident** à partir des valeurs réelles relevées :
      médiane établie à 4,79 puis échelon soutenu à 6,96 → le filtre doit redevenir `ready`
      et suivre, en un nombre d'échantillons **borné et asserté**. C'est le critère central :
      sans lui, rien ne garantit que le défaut ne réapparaisse pas.
- [x] Une reprise de circulation ne laisse PAS la médiane de stagnation faire autorité sur
      les mesures représentatives qui suivent (test dédié sur la transition purge → valide)
- [x] Une aberration isolée reste rejetée (non-régression feature-058), et la condition
      bloquante de feature-058 sur `_dispersionCleanStreak` est préservée ou réarbitrée
      → **préservée telle quelle** (tout spike écarté casse la streak)
- [ ] Un filtre non prêt durablement, filtration active, déclenche une alerte visible
      (`CRITICAL` + UI + MQTT) — l'absence totale de régulation ne doit jamais être silencieuse
      → **implémenté et revu, NON VÉRIFIÉ ON-TARGET** (aucun épisode de 5 min rejoué sur l'appareil)
- [ ] La bascule automatique de la régulation pH en `manual` (escalade feature-067) est
      journalisée en `CRITICAL` et persistée
      → **implémenté et revu, NON VÉRIFIÉ ON-TARGET** (persistance NVS + acquittement non rejoués sur l'appareil)
- [x] La réinitialisation est accessible hors debug et agit **par capteur**
      → `POST /reset-sensor-filter?sensor=ph|orp` (auth CRITICAL, 1×/10 min) + boutons dans les
      deux modales de filtre. ⚠️ Parcours UI **non rejoué on-target**.
- [x] Le même piège est instruit sur le filtre ORP (logique partagée)
      → porte, sortie de secours, alarme et réarmement couvrent l'ORP avec ses propres seuils
      (`kOrpFilterRearmCoherence` = 40 mV, bande 300–950 mV)
- [ ] L'origine de l'excursion à ~4,8 est tranchée : stagnation acide confirmée, ou
      défaillance de sonde instruite à part
      → **NON TRANCHÉE.** L'instrumentation nécessaire est livrée (bruts + delta journalisés au
      front montant d'eau et en fin de purge) ; le relevé reste à faire, cf.
      [Procédure de relevé en production](#procédure-de-relevé-en-production--trancher-lorigine-de-lexcursion-à-48).
      Spec de suivi créée pour l'hypothèse matérielle (`feature-069`).
- [x] Verdict `pool-chemistry` GO explicite
      → GO conditionnel : 3 gardes cumulatives sur la sortie de secours, bande jugée sur le
      centre de la série, échantillons rejetés jetés, tolérance aux trous (condition bloquante),
      `rearm()` ne touchant ni le détecteur figé ni le latch EMI
- [x] `pio run` bi-core verts, `pio test -e native` sans régression (suite `sensor_filter`)
      → **605 tests natifs**, zéro régression ; `pio run` core 2 et core 3 SUCCESS

## Notes d'implémentation (v2.20.3)

### Ce qui a été livré

| Volet | Détail |
|---|---|
| **Porte d'alimentation** | `SensorFilter::setContext(waterPresent, measuresValid, nowMs)` appelée avant `addSample()` à chaque cycle. Fermée si `!waterPresent \|\| !measuresValid`, + plancher de gel `kSensorFilterGateFloorMs` = 60 s après tout front montant d'eau (couvre `stabilizationDelayMin = 0`). Porte fermée ⇒ sortie **en tête** de `addSample()` : ni buffer, ni `FrozenDetector`, ni compteurs. Transition fermé → ouvert ⇒ `rearm()`. |
| **Sortie de secours** | `kSensorFilterSpikeRearmCount` = 12 rejets consécutifs **cohérents entre eux** (`max−min ≤ rearmCoherence` : 0,20 pH / 40 mV) ⇒ ré-amorçage. Série dispersée ⇒ ré-ancrage immédiat. |
| **3 gardes cumulatives** | cooldown `kSensorFilterRearmCooldownMs` = 30 min/capteur ; bande admissible jugée sur le **centre** de la série (pH [6,50 ; 8,20], ORP [300 ; 950]) sur l'échelle **BRUTE** (ADR-0039 ⇒ bande effective en pH corrigé décalée de `ph_offset`) ; cohérence revérifiée. Les 12 échantillons rejetés sont **jetés**, jamais réinjectés. Refus ⇒ `rearmDenied()` collant + `critical` + alerte MQTT, filtre non prêt. |
| **Tolérance aux trous** | Condition **bloquante** du gate, ajoutée en revue : ≤ 2 trous par série, jamais 2 consécutifs, un trou ne fait pas avancer le compteur. Sans elle, une sonde ratant un cycle sur douze pendant un échelon légitime n'aurait **jamais** ré-amorcé — l'incident se serait reproduit à l'identique. |
| **`rearm()` ≠ `reset()`** | `rearm()` vide l'estimateur mais ne touche **NI** le `FrozenDetector` (feature-022) **NI** le latch EMI (feature-058). Warmup complet fail-closed après ré-amorçage (13 échantillons, ~65 s). Remet `rejectedCount` à 0 et publie la valeur d'avant via `rejectedBeforeRearm()`. |
| **Alarme** | Filtre non prêt > `kSensorFilterNotReadyAlarmMs` = 5 min alors que la filtration tourne **et** que les mesures sont représentatives (la purge ne compte **jamais**) ⇒ `critical` + alerte MQTT, répétition 30 min, `info` de levée. Cause probable diagnostiquée dans le message. |
| **Reprise** | `POST /reset-sensor-filter?sensor=ph\|orp`, auth `RouteProtection::CRITICAL`, 1 reset/capteur/10 min (`429` + `retry_after_s`), réponse `{"queued":true}`. Boutons dans les modales pH et ORP, avec confirmation explicitant que la détection de sonde figée sera effacée. `POST /debug/sensor_filter_reset` passe **aussi** en CRITICAL (elle contournait toutes les gardes) et accepte `?sensor=`. |
| **Persistance** | `mqttCfg.phAutoManualEpoch` / `phAutoManualReason` (NVS `ph_am_epoch` / `ph_am_reason` ; absents ⇒ 0/"" donc rétrocompatible). Trace acquittée **uniquement** sur un changement réel de mode (correctif bloquant de revue : l'UI renvoyant le mode courant à chaque sauvegarde, tester la présence de la clé effaçait la trace). |
| **Concurrence** | Plus aucun `resetPhFilter()` depuis un handler async : tout passe par `requestFilterReset()`, consommé en tête d'`update()` (seul contexte `loopTask` autorisé à écrire dans un filtre). |
| **Instrumentation** | `measure_validity` journalise les bruts pH/ORP au front montant d'eau **et** en fin de purge, avec le delta (`[STAB] …`, formatage NaN-safe `n/a`). |

### Fichiers modifiés

- Firmware : `src/sensor_filter.{h,cpp}`, `src/sensors.{h,cpp}`, `src/constants.h`, `src/measure_validity.{h,cpp}`, `src/config.{h,cpp}`, `src/pump_controller.cpp`, `src/mqtt_manager.{h,cpp}`, `src/ws_manager.cpp`, `src/web_routes_control.cpp`, `src/web_routes_config.cpp`, `src/web_routes_data.cpp`, `src/web_routes_debug.cpp`, `src/version.h`
- UI : `data/app.js`, `data/index.html`, `data/app.css`
- Tests : `test/test_native_sensor_filter/test_sensor_filter.cpp`
- Docs : ADR-0041 (nouveau), note additive ADR-0027, `docs/API.md`, `docs/MQTT.md`, `docs/subsystems/{sensors,measure-validity,pump-controller,mqtt-manager,ws-manager}.md`, `docs/features/{page-ph,page-orp,page-dashboard}.md`, `CHANGELOG.md`

### Arbitrages notables

- **Geler l'alimentation sans ré-amorcer à la réouverture a été écarté** : `ready()` exige un âge ≤ `kSensorFilterMaxAgeMs` (20 s) et la porte fermée ne met pas `lastValid` à jour ⇒ le filtre devient non prêt **de toute façon**. On paierait l'indisponibilité **sans** obtenir la justesse, la médiane pré-arrêt restant autoritaire au retour. Détail et autres alternatives : [ADR-0041](../../../docs/adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md).
- **`kPhFilterSpikeReject` reste à 1,0 pH** (point 5 des pistes) : le seuil n'est pas le problème — le verrouillage vient de l'absence d'issue, pas de la valeur du seuil. Le relever déplacerait le piège au lieu de le supprimer, en affaiblissant la protection acquise en feature-058.
- **Le rejet de spike n'est pas supprimé** et la condition bloquante de feature-058 sur `_dispersionCleanStreak` est **préservée telle quelle**.

## Procédure de relevé en production — trancher l'origine de l'excursion à ~4,8

**Question ouverte** : l'excursion du pH brut à ~4,8 vient-elle de l'eau **stagnante** dans la chambre de mesure (localement très acide au voisinage du point d'injection) ou d'une **défaillance de la sonde** ? Tant qu'elle n'est pas tranchée, on ne sait pas si le correctif filtre suffit ou s'il masque un problème matériel.

L'instrumentation nécessaire est **déjà livrée** : il n'y a rien à coder, seulement à relever.

### Relevé

1. Vérifier que les logs `INFO` sont conservés (`GET /get-logs`, ou `docs/subsystems/logger.md`).
2. Provoquer ou attendre un **arrêt de filtration prolongé** (≥ 2 h, idéalement une nuit complète) — cas le plus favorable à la stagnation.
3. **Pendant** l'arrêt, relever le pH **brut** à intervalles réguliers via `GET /debug/sensor_filter_state` (champ `ph.raw`, **brut EZO sans offset**) : au minimum à T+15 min, T+1 h, T+2 h. Noter aussi `ph.median` et `ph.ready` — le filtre doit être **non prêt** (porte fermée), c'est le comportement attendu.
4. Laisser la filtration **reprendre** et récupérer les deux lignes de journal encadrant la stabilisation :
   - `[STAB] Bruts à la reprise : pH=… ORP=… mV (eau stagnante)`
   - `[STAB] Bruts en fin de purge : pH=… (delta …) ORP=… mV (delta …)`
5. Répéter sur **au moins 3 cycles** d'arrêt/reprise : une conclusion sur un seul épisode ne vaut rien.

### Lecture du résultat

| Observation | Conclusion | Suite |
|---|---|---|
| Le brut **dérive progressivement vers le bas** pendant l'arrêt, puis **remonte** à la reprise avec un **delta franc et positif** (> ~0,3 pH), de façon **reproductible** | **Stagnation acide confirmée.** Le correctif v2.20.3 traite la cause. | Clore la question. Envisager une amélioration hydraulique (position de la chambre de mesure / du point d'injection) si le delta est élevé. |
| Le brut est **aberrant d'emblée** (excursion brutale, non corrélée à la durée d'arrêt), delta de purge **quasi nul**, ou valeurs erratiques **filtration en marche** | **Défaillance de sonde probable.** | Instruire la spec de suivi d'investigation matérielle (`feature-069`). |
| Le brut reste **stable et plausible** pendant tout l'arrêt sur les 3 cycles | Ni stagnation ni défaillance reproductible : l'épisode du 16/08 était **ponctuel**. | Documenter le relevé et clore, en conservant l'alarme comme filet. |

⚠️ Toujours raisonner sur le **brut EZO** (`ph.raw`, `phRaw`, lignes `[STAB]`) et **jamais** sur la valeur corrigée : l'offset (`ph_offset`) décale l'affichage de la mesure ([ADR-0039](../../../docs/adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)).

### Observations annexes à instruire séparément

- **Sonde de température d'eau figée** (`SENSOR_FROZEN`, 900 lectures dans 0,03 °C autour de 26,6 °C) → spec de suivi `feature-068`.
- **Routes de debug encore ouvertes** (`/debug/sensor_trace_clear`, `/debug/ph_slope_refresh` — écriture dans la queue EZO), repli `|| "automatic"` de `data/app.js`, factorisation du cooldown de reset entre les deux routes → spec de suivi `feature-071`.
