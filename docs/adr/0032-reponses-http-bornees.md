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
| `GET /get-history` | (c) reserve testé → **(b) chunked depuis v2.14.0** | `reserve(points × kHistoryPointJsonMaxBytes(128) + 128)`, échec → 503 — *supersedé par l'[Extension 2026-07-30](#extension-2026-07-30--get-history-chunked-et-filet-ws-v2140)* |
| `GET /check-update` | bornage amont → **flux entrant borné depuis v2.16.1** | Release notes GitHub tronquées à `kOtaReleaseNotesMaxChars` (2048) + « … » — *complété par l'[Extension 2026-08-04](#extension-2026-08-04--check-update-parse-en-flux-entrant-v2161)* (le corps GitHub REÇU restait chargé entier en RAM) |

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

## Extension 2026-07-28 — émissions WebSocket (v2.11.1)

Le coredump du 2026-07-28 (`bad_alloc` dans `AsyncWebSocket::textAll` → `makeSharedBuffer`) a montré que la **voie WebSocket** relevait de la même classe de défaillance que les routes HTTP auditées ici : une allocation throwante non rattrapée sur un tas fragmenté, déclenchée par une simple émission vers le navigateur. La philosophie de cet ADR est étendue aux **émissions WS** :

- **Garde côté appelant, jamais dans la lib** : chaque broadcast (`broadcastSensorData` / `broadcastConfig` / `broadcastLog`) passe par `_heapAllowsPush()` **avant** toute construction de JSON — bloc contigu < `kWsPushMinAllocBytes` (8192) → push **sauté** (l'UI rate un cycle de 5 s, sans coût UX). Nuance vs l'interdiction des seuils fixes du présent ADR : elle vise les réponses dont la taille **dépend des données** ; les payloads WS sont **structurellement bornés** (`StaticJson<2048>` + `reserve(kWsSensorJsonReserveBytes)`), un seuil constant couvrant payload + copies internes + marge TOCTOU est donc légitime — et il est aligné sur `kRestartMaxAllocHeapBytes` (sous ce niveau, l'appareil est de toute façon en zone de restart auto-guérison, [ADR-0029 §Amendement](0029-auto-guerison-heap.md#amendement-2026-07-28--échantillonnage-adaptatif-v2111)).
- **Buffers de file bornés** : la file d'envoi par client passe de 32 (défaut fork, ~58 Ko épinglables par client zombie) à **5 trames** (`-DWS_MAX_QUEUED_MESSAGES=5`, `platformio.ini`) + `setCloseClientOnQueueFull(true)` — le coût mémoire d'un client WS redevient **lisible et borné**, comme celui d'une route HTTP auditée.

Détails d'implémentation et invariants (anti-ré-entrance du log, récolte des zombies) : [subsystems/ws-manager.md](../subsystems/ws-manager.md).

## Extension 2026-07-30 — `/get-history` chunked et filet WS (v2.14.0)

2ᵉ coredump `bad_alloc` dans un broadcast WS (30/07, v2.13.0) **malgré** l'extension v2.11.1 : la garde à seuil fixe est un check-then-act (TOCTOU), et le générateur de fond **G3** de l'audit churn-heap du 24/07 (différé « si récidive ») était toujours actif. Récidive → application complète de l'ADR (spec `bug-heap-toctou-ws-et-gethistory-chunked`) :

- **`/get-history` migre du patron (c) au patron (b)** — chunked zéro-buffer, patron `/download-logs` : la `String` contiguë ~40 Ko épinglée à chaque ouverture du dashboard disparaît (**la plus grosse allocation contiguë du firmware**), le 503 « heap fragmenté » n'existe plus sur cette route. Copie d'un point sous mutex court (25 ms) + `RESPONSE_TRY_AGAIN` borné ; contrat JSON octet pour octet inchangé. **Le tableau d'audit est soldé : G3 traité, plus aucune route au patron (c) avec un pire cas > 4 Ko dépendant des données.** Détails : [subsystems/history.md](../subsystems/history.md#export-chunked-get-history-v2140-bug-heap-toctou-a3).
- **Voie WS durcie en profondeur** (complète l'extension v2.11.1) : seuil `kWsPushMinAllocBytes` 8192 → **16384** (l'alignement sur `kRestartMaxAllocHeapBytes` créait une zone grise 8-16 Ko où les pushes passaient sans que l'auto-guérison s'arme) ; **`try/catch (std::bad_alloc)`** autour des 3 `textAll` (filet absolu : l'exception part de `makeSharedBuffer` avant toute mutation de file du fork → push sauté, jamais d'`abort()`) ; trames construites dans un **buffer statique `.bss`** (`kWsFrameBufBytes = 3072`) — la mention « `StaticJson<2048>` + `reserve(kWsSensorJsonReserveBytes)` » de l'extension v2.11.1 est **caduque** (constante supprimée). Détails : [subsystems/ws-manager.md](../subsystems/ws-manager.md#angle-mort-toctou-comblé--filet-bad_alloc--buffer-statique-v2140-bug-heap-toctou).

Nuance vs l'alternative « `try/catch` » **rejetée** par cet ADR pour les routes HTTP : elle reste rejetée côté HTTP (l'exception du `cbuf` laisse un état incohérent). Côté WS, le catch est **sûr et légitime** car il enveloppe une émission atomique dont l'allocation précède toute mutation d'état — il complète la garde, il ne la remplace pas.

## Extension 2026-08-04 — `/check-update` parse en flux entrant (v2.16.1)

Incident production du 2026-08-04 (appareil 2.14.0, fonctionnement prolongé) : « Erreur HTTP GitHub: -1 » systématique sur heap fragmenté (l'allocation du contexte TLS ~48 Ko échoue avant toute communication) et « JSON parse error » juste après reboot. Il a montré que la décision du 2026-07-24 avait un angle mort : elle bornait les réponses **sortantes** construites en RAM, et le « bornage amont » v2.10.4 de `/check-update` ne bornait que ce qui était *relayé* au navigateur — pas ce qui était **reçu** de GitHub. `https.getString()` chargeait la réponse entière (10 236 o mesurés pour la release v2.16.0, croissante à chaque release — corps de release jusqu'à 125 000 caractères côté GitHub), puis `deserializeJson` construisait le document complet (~2-3× l'entrée) : pic transitoire ~85-95 Ko avec la session TLS, exigeant de gros blocs contigus. **Le patron est étendu aux réponses ENTRANTES d'API externes** (spec `bug-checkupdate-heap-tls`) :

- **Parse en flux avec filtre** : `https.useHTTP10(true)` (pas de Transfer-Encoding chunked — patron ArduinoJson officiel) + `deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter))`, filtre limité aux 7 champs consommés (`tag_name`, `name`, `published_at`, `body`, `assets[].name/browser_download_url/digest`). Le corps n'est plus jamais stocké entier.
- **Budget dur NET** : `JsonDocument` sur allocateur plafonné `BudgetJsonAllocator` (`kOtaCheckJsonBudgetBytes = 8192`, `constants.h`) — en-tête de taille par bloc pour **créditer** le budget à la libération et au rétrécissement (ArduinoJson double ses buffers de chaîne pendant le parse puis les rétrécit : un comptage monotone facturerait ~4× la taille utile, et un realloc de rétrécissement ne doit jamais être refusé) ; `malloc`/`realloc`, jamais `new[]` → un refus produit `NoMemory`, jamais `std::bad_alloc`. Dépassement → **500** propre « GitHub response too large » + log dédié « Réponse GitHub hors budget JSON /check-update » (distinct du parse invalide, qui logue désormais le détail de l'erreur).
- **Troncature UTF-8 sûre** : la coupe brute `substring(0, 2048)` des release notes pouvait sectionner un caractère multi-octets (accents/emojis des notes en français) → JSON invalide côté navigateur. `utf8SafeCutLen` coupe sur une frontière de caractère.
- Logique pure testable en natif ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) : `src/ota_check_logic.h` (`AllocBudget`, `utf8SafeCutLen`), 13 tests `test/test_native_ota_check/`.

Pic mémoire du handler : ~35 Ko → **≤ 8 Ko**, hors session TLS (~48 Ko, incompressible avec `WiFiClientSecure` — hors périmètre, assumé).

Verrouillage additionnel : **tout corps de réponse d'API externe dont la taille dépend de données distantes doit être parsé en flux avec budget — un `getString()` sur un tel corps est un défaut de revue**, symétrique de la règle sur les réponses sortantes.

## Références

- Code : `src/web_routes_data.cpp` (`handleGetLogs`, `handleDownloadLogs`, `handleGetHistory`), `src/web_routes_ota.cpp` (`handleCheckUpdate`, `BudgetJsonAllocator`), `src/ota_check_logic.h` (`AllocBudget`, `utf8SafeCutLen`), `src/logs_page.h`, `src/logs_export_chunk.h`, `src/logger.h` (`forEachRecentLog` visiteur `bool`, `getLogEntryByIndex`), `src/constants.h` (`kHttpHeapMarginBytes`, `kLogsMaxEntriesPerPage`, `kLogsPageHardCap`, `kLogExportLineMaxBytes`, `kHistoryPointJsonMaxBytes`, `kOtaReleaseNotesMaxChars`, `kOtaCheckJsonBudgetBytes`)
- Spec : `specs/features/doing/bug-crash-getlogs-badalloc-audit-routes.md`
- ADR liés : [ADR-0029](0029-auto-guerison-heap.md) (fragmentation du tas, auto-guérison), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (modules purs testables en natif)
- Doc externe : [`docs/API.md`](../API.md) (contrats `/get-logs`, `/download-logs`, `/get-history`, `/check-update`), [`docs/subsystems/logger.md`](../subsystems/logger.md)
