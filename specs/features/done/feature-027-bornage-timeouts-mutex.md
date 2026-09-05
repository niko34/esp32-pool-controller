## Métadonnées
- **ID** : feature-027
- **Slug** : bornage-timeouts-mutex
- **Type** : feature
- **Statut** : done
- **Priorité** : haute
- **Version cible** : **2.11.1**
- **Dernière mise à jour** : 2026-07-06 (livrée — tests verts, revue OK ; reste la validation on-target AC3)
- **Spec(s) liée(s)** : aucune — issue de la revue de code projet (2026-06-20)

> **Annotations d'audit (2026-07-06)** — grep exhaustif sur v2.11.0 : **24 occurrences dans 4 fichiers** (mqtt_manager.cpp ×9, logger.cpp ×7, history.cpp ×6, config.cpp ×2). La spec ne connaissait ni logger.cpp ni config.cpp (cité à tort comme « bon exemple » — il contient les deux patterns). La feature-009 a répliqué le pattern dans les 2 nouveaux handlers de drainCommandQueue (inclus dans les ×9).
> 1. **Hiérarchie de risque** : le danger réel = mutex tenu pendant des I/O LittleFS avec la loopTask en attente (history._mutex pris par handlers HTTP async ; saveMqttConfig écrit LittleFS SOUS configMutex pendant que drainCommandQueue attend ; flush de persistance logger). Les sections courtes RAM pure sont bénignes — à borner pour l'uniformité, verdict documenté par site.
> 2. **Politique d'échec PAR SITE obligatoire** (le vrai travail) : logger → entrée perdue acceptable ; history → point sauté + log ; drainCommandQueue → commande reportée/abandonnée proprement ; saveMqttConfig → échec signalé (config non persistée). Throttle des logs d'échec.
> 3. AC3 non testable en natif (FreeRTOS pur) → validation par revue de concurrence + nominal on-target.
> 4. Version cible réalignée : 2.11.1 (robustesse interne, pas de fonctionnalité visible).

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Remplacer toutes les prises de mutex en **attente infinie** (`portMAX_DELAY`) par des prises **bornées** (`pdMS_TO_TICKS(kXxxTimeoutMs)`) avec gestion d'échec explicite, afin d'éliminer le risque de blocage de la `loopTask` et de reset watchdog.

## Contexte
La revue de code (2026-06-20) a relevé plusieurs prises de mutex en `portMAX_DELAY` dans `history.cpp` (`:161,169,371,411,421,667`) et `mqtt_manager.cpp` (`:430,444,530,817…`), alors que le reste du code borne systématiquement ses timeouts (ex. `config.cpp:402` avec `pdMS_TO_TICKS(kXxxTimeoutMs)`). Une attente infinie sur un mutex détenu par une autre tâche bloquée fige la `loopTask`, ce qui déclenche le watchdog (`kWatchdogTimeoutSec = 30s`) et un reboot — comportement inacceptable sur un contrôleur autonome. Cette incohérence touche la règle watchdog (#2).

## Comportement attendu
- Toute prise de mutex applicative utilise un timeout borné, cohérent avec les constantes existantes (`kI2cMutexTimeoutMs`, `kConfigMutexTimeoutMs`, etc.) ou une nouvelle constante dédiée si nécessaire.
- En cas d'échec de prise (timeout) : ne pas exécuter la section critique, logger un `warning`/`error` explicite, et retourner proprement (valeur par défaut / skip du cycle) sans corrompre l'état.
- Aucun chemin appelé depuis la `loopTask` ne doit pouvoir attendre indéfiniment un mutex.
- Le comportement nominal (mutex libre) reste inchangé.

### Cas limites
- Mutex détenu longtemps par une autre tâche (I/O LittleFS lente) → timeout → cycle sauté + log, pas de blocage.
- Contention élevée MQTT (publication pendant rotation historique) → dégradation gracieuse.
- Échec répété → throttle des logs pour ne pas saturer.

## Comportement actuel
- `history.cpp` et `mqtt_manager.cpp` prennent certains mutex avec `portMAX_DELAY` (attente infinie).
- Le reste du code (`config.cpp`, `logger.cpp`…) borne déjà ses timeouts → incohérence de pattern.

## Contraintes techniques
- ESP32 / FreeRTOS — respecter le pattern projet `pdMS_TO_TICKS(kXxxTimeoutMs)` + vérification de la valeur de retour `xSemaphoreTake`.
- Ne pas modifier la sémantique fonctionnelle (mêmes sections critiques protégées), seulement la stratégie d'attente.
- Watchdog actif ; aucun handler ne doit bloquer.
- Centraliser les nouveaux timeouts dans `constants.h`.

## Fichiers potentiellement concernés
- `src/history.cpp` — prises mutex `:161,169,371,411,421,667` [modif]
- `src/mqtt_manager.cpp` — prises mutex `:430,444,530,817…` [modif]
- `src/constants.h` — constantes de timeout manquantes (ex. `kHistoryMutexTimeoutMs`, `kMqttMutexTimeoutMs`) [modif]
- Balayage de tout `src/` pour tout autre `portMAX_DELAY` résiduel sur mutex applicatif [audit]
- `docs/subsystems/history.md`, `docs/subsystems/mqtt-manager.md` — note sur la stratégie timeout [doc]

## Nouvelles signatures / API (optionnelle)
- Constantes : `constexpr TickType_t kHistoryMutexTimeoutMs = ...;`, `constexpr TickType_t kMqttMutexTimeoutMs = ...;`

## Sécurité chimique (optionnelle)
- Dosage impliqué : non (mais améliore la disponibilité globale → indirectement la sécurité, car évite reboots intempestifs).

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun (alignement sur un pattern déjà décidé)
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun

### Champs WebSocket
- aucun

### Topics MQTT
- aucun (comportement interne)

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/history.md` — stratégie de timeout mutex
- `docs/subsystems/mqtt-manager.md` — stratégie de timeout mutex

#### Docs transverses
- `CHANGELOG.md` — entrée robustesse / fiabilité

## Critères d'acceptation
- [x] AC1 — Aucune occurrence de `portMAX_DELAY` sur un mutex applicatif ne subsiste dans `src/` (audit grep à blanc).
- [x] AC2 — Chaque prise de mutex vérifie la valeur de retour et gère l'échec (skip + log) sans corrompre l'état.
- [ ] AC3 — Sous contention simulée, la `loopTask` ne déclenche pas le watchdog. *(on-target : nominal prolongé sans régression + contention réelle — non testable en natif, cf. annotation d'audit #3)*
- [x] AC4 — Comportement nominal (mutex libre) identique à avant.
- [x] `pio run` compile sans nouveau warning.

## Plan de test manuel
1. Fonctionnement nominal prolongé → aucune régression historique/MQTT.
2. Injection d'une détention longue de mutex (test/instrumentation) → cycle sauté + log, pas de reboot.
3. Charge MQTT soutenue pendant écriture historique → pas de blocage loopTask.

## Notes pour agent
- Faire un audit exhaustif `grep -rn "portMAX_DELAY" src/` et statuer sur chaque occurrence (certaines, hors mutex applicatif comme attente de queue dédiée sur une tâche non-loop, peuvent être légitimes — documenter le verdict).
- Réutiliser les constantes de timeout existantes quand elles ont un sens ; sinon en ajouter dans `constants.h`.

## Hors périmètre
- Refonte du modèle de concurrence global (cf. feature god objects).
- Changement des sections critiques elles-mêmes.

## Notes d'implémentation (2026-07-06, v2.11.1)

**Tableau d'audit appliqué : 24/24 sites bornés** (mqtt_manager.cpp ×9, logger.cpp ×7, history.cpp ×6, config.cpp ×2). `grep -rn "portMAX_DELAY" src/` à blanc. `pio run` 0 warning, 185/185 tests natifs verts, code-reviewer **Approved** (revue de concurrence complète).

### Constantes ajoutées (`constants.h`)

| Constante | Valeur | Dimensionnement |
|---|---|---|
| `kLoggerMutexTimeoutMs` | 100 ms | Sections critiques RAM pures (buffer circulaire, `_persistBuffer`) |
| `kHistoryMutexTimeoutMs` | 2000 ms | Pire détenteur : consolidation + `saveToFile` LittleFS (~1–1,5 s) |
| `kConfigMutexTimeoutMs` | 1000 ms | Existante — réutilisée (prises récursives `xSemaphoreTakeRecursive`) |
| `kMutexTimeoutWarnThrottleMs` | 60000 ms | Max 1 warn/min/site (statique locale par site) |

### Politique d'échec par fichier

- **`logger.cpp` (×7)** : politique **silencieuse anti-récursion** — jamais de `systemLogger` dans les chemins d'échec (seul `Serial.printf` de secours dans `flushToDisk`). `log()` → entrée perdue + `_droppedLogs++` ; `getRecentLogs`/`getLogCount` → vide/0 ; `clear`/`clearAll` → no-op avant toute modif (le fichier n'est pas supprimé si la RAM n'a pas été vidée) ; `flushToDisk` swap → `_lastFlushMs` non touché (retry) ; réinsertion post-échec écriture → lignes perdues proprement (`_droppedLogs += n`).
- **`history.cpp` (×6)** : `recordDataPoint`/`consolidateData` sautés avec **retry naturel** (`lastRecord`/`lastSave` non avancés) ; `getLastHours`/`getAllData` → vecteur vide ; `importData` → `false` avant toute modif ; **`clearHistory()` devient `bool`** — la route `POST /history/clear` répond `503` « Historique occupé — réessayer » sur échec.
- **`config.cpp` (×2)** : `saveMqttConfig` → `error` throttlé (config RAM appliquée, persistance NVS perdue) ; `saveProductConfig` → warn throttlé + **dirty flag `productConfigDirty` conservé** → retry naturel.
- **`mqtt_manager.cpp` (×9)** : `publishProductState`/`publishTargetState`/bloc snapshot de `publishAllStatesInternal` → snapshot sauté (« atomique ou rien »), republication naturelle au prochain cycle (retain) ; les 6 handlers de `drainCommandQueue` → **abandon complet de la commande HA** (rien modifié avant le take, pas d'état partiel) + **resync publish** de l'état réel (`publishFiltrationState()`/`publishTargetState()`).

### Revue de code — 4 mineures

1. **Corrigée post-revue** : `clear()` incrémentait `_droppedLogs` à tort — le compteur ne trace que des *entrées* perdues, un clear en timeout est un simple no-op re-cliquable.
2. **Consignée** : `_droppedLogs` est **write-only volontairement** (aucun getter/endpoint/topic) — lisible au debugger/coredump seulement. Documenté dans `docs/subsystems/logger.md`.
3. **Consignée** : le resync `publishFiltrationState()` lit `filtrationCfg.mode` (`String`) **sans mutex** — comportement historique de la méthode, inchangé par la feature = **pattern accepté**. Documenté dans `docs/subsystems/mqtt-manager.md`.
4. **Consignée** : `getLastHours()` peut bloquer la tâche `async_tcp` **2 s max** sur contention (vs infini avant) — amélioration nette, acceptée.

### Reste à faire (on-target, AC3)

- Fonctionnement nominal prolongé → aucune régression historique/MQTT.
- Contention réelle : gros import d'historique (`POST /history/import`) pendant une consolidation → warns throttlés attendus, **pas de reboot** watchdog.
