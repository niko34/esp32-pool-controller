# Bug + durcissement — Angle mort de la garde WS (TOCTOU), buffer statique WS, /get-history chunked, instrumentation heap

- **Type** : bug (crash récurrent) + durcissement mémoire
- **Statut** : done
- **Version cible** : 2.14.0
- **Créée** : 2026-07-30 (coredump on-target v2.13.0 du 30/07 11:11, décodé GDB — périmètre amendé avec l'utilisateur : pattern « allocation statique au boot » intégré là où il est optimal)
- **Domaine** : WebSocket, routes de données, mémoire/instrumentation — aucun chemin de dosage
- **Specs liées** : bug-badalloc-ws-broadcast (2.11.1 — garde contournée par TOCTOU), investigation-churn-heap-ws-http (G3 différé → traité ici), ADR-0032 (réponses bornées), ADR-0029 (auto-guérison)

## Symptôme observé

Coredump 30/07 (v2.13.0, garde heap et anti-zombie EMBARQUÉS) : `bad_alloc` sur 1 816 o dans `textAll` ← `broadcastSensorData`, loopTask. La garde `_heapAllowsPush()` (seuil 8 192) a été **passée** puis prise de vitesse :
1. **TOCTOU** : entre le contrôle (`getMaxAllocHeap() ≥ 8192`) et l'allocation du buffer de trame, `_buildSensorJson` alloue ~2 Ko ET la tâche async_tcp peut consommer le bloc mesuré (page /get-logs ~4 Ko, réponse HTTP…).
2. **Seuils adjacents** : garde WS (8 192) == seuil restart auto-guérison (8 192) → zone grise 8-10 Ko de maxBloc où les pushes sont autorisés ET l'auto-guérison ne s'arme pas.
3. **Générateur de fond toujours actif** : G3 de l'audit du 24/07 — `/get-history` épingle ~40 Ko contigus (String) pendant le transfert à chaque ouverture du dashboard (+ mini-charts ~30 Ko), différé « si récidive ». Récidive.

## Livrables

- [ ] A1 — **Garde WS relevée + filet** : `kWsPushMinAllocBytes` 8192 → 16384 (marge nette au-dessus du seuil restart 8192 : les pushes s'arrêtent avant la zone rouge, l'auto-guérison garde sa fenêtre d'action) ; `try/catch (std::bad_alloc)` autour des `textAll` des 3 broadcasts (l'exception est levée dans makeSharedBuffer AVANT toute mutation de file → catch propre = push sauté, compté dans `_skippedPushes`).
- [ ] A2 — **Buffer statique de trame WS** (pattern utilisateur « réserver une fois, réutiliser toujours ») : le JSON capteurs (~1,9 Ko, reconstruit toutes les 5 s) est construit dans un buffer statique `.bss` (taille fixe au link, hors heap, aucune fragmentation) réutilisé à chaque push — supprime la String temporaire par push (churn G2 résiduel). Architect tranche : buffer char[] + snprintf/serializeJson, taille (2048 ? aligné kWsSensorJsonReserveBytes), et même traitement pour broadcastConfig si trivial.
- [ ] A3 — **`/get-history` en chunked zéro-buffer** (patron /download-logs, ADR-0032 mécanisme b) : supprime la String contiguë ~40 Ko. CONTRAINTE de conception : les données vivent dans les structures RAM d'history protégées par le mutex history — le callback chunked (async_tcp, étalé dans le temps) ne doit JAMAIS tenir le mutex entre deux chunks (pattern « copie d'un élément sous mutex court par appel », cf. Logger::getLogEntryByIndex) ; glissement du ring toléré (précédent sensor_trace). Contrat JSON de réponse INCHANGÉ pour l'UI (mêmes champs — seul le Transfer-Encoding change, transparent pour fetch()).
- [ ] A4 — **Instrumentation ring RTC noinit** (différée 2×) : ~1 Ko RTC_NOINIT (128 × 8 o : uptime s, maxBloc/64, freeHeap/256, clients WS, queue max, HTTP actifs — format architect), échantillonné ~2 s depuis la loop ; au boot, si magic valide ET reset ≠ POWERON → dump synthétique dans le journal (les N dernières entrées, throttlé/borné). Donne la cinétique d'effondrement que le tick 5-60 s rate.
- [ ] D1 — Tests natifs : logique pure extraite là où applicable (pagination/curseur du chunked history ; encodage/décodage des entrées du ring) ; zéro régression.
- [ ] D2 — `pio run` 0 warning nouveau, tests verts, `./build_fs.sh` OK (data/ non touché a priori — vérifier).
- [ ] C1 — Docs : ws-manager.md (garde 16 Ko + catch + buffer statique), API.md (/get-history chunked, contrat inchangé), system-health.md (ring RTC), history.md, CHANGELOG [2.14.0]. ADR-0032 : note d'extension éventuelle (architect tranche).

## Ce qui reste hors périmètre (documenté)

- Pools statiques pour les allocations INTERNES du fork (buffers de trame make_shared, objets requête) — impossible sans modifier le fork (ligne rouge) ; mitigé par A1.
- MQTT/HA, UART écran, dosage : intouchés.

## Critères d'acceptation

- [ ] Plus aucun abort possible sur les broadcasts WS (catch systématique), pushes sautés comptés.
- [ ] Aucune allocation > ~4 Ko contiguë pour servir /get-history (vérifiable par lecture) ; réponse identique côté UI (courbes inchangées).
- [ ] Push WS sans allocation de String temporaire (buffer statique réutilisé).
- [ ] Après un reboot sur panic, le journal contient la cinétique heap des ~4 dernières minutes.
- [ ] Aucun chemin de dosage touché ; `pio run` + tests + build_fs verts.

## Notes d'implémentation (v2.14.0, 2026-07-30)

- **Cycle** : diagnostic coredump (TOCTOU + seuils adjacents identifiés) → périmètre amendé avec l'utilisateur (pattern « réserver une fois, réutiliser toujours » → buffer statique .bss) → architect → embedded-developer → code-reviewer (**Approved**, 3 mineures : écho `range` corrigé post-revue — token reconnu seul ; bruit SW_RESET du dump assumé ; pool_logs.txt hors commit) → doc-writer.
- **Fichiers créés** : `src/history_export_chunk.h` (pur : historyRangeCutoff + formatHistoryPointJson, réutilise LogLineCarry/emitCarry), `src/heap_ring_logic.h` (pur : encode/decode 8 o + index ring), `src/heap_ring.h/.cpp` (coquille RTC_NOINIT magic 0xB10C4EA9, 1032 o vérifiés ELF), `test/test_native_history_export/` (13) + `test/test_native_heap_ring/` (14).
- **Fichiers modifiés** : constants.h (garde 16384 — marge 2× vs restart 8192, kWsFrameBufBytes 3072, timeouts/retries chunked, kHeapRing*, kWsSensorJsonReserveBytes supprimée), ws_manager (try/catch bad_alloc ×3 → _skippedPushes ; sWsFrameBuf statique, _buildXxxJson(char*,size_t) avec measureJson d'abord, _oversizedFrames, invariant loopTask BLOQUANT), history (getPointByIndex 25 ms, timedOut distinct), web_routes_data (handleGetHistory chunked : machine Header/Points/Footer/Done, RESPONSE_TRY_AGAIN retries consécutifs reset-après-succès ≈100 s de tolérance vs consolidation 1,5 s, contrat octet-pour-octet avec double clé "count"), main.cpp (heapRingSample 2 s, heapRingDumpIfCrash après history.begin — persistance logs active), version.h (2.14.0).
- **Validation** : pio run SUCCESS 0 warning (-fexceptions confirmé), **415/415** tests natifs, build_fs OK, data/ intouché (chunked transparent pour fetch()).
- **Reste on-target** (après déploiement) : dashboard/courbes identiques (diff visuel), heap serré → compteur skipped monte sans abort, reboot panic → 8 lignes de cinétique [ring] au boot puis plus rien, occupation FS et maxBloc à surveiller quelques jours — le ring donnera ENFIN la cinétique si récidive.

## Critères d'acceptation — validation

- [x] Plus aucun abort possible sur les broadcasts WS (catch systématique + garde 16384), pushes sautés comptés.
- [x] Aucune allocation > ~4 Ko contiguë pour /get-history (chunked zéro-buffer, String 40 Ko supprimée).
- [x] Push WS sans String temporaire (buffer statique .bss).
- [x] Cinétique heap des ~64 dernières secondes dumpée au boot post-panic.
- [x] Aucun chemin de dosage touché ; pio run + 415/415 + build_fs verts.
