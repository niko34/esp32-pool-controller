## Métadonnées
- **ID** : feature-068
- **Slug** : sonde-temperature-eau-figee
- **Type** : feature
- **Statut** : **terminé (2026-09-07)** — tests verts, revue OK, livré en v2.47.1
- **Priorité** : haute
- **Version cible** : **2.47.1** (la cible initiale 2.21.0 n'a pas été tenue : la spec est restée en attente jusqu'au verdict chimique du 07/09)
- **Dernière mise à jour** : 2026-09-07
- **Spec(s) liée(s)** : `bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue` (observation annexe du même incident), feature-022 (détection capteur figé), ADR-0041

## ⚠️ VERDICT `pool-chemistry` DU 2026-09-07 — LA PRÉMISSE DE CETTE SPEC EST RENVERSÉE

**Livrable A1 produit, et il change tout.** Méthode : pente de Nernst
`S(T) = 2,303·R·T/F` = 59,16 mV/pH à 25 °C ; l'EZO calcule
`pH = 7 + (E_iso − E)/S(T_comp)`, donc à potentiel d'électrode donné :

> `pH_lu − pH_vrai = (pH_vrai − 7) × (T_vrai − T_comp)/T_comp`  (T en kelvins)

Soit **≈ 0,7 millième de pH par °C d'erreur**. Le cas de cette spec (26,6 figé contre
25,0 réel, pH brut ≈ 6,80 puisque l'offset est de +0,50) donne **+0,0011 pH**.

Il faut **≈ 15 °C** d'erreur pour atteindre la bande morte la plus serrée (0,01) et
**≈ 75 °C** pour 0,05. Comparé aux seuils réels — bande morte 0,01-0,05, consigne 7,30,
plancher 6,70/6,20, inhibition ORP 7,60 — l'erreur est **2 à 3 ordres de grandeur en
dessous** : **chimiquement non signifiante**.

Sens de l'erreur : `T_comp` trop haute ⇒ pente surestimée ⇒ lecture tirée vers 7. Le
brut étant sous 7, le pH est lu **trop basique** ⇒ **léger sur-dosage** d'acide (ici
+0,001 pH, soit ~0 mL). Symétrie inversée si le brut passait au-dessus de 7.

**Bornage déjà en place** : les lectures DS18B20 sont filtrées à `(−55 ; 125)` °C avec
rejet de `85.0` (power-on reset) et de `DEVICE_DISCONNECTED_C` (`src/sensors.cpp:324-326`).
Même aux bornes de cette fenêtre, l'erreur plafonne à **0,05-0,07 pH**.

### Option retenue : **A / A instrumenté** — et l'option C est REFUSÉE
- **Compensation pH** : inchangée. On instrumente, on ne corrige pas — il n'y a rien à
  corriger d'un point de vue chimique.
- **Planning de filtration** : **déjà immunisé, rien à coder.** `src/filtration.cpp:86`
  (NaN ⇒ planning conservé), `:89` (bande morte de 1 °C ⇒ une T° figée ne recalcule rien),
  `src/schedule_logic.cpp:64-67` (durée clampée à [1 h ; 24 h]). Pour le planning,
  **A = C de fait**.
- **Option C refusée comme disproportionnée.** Coût du faux positif : pH non corrigé ⇒
  montée du pH ⇒ chute de la fraction HOCl (pKa 7,54 : 52 % à 7,50, **41 % à 7,70**) ⇒ la
  boucle ORP, qui **sature déjà son quota horaire de 10 min/h**, ne peut pas compenser, et
  l'inhibition à 7,60 la coupe. On échangerait **0,001 pH** d'erreur contre une
  **sous-chloration réelle en quelques heures**.

### ⛔ Piège : « suspendre la compensation » N'EXISTE PAS
Ne pas envoyer `RT` (`src/atlas_ezo.cpp:149-155`) fait retomber sur `R` : le module
**conserve sa dernière température interne**, silencieusement périmée. Cette option est
**interdite**, ici comme dans `bug-114`.

### Ce qui reste à faire dans cette spec
Uniquement le **rendre visible** (A4, A5, B1) et la documentation. A2 est tranché par ce
bloc ; A3 est **sans objet** pour le planning et réduit à néant pour la compensation.
Le vrai défaut de compensation est ailleurs : voir `bug-114`, dont la cible se déplace de
la **sonde** vers l'**offset de calibration non borné**.

### Arbitrages des cas limites (Q4)
1. **Sonde débranchée pendant l'épisode** : pas de « figé » indéfini. Les lectures
   invalides n'alimentant pas le détecteur, le drapeau doit être **effacé** après une
   grâce égale à la fenêtre stale existante, et remplacé par l'état « sonde absente ou
   invalide » — plus sévère, et déjà géré par le repli 25 °C.
2. **Aucune sonde identifiée** : ne **jamais** publier « eau figée ». Publier « sonde eau
   non identifiée ». Ne pas étendre le détecteur à un rôle inconnu : l'alerte ne serait
   pas actionnable.
3. **Sortie de l'état figé** : **immédiate, sans hystérésis** — le drapeau est purement
   informatif en option A, un battement ne coûte rien chimiquement, et une hystérésis
   ajouterait un second seuil à régler. Anti-battement éventuel **uniquement** à la
   publication MQTT (maintien 60 s).

### ADR attendu — **CRÉÉ : [ADR-0074](../../../docs/adr/0074-temperature-figee-statu-quo-instrumente.md)**
Critère 2 (décision invisible dans le code) : **« pourquoi on ne fait rien du figé sur la
compensation »**. Le chiffrage ci-dessus en est le corps. Sans cet ADR, quelqu'un
réouvrira l'option C en lisant le message de log, qui annonce un enjeu que la physique ne
soutient pas.

### Recommandation non bloquante
Publier la température **effectivement envoyée** à l'EZO : c'est le seul diagnostic qui
distingue une valeur figée, un repli et un offset.

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
- [x] A1 — Chiffrage de l'erreur de pH induite par une compensation T° fausse — **et c'est lui qui a renversé la prémisse de la spec** : ≈ 0,7 millième de pH par °C, **+0,0011 pH** sur le cas relevé, 2 à 3 ordres de grandeur sous le premier seuil de régulation
- [x] A2 — Décision **A instrumenté** actée, et **tranchée par [ADR-0074](../../../docs/adr/0074-temperature-figee-statu-quo-instrumente.md)** (option C refusée, motif écrit)
- [x] A3 — **SANS OBJET** : pour le planning, les gardes existantes rendent A et C **identiques en fait** (`filtration.cpp:86` NaN ⇒ planning conservé, `:89` bande morte 1 °C, `schedule_logic.cpp:64-67` durée clampée `[1 h ; 24 h]`) ; pour la compensation, le traitement retenu **est** le statu quo. Zéro ligne de traitement métier écrite — c'est le résultat, pas une omission
- [x] A4 — Champ `tempFrozenAlarm` en **parité stricte** `/data` ↔ WS (`+24 o`, marge résiduelle **43 o**)
- [x] A5 — Topic retain `<base>/temp_sensor_frozen` + `binary_sensor` HA `device_class: problem`, garde « aucune sonde eau », maintien 60 s **à la publication seulement**
- [x] B1 — Badge « Sonde température figée » (`warning`) + « Sonde eau non identifiée » (`danger`) sur le tableau de bord **et** la page Température, avec précédence explicite
- [x] C1 — `docs/subsystems/sensors.md`, `docs/features/page-temperature.md`, `docs/features/page-dashboard.md`, `docs/API.md`, `docs/MQTT.md`, `docs/subsystems/ws-manager.md`, `CHANGELOG.md`, `docs/user/src/guide-utilisation.html` + **ADR-0074**
- [x] D1 — Tests natifs — `test/test_native_temp_compensation/` (mutualisé avec `bug-114`) : cascade, bornes, libellés d'étage, **non-régression** (température figée ⇒ `RT` inchangé, planning inchangé, dosage inchangé) et **sentinelles structurelles**

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
- [x] Quand la sonde T° eau est figée, l'état est **visible** en UI (badge tableau de bord + page Température), en `/data`, en WS (`tempFrozenAlarm`) et en MQTT (`<base>/temp_sensor_frozen` + `binary_sensor` HA)
- [x] Le traitement décidé (**statu quo instrumenté**) est appliqué de façon **déterministe** et couvert par un test natif — y compris son versant négatif : `pump_controller.cpp` ne référence **aucun** des nouveaux symboles, `DoseInputs` n'a **aucun** champ de température, `computeAutoWindow` reste bornée `[60 ; 1440]` min même sur −127 / −55 / 0 / 85 / 125 / 1025
- [x] Sortie de l'état figé : levée **immédiate** à la première lecture hors bande, badge retiré, `INFO` journalisé (le maintien de 60 s existe **uniquement** à la publication MQTT — décision du visa, sans hystérésis firmware)
- [ ] Aucun faux positif sur un bassin en fonctionnement normal (relevé sur ≥ 48 h) — **observation terrain, hors cycle** ; le coût d'un faux positif est un badge de trop, aucune décision n'en dépend (ADR-0074)
- [x] Verdict `pool-chemistry` GO explicite sur le traitement retenu — verdict du 2026-09-07 en tête de spec, formalisé en ADR-0074
- [x] Tous les libellés utilisateur sont en français
- [x] `pio run` compile sans nouveau warning
- [x] `./build_fs.sh` passe sans dépasser la partition
- [ ] Comportement nominal testé manuellement selon le plan de test ci-dessous — **à faire au déploiement** (étapes 2 à 4 exigent de forcer une valeur constante puis de débrancher la sonde sur l'appareil)

## Notes de clôture (2026-09-07, v2.47.1)

**Ce que ce cycle a établi avant tout** : la **prémisse de cette spec était quantitativement fausse**, et c'est le chiffrage **A1** qui l'a montré. Le message de log — « compensation pH et planning filtration sur valeur figée » — annonçait un enjeu que la physique ne soutient pas : **+0,0011 pH** sur le cas relevé, quand il faudrait **≈ 15 °C** d'erreur de compensation pour atteindre la bande morte la plus serrée (0,01) et **≈ 75 °C** pour 0,05. La spec demandait de choisir entre trois traitements ; la bonne réponse était **de ne rien traiter et de tout montrer**.

**Livré** : `src/sensors.{h,cpp}`, `src/ws_manager.cpp`, `src/web_routes_data.cpp`, `src/mqtt_manager.{h,cpp}`, `data/app.js`, `data/index.html` (+ le socle commun avec `bug-114` : `src/temp_compensation_logic.h`, `src/constants.h`, `src/config.cpp`, `src/web_routes_config.cpp`, `test/test_native_temp_compensation/`).

**Preuves** : **40 suites / 1 535 cas** natifs (mesuré le 2026-09-07 : `ls -d test/test_native_* | wc -l`, `grep -rn 'RUN_TEST(' test/ | wc -l`), **13 mutants — 12 tués + 1 équivalent PROUVÉ** (garde `NaN` de la fenêtre : IEEE-754 rend `NaN >= borne` faux ; un test échouerait si `-ffast-math` était ajouté, **vérifié absent de `platformio.ini`**).

**Décisions du visa reportées dans la doc** : sortie de l'état figé **immédiate** (anti-battement de 60 s à la **publication MQTT seulement**) ; température **hors** de `alerts/sensor_frozen` (contrat externe : « le dosage est compromis ») ; « sonde eau non identifiée » **jamais** confondu avec « eau figée ».

**Reste ouvert** : les deux critères non cochés ci-dessus (observation ≥ 48 h et plan de test manuel), tous deux **on-target**. Et le message de log de feature-022 n'a **pas** été réécrit : il est factuellement exact, le contexte manquant est désormais dans ADR-0074.

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
