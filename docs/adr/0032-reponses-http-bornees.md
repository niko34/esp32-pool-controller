# ADR-0032 — Réponses HTTP bornées : pagination, chunked ou allocation unique testée

- **Statut** : Accepté
- **Date** : 2026-07-24
- **Décideurs** : Nicolas + Claude (architect, code-reviewer)
- **Spec(s) liée(s)** : `bug-crash-getlogs-badalloc-audit-routes` (v2.10.4), `bug-crash-sensor-trace-badalloc` (v2.10.1)

## Contexte

Deux coredumps réels en deux jours, même classe de crash, décodés en GDB contre l'ELF exact :

- **2026-07-23** (v2.10.0) : `GET /debug/sensor_trace` — 300 échantillons sérialisés en une réponse ~20 Ko ;
- **2026-07-24** (v2.10.2) : `GET /get-logs` — jusqu'à 200 entrées de log en une réponse de 20-40 Ko.

Mécanisme commun : `AsyncResponseStream` accumule la réponse dans un **`cbuf` contigu** qui grossit par `resize()` = **`new char[]` throwant** + copie — le besoin transitoire est **ancien bloc + nouveau bloc** simultanément. Sur tas fragmenté (plus gros bloc libre parfois < 8 Ko, cf. [ADR-0029](0029-auto-guerison-heap.md)), `std::bad_alloc` n'est rattrapé nulle part sur la tâche `async_tcp` → `std::terminate()` → `abort()` → **reboot déclenché depuis le navigateur**.

L'approche de durcissement v2.8.2 est **invalidée par ces deux coredumps** :

- garde à **seuil heap FIXE** (`kMinHeapForLogsResponse` = 24 Ko), indépendant de la taille réelle de la réponse → laisse passer tous les cas où le bloc disponible est inférieur à la réponse finale ;
- croyance fausse documentée dans le code (« le stream grandit par `realloc`, jamais d'`operator new` → jamais d'abort ») — le `cbuf` est bien un `new[]` throwant.

Un audit exhaustif des routes HTTP construisant leur réponse en mémoire s'imposait, avec une règle générale pour ne pas retraiter cette classe de bug route par route.

## Décision

**Toute réponse HTTP dont la taille peut dépasser ~4 Ko doit utiliser l'un des trois patrons suivants — les gardes à seuil heap fixe sont interdites :**

- **(a) Paginée + pré-dimensionnée exactement** : page bornée (~4 Ko max), `measureJson()` sur le document réel, garde `taille + kHttpHeapMarginBytes (4096) > ESP.getMaxAllocHeap()` → **503** propre, sinon `beginResponseStream(type, taille)` **pré-dimensionné exact** (une seule allocation, zéro `resize()`).
- **(b) Chunked sans accumulation RAM** : `beginChunkedResponse()`, le callback remplit le buffer TCP borné fourni par AsyncWebServer — aucune structure proportionnelle à la taille de l'export. Aucune garde heap nécessaire.
- **(c) Allocation unique testée** : `String::reserve(pire_cas)` dont le **retour `bool` EST le test du bloc contigu** — échec → **503** propre, jamais de JSON tronqué silencieux ; le `reserve` doit couvrir le pire cas (sinon réallocation en cours de concaténation et garde contournée).

La marge commune est `kHttpHeapMarginBytes = 4096` (`constants.h`). `kMinHeapForLogsResponse` est **supprimée**.

## Alternatives considérées

- **Helper commun `beginGuardedStream()`** (rejetée) — les trois patrons reposent sur des mécanismes différents (JsonDocument mesuré / callback chunked / String réservée) : un helper unique n'aurait factorisé aucune duplication réelle et aurait masqué le choix du patron, qui est la décision importante par route.
- **`try/catch` autour de `serializeJson` / des écritures stream** (rejetée) — masque le problème au lieu de le prévenir : l'exception part du `resize()` du `cbuf`, dont l'état est alors incohérent ; on répondrait 500 avec une réponse partiellement bufferisée, et le coût mémoire transitoire (ancien + nouveau bloc) resterait.
- **Relever le seuil fixe** (rejetée) — c'est l'approche v2.8.2, invalidée : aucun seuil constant ne borne une réponse dont la taille dépend des données (longueur libre des messages de log, corps de release GitHub…).

## Audit des routes (v2.10.4)

Routes corrigées ou bornées par cette spec :

| Route | Patron | Correctif |
|---|---|---|
| `GET /get-logs` | (a) paginée | Page 30 entrées (hard cap 60, queue même-ms incluse), `measureJson` + garde + stream pré-dimensionné |
| `GET /download-logs` | (b) chunked | Machine à états fichier → RAM, report de ligne `logs_export_chunk.h`, zéro accumulation |
| `GET /get-history` | (c) reserve testé | `reserve(points × kHistoryPointJsonMaxBytes(128) + 128)`, échec → 503 |
| `GET /check-update` | bornage amont | Release notes GitHub tronquées à `kOtaReleaseNotesMaxChars` (2048) + « … » |

Patrons de référence préexistants : `GET /debug/sensor_trace` (patron (a), v2.10.1) ; `GET /coredump/download` (patron (b)).

Routes prouvées **SÛRES** (borne structurelle, aucune modification) :

| Route(s) | Preuve de borne |
|---|---|
| `GET /data` | `StaticJson<1152>` (pile) |
| `GET /get-config` | `StaticJson<2304>` (pile) |
| Routes calibration (`web_routes_calibration.cpp`) | `StaticJson<128>` par réponse |
| Routes auth, sensor_id, coredump/info, contrôle (pompes, filtration, éclairage…) | `JsonDocument` de quelques champs fixes, très en dessous de 4 Ko |
| Fichiers statiques (`data/` gzippé), `/coredump/download` | Servis par AsyncWebServer sans bufferisation applicative |

## Conséquences

### Positives

- Plus aucun `abort()` déclenchable depuis le navigateur par simple consultation d'une page : le pire cas devient un **503** propre (« heap fragmenté, réessayez ») ou un export légèrement dégradé.
- Le coût mémoire de chaque route est **lisible dans le code** (taille de page, capacité de carry, pire cas du reserve) et vérifié par tests natifs (`test_native_logs_page`, `test_native_logs_export` — 21 tests).
- L'audit ci-dessus sert de référence : toute route absente des deux tableaux est une régression d'audit.

### Négatives / dette assumée

- `/get-logs` demande plusieurs allers-retours pour un journal plein (l'UI boucle, max 8 pages) — latence perçue légèrement supérieure sur 200 entrées.
- `/download-logs` est **best-effort** : le ring RAM peut tourner entre deux chunks (doublon/saut d'entrée tolérés) et une prise de mutex en échec termine l'export sans erreur ; lignes > 256 octets tronquées « … ».
- Cas dégénéré de la pagination : > 60 entrées dans la même milliseconde → entrées même-ms résiduelles invisibles au curseur (restent dans `/download-logs`) — jamais observé en pratique.
- `/check-update` ne relaie plus que les 2048 premiers caractères des notes de release.

### Ce que ça verrouille

- **Toute nouvelle route HTTP** dont la réponse peut dépasser ~4 Ko doit choisir explicitement un des trois patrons (a)/(b)/(c) — un `AsyncResponseStream` non pré-dimensionné sans garde exacte est un défaut de revue.
- **Interdiction des gardes heap à seuil fixe** : la garde compare toujours la taille **réelle** (mesurée ou pire-cas réservé) au bloc contigu disponible.
- `kHttpHeapMarginBytes` (4096) est la marge unique partagée (alias `kSensorTraceHeapMarginBytes` conservé pour l'historique v2.10.1).

## Références

- Code : `src/web_routes_data.cpp` (`handleGetLogs`, `handleDownloadLogs`, `handleGetHistory`), `src/web_routes_ota.cpp` (`handleCheckUpdate`), `src/logs_page.h`, `src/logs_export_chunk.h`, `src/logger.h` (`forEachRecentLog` visiteur `bool`, `getLogEntryByIndex`), `src/constants.h` (`kHttpHeapMarginBytes`, `kLogsMaxEntriesPerPage`, `kLogsPageHardCap`, `kLogExportLineMaxBytes`, `kHistoryPointJsonMaxBytes`, `kOtaReleaseNotesMaxChars`)
- Spec : `specs/features/doing/bug-crash-getlogs-badalloc-audit-routes.md`
- ADR liés : [ADR-0029](0029-auto-guerison-heap.md) (fragmentation du tas, auto-guérison), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (modules purs testables en natif)
- Doc externe : [`docs/API.md`](../API.md) (contrats `/get-logs`, `/download-logs`, `/get-history`, `/check-update`), [`docs/subsystems/logger.md`](../subsystems/logger.md)
