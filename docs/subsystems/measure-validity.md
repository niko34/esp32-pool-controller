# Subsystem — `measure_validity`

- **Fichiers** : [`src/measure_validity.h`](../../src/measure_validity.h), [`src/measure_validity.cpp`](../../src/measure_validity.cpp) (coquille impure) + [`src/measure_validity_logic.h`](../../src/measure_validity_logic.h), [`src/measure_validity_logic.cpp`](../../src/measure_validity_logic.cpp) (logique pure)
- **Singleton** : `extern MeasureValidityManager measureValidity;`
- **Introduit par** : feature-061 (v2.13.0) ; étendu à la température en itération 2 (v2.13.0)

## Rôle

Décide si les mesures **pH/ORP/température sont représentatives de l'eau du bassin**. Les sondes sont montées dans le circuit de filtration : filtration arrêtée = eau stagnante autour des sondes → la valeur dérive localement (consommation du désinfectant dans le volume mort, absence de brassage) et ne reflète plus le bassin. À la reprise de la circulation, un **délai de purge** est appliqué (chasser l'eau stagnante de la chambre de mesure) avant de redéclarer les mesures valides.

La **température** est couverte depuis l'**itération 2** : la sonde DS18B20 est dans la canalisation → à l'arrêt, l'eau du tuyau dérive vers la température ambiante du local. **Même drapeau `measures_valid`, pas de validité séparée** : le délai de purge de 5 min est conservateur pour la température (l'eau du tuyau se renouvelle en < 1 min de circulation), mais une validité distincte ajouterait de la complexité pour un gain nul.

Le verdict est consommé par :
- **`history.recordDataPoint()`** — point 5 min : pH/ORP/température forcés à `NaN` si invalide (→ `null` sérialisé, trous dans les courbes) ; voir [history.md](history.md#exclusion-des-mesures-phorptempérature-sans-circulation-deau-feature-061-v2130) ;
- **WS `sensor_data` + `GET /data`** — champs `water_present` / `measures_valid` / `measures_stab_remaining_s`, l'UI arbitre l'affichage (badges + « `--` »).

**La chaîne de mesure interne ne change PAS** : les sondes continuent d'être lues, filtrées et publiées (les champs live `ph`/`orp`/`temperature`/`phRaw`/… restent présents dans les payloads — page calibration et onglet Avancé intacts). Seuls la **présentation** et la **persistance historique** changent. MQTT/HA : **hors périmètre**, inchangé.

**Mode Auto de la filtration non impacté** : le calcul de durée lit `sensors.getTemperature()` **en direct** (`filtration.cpp`), jamais le verdict de validité — par construction du pattern (la chaîne interne est intacte), l'extension température ne change rien à la filtration. Idem MQTT (live).

## Architecture (Humble Object, [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md))

- **`measure_validity_logic`** (pur, testable en natif — 14 tests Unity) : `evaluateMeasureValidity(MeasureValidityInputs) → MeasureValidityResult {valid, stabilizationRemainingS}`. Headers C uniquement, pas d'Arduino/FreeRTOS.
- **`measure_validity`** (coquille) : collecte les entrées, détecte le front montant d'eau, appelle la logique pure et **met le verdict en cache** sous spinlock `portMUX`.

### Règles de décision (ordre strict, priorité décroissante)

1. **Eau absente** → invalide, `remaining = 0` (pas de compte à rebours : on ignore quand la circulation reprendra) ;
2. **Délai de purge == 0** → valide, `remaining = 0` (purge désactivée : valide dès la reprise) ;
3. **Front montant jamais observé** (`waterRiseMs == 0`, sentinelle) → invalide, délai complet (conservateur — la coquille arme toujours le front, ce cas ne survient pas en pratique) ;
4. **`(nowMs − waterRiseMs) < délai`** → invalide, `remaining` = restant arrondi **seconde SUP** (soustraction non signée wrap-safe) ;
5. Sinon → **valide**, `remaining = 0`.

Frontières figées par les tests : `elapsed == délai − 1` → invalide (`remaining = 1 s`) ; `elapsed == délai` → valide.

### Délai de purge

Réutilise **`mqttCfg.stabilizationDelayMin`** (défaut **5 min**) — même sémantique que la garde de dosage : temps de renouvellement de l'eau dans la chambre de mesure. Aucune constante nouvelle. `stabilizationDelayMin = 0` → purge désactivée.

## API publique

```cpp
void update();          // loopTask, AVANT history.update() (le point 5 min lit le cache)
void getSnapshot(bool& waterPresent, bool& measuresValid,
                 uint32_t& stabilizationRemainingS) const;  // snapshot cohérent (1 section critique)
bool waterPresent() const;              // getters unitaires (cohérence non garantie ENTRE eux)
bool measuresValid() const;
uint32_t stabilizationRemainingS() const;
```

## Concurrence

Le cache est lu depuis **deux contextes** : `loopTask` (history, push WS) et `async_tcp` (`GET /data`) → snapshot sous spinlock `portMUX` (pattern `_externalMux` de filtration : section critique minimale, **jamais de mutex bloquant en contexte async**). Règle : les consommateurs lisent le **cache**, jamais `filtration.resolveWaterPresence()` directement — la résolution (source unique feature-056, [ADR-0026](../adr/0026-mode-installation.md)) n'est appelée que par `update()` sur `loopTask`.

Skew assumé (conservateur) : un consommateur peut voir un `water_present` frais d'un tour de loop avant que `measures_valid` du cache ne le reflète.

## Contrat des 3 champs (WS `sensor_data` + `GET /data`)

| Champ | Type | Sémantique |
|-------|------|------------|
| `water_present` | bool | Présence d'eau résolue (source unique feature-056). Déjà présent en WS depuis v2.19.0 ; **ajouté à `/data`** en feature-061 (lu depuis le cache). |
| `measures_valid` | bool | `true` = mesures pH/ORP/température représentatives du bassin (drapeau unique pour les 3 mesures — itération 2). `false` = eau absente **ou** purge post-reprise en cours. `false` au boot tant qu'`update()` n'a pas tourné (conservateur). |
| `measures_stab_remaining_s` | uint | Secondes restantes de purge (arrondi seconde sup). `0` si valide **ou** eau absente. ⚠️ Nom **distinct** de `stabilization_remaining_s` (stabilisation post-calibration des **pompes**) : ici, purge de la **chambre de mesure**. |

## Consommateurs du verdict `valid`

| Consommateur | Usage |
|---|---|
| `history` | exclusion des points pH/ORP/température du point 5 min |
| UI (WS + `/data`) | badges « Filtration arrêtée » / « Stabilisation : ~N min » |
| `pump_controller` | suspension de l'injection pendant la purge |
| **`sensor_filter` (v2.20.3)** | **porte d'alimentation** des filtres pH/ORP — voir ci-dessous |

### Nouveau consommateur : la porte d'alimentation des filtres (v2.20.3)

Jusqu'en v2.20.2, le verdict `valid` suspendait l'**injection** mais pas l'**ingestion** : les filtres pH/ORP continuaient d'avaler les mesures de stagnation, leur médiane convergeait vers la valeur de l'eau immobile, puis cette médiane acquérait **autorité** sur les mesures représentatives suivantes — qu'elle rejetait une à une comme aberrantes. Résultat : verrouillage définitif du filtre pH et perte totale de régulation pH **et** chlore (incident du 2026-08-16, ≥ 3 h).

`SensorManager::_updateFilterGates()` lit désormais le **cache** (`getSnapshot()`, jamais `filtration.resolveWaterPresence()`) et le transmet aux deux filtres via `SensorFilter::setContext(waterPresent, measuresValid, nowMs)`, **avant** `addSample()` à chaque cycle de lecture. Porte fermée ⇒ l'échantillon n'entre ni dans le buffer, ni dans le détecteur de sonde figée, ni dans aucun compteur ; réouverture ⇒ ré-amorçage de l'estimateur.

L'information nécessaire **existait déjà** et était publiée : le correctif consiste à la **consommer**, pas à la produire. Détail du mécanisme : [sensors.md](sensors.md#porte-dalimentation-du-filtre-v2203) · décision : [ADR-0041](../adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md).

> ⚠️ Le plancher de gel de 60 s (`kSensorFilterGateFloorMs`) appliqué par le filtre après tout front montant d'eau est **indépendant** de `stabilizationDelayMin` : il couvre le cas où l'utilisateur a réglé le délai de purge à `0`. `measure_validity` n'est pas modifié sur ce point.

## Comportement au boot

Eau présente au premier `update()` → front montant armé au boot → **délai de purge complet appliqué** (conservateur : on ignore depuis quand l'eau circule ni ce qui a stagné pendant l'extinction).

## Logs

| Événement | Niveau | Message |
|---|---|---|
| Front montant d'eau (purge > 0) | INFO | `Reprise circulation — stabilisation des mesures pH/ORP N min` |
| Front montant d'eau (purge > 0), v2.20.3 | INFO | `[STAB] Bruts à la reprise : pH=6.957 ORP=556.0 mV (eau stagnante)` |
| Fin de purge (transition purge → valide uniquement) | INFO | `Stabilisation terminée — mesures pH/ORP de nouveau représentatives` |
| Fin de purge, v2.20.3 | INFO | `[STAB] Bruts en fin de purge : pH=7.210 (delta +0.253) ORP=612.0 mV (delta +56.0)` |

Une retombée d'eau **pendant** la purge n'émet rien.

### Instrumentation des bruts au front montant d'eau (v2.20.3)

`measure_validity` mémorise les **bruts EZO** (`sensors.getPhRaw()` / `getOrpRaw()`, donc **sans** offset pH — [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)) à l'instant du front montant, puis journalise en fin de purge les nouveaux bruts **et le delta**.

But : **trancher a posteriori l'origine des excursions de mesure à la reprise de circulation**, question laissée **ouverte** par le cycle v2.20.3 :

- delta pH **fortement positif** (eau stagnante nettement plus acide que le bassin) ⇒ confirme l'hypothèse de **stagnation acide** dans la chambre de mesure, au voisinage du point d'injection ;
- delta **quasi nul avec une valeur aberrante** ⇒ oriente vers une **défaillance de sonde**, à instruire séparément.

Formatage NaN-safe : une mesure absente s'écrit `n/a`, **jamais** `nan` ni `0.00` (qui se confondrait avec une vraie valeur dans le journal). Le delta vaut `n/a` si l'une des deux bornes manque.

> ⚠️ Ce module inclut désormais `sensors.h` (lecture seule de deux accesseurs scalaires, contexte `loopTask`). C'est le seul couplage ajouté ; la logique **pure** de `measure_validity_logic` reste intacte et sans dépendance.

## Cas limites

- **`stabilizationDelayMin` modifié pendant une purge** : pris en compte au tour de loop suivant (le délai est relu à chaque `update()`).
- **`millis() == 0` au front montant** : armé à `1` (évite la sentinelle « jamais »).
- **Repli HTTP `/data`** : ne porte pas `filtration_state_stale` (asymétrie préexistante) — l'UI en repli HTTP dégrade « Signal filtration périmé » en « Filtration arrêtée » ; voir [API.md](../API.md#get-data--write).

## Fichiers liés

- [`src/main.cpp`](../../src/main.cpp) — `measureValidity.update()` dans la loop, **avant** `history.update()`
- [`src/history.cpp`](../../src/history.cpp) — exclusion pH/ORP/température du point 5 min
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) — publication des champs
- [`test/test_native_measure_validity/`](../../test/test_native_measure_validity/) — 14 tests natifs de la logique pure
- [filtration.md](filtration.md) — `resolveWaterPresence()`, source unique ([ADR-0026](../adr/0026-mode-installation.md))
