# Bug — Crash `bad_alloc` sur `GET /debug/sensor_trace` : l'onglet Avancé fait redémarrer l'ESP32

- **Type** : bug (format minimal `specs/modele.md`)
- **Statut** : done
- **Version cible** : 2.10.1
- **Créée** : 2026-07-23 (coredump on-target v2.10.0, analysé via GDB — crash à la première ouverture de l'onglet Avancé après déploiement)
- **Domaine** : Web (route debug + UI onglet Avancé) — aucun chemin de dosage touché
- **Spec liée** : feature-057 (origine du ring buffer de trace), ADR-0029 (contexte fragmentation heap)

## Symptôme observé

Coredump (partition coredump, décodé GDB contre l'ELF 2.10.0) : `abort()` sur tâche `async_tcp`, déclenché par `std::bad_alloc` non rattrapé :

```
operator new[] (sz=20475)                      ← échec allocation contiguë
 ← cbuf::resize(20475) (cores/esp32/cbuf.cpp:49)
 ← AsyncResponseStream::write
 ← ArduinoJson serializeJson (récursion visit/accept)
 ← lambda GET /debug/sensor_trace (web_routes_debug.cpp:30)
 ← AsyncWebServer _parseLine / _onData (async_tcp)
```

## Cause racine

Le handler `/debug/sensor_trace` sérialise les **300 échantillons** du ring buffer (`kSensorTraceBufferSize`, constants.h:255) dans un `AsyncResponseStream`, qui bufferise la réponse entière dans **un seul `cbuf` contigu** agrandi par `resize()`. ~70 o/échantillon × 300 ≈ **20 Ko d'un seul tenant** : sur le heap fragmenté de cet appareil (cf. ADR-0029 — le plus gros bloc libre peut descendre sous 8 Ko), l'allocation échoue, `new[]` lève `std::bad_alloc`, rien ne l'attrape sur la pile async_tcp → `std::terminate` → `abort()` → reboot.

L'UI déclenche la route à chaque activation de l'onglet Avancé et sur clic Rafraîchir (data/app.js:5065/5120) : l'utilisateur peut redémarrer l'appareil involontairement depuis le navigateur.

Note : l'auto-guérison heap (ADR-0029) ne protège pas — son seuil de bloc contigu est 8 Ko, la route en exige ~20 Ko.

## Correctif attendu (« vrai fix » : pagination)

1. **Pagination de la route** : `GET /debug/sensor_trace?offset=N&count=M` — M borné par une constante (`kSensorTraceMaxSamplesPerPage`, ~50 → ~3,5 Ko/page), défauts sûrs sans paramètres (première page), champs de réponse permettant à l'UI d'enchaîner (`count` total déjà présent, ajouter ce qu'il faut : `offset`, nombre retourné, etc. — architect tranche le contrat exact).
2. **UI (onglet Avancé)** : enchaîner les pages jusqu'à complétude avant de tracer les courbes ; le bouton Rafraîchir recharge tout ; comportement inchangé vu de l'utilisateur.
3. **Garde-fou défensif** (ceinture + bretelles) : avant sérialisation, `measureJson(doc)` comparé à `ESP.getMaxAllocHeap()` avec marge → `503 {"error":"heap fragmenté"}` au lieu d'un crash, même pour une page.
4. **Cohérence du snapshot** : le ring buffer avance entre deux pages (échantillonnage toutes les `kPhOrpSensorIntervalMs`) — définir le comportement (tolérer le glissement, ou ancrer l'offset sur un identifiant d'échantillon) ; architect tranche, simplicité privilégiée (route de debug).

## Critères d'acceptation

- [x] Ouvrir l'onglet Avancé avec 300 échantillons en buffer ne provoque plus jamais d'allocation > ~4 Ko d'un seul tenant pour cette route (pages de 50 échantillons ≈ 3,5 Ko, stream pré-dimensionné).
- [x] Les courbes de l'onglet Avancé affichent toujours l'intégralité de la trace (pagination transparente, agrégation séquentielle bornée à 12 itérations dans `loadSensorTrace()`).
- [x] `count`/`offset` hors bornes → réponse propre (clamp silencieux via `computeTracePage()`, jamais de 400), jamais de crash.
- [x] Heap trop fragmenté même pour une page → 503 propre (`measureJson` + `kSensorTraceHeapMarginBytes` vs `ESP.getMaxAllocHeap()`), jamais d'abort.
- [x] Aucun chemin de dosage touché.
- [x] `pio run` sans warning nouveau, 332 tests natifs verts (316 + 16 nouveaux `test/test_native_trace_page/`) ; `./build_fs.sh` passe (data/app.js modifié).

## Notes

- Coredump de référence : `coredump.bin` (racine, non versionné), décodage GDB du 2026-07-23.
- La lecture du ring buffer est lock-free (contrat `SensorTraceSample`, sensors.h:176) — la pagination ne doit pas introduire de verrou côté loop.
- `docs/API.md` : vérifier si `/debug/sensor_trace` y est documenté → mettre à jour le contrat.

## Notes d'implémentation (v2.10.1, 2026-07-23)

- **Cycle** : diagnostic coredump (GDB sur ELF 2.10.0) → architect → web-ui-developer → tests → code-reviewer (**Approved**) → doc-writer, une itération.
- **Fichiers créés** : `src/sensor_trace_page.h` (clamp pur header-only `computeTracePage`, invariants offset≤total / n≤max / offset+n≤total pour tout input y compris SIZE_MAX), `test/test_native_trace_page/` (16 tests dont balayage adversarial 10³ combinaisons).
- **Fichiers modifiés** : `src/constants.h` (`kSensorTraceMaxSamplesPerPage=50`, `kSensorTraceHeapMarginBytes=4096`), `src/sensors.h/.cpp` (`appendSensorTraceJson(JsonArray, offset, count)`, snapshot local `_traceCount`/`_traceIdx`, indexation `% kSensorTraceBufferSize` — aucun hors-bornes possible même sur lectures incohérentes, contrat lock-free inchangé), `src/web_routes_debug.cpp` (handler paginé, ordre impératif doc → measureJson → garde 503 → `beginResponseStream(type, sz)` pré-dimensionné — une allocation exacte, zéro resize, vérifié dans le fork WebResponses.cpp:911-916), `data/app.js` (`loadSensorTrace` : agrégation séquentielle bornée à 12 pages, 503 → « Mémoire fragmentée, réessayez. »), `src/version.h` (2.10.1).
- **Décisions architect** : glissement du ring toléré entre pages (~1 échantillon max à une frontière — 1 échantillon/5 s vs pagination < 1 s) ; clamp silencieux plutôt que 400 ; chunked response écartée (sérialisation ré-entrante trop fragile vs 6 pages de 3,5 Ko) ; pas d'ADR.
- **Revue** : 3 mineures acceptées (sur-comptage `returned` si `clearSensorTrace` concourant — auto-corrigé à la page suivante ; garde 503 probabiliste TOCTOU — marge 4 Ko vs page 3,5 Ko ; doc API à jour — fait).
- **Validation** : `pio run` SUCCESS 0 warning (Flash 81,6 %), `./build_fs.sh` OK (320 K), 332/332 tests natifs (11 suites). Docs : `docs/API.md` (contrat paginé), `docs/subsystems/sensors.md`, `docs/features/page-settings.md`, `CHANGELOG.md` [2.10.1].
- **Reste on-target** : déployer 2.10.1 (`./deploy.sh ota-all` — firmware ET FS, app.js modifié), ouvrir l'onglet Avancé buffer plein (6 requêtes réseau, courbes complètes), `curl '?offset=9999'` → 200 `samples:[]`.
