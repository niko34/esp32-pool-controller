# ADR-0079 — Le planning auto de filtration est figé à l'arrêt du cycle, promu à minuit

- **Statut** : Accepté
- **Date** : 2026-09-13
- **Décideurs** : pool-chemistry (audit + chiffrage du refus d'horizon, P2), architect (plan retenu), embedded-developer, test-engineer, code-reviewer, doc-writer
- **Spec(s) liée(s)** : [bug-126](../../specs/features/done/bug-126-le-recalcul-du-planning-annule-le-cycle-qui-l-a-produit.md)

## Contexte

Le planning de filtration en mode `auto` était recalculé **pendant la marche**, cinq minutes après le démarrage (`FiltrationManager::update()`, ancien bloc). `decideFiltrationRun()` — une décision de **NIVEAU**, jamais de front, [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) — relit alors la plage recalculée à chaque tour. Si la température avait baissé depuis le calcul de la veille, la fenêtre centrée sur `kFiltrationPivotHour = 13.0f` se rétractait ; si le nouveau début tombait après l'heure courante, le cycle qui venait de produire la mesure **s'arrêtait lui-même**.

Journal de l'incident du 2026-09-13 :

```
07:17:00  INFO  Démarrage filtration
07:22:00  INFO  Planning auto: 21.6°C → 07:36-18:24
07:22:00  INFO  Arrêt filtration
```

La mesure n'est fiable que parce que la pompe tourne, et elle sert à arrêter la pompe : la boucle est circulaire par construction, pas par accident d'implémentation.

`pool-chemistry` a établi qu'aucun risque sanitaire n'était engagé (19 min perdues sur ~11 h de fenêtre, chlore décalé et non perdu), mais que le défaut était quotidien en intersaison et polluait le journal au point de fausser l'analyse du droop de dosage.

## Décision

**Le planning auto n'est jamais écrit pendant que la pompe tourne.** Deux moments seulement en modifient l'état :

1. **À l'arrêt d'un cycle**, la moyenne incrémentale de température accumulée pendant ce cycle (échantillons post-purge **et** en plage, sonde eau exclusivement) est figée dans un **planning en attente**, `PendingSchedule` ([`src/filtration.h`](../../src/filtration.h)) — une structure **100 % RAM**, jamais écrite dans `filtrationCfg`.
2. **À minuit**, si les conditions de promotion tiennent, ce planning en attente devient le planning courant (`filtrationCfg.start/end`), une seule écriture NVS, et c'est la **seule** issue possible de sa modification automatique.

Pourquoi minuit et pas « le prochain démarrage » : `decideFiltrationRun()` décide sur le **niveau** de la plage horaire, pas sur un front. Écrire un planning dont la fin est postérieure à l'heure d'arrêt ferait **redémarrer la pompe au tour de boucle suivant** — c'est très exactement le mécanisme du défaut d'origine, reproduit sous une autre forme. Minuit est le seul instant où la pompe est **structurellement** à l'arrêt en mode auto, et ce n'est pas une convention : deux `static_assert` de [`constants.h`](../../src/constants.h:2372) l'imposent à la compilation —

```cpp
static_assert(kFiltrationPivotHour + kFiltrationRefTempMaxC / 4.0f < 24.0f, ...);
static_assert(kFiltrationPivotHour - kFiltrationRefTempMaxC / 4.0f >= 0.0f, ...);
```

Ces deux assertions bornent la fenêtre calculable (`computeAutoWindow`, borne haute `kFiltrationRefTempMaxC = 40 °C`) à l'intérieur de `[0h, 24h)` : la fenêtre auto ne peut **jamais** franchir minuit, donc la promotion à 00:00 ne peut **jamais** tomber au milieu d'une plage en cours. Un compilateur qui refuse de builder est une garantie plus forte qu'un commentaire.

La référence de température devient une **moyenne incrémentale** (`CycleTempAccumulator`, [`src/schedule_logic.h`](../../src/schedule_logic.h:143)) des échantillons pour lesquels les **trois conditions cumulatives** de `isSampleEligible()` tiennent : purge terminée (`measureValidity.isCirculationSettled()`), horaire en plage (`isMinutesInRange`), sonde non figée (`!sensors.isTemperatureFrozen()`). Elle est alimentée exclusivement par `sensors.getWaterTemperature()` (chemin nominal, offset de calibration borné appliqué) avec un repli de tolérance `selectFiltrationRefTemp()` pour un appareil neuf sans rôle de sonde attribué — jamais `sensors.getTemperature()`, qui retomberait sur une sonde `Circuit` (température du boîtier électronique, pas de l'eau).

## Alternatives considérées

- **Ne pas annuler un cycle déjà démarré** (rejetée) — corrige le symptôme (l'arrêt intempestif) sans corriger la cause : le planning continuerait d'être recalculé en marche, sur une mesure dont la validité dépend de la marche elle-même. Le cycle suivant aurait pu se sur-filtrer sans qu'aucune information nouvelle ne le justifie, et le journal resterait pollué de recalculs en cours de route.
- **Séparer planning stocké et fin effective du cycle en cours** (rejetée) — cette option maintenait `remainingRangeMinutes()` sur une notion de « fin effective » distincte du planning stocké, ce qui aurait exigé de faire transiter une seconde fenêtre jusqu'au dosage (`pump_controller.cpp`). Rejetée par contamination avec le refus d'élargir l'horizon de dosage (voir plus bas) : toute variante qui fait dépendre `remainingRangeMinutes()` d'autre chose que le planning **stocké** rouvre le risque *(e)* de la spec (gel du dosage) sans qu'aucun gain n'ait été démontré.
- **Recalculer à l'arrêt en appliquant aussitôt** (rejetée) — séduisante en apparence (« pourquoi attendre minuit si la pompe vient de s'arrêter ? »), mais son échec découle directement de la décision de niveau déjà citée : `decideFiltrationRun()` ne regarde pas *comment* on est arrivé à l'état courant, seulement si l'heure courante tombe dans `[start, end)`. Un planning appliqué immédiatement après l'arrêt, dont la fenêtre engloberait l'heure courante (cas fréquent : une fenêtre centrée sur le pivot qui s'élargit ou glisse), **relance la pompe au tour suivant**. C'est un second défaut circulaire, pas une correction.
- **Moyenne sur 24 h** (rejetée) — la sonde de référence est posée sur le circuit de filtration ; hors cycle de filtration, l'eau qui stagne dans les canalisations se réchauffe et n'est représentative de rien. `history.cpp` enregistre la température sans garde de circulation : une moyenne 24 h serait polluée par des heures d'eau stagnante, avec un poids d'autant plus grand que la filtration est courte (donc précisément aux températures basses, où l'erreur compte le plus).

## Conséquences

### Positives

- Le journal cesse d'afficher des paires `Démarrage`/`Arrêt` séparées de quelques minutes : chaque cycle programmé se déroule jusqu'à son terme prévu.
- La température de référence est désormais mesurée sur une base représentative (moyenne de tout le cycle post-purge, au lieu d'un instantané pris 5 min après le démarrage — précisément l'heure la plus froide du cycle, cf. CHANGELOG).
- L'ancienne bande morte sur `_lastScheduledTemp` (voir amendement d'[ADR-0074](0074-temperature-figee-statu-quo-instrumente.md)), inopérante après chaque redémarrage parce que non persistée, est remplacée par deux gardes qui tiennent même après reboot : une sonde figée ne nourrit plus la moyenne, et une fenêtre calculée identique à la fenêtre courante ne produit aucun `pending`.
- Une plage `start == end` héritée d'un état antérieur (NVS corrompue, ancien bug, saisie manuelle incohérente) est désormais auto-réparée en mode `auto` plutôt que de figer la filtration à l'arrêt en silence (voir plus bas, bornes de plausibilité).

### Négatives / dette assumée

- **Le planning en attente n'est visible nulle part côté utilisateur.** Ni `/get-config`, ni le WebSocket, n'exposent qu'un nouveau planning a été calculé et attend minuit pour s'appliquer, ni sur quelle température il repose. Reporté explicitement à une spec de suivi (voir CHANGELOG et la spec créée dans ce lot) — arbitrage C du plan retenu : ni `ux-ui-designer` ni `web-ui-developer` n'étaient dans ce cycle.
- **Un reboot avant minuit dégrade d'un jour.** Le planning en attente étant 100 % RAM, il est perdu à tout redémarrage. Le lendemain repart sur le planning de la veille (dernier planning promu), et le cycle du jour recalcule un nouveau `pending` à son propre arrêt — dégradation d'un jour, auto-réparatrice, jamais persistante. Voir « L'arbitrage RAM vs NVS » plus bas.
- **Le drapeau d'amorçage (`_haveRepresentativeMeasure`) est lui aussi RAM.** Un reboot ré-autorise l'amorçage : le premier planning après redémarrage peut reposer sur moins de `kFiltrationMinRepresentativeSamples` échantillons (mesure instantanée après purge, cas de mise en service). C'est un angle mort assumé du même arbitrage, pas un défaut distinct — il partage la justification et la même auto-réparation en 24 h.

### Ce que ça verrouille

- **`decideFiltrationRun()` reste, et doit rester, une décision de niveau.** Toute variante future qui voudrait réagir à un front (« ce cycle vient de démarrer, ne pas y toucher ») devra le faire dans la coquille (`filtration.cpp`), jamais dans `schedule_logic.cpp` — l'invariant de characterization de feature-038 continue de s'appliquer.
- **Une seule écriture NVS automatique du planning par jour**, à la promotion — zéro si la fenêtre calculée égale la fenêtre courante.
- **La fenêtre auto ne franchit jamais minuit** — verrouillé par les deux `static_assert` de `constants.h`. Toute évolution de `kFiltrationRefTempMaxC` ou `kFiltrationPivotHour` qui ferait échouer ces assertions doit être traitée comme un signal d'alarme, pas contournée par un `#ifdef` ou un cast.
- **`pump_controller.cpp` n'est pas concerné par ce mécanisme.** Aucune ligne n'y a été modifiée dans ce lot ; le repli d'horizon de dosage envisagé (option refusée ci-dessous) reste fermé tant qu'aucun des trois garde-fous listés n'est posé.

## Le refus d'élargir l'horizon de dosage

C'est le point que cet ADR doit porter le plus clairement, parce qu'il sera rouvert : la spec envisageait d'aligner l'horizon de répartition du dosage (`remainingRangeMinutes()`, consommé par `pump_controller.cpp:3980` et `:4455` via le mode `scheduled`) sur la fenêtre **effective** du cycle en cours plutôt que sur le planning stocké. **Refusé, sans ambiguïté, dans ce lot.**

`remainingRangeMinutes()` continue de lire le planning **stocké** (`filtrationCfg.start/end`), inchangé par ce lot. La raison tient dans la formule de répartition elle-même ([`src/dosing_logic.cpp:498-500`](../../src/dosing_logic.cpp)) :

```cpp
int nWin = in.horizonMinutes / in.windowMinutes;
if (nWin < 1) nWin = 1;
float v = remaining / static_cast<float>(nWin);
```

`v` — le volume par fenêtre — **croît quand l'horizon décroît**. Raccourcir l'horizon de dosage pour le faire coïncider avec une fin de cycle qui recule (le symptôme même de ce bug) ferait donc **grossir** chaque fenêtre de dosage à mesure que la journée avance, à l'exact opposé de l'effet recherché par une répartition lissée. Au pire cas, à 23:45 avec un horizon borné à la minute suivante, `nWin = 1` et **tout le reliquat de la journée part dans une seule fenêtre**.

`pool-chemistry` a chiffré ce cas sur un Mode Boost déclenché à 21:00 avec 600 mL de reliquat non injecté : **+1,7 mg/L de chlore libre** injectés de nuit, pour une cible de 2,5-3 mg/L en journée — un doublement de facto de l'intensité de dosage sur une fenêtre où aucune consommation par les baigneurs ne le justifie.

Trois garde-fous seraient **exigibles** avant de rouvrir cette option dans une variante future :

1. **Horizon borné à l'expiration du forçage en cours** (fin de `forceOn`/Boost), pas à minuit — minuit n'a de sens que pour le planning, pas pour un forçage temporaire.
2. **Plancher explicite sur `nWin`**, indépendant du `if (nWin < 1) nWin = 1` déjà présent, qui protège seulement contre la division par zéro, pas contre la concentration du reliquat.
3. **Dose par fenêtre jamais supérieure à la dose nominale calculée en plage horaire normale** — un contrôle a posteriori sur `v`, pas seulement sur `nWin`.

Sans ces trois conditions réunies, l'horizon de dosage **reste** aligné sur `filtrationCfg`, jamais sur une notion de fenêtre effective distincte.

## L'arbitrage RAM vs NVS

Le planning en attente n'est **jamais persisté**. L'alternative — deux écritures NVS par jour (une au calcul, une à la promotion) et un chemin de rejeu au boot pour retrouver un `pending` en cours avant sa promotion — a été écartée : le coût du choix RAM est un jour de planning périmé en cas de reboot avant minuit, **auto-réparateur** au cycle suivant (le prochain arrêt de cycle recalcule un `pending` neuf). Ajouter une seconde écriture NVS quotidienne et un chemin de reprise au boot pour économiser cette dégradation d'un jour n'a pas été jugé proportionné : c'est un coût borné, connu, et qui ne s'aggrave jamais (il ne peut se répéter que si l'appareil redémarre chaque soir avant minuit, ce qui est un problème d'un tout autre ordre).

## Les bornes de plausibilité `[4, 40] °C`

`isPlausibleReferenceTemp()` ([`src/schedule_logic.h`](../../src/schedule_logic.h:152)) borne la référence utilisée par `computeAutoWindow()`. Elle ferme deux cas dégénérés mesurés par `pool-chemistry` :

- **44 °C** : la fenêtre calculée commence à franchir minuit (`computeAutoWindow` centre `durée = T/2` sur le pivot 13h) — devenu **inatteignable** par la voie automatique, la borne haute étant fixée à 40 °C.
- **48 °C** : la durée sature à 24 h, `start == end`, `isMinutesInRange` rend **false** en permanence ⇒ la filtration ne tourne plus **jamais**, en silence. C'était le cas le plus grave : un mode dégradé rendu **persistant** par la bascule à minuit au lieu de rester transitoire (l'ancien recalcul en marche, bien que défectueux, au moins réévaluait la température à chaque cycle).

Les deux bornes sont traitées de façon **asymétrique**, et c'est délibéré :

- **Borne basse** (`< 4 °C`, verdict `TooCold`) : traitée comme un **hivernage normal**. `constants.h` le documente lui-même (« sous 4 °C : hivernage, pas de planning »). Log `info`, planning conservé, **aucune alerte MQTT**. Un `critical` et une alerte quotidienne tout l'hiver auraient appris à l'utilisateur à ignorer le canal d'alerte — c'est la même logique qui a produit l'arbitrage `TooCold`/`Implausible` distinct dans `RefTempVerdict`.
- **Borne haute** (`> 40 °C`, verdict `Implausible`) : traitée comme une **panne de sonde**. Log `critical` + alerte MQTT `filtration_temp_implausible` — aucune eau de bassin n'atteint cette température, la mesure est nécessairement fausse (repli sur une sonde `Circuit`, sonde déconnectée mal filtrée en amont, dérive électronique).

## La règle « post-purge ET en plage »

Elle remplace une exclusion qui avait été envisagée comme spécifique au Mode Boost. La règle générale — un échantillon n'est éligible que si la circulation est stabilisée **et** que l'horaire courant tombe dans la plage active — est plus simple **et** plus couvrante :

- Un Boost déclenché à 16:00, en pleine plage horaire normale, garde ses échantillons en plage et produit quand même un `pending` valide à l'arrêt du cycle — nul besoin de traiter le Boost à part.
- Le cas qui avait motivé l'exclusion spécifique — un `forceOn` nocturne qui ferait tourner la pompe hors plage et polluerait la moyenne de 240 échantillons tous froids — est fermé par la condition générale `inSchedule`, sans code dédié au Boost.

## Références

- Code : [`src/filtration.h`](../../src/filtration.h) (`PendingSchedule`, `CycleTempAccumulator`, méthodes privées), [`src/filtration.cpp`](../../src/filtration.cpp) (`sampleCycleTemperature`, `writePendingSchedule`, `promotePendingScheduleIfDue`, `ensureTimesValid` — réparation `start == end`), [`src/schedule_logic.h`](../../src/schedule_logic.h) / [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) (logique pure : `isSampleEligible`, `accumulateCycleTemp`, `isPlausibleReferenceTemp`, `selectFiltrationRefTemp`, `resolveReferenceTemp`, `decideSchedulePromotion`), [`src/constants.h`](../../src/constants.h:2352) (bornes, seuil de représentativité, `static_assert` anti-franchissement de minuit), [`src/dosing_logic.cpp:498-500`](../../src/dosing_logic.cpp) (formule de répartition `v = remaining / nWin`, motif du refus d'horizon)
- Tests : [`test/test_native_schedule/test_schedule_logic.cpp`](../../test/test_native_schedule/test_schedule_logic.cpp), [`test/test_native_temp_compensation/test_temp_compensation.cpp`](../../test/test_native_temp_compensation/test_temp_compensation.cpp) (sentinelle re-ciblée après le retrait de `_lastScheduledTemp`)
- Doc : [`docs/subsystems/filtration.md`](../subsystems/filtration.md), [`docs/features/page-filtration.md`](../features/page-filtration.md), [`docs/MQTT.md`](../MQTT.md), [`CHANGELOG.md`](../../CHANGELOG.md)
- Spec : [`bug-126`](../../specs/features/done/bug-126-le-recalcul-du-planning-annule-le-cycle-qui-l-a-produit.md)
- ADR liés : [ADR-0074](0074-temperature-figee-statu-quo-instrumente.md) (amendé dans ce lot — le mécanisme d'immunisation du planning contre une sonde figée change, la conclusion ne bouge pas), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique métier pure, Humble Object — [feature-038](../../specs/features/done/feature-038-filtration-horaire-testable.md) l'applique à `schedule_logic` sans ADR dédié, équivalence stricte préservée par ce lot), [ADR-0021](0021-repartition-scheduled.md) (`remainingRangeMinutes` comme horizon de répartition, non modifié par ce lot)
