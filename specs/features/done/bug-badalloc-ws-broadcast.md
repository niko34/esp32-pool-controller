# Bug — `bad_alloc` non rattrapé dans les broadcasts WebSocket + fenêtre aveugle de l'auto-guérison heap

- **Type** : bug + durcissement
- **Statut** : done
- **Version cible** : 2.11.1
- **Créée** : 2026-07-28 (coredump on-target v2.11.0, décodé GDB)
- **Domaine** : WebSocket (ws_manager) + auto-guérison heap (main.cpp/self_heal) — aucun chemin de dosage
- **Specs liées** : ADR-0029 (auto-guérison heap), ADR-0032 (réponses HTTP bornées — même philosophie, voie WS non couverte), investigation-churn-heap (ouverte en parallèle — cause racine)

## Symptôme observé

Coredump 2026-07-28 : `abort()` sur loopTask — `WsManager::broadcastSensorData` (ws_manager.cpp:140) → `AsyncWebSocket::textAll` (payload 1 777 o) → `makeSharedBuffer` (AsyncWebSocket.cpp:862, `std::vector<uint8_t>` en shared_ptr) → `operator new` → `std::bad_alloc` non rattrapé → `std::terminate`. Le plus gros bloc libre était < ~1,8 Ko : l'auto-guérison (seuil 8 Ko, échantillonnage 60 s + débounce) n'a pas eu le temps d'agir — le crash a gagné la course. Reboot brutal (pas de sauvegarde des cumuls ni d'arrêt ordonné des pompes, contrairement au restart fail-safe).

## Correctif attendu

1. **Garde heap sur TOUS les broadcasts WS** (`ws_manager.cpp` : broadcastSensorData, broadcastConfig, broadcasts de logs / callback logger→WS, et tout autre `textAll`/`text`) : si `ESP.getMaxAllocHeap()` < seuil (constante, à dimensionner : payload max attendu + marge — architect tranche), **sauter le push** (l'UI rate un cycle de 5 s, sans conséquence) + warn throttlé + compteur de pushes sautés (diagnostic).
2. **Resserrer la fenêtre de l'auto-guérison** : quand `maxAllocHeap` passe sous le seuil d'alerte (`kLowHeapWarnBytes`), accélérer l'échantillonnage/décision (au lieu du tick fixe 60 s) — mécanisme exact à trancher par l'architect (échantillonnage rapproché, débounce raccourci sous seuil critique, ou les deux), SANS casser les invariants ADR-0029 (débounce anti-faux-positif, budget kAutoHealMaxReboots, mode maintenance).
3. Ne PAS toucher à la bibliothèque fork (makeSharedBuffer) — la garde se fait côté appelant.

## Critères d'acceptation

- [x] Aucun broadcast WS ne peut plus lever un `bad_alloc` non rattrapé (garde systématique côté appelant) — `_heapAllowsPush()` en tête des 3 broadcasts, avant construction du JSON (`kWsPushMinAllocBytes = 8192`).
- [x] Heap sous seuil critique → pushes sautés avec trace throttlée (WARN 30 s depuis `update()` uniquement, compteur `skippedPushCount()`), appareil vivant, auto-guérison déclenchée dans une fenêtre nettement < 60 s (cadence adaptative 5 s → débounce confirmé en ~15 s).
- [x] Invariants ADR-0029 préservés (bloc extrait vers `checkHeapHealth()` à diff logique nul ; débounce, budget partagé, uptime min, maintenance, garde OTA inchangés).
- [x] Aucun chemin de dosage touché ; `pio run` 0 warning nouveau, tests natifs verts (368/368, dont +5 sur `heapSampleIntervalMs`).

## Extension de périmètre (2026-07-28, issue de l'investigation churn-heap)

L'investigation (`investigation-churn-heap-ws-http.md`) a identifié le générateur n°1 : **client WS zombie** (file de 32 buffers ~1,8 Ko conservée connexion ouverte, aucun keepalive). Quick wins repliés dans ce cycle :

- [x] Q1 — Fermer les clients WS à file saturée : `setCloseClientOnQueueFull(true)` à la connexion + `-DWS_MAX_QUEUED_MESSAGES=5` (platformio.ini). La fermeture manuelle dans `update()` via `queueLen()` a été **rejetée en revue** (itérer `getClients()` sans le verrou privé du fork = UB face à l'érase concurrent d'`async_tcp`) — le fork ferme lui-même sous SON verrou.
- [x] Q2 — `keepAlivePeriod(kWsKeepAlivePeriodS = 30)` à la connexion. Nuance revue : le fork n'envoie le PING que file vide → couvre les zombies inactifs ; les autres sont couverts par Q1.
- [x] Q3 — `_buildSensorJson` : `reserve` → `kWsSensorJsonReserveBytes = 2048`.
- [x] Q4 — CRITICAL « Mémoire faible » + alerte MQTT `low_memory` émis sur transition uniquement (pattern edge) + INFO « Mémoire rétablie » au retour. Note : alerte non-retained, abonné hors ligne pendant la transition → épisode raté (documenté).

Reportés (futur cycle si récidive) : /get-history chunked (G3), purge du code mort broadcastLog/setLogCallback, instrumentation ring RTC noinit, push WS restreint aux clients authentifiés.

## Notes d'implémentation (v2.11.1, 2026-07-28)

- **Cycle** : architect → embedded-developer (base + extension Q1-Q4 en reprise de contexte) → code-reviewer (**Request changes** ×1 : itération getClients() sans verrou depuis loopTask = UB face à l'érase concurrent d'async_tcp sous le verrou privé du fork — corrigé selon la prescription : `-DWS_MAX_QUEUED_MESSAGES=5` (platformio.ini) + `setCloseClientOnQueueFull(true)`, le fork ferme lui-même sous SON verrou, boucle manuelle supprimée) → doc-writer.
- **Livré** : garde `_heapAllowsPush()` (8 Ko, ne logge jamais — invariant anti-ré-entrance, compteur atomique, warn throttlé dans update() seul) sur les 3 broadcasts ; `checkHeapHealth()` extraite à cadence adaptative 60 s → 5 s sous kLowHeapWarnBytes (`heapSampleIntervalMs` pure, +5 tests — débounce confirmé en ~15 s au lieu de 180 s, bloc ADR-0029 à diff logique nul) ; anti-zombie fork (file 5 trames vs 32, keepalive 30 s — PING émis file vide uniquement, nuance documentée) ; reserve JSON 2048 ; alerte low_memory en edge (note : publishAlert non-retained, épisode borné par l'INFO de rétablissement).
- **Validation** : `pio run` SUCCESS (warnings du rebuild complet = préexistants : redéfinitions CONFIG_LWIP de feature-014, OneWire), **368/368** tests natifs.
- **Docs** : ADR-0029 amendé (échantillonnage adaptatif), ADR-0032 étendu (émissions WS), ws-manager.md, system-health.md, CHANGELOG [2.11.1].
- **Reste on-target** : scénario zombie réel (UI mobile ouverte, écran verrouillé 5-10 min, réouverture) → attendu : client fermé par le fork à 5 trames, aucun crash, log éventuel de pushes sautés si heap contraint ; vérifier `skippedPushCount` via les logs.
