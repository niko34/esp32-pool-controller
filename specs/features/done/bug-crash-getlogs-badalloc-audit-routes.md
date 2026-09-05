# Bug — Crash `bad_alloc` sur `GET /get-logs` + audit des routes à réponse bufferisée non bornée

- **Type** : bug (format minimal `specs/modele.md`) + audit systématique
- **Statut** : done
- **Version cible** : 2.10.4
- **Créée** : 2026-07-24 (coredump on-target v2.10.2, décodé GDB contre l'ELF exact reconstruit du commit 1dda00d)
- **Domaine** : Web (routes de données) — aucun chemin de dosage touché
- **Spec liée** : bug-crash-sensor-trace-badalloc (v2.10.1 — même classe de bug, pattern de correctif validé)

## Symptôme observé

Coredump du 2026-07-24 09:41 : `abort()` sur core 0 (tâche async_tcp), pile partiellement corrompue par l'unwind d'exception mais adresses de retour claires : `handleGetLogs` → `Logger::forEachRecentLog` → écritures `AsyncResponseStream` (web_routes_data.cpp:333 = enregistrement `/get-logs`). Même signature que le coredump du 2026-07-23 (`cbuf::resize` → `operator new[]` → `std::bad_alloc` non rattrapé → `std::terminate` → `abort`).

## Cause racine

`handleGetLogs` (web_routes_data.cpp:206-260) streame jusqu'à `kMaxLogEntries` (200) entrées dans un `AsyncResponseStream` : réponse de **20-40 Ko dans un `cbuf` contigu** qui grossit par `resize()` (`new[]` + copie → besoin transitoire ancien + nouveau bloc).

Le durcissement v2.8.2 de ce même handler est **insuffisant et son commentaire est faux** :
- le garde-fou `heap_caps_get_largest_free_block ≥ kMinHeapForLogsResponse` (24 Ko, constants.h:146) est un seuil FIXE indépendant de la taille réelle de la réponse → laisse passer les cas bloc dispo < réponse finale ;
- le commentaire « Le StreamString grandit par realloc (jamais operator new → jamais d'abort) » est erroné : `AsyncResponseStream` = `cbuf` = `new char[]` throwant (prouvé par 2 coredumps).

## Correctif attendu

1. **`/get-logs`** : appliquer le pattern validé en 2.10.1 sur `/debug/sensor_trace` :
   - plafonner le premier chargement (pagination — le paramètre `?since=` existe déjà pour l'incrémental ; ajouter un mécanisme de page borné, architect tranche le contrat en cohérence avec l'UI `data/app.js`) ;
   - taille de page telle que la réponse max reste petite (~4 Ko) ;
   - pré-dimensionner le stream (`beginResponseStream(type, taille)`) après estimation, garde 503 basée sur la taille **réelle** estimée + marge vs `ESP.getMaxAllocHeap()` ;
   - corriger le commentaire mensonger v2.8.2.
2. **Audit systématique** de TOUTES les routes HTTP qui construisent des réponses en mémoire : `AsyncResponseStream` non pré-dimensionné, `String` géante par concaténation (`handleGetHistory` suspect n°1 : ~315 points), `serializeJson` vers stream/String, exports (`/download-logs`), backup config… Classer par taille max possible de réponse et appliquer le pattern là où la réponse peut dépasser ~4-8 O Ko contigus.
3. **UI** : adapter `data/app.js` si le contrat `/get-logs` change (chargement paginé du journal, comme l'onglet Avancé).

## Critères d'acceptation

- [x] Ouvrir la page Journal avec 200 entrées ne provoque plus jamais d'allocation contiguë > ~8 Ko pour cette route (page de 30 entrées ~4 Ko, `measureJson` + garde + stream pré-dimensionné exact).
- [x] Le journal complet reste consultable dans l'UI (pagination transparente dans `loadLogs()`, max 8 itérations, curseur strictement croissant).
- [x] Heap trop fragmenté → 503 propre partout, jamais d'abort (`/get-logs` garde exacte, `/get-history` reserve testé ; `/download-logs` chunked = aucune garde nécessaire).
- [x] Toutes les routes identifiées par l'audit sont soit corrigées, soit documentées comme bornées avec la preuve de leur borne (tableau d'audit : `docs/adr/0032-reponses-http-bornees.md`).
- [x] Aucun chemin de dosage touché.
- [x] `pio run` sans warning nouveau, 353/353 tests natifs verts (332 + 21 nouveaux : `test_native_logs_page`, `test_native_logs_export`) ; `./build_fs.sh` OK (320 K).

## Notes

- Coredump de référence : `coredump.bin` (racine, 2026-07-24), ELF exact 2.10.2 reconstruit en worktree.
- L'entrée `LogEntry.message` est une `String` de longueur libre → l'estimation de taille doit se faire sur les longueurs réelles, pas sur un forfait.
- Pattern de référence : `src/web_routes_debug.cpp` (handler sensor_trace v2.10.1) + `src/sensor_trace_page.h`.

## Notes d'implémentation (v2.10.4, 2026-07-24)

- **Cycle** : décodage coredump (GDB, ELF 2.10.2 exact reconstruit en worktree — la pile étant corrompue par l'unwind d'exception, les adresses de retour ont suffi) → audit exhaustif des routes → architect → web-ui-developer → tests → code-reviewer (**Request changes** ×1 : `kHistoryPointJsonMaxBytes` 96→128 sur mesure réelle du point JSON 113-120 o, appliqué → conforme) → doc-writer.
- **Fichiers créés** : `src/logs_page.h` (admission de page pure : soft 30 / hard cap 60 / queue même-milliseconde incluse — sans elle le curseur `?since=` strictement supérieur perdrait des entrées), `src/logs_export_chunk.h` (`LogLineCarry<N>` + `emitCarry`, garantie de progression du flux chunked), `test/test_native_logs_page/` (13 tests) + `test/test_native_logs_export/` (8 tests).
- **Fichiers modifiés** : `src/web_routes_data.cpp` (3 handlers réécrits : get-logs paginé JsonDocument→measureJson→garde→stream exact ; download-logs chunked machine à états shared_ptr fichier→RAM→fin ; get-history reserve() testé→503), `src/logger.{h,cpp}` (visiteur bool + `getLogEntryByIndex` best-effort), `src/web_routes_ota.cpp` (notes release tronquées 2048), `src/constants.h` (`kHttpHeapMarginBytes` généralisée, `kMinHeapForLogsResponse` SUPPRIMÉE — seuil fixe invalidé par 2 coredumps), `data/app.js` (boucle de pagination loadLogs, max 8, curseur croissant, compat ancienne API), `src/version.h` (2.10.4).
- **Revue — points validés notables** : chunked jamais 0 avant la vraie fin (vérifié jusqu'à WebResponses.cpp : maxLen ≥ MSS/2), File fermé par destructeur même sur déconnexion, rotation du fichier pendant download tolérée, aucune imbrication de mutex (flushToDisk swap-then-IO), JsonDocument ≤ ~4 Ko sous mutex logger accepté.
- **Réserves mineures notées (non traitées, assumées)** : timeout mutex de `getLogEntryByIndex` indistinguable de la fin de buffer (export best-effort — suggestion revue : 2-3 retries entre chunks) ; rollover `millis()` 49,7 j sur le curseur `since` (préexistant) ; troncatures UTF-8 possibles mi-séquence (export/notes, cosmétique).
- **Validation** : `pio run` SUCCESS 0 warning nouveau, `./build_fs.sh` OK (320 K), **353/353** tests natifs (13 suites).
- **Docs** : **ADR-0032 réponses-http-bornées** (règle : paginée+pré-dimensionnée / chunked / allocation testée ; seuils heap fixes interdits ; tableau d'audit des routes prouvées bornées), `docs/API.md`, `docs/subsystems/logger.md`, `docs/features/page-settings.md`, `CHANGELOG.md` [2.10.4], index ADR + arbre docs/README.md.
- **Reste on-target** (après déploiement `ota-all` — firmware ET FS) : page Journal avec 200 entrées (boucle de pagination visible réseau), export `/download-logs` avec `/system.log` plein, `/get-history?range=all`, et absence de reboot sur heap chargé.
