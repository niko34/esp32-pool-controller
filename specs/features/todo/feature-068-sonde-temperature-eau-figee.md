## Métadonnées
- **ID** : feature-068
- **Slug** : sonde-temperature-eau-figee
- **Type** : feature
- **Statut** : todo
- **Priorité** : haute
- **Version cible** : 2.21.0
- **Dernière mise à jour** : 2026-08-21
- **Spec(s) liée(s)** : `bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue` (observation annexe du même incident), feature-022 (détection capteur figé), ADR-0041

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — la température figée alimente la **compensation pH de l'EZO** et le **planning auto de filtration**
- [ ] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

La sonde de température d'eau a été détectée **figée** pendant l'incident du 2026-08-16, sans qu'aucune conséquence n'en soit tirée : la valeur figée continue d'alimenter la compensation de température du pH et le planning automatique de filtration. Il faut décider quoi faire d'une température qu'on sait fausse, et le rendre visible.

## Contexte

Relevé dans le journal de l'incident :

```
[SENSOR_FROZEN] Sonde T° eau figée : 900 lectures dans une bande < 0.03 °C autour de
26.6 °C — compensation pH et planning filtration sur valeur figée
```

900 lectures à ~2 s d'intervalle ≈ **30 min** sans la moindre variation de quantification (l'epsilon 0,03 °C est inférieur au ½ LSB du DS18B20, donc une sonde vivante casse le run — cf. feature-022, condition `pool-chemistry` #1).

La détection existe (`isTemperatureFrozen()`), elle est **warning-only** et **assumée comme telle** aujourd'hui : aucun impact dosage, aucune remontée MQTT, aucune indication UI. Le message de log dit lui-même ce qui est en jeu :

1. **Compensation T° du pH** : la valeur est envoyée à l'EZO pH (`RT,<t>`). Une température fausse fausse la mesure de pH — donc, en cascade, la régulation pH **et** l'inhibition ORP sur pH brut (feature-067).
2. **Planning automatique de filtration** : la durée de filtration calculée sur la température de l'eau devient fausse.

Le cycle v2.20.3 (verrouillage du filtre pH) a explicitement laissé ce point hors périmètre, en le renvoyant vers une spec dédiée.

## Comportement attendu

À arbitrer par `architect` + `pool-chemistry`, mais le cadre est le suivant.

### 1. Rendre l'état visible

- Champ WebSocket / `GET /data` exposant l'état « sonde T° eau figée » (nom à décider, aligné sur la convention existante `phFilterAlarm` / `orpFilterAlarm`).
- Badge sur la **carte Température** du tableau de bord et sur la page `/temperature`, en français.
- Topic MQTT retain + `binary_sensor` HA `device_class: problem` (patron `ph_filter_stuck`, v2.20.3).
- Alerte `alerts/sensor_frozen` : **décider** si la température y entre (aujourd'hui exclue volontairement : sévérité jugée différente) ou si un type d'alerte distinct est préférable.

### 2. Décider du traitement métier

Trois options à trancher (au moins) :

| Option | Compensation pH | Planning filtration |
|---|---|---|
| **A — statu quo instrumenté** | continue sur la valeur figée | continue | 
| **B — repli explicite** | bascule sur `kEzoFallbackTempC` (25 °C) comme si la sonde était absente | repli sur une durée par défaut |
| **C — fail-closed** | suspend la régulation pH automatique tant que la sonde est figée | conserve le dernier planning valide |

`pool-chemistry` doit statuer : une compensation à 25 °C **connue comme approximative** est-elle préférable à une compensation à 26,6 °C **peut-être juste, peut-être fausse depuis des heures** ? L'écart de pH induit par 1,6 °C d'erreur doit être chiffré dans la spec avant décision.

### Cas limites

- **Bassin réellement stable** : une eau à température constante au 1/100 °C près pendant 30 min est physiquement improbable sur un DS18B20 (bruit de quantification), mais le cas doit être discuté — le risque de faux positif conditionne le choix entre A, B et C.
- **Sortie de l'état figé** : la détection sort immédiatement dès une lecture hors bande (feature-022). Prévoir un `INFO` de levée et le retrait du badge.
- **Sonde débranchée pendant l'épisode** : `DEVICE_DISCONNECTED_C` ⇒ lecture invalide, qui n'alimente pas le détecteur. Vérifier qu'on ne reste pas « figé » indéfiniment sur un état obsolète.
- **Sonde unique vs double** (feature-020, sonde « eau » + « circuit ») : la détection ne concerne que la sonde identifiée « eau ». Comportement si aucune sonde n'est identifiée ?

## Comportement actuel

`FrozenDetector` alimenté par les lectures DS18B20 **valides** de la sonde « eau », accessible via `isTemperatureFrozen()`, journalisé en `warning` edge-triggered. **Aucun** autre effet : pas de champ WS, pas de badge, pas de MQTT, aucune influence sur la compensation ni sur le planning.

## Contraintes techniques
- ESP32, offline first
- Vanilla JS dans `data/` (pas de framework, pas de bundler)
- Français dans toute l'UI utilisateur
- Le détecteur `FrozenDetector` est une **classe pure** (ADR-0017) — toute nouvelle logique de décision doit y rester testable en `env:native`
- Bus OneWire : contrat **mono-appelant** (`SensorManager` uniquement)
- Ne pas dégrader le budget de payload WS sans le décompter explicitement (patron v2.20.3)

## Fichiers potentiellement concernés
- `src/sensors.{h,cpp}` — accesseur, journalisation, décision de repli
- `src/constants.h` — seuils (`kTempFrozenSamples`, `kTempFrozenEpsilonC`, éventuel délai avant effet)
- `src/pump_controller.cpp` — si option C (inhibition régulation pH)
- `src/filtration.cpp` — si le planning auto doit ignorer une T° figée
- `src/ws_manager.cpp`, `src/web_routes_data.cpp` — nouveau champ
- `src/mqtt_manager.{h,cpp}` — topic + auto-discovery
- `data/index.html`, `data/app.js`, `data/app.css` — badge
- `docs/subsystems/sensors.md`, `docs/features/page-temperature.md`, `docs/features/page-dashboard.md`, `docs/API.md`, `docs/MQTT.md`

## Livrables — check-list
- [ ] A1 — Chiffrage de l'erreur de pH induite par une compensation T° fausse (entrée du verdict `pool-chemistry`)
- [ ] A2 — Décision A / B / C actée (ADR si la décision est structurante)
- [ ] A3 — Traitement métier implémenté selon la décision
- [ ] A4 — Champ WS + `/data` exposant l'état figé
- [ ] A5 — Topic MQTT retain + `binary_sensor` HA `device_class: problem`
- [ ] B1 — Badge « Sonde température figée » (tableau de bord + page Température)
- [ ] C1 — `docs/subsystems/sensors.md`, `docs/features/page-temperature.md`, `docs/features/page-dashboard.md`, `docs/API.md`, `docs/MQTT.md`, `CHANGELOG.md`
- [ ] D1 — Tests natifs sur la logique de décision (repli / fail-closed)

## Sécurité chimique
- Dosage impliqué : **oui** (indirectement) — la compensation T° fausse la mesure de pH, donc la régulation pH et l'inhibition ORP sur pH brut
- Limites journalières / horaires concernées : aucune directement
- Filtration requise avant action : sans objet
- Calibration capteur modifiée : non
- Algorithme PID touché : non (l'entrée du PID est affectée, pas l'algorithme)
- Risque identifié : **doser sur un pH mesuré à une température fausse**. Un repli à 25 °C introduit une erreur **connue et bornée** ; conserver une valeur figée introduit une erreur **inconnue et non bornée**.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 2 (choix de ne PAS faire quelque chose / compromis entre options équivalentes), 3 (alternatives crédibles à tracer)
- **Sujet pressenti** : « Que faire d'une mesure de température qu'on sait figée : repli, fail-closed, ou statu quo instrumenté »

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /data` — nouveau booléen d'état figé (nom à décider)

### Champs WebSocket
- `sensor_data` — nouveau booléen d'état figé

### Topics MQTT
- `{base}/temp_sensor_frozen` (nom à décider, retain) + `binary_sensor` HA `device_class: problem`

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/features/page-temperature.md`, `docs/features/page-dashboard.md`
- `docs/subsystems/sensors.md`, éventuellement `docs/subsystems/filtration.md` et `docs/subsystems/pump-controller.md`

#### Docs transverses
- `docs/API.md`, `docs/MQTT.md`, `CHANGELOG.md`

#### Docs immuables
- `docs/adr/NNNN-<slug>.md` si la décision A/B/C est structurante

## Critères d'acceptation
- [ ] Quand la sonde T° eau est figée, l'état est **visible** en UI, en `/data`, en WS et en MQTT
- [ ] Le traitement décidé (repli / fail-closed / statu quo) est appliqué de façon **déterministe** et couvert par un test natif
- [ ] Sortie de l'état figé : levée immédiate à la première lecture hors bande, badge retiré, `INFO` journalisé
- [ ] Aucun faux positif sur un bassin en fonctionnement normal (relevé sur ≥ 48 h)
- [ ] Verdict `pool-chemistry` GO explicite sur le traitement retenu
- [ ] Tous les libellés utilisateur sont en français
- [ ] `pio run` compile sans nouveau warning
- [ ] `./build_fs.sh` passe sans dépasser la partition
- [ ] Comportement nominal testé manuellement selon le plan de test ci-dessous

## Plan de test manuel
1. **Nominal** : bassin en fonctionnement, sonde vivante → aucun badge, aucun topic `ON`, compensation T° inchangée.
2. **Sonde figée simulée** : forcer une valeur constante (sonde hors de l'eau dans un environnement stable, ou mock) → badge affiché, `ON` MQTT, comportement décidé effectivement appliqué, log `warning`.
3. **Sortie de l'état figé** : replonger la sonde → levée immédiate, badge retiré, `INFO`.
4. **Sonde débranchée** : vérifier qu'on ne reste pas bloqué sur un état « figé » obsolète.

## Notes pour agent
- Le message de log existant nomme déjà les deux consommateurs impactés : partir de là, ne pas en chercher d'autres sans vérification.
- Réutiliser les patrons livrés en v2.20.3 : drapeau firmware edge-triggered → champ `/data` + WS → topic retain + `binary_sensor` HA `device_class: problem` → badge UI. Ne pas inventer un nouveau mécanisme.
- ⚠️ Ne pas confondre avec la détection **pH/ORP** figé (feature-022), qui rend le filtre non prêt et bloque le dosage : la température n'a jamais eu cet effet, et c'est précisément la question posée.

## Hors périmètre
- Détection de sonde de température **de circuit** figée (sonde non utilisée par la compensation ni par le planning)
- Remplacement matériel de la sonde (décision utilisateur)
