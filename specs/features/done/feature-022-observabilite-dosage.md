## Métadonnées
- **ID** : feature-022
- **Slug** : observabilite-dosage
- **Type** : feature
- **Statut** : done
- **Priorité** : normale
- **Version cible** : **2.10.0**
- **Dernière mise à jour** : 2026-07-05 (**livrée en v2.10.0** — périmètre = bloc RE-SCOPING ci-dessous, il REMPLACE le périmètre d'origine ; voir « Notes d'implémentation » en fin de spec)

## RE-SCOPING (2026-07-05) — périmètre exécuté en 2 passes, v2.10.0

Audit contre le code v2.9.2 : le point 1 d'origine est ~fait (enum `DoseRefusal` 036, WS `phDoseBlockedReason` 025, chips/boutons 006). Périmètre retenu :

### Passe 1 — observabilité HA/dashboard (trivial)
- [x] **P1.1 (ex-R1)** : auto-discovery HA `binary_sensor` `device_class: problem` pour l'indisponibilité capteur pH/ORP. Le topic `alerts/sensor_stale` existant publie du JSON/payload vide (inadapté à un binary_sensor) → publier un état simple dédié (ON/OFF retain) + bloc discovery calqué sur `ph_limit` (mqtt_manager.cpp l.1071-1077). *Livré : `{base}/ph|orp_sensor_problem` (ON = stale OU figé, dédupliqué) + 2 entités « Capteur pH/ORP — problème ».*
- [x] **P1.2 (ex-R2/R3)** : segment « Capteur indisponible » dans le badge multi-états du dashboard (slot réservé par feature-012 dans `composeSensorBadge`, priorité danger). Piloté par les signaux existants (`ph`/`orp` null dans le WS) — **aucun champ WS nouveau** (R3 abandonné). *Livré : priorité 0 (la plus haute), garde anti-faux-positif au boot.*

### Passe 2 — détection capteur figé (sécurité, logique pure)
- [x] **P2.1 (ex-point 4 + R4)** : détection variance nulle pH/ORP en **logique pure testable** (module `sensor_filter`, ADR-0017) : fenêtre glissante de N lectures valides, si étendue < epsilon → capteur figé. Fenêtre/epsilon à valider par pool-chemistry (piège : eau parfaitement stable = faux positif). *Livré : classe pure `FrozenDetector` (« run ancré » O(1) au lieu d'une fenêtre stockée), N=30, epsilon = ½ LSB — validé pool-chemistry.*
- [x] **P2.2** : intégration fail-closed SANS toucher à l'ordre des gardes verrouillé : capteur figé ⇒ `isPhFilterReady() = false` (la garde `FilterNotReady` existante de `evaluateDose` fait foi) + log critical edge-triggered + alerte MQTT (topic alerts existant) + reflet UI via les chemins existants. Pas de nouvelle valeur d'enum `DoseRefusal` (arbitrage confirmé par pool-chemistry). *Livré : log `[SENSOR_FROZEN]` + alerte `{base}/alerts/sensor_frozen` (JSON retain, clear vide).*
- [x] **P2.3** : température (R4) : détection figée DS18B20 en **warning-only** (aucun impact dosage ; effets réels limités à la compensation EZO pH et au planning auto filtration). *Livré : `FrozenDetector` dédié N=900 (≈30 min), epsilon 0.03 °C, hors alerte MQTT.*

### Écartés explicitement (sur besoin constaté uniquement)
- Compteurs de refus par cause + topics `dose_refusal_counts` (point 2) — régulation stable, logique testée, besoin non démontré.
- Refresh périodique 30 min du cache cal_points (point 3) — ceinture-bretelles d'un cas hypothétique déjà mitigé (Pass 3.5/024).
- Topic MQTT retain `dose_refusal/*` (reliquat du point 1) — seulement si automatisations HA sur les refus un jour.
- Piste ring RAM injections (héritage 008) — inchangée, sur besoin constaté.
- **Spec(s) liée(s)** : `specs/features/done/feature-021-migration-atlas-ezo.md` (origine des recommandations pool-chemistry non bloquantes), `specs/features/done/feature-007-fraicheur-capteurs.md` (reliquats hérités à la clôture, cf. section dédiée)

> **⚠️ Note de fraîcheur (2026-07-04)** : cette spec date de mai et a été partiellement rattrapée par le code :
> - La **cause de refus est déjà exposée** : `evaluateDose()` (feature-036, `dosing_logic.h`) retourne un enum `DoseRefusal` (15 gardes), et le WS publie `phDoseBlockedReason` / `orpDoseBlockedReason` (feature-025, `ws_manager.cpp` l.204-207) consommés par l'UI (chips + boutons). → La **Pass A** et le champ WS de la Pass B sont essentiellement livrés ; reste : topics MQTT dédiés, compteurs par cause, ligne UI page Régulation.
> - `canDose(int)` n'est plus le point central : les gardes vivent dans la logique pure `dosing_logic` (ADR-0017), testée en natif. Adapter le vocabulaire de la spec à la relecture.
> - La contrainte « flash 98,8 % » est obsolète : layout v3 (ADR-0019) → marge app ~276 KB par slot.

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
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
- [x] pool-chemistry — **obligatoire** : modifie l'API et l'observabilité de `canDose()` qui régule le dosage chimique
- [ ] ux-ui-designer — uniquement si on choisit d'afficher les compteurs de refus dans une nouvelle surface UI
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Améliorer l'observabilité du dosage chimique en exposant la **cause de refus** de `canDose(int pumpIndex)` (au lieu d'un simple `bool`), en publiant des **métriques compteur** par cause, et en détectant un **capteur figé** (variance nulle sur N lectures consécutives — EZO qui répondrait sans erreur I²C mais toujours la même valeur).

## Contexte

feature-021 a livré un `canDose(int)` durci avec 10 garde-fous fail-closed et un log refus edge-triggered (1×/cause). En revue post-implémentation, `pool-chemistry` a recommandé 4 améliorations non bloquantes, consignées ici pour ne pas être perdues :

1. **`getDoseRefusalReason(int pumpIndex)`** retournant un enum (`OK`, `STALE`, `NOT_CALIBRATED`, `STABILIZING`, `DAILY_LIMIT`, `HOURLY_LIMIT`, `FILTRATION_OFF`, `BURST`, `WATCHDOG_OFF`, `MODE_NOT_AUTO`) pour exposer la cause à MQTT/UI sans parser les logs.
2. **Métrique compteur "refus dosage par cause"** dans `safetyLimits` (ou structure dédiée) pour observabilité long terme — un PID mal tuné saute aux yeux si `BURST` ou `STABILIZING` se compte par centaines.
3. **Refresh périodique async du cache cal_points** toutes les 30 min via la queue `_ezoQueue` (priorité basse, jamais bloquant) — belt-and-suspenders contre un hypothétique cas où l'EZO perdrait sa cal en runtime sans le signaler.
4. **Détection capteur figé** : variance nulle sur N lectures consécutives (par exemple `kSensorFrozenSampleCount = 30` à 5 s/cycle = 2.5 min) → bloquer dosage avec cause `SENSOR_FROZEN`.

Ces 4 points renforcent la sécurité chimique sans changer l'algorithme PID. Ils sont indépendants entre eux et peuvent être livrés en passes courtes.

## Reliquats hérités de feature-007 (clôturée supersédée le 2026-07-04)

feature-007 (fraîcheur capteurs) a été clôturée : son cœur sécurité (« le PID ne consomme jamais une valeur stale ») est intégralement couvert en plus strict par features 021/025/036 (`kSensorStaleTimeoutMs = 20 s`, `getPh()` → NaN si stale, garde `FilterNotReady` fail-closed testée en natif). Restent 4 éléments d'**observabilité** non couverts, reportés ici :

| # | Reliquat | Intérêt | Détail |
|---|---|---|---|
| R1 | **Auto-discovery HA `binary_sensor` `device_class: problem` pour le stale capteur** | **Élevé, coût trivial** | Le topic `{base}/alerts/sensor_stale` existe (feature-021, retain, edge-triggered) mais **sans discovery** → aucune entité HA automatique. Le pattern existe déjà dans le code pour `ph_limit` (`mqtt_manager.cpp` l.1071-1077) : à dupliquer. S'intègre naturellement à la Pass B (topics MQTT). |
| R2 | Badge stale explicite sur les **cartes dashboard** pH/ORP | Moyen (cosmétique) | Les pages détail sont couvertes (chips « EZO indisponible », feature-025/034) ; le dashboard ne montre que `--` + mini-chart gris, sans libellé. |
| R3 | Champs WS booléens dédiés `ph/orp_sensor_fresh` + cas « WS déconnecté → badge gris (inconnu), pas rouge » | Faible | L'info transite déjà via valeurs `null` + `phDoseBlockedReason`. À ne faire que si R2 est retenu et a besoin d'un signal explicite. |
| R4 | Fraîcheur **température** (timestamp + détection capteur figé DS18B20) | Faible | Panne franche déjà couverte (lecture 2 s → NaN immédiat). Un capteur **figé qui répond** ne serait pas détecté — sans impact dosage ; effets réels : compensation température EZO pH et planning auto filtration sur valeur figée. Recoupe la détection variance nulle (point 4 ci-dessus) : traiter ensemble. |

**Piste optionnelle héritée de feature-008 (clôturée écartée le 2026-07-04)** : si un besoin d'audit des injections émerge à l'usage, la variante rentable est un **ring RAM des ~50 dernières injections** (ts, produit, mode, volume, durée, cause d'arrêt) exposé via un GET simple + petite table UI — sans persistance, sans rotation, sans export CSV. À n'ouvrir que sur besoin constaté, pas préventivement.

## Comportement attendu

### Côté firmware
- `pump_controller::DoseRefusalReason` enum public dans `pump_controller.h`
- `DoseRefusalReason getLastDoseRefusalReason(int pumpIndex)` retourne la dernière cause de refus (mise à jour à chaque appel `canDose(int)` qui retourne `false`, remise à `OK` au 1er retour `true`)
- Compteurs incrémentés à chaque refus : `uint32_t refusalCount[2][kNbRefusalReasons]` exposés via getter
- Détection variance nulle : tableau circulaire `_phReadingHistory[kSensorFrozenSampleCount]` + `_orpReadingHistory[...]`. Si `max - min < kSensorFrozenEpsilon` (par exemple 0.001 pH ou 0.5 mV ORP) sur N lectures successives → `getPh()`/`getOrp()` retourne NaN ET log critical
- Refresh cache cal_points : nouveau type d'item de queue `EzoCmdKind::RefreshCalPoints`. Enqueue automatique toutes les 30 min depuis `Sensors::update()` (uniquement si queue libre). Mise à jour du cache à la réception.

### Côté MQTT
- Nouveau topic `{base}/dose_refusal/ph` (retain) — payload = string nom de la cause (ex. `"STALE"`, `"NOT_CALIBRATED"`)
- Nouveau topic `{base}/dose_refusal/orp` (retain)
- Compteurs publiés (non retain, low frequency, 1× par minute) sur `{base}/dose_refusal_counts/ph` et `/orp` avec payload JSON `{"DAILY_LIMIT": 12, "BURST": 3, ...}`

### Côté UI (page Régulation)
- Nouvelle ligne « Dernière cause de refus » sous chaque card pH / ORP avec valeur `latestSensorData.doseRefusalReason.ph` / `.orp`
- Optionnel (à arbitrer ux-ui-designer) : mini graphique "refus dosage par cause" sur une fenêtre 24h depuis les compteurs

### Cas limites
- Capteur figé sur valeur exacte 7.000 (pH parfait) : ne PAS bloquer si la stabilité est plausible. Discriminer par `kSensorFrozenSampleCount` long (2-5 min) + `kSensorFrozenEpsilon` strict (0 absolu, pas seulement < epsilon)
- Démarrage à froid : pas de variance pendant les premiers cycles → ignorer la détection figée tant que `_phReadingHistory` n'est pas plein

## Comportement actuel

- `canDose(int pumpIndex)` retourne `bool`. La cause de refus n'est visible que dans les logs `info` edge-triggered (`logRefusalOnce(pumpIndex, "...")`)
- Pas de compteur refus
- Cache cal_points jamais rafraîchi sauf après cal réussie ou retour de bus dégradé
- Pas de détection capteur figé

## Contraintes techniques

- Cohérence avec les 10 garde-fous existants — l'enum `DoseRefusalReason` doit couvrir TOUTES les causes actuelles
- Pas de modification du chemin chaud `canDose(int)` qui doit rester < 100 µs (pas d'allocation, pas d'I²C)
- Pas de régression sur les contrats publics existants (topics MQTT, payload WS, codes HTTP)
- Flash actuelle 98.8 % — cette feature doit rester sobre (target +1 KB max). Si dépassement, prioriser refusal reason + détection figée, reporter compteurs et refresh périodique en feature-023.

## Fichiers potentiellement concernés

- `src/pump_controller.h/cpp` — enum + getter + compteurs
- `src/sensors.h/cpp` — détection figée + refresh périodique cache
- `src/mqtt_manager.cpp` — nouveaux topics
- `src/ws_manager.cpp` — nouveau champ `doseRefusalReason`
- `src/constants.h` — `kSensorFrozenSampleCount`, `kSensorFrozenEpsilonPh`, `kSensorFrozenEpsilonOrp`, `kCacheRefreshIntervalMs = 30 * 60 * 1000`
- `data/index.html`, `data/app.js`, `data/app.css` — affichage cause refus
- `docs/subsystems/pump-controller.md`, `docs/subsystems/sensors.md`, `docs/MQTT.md`, `docs/API.md`

## Livrables — check-list

> Check-list ré-arbitrée par le RE-SCOPING 2026-07-05 : les items non cochés ci-dessous sont **écartés explicitement** (voir bloc RE-SCOPING), pas des oublis.

- [ ] Enum `DoseRefusalReason` exposé + getter — *déjà couvert par feature-036 (`DoseRefusal`) + feature-025 (WS `phDoseBlockedReason`)*
- [ ] 10 causes mappées dans `canDose(int)` + cause `SENSOR_FROZEN` ajoutée — *écarté : le figé passe par la garde `FilterNotReady` existante, pas de nouvelle valeur d'enum (arbitrage pool-chemistry)*
- [ ] Compteurs RAM par cause + getter — *écarté (besoin non démontré)*
- [x] Détection variance nulle pH/ORP sur fenêtre glissante — *livré P2.1/P2.2 (`FrozenDetector` run ancré)*
- [ ] Refresh périodique async cache cal_points — *écarté (cas hypothétique déjà mitigé Pass 3.5/024)*
- [ ] 2 nouveaux topics MQTT (cause + compteurs) — *écarté ; remplacé par `ph|orp_sensor_problem` + `alerts/sensor_frozen`*
- [ ] Champ WS `doseRefusalReason` — *déjà couvert (feature-025)*
- [ ] UI : ligne « Dernière cause de refus » page Régulation — *déjà couvert (chips feature-025/006)*
- [x] R1 — Auto-discovery HA `binary_sensor` `device_class: problem` (hérité feature-007) — *livré P1.1*
- [x] R2 — Badge stale sur cartes dashboard pH/ORP (hérité feature-007) — *livré P1.2 (segment « Capteur indisponible »)*
- [ ] R3 — Champs WS `ph/orp_sensor_fresh` dédiés — *abandonné (signaux `null` existants suffisent)*
- [x] R4 — Fraîcheur température / DS18B20 figé (hérité feature-007) — *livré P2.3 (warning-only)*
- [x] `pio run` SUCCESS, 0 nouveau warning, marge flash respectée
- [x] Doc à jour (subsystems + MQTT + dashboard + CHANGELOG)

## Sécurité chimique (obligatoire — pool-chemistry doit valider)

- **Dosage impliqué : oui** — modifie l'API `canDose()` qui pilote directement les pompes
- **Algorithme PID touché : non** — uniquement l'observabilité
- **Risques** : la détection capteur figé peut produire des faux positifs (eau parfaitement stable). Calibrage de `kSensorFrozenSampleCount` et `kSensorFrozenEpsilon` à valider par pool-chemistry. Trop strict → faux positifs ; trop laxiste → la détection est inutile.

## ADR attendu ?
- **Probable** : non. Pas de contrainte structurante, pas de verrouillage long terme, pas d'alternatives à arbitrer.
- Si la détection capteur figé est complexe à calibrer ou nécessite une stratégie multicouche → ADR potentiel.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun (sauf si on expose un endpoint diagnostic `GET /dose_refusal/{ph|orp}` — à arbitrer)

### Champs WebSocket
- `doseRefusalReason` : `{ ph: string, orp: string }`

### Topics MQTT
- `{base}/dose_refusal/ph` (retain)
- `{base}/dose_refusal/orp` (retain)
- `{base}/dose_refusal_counts/ph` (no retain, low frequency)
- `{base}/dose_refusal_counts/orp` (no retain, low frequency)

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/features/page-regulation.md` (ou `page-ph.md` / `page-orp.md`) — ligne « Dernière cause de refus »

#### Docs transverses
- `docs/MQTT.md` — 4 nouveaux topics
- `docs/API.md` — champ WS `doseRefusalReason`
- `docs/subsystems/pump-controller.md` — enum + détection figée
- `docs/subsystems/sensors.md` — refresh périodique cache cal_points

#### Docs immuables
- aucun

## Critères d'acceptation
- [ ] AC1 — `canDose(int)` retourne `false` avec cause exacte loggée et exposée via `getLastDoseRefusalReason` — *hors périmètre re-scopé (déjà couvert par features 036/025)*
- [ ] AC2 — Compteurs incrémentés à chaque refus, exposés via topic MQTT — *écarté (re-scoping)*
- [x] AC3 — Détection capteur figé bloque dosage après 30 lectures identiques (à 5s/cycle = 2.5 min) — *livré ; la cause est portée par le log `[SENSOR_FROZEN]` + garde `FilterNotReady`, pas par une valeur `SENSOR_FROZEN` d'enum (arbitrage pool-chemistry). Couvert par tests natifs (`test_frozen_30_identical_readings_blocks_ready`)*
- [ ] AC4 — Refresh cache cal_points effectif toutes les 30 min sans bloquer la queue — *écarté (re-scoping)*
- [x] AC5 — Pas de régression sur les garde-fous existants — *163/163 tests natifs verts, aucune modification des gardes de `evaluateDose`*

## Plan de test manuel
1. Forcer une cause de refus (ex. débrancher EZO pH) → vérifier topic `dose_refusal/ph` = `STALE` (ou `NOT_CALIBRATED` selon timing)
2. Provoquer 5 cycles de dosage en rafale (manuel) → vérifier compteur `BURST` incrémenté + topic compteurs publié
3. Bloquer la sonde dans une eau parfaitement stable 3 min → vérifier `SENSOR_FROZEN` détecté
4. Attendre 30 min après boot → vérifier qu'un `Cal,?` async a été émis (log) sans blocage du loopTask

## Notes pour agent

- Cette feature est un **follow-up** de feature-021. Toutes les recommandations proviennent de la revue `pool-chemistry` post-Pass 3.
- Découpage suggéré en 3 passes courtes pour éviter timeouts agents :
  - Pass A : enum + `getLastDoseRefusalReason` + 10 causes mappées (sans compteurs ni détection figée)
  - Pass B : compteurs + topics MQTT + champ WS + UI ligne refus
  - Pass C : détection variance nulle + refresh périodique cache
- Cohérence enum : si une nouvelle garde est ajoutée à `canDose(int)` après cette feature, l'enum doit être étendue.

## Hors périmètre
- Modification de l'algorithme PID (algorithme inchangé)
- Modification des limites de dosage (`maxPhMinusMlPerDay`, etc.)
- Persistance des compteurs (RAM uniquement, reset au boot — historique de niveau supérieur reste à l'historique projet)
- Test unitaire (delégué à feature-023)

## Notes d'implémentation (2026-07-05, v2.10.0)

### Design retenu — « run ancré » au lieu d'une fenêtre glissante

La spec d'origine prévoyait un tableau circulaire de N lectures (`_phReadingHistory[30]`, 240 octets × 3 détecteurs). Implémentation finale : classe pure `FrozenDetector` (`src/sensor_filter.h/cpp`) en **O(1)** mémoire et CPU — 3 champs seulement (`runMin`/`runMax`/`runCount`). Chaque échantillon accepté qui garde `(max − min) < epsilon` étend les bornes et allonge le run ; dès qu'une lecture sort de la bande, le run est **ré-ancré** sur elle (`runCount = 1`) → sortie **immédiate** de l'état figé, **sans re-warmup** (la valeur figée était stable par définition, le filtre est resté cohérent). `samples = 0` désactive la détection (préserve l'init positionnelle courte de `SensorFilter::Config` dans les tests existants — champs ajoutés en fin de struct, agrégat gnu++11 conservé).

Composition dans `SensorFilter`, alimentée **uniquement par les bruts ACCEPTÉS** : l'état figé persiste pendant une rafale de rejets (condition pool-chemistry #4). `reset()` du filtre réinitialise aussi le détecteur (condition #2). Figé ⇒ `ready() == false` ⇒ garde `FilterNotReady` existante de `evaluateDose` — **aucune modification des gardes**, pas de nouvelle valeur `DoseRefusal`.

### Valeurs finales et justification ½ LSB (condition pool-chemistry #1)

| Constante | Valeur | Justification |
|---|---|---|
| `kSensorFrozenSamples` | 30 (≈ 2,5 min à 5 s) | latence de détection vs faux positifs |
| `kSensorFrozenEpsilonPh` | 0.0005 | **½ LSB** EZO pH (résolution 0.001) |
| `kSensorFrozenEpsilonOrp` | 0.05 mV | **½ LSB** EZO ORP (résolution 0.1 mV) |
| `kTempFrozenSamples` | 900 (≈ 30 min à 2 s) | l'eau d'un bassin varie lentement mais jamais à 0.0625 °C près sur 30 min sonde immergée |
| `kTempFrozenEpsilonC` | 0.03 °C | < ½ LSB DS18B20 12 bits (0.0625/2) — alimenté par la valeur **brute non arrondie** |

Rationale ½ LSB : un capteur **vivant** bruite d'au moins ±1 LSB (quantification + bruit sonde/thermique) ; avec epsilon = ½ LSB, un toggle de 1 LSB — même en float32 — casse le run. Aucune eau réelle, même parfaitement stable, ne peut être déclarée figée tant que la dernière décimale bouge. **Consigne terrain : si faux positifs, durcir N — ne JAMAIS élargir epsilon.**

### Verdicts

- **pool-chemistry** : PRÉ GO SOUS CONDITIONS (4) → POST GO, les 4 conditions levées (#1 epsilon ½ LSB, #2 reset détecteur dans `reset()`, #3 log `[SENSOR_FROZEN]` nommé explicitement — la garde mutualisée `FilterNotReady` ne dit pas la cause, #4 persistance pendant rafale de rejets). Recommandation non bloquante : donner une visibilité HA au figé **température** (aujourd'hui log warning seulement, hors `sensor_problem` et hors alerte) dans une passe ultérieure.
- **code-reviewer** : Approved, 2 mineures non bloquantes : (1) changer le **rôle** d'une sonde température à chaud ne reset pas le run du `FrozenDetector` T° — impact nul en pratique ; (2) ajouter un jour un commentaire sur l'interaction re-sync/frozen dans `sensor_filter.cpp`.
- **test-engineer** : **11 tests natifs ajoutés** (`test/test_native_sensor_filter/` : constantes, 30 lectures → bloque `ready()`, toggle 1 LSB casse le run, 29 échantillons insuffisants, sortie immédiate sans re-warmup, persistance pendant rafale de rejets, clear par `reset()`, étendue == epsilon casse le run, config legacy = désactivé, `samples=0` jamais figé, saturation `runCount` sans overflow) — **163/163 verts**, `pio run` 0 warning, `build_fs` OK.

### Reste on-target (matériel requis)

- Entités HA « Capteur pH/ORP — problème » visibles et fonctionnelles (ON sur stale, ON sur figé, OFF au retour).
- Segment « Capteur indisponible » du badge dashboard.
- Simulation d'un capteur figé difficile sans matériel dédié : **maintenir la sonde pH dans un tampon très stable ne suffira PAS** (le bruit LSB d'un capteur vivant casse le run — c'est le design). Le vrai test est un **EZO en panne** (module qui répond I²C mais valeur figée) ; à défaut, vérifier via les logs `[SENSOR_FROZEN]` sur incident terrain réel.
