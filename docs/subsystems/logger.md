# Subsystem — `logger`

- **Fichiers** : [`src/logger.h`](../../src/logger.h), [`src/logger.cpp`](../../src/logger.cpp)
- **Singleton** : `extern Logger systemLogger;`


> ⚠️ **Accès fichiers — 8 des sites `fopen` du firmware sont ici**
> ([`logger.cpp:442`](../../src/logger.cpp:442), **523**, **615-616**, **683**,
> **697**, **707**, **719** — numéros de ligne à jour au **v2.52.1**, feature-129
> ayant déplacé ces sites). **Sept sur huit** sont classés **contention TCP** :
> `flushToDisk()` s'exécute **sur la tâche qui a émis le log** — `loopTask`,
> `mqttTask` ou `async_tcp` — parce que `ERROR` et `CRITICAL` déclenchent un flush
> **synchrone** ([`logger.cpp:234`](../../src/logger.cpp:234)). Il n'existe pas
> de « tâche du logger ». L'échec du `malloc` du verrou newlib appelle
> **`abort()`**. Voir [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md)
> (dont la décision **(f)** lève la contradiction de l'interdit `critical()` posé
> par [ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md)) et le
> recensement [acces-fichiers.md](acces-fichiers.md). ✅ **Gardes posées depuis la
> v2.43.0** (cycle 2 de `bug-100`) — la mention « garde non posée » qui figurait
> ici était périmée.
>
> Le **huitième** site est la sonde de [`reconcileLogTmp()`](#reprise-dune-rotation-interrompue-au-démarrage-bug-099-v2481)
> ([`logger.cpp:442`](../../src/logger.cpp:442)), seul site **classe B** du journal
> ([ADR-0076](../adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md)).

## Rôle

Log central du firmware avec buffer circulaire en RAM + persistance sur la partition `history` + push temps réel via WebSocket. Niveaux : `DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL`.

## API publique

> ⚠️ **feature-129 (v2.52.1)** : le stockage est passé d'un `std::vector<LogEntry>`
> de `String` à une **arène circulaire d'octets en `.bss`** — voir
> [« Arène circulaire »](#arène-circulaire-du-journal-feature-129) ci-dessous et
> [ADR-0082](../adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md).
> `LogEntry` **n'existe plus** : remplacé par deux types **distincts, non
> interchangeables** — `LogRecord` (vue jamais dans l'arène, callback WS et
> `copyEntryByIndex`) et `LogArenaView` (vue dans l'arène, valide sous mutex
> seulement, `forEachRecentLog`). `getLogEntryByIndex()` est renommé
> `copyEntryByIndex()`. `_persistBuffer` a disparu : la ligne persistée est
> **dérivée** de l'arène au moment du flush, jamais stockée en double.

```cpp
void begin();

// log() n'exécute AUCUNE allocation dynamique (memcpy sous mutex vers l'arène).
void log(LogLevel level, const char* message, size_t len);
void log(LogLevel level, const String& message);  // délègue à la surcharge ci-dessus

// Chaque niveau a DEUX surcharges : const char* ET const String&. La surcharge
// const char* évite de construire une String temporaire à partir d'un tampon
// pile existant (ex. char msg[480] dans pump_controller.cpp) — sur les sites de
// garde chimique, cette String évitée aurait pesé jusqu'à 480 o par appel.
void debug(const char* message);
void debug(const String& message);
void info(const char* message);
void info(const String& message);
void warning(const char* message);
void warning(const String& message);
void error(const char* message);
void error(const String& message);
void critical(const char* message);
void critical(const String& message);

// Push WS temps réel. `LogRecord::message` pointe dans le tampon de
// l'APPELANT de log() — JAMAIS dans l'arène (condition C3, feature-129).
void setLogCallback(std::function<void(const LogRecord&)>);

// Itération en place des logs récents, SANS copie (v2.8.2) ; le visiteur
// retourne false pour arrêter l'itération (v2.10.4 — pagination /get-logs).
// Le visiteur reçoit un LogArenaView : `message` pointe DANS l'arène,
// valide SOUS CE MUTEX SEULEMENT (feature-129).
void forEachRecentLog(size_t count, unsigned long sinceTimestamp,
                      const std::function<bool(const LogArenaView&)>& visitor) const;

// Copie UNE entrée sous mutex, DANS le tampon fourni par l'appelant — jamais
// de pointeur d'arène rendu (v2.10.4 — export chunked /download-logs ;
// renommé copyEntryByIndex en feature-129, même contrat best-effort).
bool copyEntryByIndex(size_t chronoIdx, LogRecord& out, char* buf, size_t bufLen) const;

void clear();      // vide uniquement l'arène (RAM)
void clearAll();   // vide l'arène + supprime /system.log et /system.log.tmp
size_t getLogCount();

// Persistance — réconcilie le `.tmp` de rotation EN TÊTE d'appel (v2.48.1,
// bug-099) : cf. « Reprise d'une rotation interrompue au démarrage »
void setPersistenceFs(fs::FS* fs);
void update();              // flush différé
void flushToDisk();         // flush immédiat
fs::FS* getPersistenceFs() const;
```

Fonction libre (`logger.h`, v2.43.0, bug-100) :

```cpp
// Flushs disque REFUSÉS pour famine mémoire, cumulés depuis le boot (les lignes
// sont conservées, le flush est reporté). Publié en `ldef` dans le payload
// RETENU <base>/diagnostic.
uint32_t loggerFopenDeferralCount();
```

## Toggle DEBUG runtime

`Logger::debug(const String&)` est conditionné par le booléen `authCfg.debugLogsEnabled` (default `false`, persisté en NVS sous la clé `debug_logs`). Le check est posé en **toute première ligne** de la fonction (early return) — aucune allocation `String`, aucun lock mutex, aucun push WS, aucune écriture fichier n'a lieu quand le toggle est désactivé.

```cpp
void Logger::debug(const String& msg) {
    if (!authCfg.debugLogsEnabled) return;  // early return — pas de coût
    log(LogLevel::DEBUG, msg);
}
```

Les autres niveaux (`info`, `warning`, `error`, `critical`) **ne sont pas affectés** par ce toggle.

⚠️ **`DEBUG` n'est pas gratuit** : il épargne le fichier `/system.log`, **mais pas** le ring RAM, le push WebSocket ni la console — et la construction des arguments doit être gardée au site d'appel. Voir [Coût RÉSIDUEL d'un détail passé en DEBUG](#coût-résiduel-dun-détail-passé-en-debug-feature-109).

Activation utilisateur : Paramètres → Avancé → card Logs → switch « Logs DEBUG activés ». Effet immédiat (pas de redémarrage). La valeur est lue à chaque appel à `debug()` depuis la variable globale `authCfg`, sans mutex (lecture booléenne atomique sur ESP32).

Default `false` choisi pour alléger le buffer en production : les logs `DEBUG` (`Diagnostic publié`, `Consolidation terminée: N points`, `MQTT publish drop`, etc.) ne saturent plus les 200 entrées de `kMaxLogEntries`. L'utilisateur active explicitement le switch en cas de session de diagnostic.

Le filtre UI `#log_level_debug` de la page Logs reste indépendant : il filtre l'affichage navigateur des entrées DEBUG déjà produites par le firmware. Les deux mécanismes sont complémentaires :

| Mécanisme | Effet |
|---|---|
| Switch « Logs DEBUG activés » (firmware) | Décide si `Logger::debug()` **produit** les entrées |
| Filtre `#log_level_debug` (UI) | Décide si l'UI **affiche** les entrées DEBUG produites |

## Arène circulaire du journal (feature-129)

**Remplace, depuis la v2.52.1**, le `std::vector<LogEntry>` de `String` qui
s'allouait sur le tas interne 8 bits **au milieu du plus gros bloc libre** et le
fractionnait — mesuré en production : 168 octets (les deux `String` d'une seule
ligne de journal) ont détruit 23 552 octets de contiguïté à la première
sauvegarde d'historique. Décision complète, alternatives écartées (PSRAM en
tête) : [ADR-0082](../adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md).

Trois objets `.bss` de taille **fixe**, en RAM **interne** (jamais PSRAM — le
danger de cache coupé pendant `flushToDisk()` l'interdit, voir l'ADR) :

| Objet | Constante | Taille | Rôle |
|---|---|---:|---|
| `sLogArena` | `kLogArenaBytes` | 16 384 o | texte des messages, file circulaire d'octets |
| `sLogSlots` | `kMaxLogEntries` × 20 o | 4 000 o | descripteurs POD (`LogSlot` : `timestamp`, `wallSec`, `seq`, `off`, `len`, `level`, `flags`) |
| `sFlushLine` | `kLogPersistLineMax` | 832 o | ligne de flush — propriété **exclusive** du flusher, jamais partagée avec `log()` |

Coût mesuré : **+21 240 octets de `.bss`** (21 216 prédits). `log()` n'exécute
plus **aucune allocation dynamique** : un `memcpy` sous le mutex existant, vers
un emplacement de taille fixe décidé par le module pur
[`src/log_arena_logic.h`](../../src/log_arena_logic.h) (`ADR-0017` — zéro
dépendance Arduino, zéro mutex, zéro I/O).

### Constantes ([`constants.h`](../../src/constants.h), section « JOURNAL »)

| Constante | Valeur | Motif |
|---|---:|---|
| `kLogArenaBytes` | 16 384 | 82 o/entrée pour 200 entrées (mesuré : 52 o) — corrige une pénurie de **contiguïté**, pas d'octets |
| `kLogMessageMaxBytes` | **768** | ⚠️ pas 256, voir ci-dessous. Ne réserve **aucun** octet de `.bss` : borne le pire cas de *profondeur*, pas la mémoire |
| `kLogTruncTail` | `"... (tronque)"` | marqueur ASCII **strict** (jamais « … », 3 o, qui exigerait de compter en octets et non en caractères) |
| `kLogPersistLineMax` | 832 | horodatage (19) + espace + niveau (≤5) + espace + `kLogMessageMaxBytes` + marqueur + `\n`/NUL |
| `kLoggerFlushRedoMaxPasses` | 3 | borne la boucle de reprise de `flushToDisk()` sous rafale ERROR/CRITICAL concurrente — recommandation P2 `pool-chemistry` |

Quatre `static_assert` de **plancher** en gardent la cohérence à la
compilation : `kLogArenaBytes` ≥ 200 × 64 o, `kLogMessageMaxBytes` ≤ 1/8 de
l'arène (un seul message ne peut pas monopoliser l'arène), `kLogPersistLineMax`
suffisant pour le pire cas formaté, et **`kLogMessageMaxBytes >= 640`** —
celui-ci verrouille explicitement le plafond contre un retour en arrière
silencieux.

feature-130 (ADR-0082) ajoute leur **symétrique** : deux `static_assert` de
**plafond**, `kLogArenaBytes <= 16384` et `kMaxLogEntries <= 200`. Sans eux,
doubler l'arène (ou le nombre d'entrées) ne demandait de toucher **aucune**
assertion — donc aucune justification écrite — puisque les quatre asserts
d'origine ne contraignent qu'un plancher. `sLogArena`/`sLogSlots` sont des
objets `.bss` de taille **fixe** : les grossir grignote la même ressource
rare que feature-129 a reconquise, la contiguïté du tas 8 bits. Un agent qui
a besoin de plus **doit** remonter ces deux bornes ici, explicitement —
jamais en changeant seulement `kLogArenaBytes`/`kMaxLogEntries` sans y
toucher.

Deux `static_assert` supplémentaires verrouillent l'**empreinte réelle**,
ailleurs que dans `constants.h` :

- [`log_arena_logic.h`](../../src/log_arena_logic.h) :
  `sizeof(LogSlot) == 20` — tout champ ajouté ou élargi à `LogSlot` qui
  ferait dériver ce POD de 20 octets doit se justifier à cet endroit précis
  (`sLogSlots[kMaxLogEntries]` et le budget `.bss` ci-dessous en dépendent).
- [`logger.cpp`](../../src/logger.cpp) :
  `sizeof(sLogArena) + sizeof(sLogSlots) + sizeof(sFlushLine) <= 21216` —
  verrouille l'empreinte `.bss` **mesurée** des trois objets, pas une
  reformulation arithmétique de `kLogArenaBytes + kMaxLogEntries ×
  sizeof(LogSlot) + kLogPersistLineMax` (qui ne vérifierait que le calcul,
  pas ce que le lien a réellement réservé — un padding de structure
  inattendu passerait inaperçu).

Ces six `static_assert` sont vérifiés à la compilation ; la sentinelle
[`tools/check_memory_discipline.sh`](../BUILD.md#toolscheck_memory_disciplinesh--la-discipline-mémoire-du-journal-reste-vraie-adr-0082-feature-130)
(feature-130, job `memory-discipline` de `hygiene-specs.yml`) referme la
brèche que ces asserts ne couvrent pas : rien n'empêchait, avant elle,
qu'un `std::vector<String>` soit réintroduit dans `logger.h` — il aurait
compilé, passé `pio test -e native`, et ne rencontré aucun contrôle.

#### Pourquoi 768 et non 256 — le plafond a failli tronquer les gardes chimiques

Le comptage initial (718 sites, littéraux agrégés *par appel*, max 257 o) est
**méthodologiquement faux pour les messages de sécurité** : les plus longs sont
construits **avant** l'appel — dans un `char[]` ou par concaténation — et
l'appel à `systemLogger` ne porte plus qu'un préfixe court (`"[Dosage pH] "`).
Pire cas réel mesuré par `pool-chemistry` : **≈ 630 o**
([`mqtt_manager.cpp:1614`](../../src/mqtt_manager.cpp:1614), refus de mode pH +
`phAutoManualReason`). Huit des dix sites concernés portent des messages de
sécurité chimique.

Précédent déjà commis puis corrigé, cité en commentaire à
[`pump_controller.cpp:3609`](../../src/pump_controller.cpp:3609) : un ancien
`msg[248]` coupait à « …bande mor » — c'était l'**ENTRÉE du défaut** que ce
filet devait rendre traçable qui disparaissait. Plafonner à 256 aurait réinfligé
ce défaut à tous les sites de garde à la fois — condition bloquante **C1**
(`pool-chemistry`).

**Validé par mutation** (`test/test_native_log_guard_messages/`, phase B,
16/09/2026) : reconstruire les dix messages de garde pire cas et affirmer
`len <= kLogMessageMaxBytes`. Ramener la constante à 256 fait échouer **six des
dix** cas — pas huit comme estimé au départ : `fs_guard.cpp`, `oom_guard.cpp` et
`main.cpp` sont bornés **structurellement** par de petits tampons de pile fixes
ou des littéraux courts, et un site capteur reste sous 256 à son pire cas
honnête. Une mutation qui ne tue que six mutants sur dix décrits n'est pas une
faiblesse du test — c'est une propriété mesurée du firmware.

### Les six compteurs d'observabilité

Atomiques `relaxed`, lisibles depuis n'importe quelle tâche **sans** prendre le
mutex du logger, jamais journalisés dans leur propre chemin d'échec (ferme la
récursion `systemLogger → broadcastLog → _heapAllowsPush → systemLogger`,
documentée en tête de [`heap_metrics.h`](../../src/heap_metrics.h)). Publiés par
`GET /get-system-info` — voir [`docs/API.md`](../API.md#get-get-system-info)
pour le contrat complet.

| Compteur | Getter C++ | Sens |
|---|---|---|
| `log_entries_current` | `logEntriesCurrent()` | entrées vivantes dans l'arène (≤ 200) |
| `log_arena_bytes_used` | `logArenaBytesUsed()` | octets occupés dans `sLogArena` (≤ 16 384) |
| `log_arena_truncations` | `logArenaTruncations()` | messages tronqués (dépassement de `kLogMessageMaxBytes`) depuis le boot |
| `log_arena_evictions` | `logArenaEvictions()` | créneaux évincés de l'arène depuis le boot, tous motifs confondus |
| `log_persist_overrun` | `logPersistOverruns()` | créneaux évincés **avant** d'avoir été persistés alors qu'ils auraient dû l'être (C6, distinct de `log_arena_evictions`) |
| `log_arena_overlap_guard_trips` | `logArenaOverlapGuardTrips()` | déclenchements du détecteur d'invariant `logArenaWriteOverlapsLive()` — **doit rester à 0** |

`log_persist_overrun` est la trace de l'**inversion de propriété** posée par la
condition C6 : `_persistBuffer` était une file *indépendante* de l'arène ;
dériver la ligne persistée de l'arène rend les deux **solidaires** — sous
rafale, une ligne `WARNING` peut désormais être évincée de l'arène avant
d'avoir atteint `/system.log`. `ERROR`/`CRITICAL` restent couverts par le flush
synchrone, inchangé.

### Politique « pas d'enjambement » (C4)

Si la place contiguë en fin d'arène est insuffisante pour un message, elle est
**abandonnée** (`tailDrop`) et l'écriture repart à l'offset 0 — un message n'est
**jamais** scindé en deux par la frontière de fin d'arène, ce qui permet à tout
lecteur de rendre un `const char*` contigu. Le gaspillage est borné par
`kLogMessageMaxBytes`.

### Le détecteur d'invariant qui survit en production

`logArenaWriteOverlapsLive()` (`log_arena_logic.h`/`.cpp`) est un filet
**indépendant** de `logArenaPlan()`/`logRingEvictCount()`, exécuté juste avant
tout `memcpy` dans l'arène. Il a été ajouté après qu'un défaut de géométrie —
la place libre calculée depuis `writeOff` au lieu de `head`, invisible aux cinq
autres compteurs pendant qu'il corrompait des messages vivants — a été trouvé et
corrigé avant livraison (reproduit à la 508ᵉ et à la 853ᵉ écriture en
simulation). Sur déclenchement, le repli est d'évincer **tout** l'anneau plutôt
que d'écrire par-dessus un message vivant : perdre le journal vaut mieux que le
falsifier ([ADR-0072](../adr/0072-absence-de-valeur-hors-du-domaine.md)). Détail
complet, y compris les trois enseignements du post-mortem :
[ADR-0082](../adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md).

### Contrainte de pile

Aucun tampon de `kLogMessageMaxBytes` sur la pile, ni dans `log()` ni dans le
flush — `log()` s'exécute depuis `async_tcp`, qui a une pile courte. Le `memcpy`
va directement de `message.c_str()` vers l'arène ; le flush écrit en deux temps
(en-tête horodaté dans `sFlushLine`, ~32 o, puis les octets lus **directement**
depuis `sLogArena` vers `sFlushLine`), sans jamais reconstituer une ligne de
800 o ailleurs.

### Le nom d'un niveau — `logLevelName()`, sans allocation (feature-130, ADR-0082)

`String Logger::getLevelString(LogLevel)` **n'existe plus**. Remplacée par

```cpp
// log_arena_logic.h, réexportée par logger.h
const char* logLevelName(LogLevel level);   // pointeur STATIQUE, jamais nullptr
```

Le pourquoi est l'enseignement du lot : `log()` construisait auparavant son
en-tête avec `getLevelString(level).c_str()` — un appel qui allouait une
`String` **temporaire**, puis en extrayait le pointeur interne. La garantie
« `log()` n'alloue rien », établie par feature-129, ne reposait donc **pas**
sur le type de retour, mais sur l'optimisation *short string* d'Arduino
(`String` de moins de 14 caractères stockée **en place**, sans `malloc`) : les
cinq libellés (`DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL`) font 4 à 7
caractères, tous sous ce seuil. C'est une propriété **invisible dans le
code** — rien n'écrivait « ceci ne doit jamais dépasser 13 caractères » — et
qu'un futur libellé plus long (`WARNING_HIGH`, par exemple) aurait cassée en
silence, sur le chemin exact que feature-129 venait de nettoyer.

`logLevelName()` porte désormais la garantie **dans son type de retour** :
un `const char*` littéral (durée de vie programme) ne peut pas allouer, quelle
que soit sa longueur. `tools/check_memory_discipline.sh` (M3A) refuse en plus
toute déclaration `String` dans le chemin de journalisation lui-même — la
garantie ne dépend donc plus ni d'une optimisation de bibliothèque, ni de la
discipline d'un futur appelant.

Trois appelants migrés, tous vers un stockage **par pointeur** (ArduinoJson
ne copie pas un `const char*`) au lieu d'une copie de `String` :
[`logger.cpp:258`](../../src/logger.cpp:258) (en-tête de ligne persistée),
[`ws_manager.cpp:282`](../../src/ws_manager.cpp:282) (`broadcastLog`, champ
`level` du push WebSocket) et
[`web_routes_data.cpp:866,929`](../../src/web_routes_data.cpp:866) (`/get-logs`,
export `/download-logs`).

## Lecture des logs récents — `forEachRecentLog` (v2.8.2, visiteur `bool` en v2.10.4, `LogArenaView` en feature-129)

```cpp
void forEachRecentLog(size_t count, unsigned long sinceTimestamp,
                      const std::function<bool(const LogArenaView&)>& visitor) const;
```

Itère les `count` entrées les plus récentes (dont le `timestamp > sinceTimestamp`, `0` = pas de filtre) **en place, sous mutex, sans copie**. Le `visitor` est appelé pour chaque entrée dans l'ordre chronologique, avec un `LogArenaView` **pointant dans `sLogArena`** — aucune allocation interne, aucune copie de texte.

**Retour du visiteur (v2.10.4, bug-crash-getlogs-badalloc)** : `false` = **arrêt anticipé** de l'itération. C'est le mécanisme de pagination de `/get-logs` : la page complète est construite en **une seule prise de mutex** (jamais de double passage), l'arrêt survient quand la page est pleine (règles d'admission : [`src/logs_page.h`](../../src/logs_page.h)).

**Contrat mutex** : le `_mutex` est tenu pendant toute l'itération. Le `visitor` s'exécute donc **sous mutex** ; il ne doit faire que des écritures mémoire rapides — **allocations BORNÉES tolérées** (ex. le `JsonDocument` d'une page ≤ ~4 Ko de `/get-logs`), pas d'I/O bloquante, pas de yield, et ne **jamais** rappeler `systemLogger` (risque de deadlock / récursion).

⚠️ **Tout stockage du `message` d'un `LogArenaView` reçu par le visiteur DOIT en faire une COPIE IMMÉDIATE** — ex. `JsonString(v.message, v.len)`, jamais `o["x"] = v.message`. ArduinoJson 7 stocke un `const char*` **par référence** : `o["x"] = v.message` sérialiserait le pointeur **après** la libération du mutex, potentiellement sur un octet déjà réécrit. `web_routes_data.cpp` (`handleGetLogs`) applique cette règle ; c'est le **mutant non couvrable en test natif** documenté dans [ADR-0082](../adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md) — porté par la revue de code, jamais par la suite automatisée (ADR-0073).

### Contexte : correctif du crash `/logs` (`std::bad_alloc`)

Cette méthode **remplace** l'ancienne `std::vector<LogEntry> getRecentLogs(size_t)`. L'ancienne API **deep-copiait** tout le buffer (y compris les `String`) dans un vecteur retourné par valeur : sous fragmentation du tas, `operator new` levait `std::bad_alloc`, non rattrapée → `abort()` → PANIC → reboot. L'appareil devenait injoignable en HTTP alors que MQTT continuait de fonctionner (diagnostiqué via coredump). En itérant sans copie et en streamant la réponse, `forEachRecentLog` supprime cette allocation massive.

### Gardes heap des routes logs (v2.10.4, [ADR-0032](../adr/0032-reponses-http-bornees.md))

La constante `kMinHeapForLogsResponse` (24 Ko, v2.8.2) est **supprimée** : un seuil FIXE indépendant de la taille réelle de la réponse ne protégeait pas (2 coredumps `bad_alloc` → `abort`, 2026-07-23 `/debug/sensor_trace` et 2026-07-24 `/get-logs` — le commentaire v2.8.2 « le stream grandit par realloc, jamais d'abort » était faux : le `cbuf` d'`AsyncResponseStream` est un `new char[]` throwant). Remplacée par :

- **`/get-logs`** : page bornée (30 entrées, hard cap 60), `measureJson()` de la page réelle + marge `kHttpHeapMarginBytes` (4 Ko) comparés à `ESP.getMaxAllocHeap()` → **503** `{"error":"heap fragmenté, réessayez"}` ou stream **pré-dimensionné exact**. Chaque entrée porte le champ `trunc` (booléen) — voir [`docs/API.md`](../API.md#get-get-logs--write).
- **`/download-logs`** : flux **chunked zéro-RAM** (aucune garde nécessaire — rien de proportionnel à l'export n'est alloué), voir `copyEntryByIndex` ci-dessous.

Contrats détaillés : [`docs/API.md`](../API.md#get-get-logs--write).

## Copie unitaire — `copyEntryByIndex` (v2.10.4, renommée `getLogEntryByIndex` → `copyEntryByIndex` en feature-129)

```cpp
bool copyEntryByIndex(size_t chronoIdx, LogRecord& out, char* buf, size_t bufLen) const;
```

Copie **UNE** entrée sous mutex (`chronoIdx` chronologique, `0` = la plus ancienne) **dans le tampon `buf` fourni par l'appelant** — jamais de pointeur d'arène rendu à l'appelant, contrairement à `forEachRecentLog`. Si `slot.len > bufLen - 1`, le message est clampé UTF-8 (`utf8ClampLen()`, [`src/utf8_clamp.h`](../../src/utf8_clamp.h)) et `out.flags` porte `kLogFlagTruncated`. Retourne `false` si l'index dépasse le nombre d'entrées, si `buf`/`bufLen` est invalide, **OU** sur timeout mutex (`kLoggerMutexTimeoutMs`) — les trois cas sont indistinguables pour l'appelant : contrat **best-effort** assumé. Consommée par le callback chunked de `/download-logs` (une entrée par itération, une seule copie à la fois — plus de `String` intermédiaire) ; le ring peut tourner entre deux chunks (doublon/saut tolérés — export de diagnostic). Le report de ligne entre deux chunks est géré par le module pur [`src/logs_export_chunk.h`](../../src/logs_export_chunk.h) (`LogLineCarry<kLogExportLineMaxBytes>` / `emitCarry`, garantie de progression — jamais `0` avant la vraie fin ; lignes > 256 o tronquées « … »).

## Persistance sur LittleFS (partition history)

Activable via `setPersistenceFs(LittleFS*)` après montage de la partition `history`. Paramètres :
- Flush périodique : **10 min** (`kFlushIntervalMs = 600000`) — le flush immédiat sur ERROR/CRITICAL et le coredump couvrent les crashes.
- **Flush immédiat** sur `ERROR` et `CRITICAL` : `flushToDisk()` est appelé directement sans attendre l'intervalle périodique.
- Taille max du fichier : **8 KB** (`kMaxLogFileBytes = 8192`) — réduit de 16 → 8 KB en v2.11.0 ([ADR-0033](../adr/0033-budget-espace-partition-history.md)) pour tenir le budget d'espace garanti de la partition `history` (64 KB, pire cas log 2-3 blocs au lieu de 4-5). Historique : 32 → 16 KB antérieurement.
- Rotation : quand le fichier dépasse 8 KB, tronqué aux **6 derniers KB** (`kRotateKeepBytes = 6144`, était 12288) — le `.tmp` de rotation ne pèse plus que 2 blocs.

> ⚠️ Ces deux plafonds **font partie du budget d'espace** de la partition `history` (pire cas 13/16 blocs, [ADR-0033](../adr/0033-budget-espace-partition-history.md)) : ne pas les ré-augmenter sans recalculer le budget.

> 📋 **Les chiffres de cette page sont justes, ceux du code ne l'étaient pas.**
> Le commentaire de rotation de `logger.cpp` annonçait « 32KB / 24 derniers KB »
> face à des constantes valant **8 192** et **6 144** — **faux d'un facteur 4**,
> pendant des mois. Corrigé au lot `bug-099`, et corrigé **par référence aux
> constantes** (plus aucun chiffre en dur dans ce commentaire) pour qu'il ne
> pourrisse plus. Vérification faite : **ce document ne portait pas la même
> erreur** (8 KB / 6 derniers KB, historique « 32 → 16 KB antérieurement »
> correct). L'écart de chiffrage restant est celui de
> `kCalibrationAuthWarnThrottleMs`, signalé
> [plus bas](#émetteur-non-authentifié--le-motif-du-throttle-de-calibration-v2461).

### Reprise d'une rotation interrompue au démarrage (`bug-099`, v2.48.1)

La rotation ci-dessus **n'est pas atomique** : `flushToDisk()` exécute
`remove("/system.log")` **puis** `rename("/system.log.tmp", "/system.log")` —
deux opérations LittleFS distinctes. Une coupure entre les deux laisse le journal
**absent** et le `.tmp` porteur de l'**unique copie** des `kRotateKeepBytes`
(6 144) derniers octets.

`Logger::reconcileLogTmp()` (privée, appelée **en tête** de
`setPersistenceFs()`) réconcilie cet état au démarrage :

| `/system.log` | `.tmp` | Action | Journal |
|---|---|---|---|
| absent | présent | **PROMOUVOIR** — `rename(tmp → log)` | `WARNING` « Journal systeme recupere depuis le fichier temporaire de rotation … » |
| présent | présent | **EFFACER** le `.tmp` (la rotation se rejouera) | `WARNING` « Fichier temporaire de rotation orphelin efface au demarrage … » |
| — | absent | rien | **SILENCE** (premier démarrage compris) |
| admission mémoire refusée | — | **ABSTENTION** : ni promotion, ni effacement | **SILENCE** (muette, voir plus bas) |

#### Pourquoi l'effacement aveugle serait faux

La formulation initiale de la spec — « effacer tout `.tmp`, sa survie prouve
qu'il est orphelin » — est **fausse dans la fenêtre `remove`/`rename`**, et
l'appliquer **détruirait** le journal au lieu de nettoyer un déchet. Dans l'état
`log absent + tmp présent`, le `.tmp` **est** le journal.

#### Pourquoi le `.tmp` promu est complet

Le `remove` n'est atteint que **sous `tmpOk`** : le `.tmp` a été entièrement
recopié, `wf.close()` a finalisé ses métadonnées, et sa taille relue est
**> 0**. Un `.tmp` visible après un `remove` réussi est donc, par construction,
un fichier **complet et validé** — pas un fragment d'écriture. Un `.tmp`
incomplet, lui, ne peut coexister qu'avec un journal **présent** : il tombe dans
la branche `Discard`, où l'original fait foi.

#### ⚠️ CONTRAINTE D'ORDRE — le bloc ne se déplace pas

L'ouverture du marqueur `--- DÉMARRAGE ---` est en mode `"a"`, qui **crée**
`/system.log` s'il est absent. Un `reconcileLogTmp()` déplacé **après** elle
verrait la sonde répondre **toujours** « présent » : la branche `Promote`
deviendrait **inatteignable** et le survivant partirait en `Discard`. Le code
censé sauver le journal le détruirait — **silencieusement**, avec un `WARN` qui
annonce un nettoyage réussi.

La réconciliation précède aussi le `flushToDisk()` de fin de
`setPersistenceFs()`, lequel peut **relancer une rotation** donc recréer un
`.tmp` : on ne réconcilie pas par-dessus. L'ancrage est commenté au point exact
dans [`logger.cpp`](../../src/logger.cpp) et **testé par contrôle textuel du
source** ([`test/test_native_logger_logic/`](../../test/test_native_logger_logic/),
30 cas — dont un dépouilleur de commentaires qui empêche un appel *mentionné en
commentaire* de faire passer le test).

#### Niveaux de log, et le silence nominal

- **`WARNING` uniquement, jamais `error()`/`critical()`** : ces deux-là
  déclenchent le flush **synchrone** ([`logger.cpp:234`](../../src/logger.cpp:234)),
  qui rouvrirait `/system.log` **pendant** la réconciliation. C'est la règle
  générale de cette page — *tout code écrit pour éviter `fopen` en famine est
  contraint à `WARN`/`INFO`* — appliquée à un chemin qui, lui, manipule les
  fichiers eux-mêmes.
- **Pas de throttle** : au plus **une** ligne par démarrage, la rareté est
  structurelle.
- **Le cas nominal ne coûte rien** : aucune ligne, aucun octet de journal, aucune
  ouverture au-delà de la sonde. C'est ce qui rend acceptable un bloc exécuté à
  **chaque** boot dans un fichier dont la rétention se compte en heures.
- **L'abstention est MUETTE volontairement.** Sur refus d'admission, l'arène
  elle-même (`sLogArena`/`sLogSlots`, feature-129) est précisément la ressource
  sous pression, et `history.cpp` émet déjà son `WARN` de famine juste après.
  L'observabilité durable de ce refus est le compteur du `fs_guard`
  ([acces-fichiers.md](acces-fichiers.md#observabilité-des-refus)), pas une ligne
  de journal.

#### Ce que ce lot ne corrige PAS

`bug-099` avait été ouverte pour un motif **budgétaire** : deux blocs LittleFS
immobilisés sur une partition `history` de 64 Ko. Ce motif était **réel et
mesuré** — sous 64 Ko, le `.tmp` orphelin abaissait le plafond admissible de
`/history.json` de **8 176 o** et faisait refuser **73 tailles sur 124** dans la
bande réaliste 20-31 Ko. Sous le **layout v5**
([ADR-0068](../adr/0068-partitions-layout-v5-flash-8-mo.md), 256 Ko) : **zéro
refus**. **La conséquence budgétaire est dissoute** — ce que la requalification
du 2026-09-06 avait vu juste, et ce que la spec n'avait pas vu. Ce lot ne vaut
**que** par la **perte de journal**.

#### Ce qui n'est pas prouvé

- **Aucune exécution sur cible.** Ni `Promote` ni `Discard` n'ont tourné contre
  un LittleFS réel, et la **sémantique de `rename` quand la cible existe** n'est
  **jamais** testée.
- **L'inférence « échec de l'appel FS ⇒ le `.tmp` était absent » est une
  hypothèse** : un `rename` échouant pour corruption ou `ENOSPC` produirait le
  même silence. Acceptable parce qu'elle ne pilote que la **journalisation**,
  jamais un effet destructeur — et parce que la réconciliation est
  **idempotente** : un `rename` échoué laisse le `.tmp` intact, le démarrage
  suivant réessaie.
- **Dégradation bornée si la sonde se trompe** (relevé en revue) : si `exists()`
  répondait « absent » à tort — journal présent mais illisible en `"r"` — la
  promotion écraserait la **tête** du journal, jamais la **queue** (le `.tmp`
  porte les octets les plus récents). La perte porterait sur les lignes que la
  rotation allait de toute façon supprimer.

Module pur et découpage intention/fait :
[`src/logger_logic.h`](../../src/logger_logic.h) — `evaluateLogTmpRecovery()`
rend l'**intention** avant tout effet de bord, `classifyLogTmpOutcome()` rend le
**fait** à partir du code de retour FS. Décision complète, alternatives écartées
(effacement aveugle, sonde du `.tmp`, classe A, remplacement atomique renvoyé à
une spec de suite) : [ADR-0076](../adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md).

### 🔒 Un log `ERROR` depuis un chemin anti-`fopen` déclenche l'`abort()` qu'on prévient (v2.41.0)

**Trouvaille du lot B-3 de bug-080** ([ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md)). Le flush immédiat décrit ci-dessus n'est pas seulement « immédiat » : il est **synchrone, dans la tâche appelante**.

```
systemLogger.error(...)          logger.cpp:234  →  si ERROR ou CRITICAL
  → flushToDisk()                logger.cpp:557
    → _persistFs->open("/system.log", "a")
      → fopen()  →  __sfp  →  __retarget_lock_init_recursive
        → lock_init_generic (newlib locks.c:77)   ← malloc de 92 o (rectifié en v2.43.0 ; « ~88 o » était faux)
          → si le malloc échoue : abort()          ← AUCUN code de retour à tester
```

C'est exactement le chemin du PANIC du 29/08, et celui que la [garde 503 « fichiers statiques »](web-server.md#garde-503--fichiers-statiques--v2410-bug-080-lot-b-3-adr-0067) existe pour éviter.

> **Règle générale, pas particularité de B-3** : **tout code écrit pour éviter `fopen` en famine mémoire est contraint à `WARN` / `INFO`.** Émettre un `ERROR` ou un `CRITICAL` depuis un tel chemin y rouvre le crash par la porte du journal. S'y ajoute : **aucun log depuis la tâche `async_tcp`** — le drain se fait depuis la loop.

Corollaire noté pour la suite : `flushToDisk()` **lui-même** est un `fopen` abort-prone, et il n'est **pas** couvrable par un 503 (il n'y a pas de requête à refuser). Identifié, **hors périmètre** de B-3, lot ultérieur.

> ✅ **Fermé en v2.43.0** (bug-100, [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md)) — voir la section suivante. La règle « WARN/INFO uniquement depuis un chemin anti-`fopen` » reste **entièrement en vigueur** : elle est ce qui rend le correctif compatible avec lui-même.

### Toutes les ouvertures du journal sont gardées (v2.43.0, bug-100 / [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md))

`logger.cpp` ouvre `/system.log` (ou son `.tmp` de rotation) en **8 endroits**
(**7** jusqu'à la v2.48.0 — la sonde de `reconcileLogTmp()` est le huitième,
`bug-099`). Tous passent par `openIfHeapAllows()` / `existsIfHeapAllows()`
([`src/fs_guard.h`](../../src/fs_guard.h)) — plus aucun `fopen` nu.

**Sept des huit sites sont de CLASSE A (seuil 8 192)**, y compris celui du
marqueur de boot. Ce n'est pas une précaution : `flushToDisk()` s'exécute **sur
la tâche qui a émis le log**, il n'existe pas de « tâche du logger ». Une ligne
`ERROR` émise depuis un handler `async_tcp` ouvre le fichier **pendant** qu'une
réponse est en vol, donc avec des pbuf lwip alloués.

**Le huitième — la sonde de `reconcileLogTmp()`
([`logger.cpp:442`](../../src/logger.cpp:442)) — est de CLASSE B (seuil 5 120)**,
et c'est le seul du journal. Motif et invariant :
[ADR-0076](../adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md) — en
classe A on s'abstiendrait dans toute la bande 5 120–8 192 où le filesystem est
sain, et **au boot une abstention ne se rejoue jamais**.

#### Une seule mesure par flush, six ouvertures ensuite

`flushToDisk()` prend **une** admission
([`logger.cpp:602`](../../src/logger.cpp:602)) puis ouvre jusqu'à six fois avec
`kFsGuardAlreadyAdmitted` : append, repli `"w"`, relecture de taille, source de
rotation, `.tmp` en écriture, `.tmp` en relecture. **6 mesures deviennent 1** —
recompté au lot `bug-099` : ce chiffre-là est **inchangé**, le huitième site est
hors du flush.

Ce n'est pas une optimisation de confort : le parcours TLSF qui mesure le tas
coûte **d'autant plus cher que le tas est fragmenté**, c'est-à-dire exactement
dans le régime pour lequel la garde existe. Et la rotation est une séquence
**indissociable** — la re-mesurer à mi-parcours autoriserait un `.tmp` sans
pouvoir le relire.

Les **deux** sites hors du flush prennent **chacun leur propre** mesure :
[`logger.cpp:523`](../../src/logger.cpp:523) (marqueur `--- DÉMARRAGE ---` de
`setPersistenceFs()`, classe A) et [`logger.cpp:437-442`](../../src/logger.cpp:437)
(admission + sonde de `reconcileLogTmp()`, classe B — **une** admission pour la
sonde **et** l'appel `rename`/`remove` qui suit). Total : **3 admissions, 8
ouvertures**.

#### Ce que devient le journal sous famine (mis à jour, feature-129)

Sur refus d'admission, `flushToDisk()` **ne bouge pas `_flushCursor`** et
repousse `_lastFlushMs`. Avant feature-129, le buffer non flushé était
**réinséré** dans `_persistBuffer` (structure séparée, plafonnée à 100
entrées) ; cette réinsertion n'existe plus, parce que la ressource qu'elle
protégeait n'existe plus non plus — **l'arène EST désormais la file d'attente**
(condition C6). Conséquences, dans l'ordre :

1. **Aucune ligne n'est perdue tant que l'arène tient.** Le flush est
   **reporté**, le curseur reprendra exactement là au prochain appel. Sans le
   `_lastFlushMs = millis()`, `update()` rappellerait `flushToDisk()` **à la
   fréquence de la loop** pendant toute la famine.
2. **Au-delà, la perte est bornée par la profondeur de l'arène (200 entrées),
   pas par une file dédiée de 100.** Sous famine prolongée, ce sont les
   créneaux **les plus anciens** qui sont évincés — y compris potentiellement
   un `WARNING` pas encore flushé. C'est exactement ce que compte
   `log_persist_overrun` (C6, voir [« Arène circulaire »](#arène-circulaire-du-journal-feature-129)) : distinct des évictions RAM ordinaires (`log_arena_evictions`), il ne compte **que** les créneaux perdus pour `/system.log` alors qu'ils auraient dû y arriver.
3. **`ERROR`/`CRITICAL` restent couverts par le flush synchrone**, inchangé —
   la dégradation ci-dessus ne les concerne pas.
4. **Le refus n'est jamais silencieux**, mais son canal durable n'est **pas** le
   journal : c'est le compteur **`ldef`** du payload **retenu**
   `<base>/diagnostic`.

> ⚠️ **Le refus est annoncé en `Serial` uniquement**, jamais via `systemLogger`.
> Journaliser ici rappellerait `flushToDisk()` (récursion sur `ERROR`/`CRITICAL`)
> et rouvrirait le chemin même que l'on ferme :
> `[LOGGER] Famine memoire : flush disque differe`.

#### `setPersistenceFs()` arme **inconditionnellement**

`_persistEnabled = true` est posé **avant** l'ouverture gardée du marqueur de
démarrage. Si l'ouverture est refusée, seul le **marqueur** se perd — la
persistance reste armée, et le journal du boot sera écrit au premier flush qui
passera. C'est ce qui permet à `history.cpp` d'appeler `setPersistenceFs()`
**avant de sortir sur son tripwire** : on ne perd pas la trace de l'épisode que
l'on vient de détecter.

Sur échec, `Serial` seul : `[LOGGER] ERREUR: impossible d'ouvrir /system.log au démarrage`.

**Émetteurs ajoutés en v2.41.0** — garde 503 fichiers statiques, **2 lignes par épisode maximum**, drainés depuis la loop (`WebServerManager::drainLowHeapGuard()`) :

| Niveau | Message | Front |
|---|---|---|
| `WARN` | `Service fichiers suspendu (503) — maxBloc8=X o < 8192 o` | 1ᵉʳ refus drainé |
| `INFO` | `Service fichiers rétabli — N refus en T s` | 30 s sans refus **et** tas `>= 8192` |

Débit **nul en régime normal**, et borné à ~150 o par épisode en famine — contre plusieurs centaines de lignes si on journalisait par requête (une rafale de chargement de page produit 6 à 20 refus). C'est précisément la sur-journalisation qui a détruit la fenêtre forensique du 15/08. Un **WARN sans son INFO** signifie que l'épisode est toujours en cours, ou qu'un reboot l'a interrompu — auquel cas la bannière de démarrage le clôt.

### Rétention réelle du journal persistant (v2.20.2)

`kMaxLogFileBytes` et `kRotateKeepBytes` sont **inchangés** : la rétention ne se règle pas en agrandissant le fichier (le budget de la partition l'interdit), elle se règle en **réduisant le débit des émetteurs périodiques**. Le fichier oscillant entre 6 144 et 8 192 octets, la fenêtre couverte vaut `6144…8192 ÷ débit`.

| | Débit des traces périodiques | Rétention réelle du fichier `/system.log` |
|---|---|---|
| **Avant** (2.20.0/2.20.1) | **≈ 14,1 Ko/h** | **26 à 34 minutes** |
| **Après** (2.20.2) | **≈ 0,41 Ko/h** (−97 %) | **14,6 à 19,5 heures** |

Ce n'est pas une optimisation de confort : **le post-mortem du crash de production du 15/08 (16:43) a été détruit par notre propre journalisation** avant d'avoir pu être lu. Deux émetteurs produisaient à eux seuls la quasi-totalité du flot, dont ~90 % la **même ligne**.

Cadences des émetteurs périodiques qui écrivent dans `/system.log` :

| Trace | Avant | Après | Où |
|---|---|---|---|
| `WARN Alloc heap échouée …` | throttle **fixe** 30 s (≈ 120 lignes/h) | **backoff** immédiat → 30 s → 5 min → 30 min, retour au palier initial après 1 h sans échec | [oom-guard.md](oom-guard.md#backoff-du-warn-déchec--restaurer-la-fenêtre-forensique-v2202) |
| `INFO Heap 8bits: …` | 5 min, ligne longue (partie `interne:` incluse) | **30 min**, ligne raccourcie (`interne:` **retiré** — valeur figée, sans information) | [system-health.md](system-health.md#trace-de-calibrage-des-seuils-mémoire-v2190) |

**Deux émetteurs périodiques ajoutés en v2.49.0** (`feature-122`, lot C-1a de `bug-080`) — `INFO Heap masq boot: …` et `INFO Heap masq fen: …`, à la **même cadence de 30 min** que `Heap 8bits:` (`kHeap8LogIntervalMs`), lignes de **142** et **136** caractères au pire cas à l'origine (tampons de pile `char[160]`). **Depuis la v2.51.0** (lot B2, champ `b804=`), `ligne3` porte un tampon `char[176]` et son pire cas mesuré passe à **163** caractères — `ligne2` est inchangée :

| Régime | Débit ajouté | Motif |
|---|---|---|
| Toutes conditions (deux lignes par période de 30 min) | **≈ 0,55 Ko/h** | ces deux lignes sont le **chemin de dernier recours** en pleine famine : `snprintf` sur pile, **aucun bloc contigu du tas requis**, là où `GET /get-system-info?hf=1` réclame **~1,9 Ko** contigus (v2.51.0 ; ~1,7 Ko avant le lot B2). Elles portent les **planchers** et le contexte du creux, pas un instantané. Voir [system-health.md](system-health.md#les-deux-lignes-de-journal-agrégées) |

> ⚠️ **La ligne `Heap 8bits:` n'a PAS été élargie** pour les accueillir : son tampon est à ~117 caractères sur 128 au pire cas, et c'est la **seule série longue** du projet (152 relevés historiques). Un champ de plus l'aurait tronquée silencieusement. Même précaution sur les lignes neuves : leur pire cas ne tient que parce que les octets de contexte sont des `uint8_t` — élargis à 32 bits, `ligne2` passerait à 167 octets et **perdrait le champ `tem=`**, le plus à droite, c'est-à-dire le témoin. Une sentinelle de test verrouille ces types.

> **Règle générale** : avant d'ajouter une trace **périodique** persistée, chiffrer son débit en Ko/h et vérifier la rétention résultante. Une trace de 100 octets toutes les 30 s consomme **à elle seule** 12 Ko/h, soit plus que la taille totale du fichier — elle rend le journal inutilisable pour tout post-mortem.

### Dimensionner un tampon en octets UTF-8, jamais en caractères visés

Retour d'expérience du 2026-09-19 (feature-131, sonde `maxBloc8 min` de
[`history.cpp`](../../src/history.cpp), voir [history.md](history.md#sonde-f1--mesure-sur-le-terrain-et-défaut-de-dimensionnement-trouvé-en-production-2026-09-19)) :
un tampon `char[64]` a été dimensionné en comptant les caractères du préfixe
littéral, pas ses octets. Le préfixe pesait **59 octets** en UTF-8 (chaque
accent — l'« é » de « écriture » — coûte **2 octets**, pas 1) : `snprintf` ne
laissait plus que 4 chiffres au nombre attendu, et le suffixe (« o ») partait
avec eux, sans laisser d'indice visible de la troncature dans la ligne
produite.

**Un diagnostic qui perd des chiffres en silence est pire que pas de
diagnostic** : il fournit un nombre faux d'apparence plausible, qui peut faire
conclure l'inverse de la vérité (ici, `5528` a été lu — à tort — comme une
chute catastrophique de contiguïté). Deux règles à appliquer à tout nouveau
tampon de log formaté (`snprintf` sur `char[]`) :

1. Compter le **littéral en octets réels**, pas en caractères visuellement
   comptés — tout accent, guillemet typographique ou symbole non-ASCII du
   message coûte plus d'un octet.
2. Laisser une marge large pour la partie variable (au moins **dix fois** la
   largeur décimale du type, ex. `uint32` → 10 chiffres → marge de 100) plutôt
   que d'ajuster au plus juste : le gain mémoire d'un tampon serré n'a aucune
   commune mesure avec le risque de lire un nombre faux comme s'il était bon.

**Émetteur conditionnel ajouté en v2.20.5** — `WARN [Dosage pH|ORP] démarrage décidé mais AUCUN débit commandé …`, throttle **5 min** par grandeur (`kDosingNoFlowWarnIntervalMs`), ligne ≈ 180 octets :

| Régime | Débit ajouté | Effet sur la rétention |
|---|---|---|
| Nominal (une occurrence au démarrage de chaque cycle, ~24 cycles/j par grandeur) | **≈ 0,36 Ko/h** | 14,6-19,5 h → **≈ 8-10 h** |
| Pathologique (régulation décidée en continu sans débit, pH **et** ORP saturant le throttle) | **≈ 4,3 Ko/h** | **≈ 1,4-1,9 h** |

Le régime pathologique est **le seul cas où ce WARN doit être vu en rafale** ; il consomme alors la rétention, mais c'est précisément la fenêtre où on veut la trace. Le régime nominal reste sous le plafond du budget. Détail du sens du message : [pump-controller.md](pump-controller.md#warn--démarrage-décidé-mais-aucun-débit-commandé-). Deux états ne nourrissent pas ce WARN : le refus de **gate de caractérisation** (critical dédié ci-dessous) et le refus de **plan** (v2.22.0 : entrées invalides fail-closed uniquement — l'ex-refus de **seuil C** n'existe plus, le régime sous-plancher maintient la séance ouverte et ne peut plus produire ce WARN, [ADR-0046](../adr/0046-accumulateur-demande-sous-pouls-minimal.md)). Sans cette exclusion, le WARN sortirait en boucle sur un régime sain (inondation type 2.20.2).

**Émetteurs de l'ordonnanceur d'impulsions ajoutés en v2.21.0** ([ADR-0043](../adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)) — throttle commun `kPulseRefusalLogIntervalMs = 1 800 000` ms (**30 min**, compteur indépendant par branche et par grandeur) : ces refus décrivent des **états de configuration persistants** — 5 min les ferait tourner le journal, 30 min les rend visibles dans la journée (leçon 2.20.2) :

| Niveau | Message (symétrique ORP ; variantes `[Scheduled]` / `[Scheduled ORP]` avec « dosage programmé BLOQUÉ ») | Sens |
|---|---|---|
| `CRITICAL` | `[Dosage pH] Pompe non caractérisée (duty calibré=X, amplitude=Y, débit=Z mL/min) — dosage automatique BLOQUÉ (pesée au banc requise)` | Gate de caractérisation fail-closed : aucune pesée saisie, débit implausible, ou amplitude ≠ duty calibré |
| `CRITICAL` | `[Dosage pH] Amplitude pompe (Y/255) sous le plancher de rotation (98) — dosage automatique BLOQUÉ (relever la puissance pompe et recalibrer)` | Condition B2 : jamais dégrader en silence sous le seuil de rotation |

> **v2.22.0 (feature-073, [ADR-0046](../adr/0046-accumulateur-demande-sous-pouls-minimal.md))** : l'`INFO` « `Consigne X mL/min sous le pouls minimal … — injection différée (seuil C)` » est **supprimé** (la demande sous le plancher est désormais accumulée, plus refusée). Remplacé par `INFO [Dosage pH|ORP] Dosage par accumulation : impulsion minimale de N s (X mL cumulés, résidu Y mL)` — émis **une seule fois par séance** (premier tir d'accumulateur, flag `_carryPulseLogged` réarmé à la fermeture de séance), pas de throttle temporel : le débit est borné par la cadence des séances (pause mélange 15-20 min). Voir [pump-controller.md](pump-controller.md#accumulateur-sous-le-pouls-minimal-feature-073-v2220).
>
> **v2.25.3 ([ADR-0051](../adr/0051-pouls-minimal-double-rendement-avant-comptage.md))** — le `N s` de ce message est dérivé de `kPulseMinMs` : il affiche désormais **`10 s`** (contre `5 s` jusqu'en 2.25.2). Un filtre de journal ou une recherche portant sur la chaîne « impulsion minimale de 5 s » ne matche plus. Volume de journal **réduit** : les impulsions étant deux fois moins fréquentes, la cadence des séances — donc celle de ce log — diminue.

> **v2.25.1** — la cause de refus de dosage est journalisée **edge-triggered sur changement de texte** (`logRefusalOnce()`, `INFO [Dosage pH|ORP] Refus : …`, comportement inchangé). La **déconflation des libellés** subdivise cependant l'ancien libellé unique « filtre capteur non prêt (warmup / EZO injoignable) » en quatre : au démarrage, une séquence porte fermée → périmé → remplissage produit désormais **jusqu'à 3 lignes** au lieu d'1 (~70 o chacune), une fois par démarrage — impact rétention négligeable, et c'est exactement l'information qui manquait pour diagnostiquer un arrêt de régulation. Nouvel émetteur : `WARNING Alerte: ph_dose_sensor_fault - [Dosage pH] Refus : … — régulation pH à l'arrêt depuis N s : vérifier la sonde` (via `publishAlert()`, **une seule ligne par épisode** de panne continue, après `kDoseSensorFaultAlertDelayMs` = 60 s). Le `systemLogger.warning()` initialement ajouté à côté a été **retiré en revue** : `publishAlert()` journalise déjà le message, y compris broker déconnecté — deux lignes quasi identiques par épisode dans une partition de 64 Ko. Voir [pump-controller.md](pump-controller.md#alerte-de-panne-de-mesure-v2251).

Émetteurs ponctuels associés (non throttlés, événementiels) : `INFO Pesée pompe N enregistrée : duty X, débit Y ml/min` / `WARN Calibration pompe N effacée — pompe non caractérisée (dosage auto refusé)` / `WARN Pesée pompe N refusée : …` (route `/save-config`) ; `WARN [Config] pN_pulse_flow NVS implausible (…) — pompe N non caractérisée` (assainissement au chargement) ; `CRITICAL [Sécurité] Injection pH|ORP manuelle refusée : débit non caractérisé (pesée requise)` (409 `flow_uncharacterized`) ; `INFO Arrêt dosage pH|ORP (auto): session=Xs pompe-ON=Ys vol≈Z mL total jour=…` (nouveau format du log d'arrêt de session).

**Émetteurs de l'assistant de mesure du débit ajoutés en v2.24.0 (feature-075)** — tous **événementiels, non throttlés**, conformes à la règle générale : aucun n'est périodique, leur débit est borné par l'interaction utilisateur (session limitée à 15 min + 3 purges max, ≤ ~10 lignes par session complète — impact rétention négligeable). La suspension du dosage est journalisée **one-shot par transition** (jamais par tour de loop — état précédent `_flowAssistSuspendPrev` loopTask-only) :

| Niveau | Message | Émetteur |
|---|---|---|
| `INFO` | `Dosage automatique suspendu (assistant de mesure du débit)` / `Dosage automatique rétabli (fin de l'assistant …)` | transition du flag dans `PumpController::update()` |
| `INFO` | `[Assistant débit] Session ouverte (pompe N) — dosage automatique suspendu` / `Session fermée (pompe N) — dosage automatique rétabli` | handlers `/flow-assist/start` / `/exit` |
| `INFO` | `[Assistant débit] Purge\|Mesure démarrée : pompe N, X s, duty D, crédit majorant Y mL` | `/flow-assist/start` |
| `INFO` | `[Assistant débit] Marche arrêtée par anticipation (pompe N, X s écoulées, crédit Y mL au majorant)` / `Marche interrompue (perte de filtration) — crédit Y mL au majorant (X s écoulées)` | arrêt anticipé / abandon serveur |
| `WARN` | `[Assistant débit] Session expirée (15 min sans interaction) — dosage automatique rétabli` | timeout serveur (`updateManualInject`, loopTask) |

Voir [pump-controller.md](pump-controller.md#suspension--assistant-de-mesure-du-débit--feature-075-v2240).

> ⚠️ **Paragraphe obsolète, corrigé en feature-129.** `_persistBuffer`
> (`std::vector<String>`, plafonné à 100 entrées) **n'existe plus** : la ligne
> persistée est désormais **dérivée directement de l'arène** au moment du
> flush (`sFlushLine`, voir [« Arène circulaire »](#arène-circulaire-du-journal-feature-129)).

**Logs pré-persistance — rattrapés depuis feature-129 (condition C7).** Avant ce
lot, tant que `setPersistenceFs()` n'avait pas été appelé, les entrées émises
n'étaient **jamais** accumulées pour `/system.log` — perdues pour la
persistance dès que le ring RAM tournait. **Ce n'est plus le cas** :
`_persistEnableSeq` est figé au nombre d'entrées déjà poussées au moment de
l'activation, et `_flushCursor` réarmé à 0 — le flush qui suit **rattrape tout
le démarrage**, filtré à `WARNING`/`ERROR`/`CRITICAL` (`logShouldPersist()`,
[`src/log_arena_logic.h`](../../src/log_arena_logic.h)). Sans ce filtre, un
boot verse plus de `kMaxLogFileBytes` (8 192 o) d'`INFO` et déclenche une
rotation immédiate qui emporterait les lignes de sécurité qu'on vient
d'écrire.

**Bénéficiaire identifié** : le `CRITICAL` de [`config.cpp`](../../src/config.cpp)
— « Régulation pH en MANUEL suite à une bascule automatique non acquittée » —
émis pendant le chargement de la configuration, **avant** que
`setPersistenceFs()` n'ait pu être appelé. C'était la **seule trace persistante
possible** de cet état de sécurité au redémarrage ; elle atteint désormais
`/system.log`. C'est le changement visible utilisateur le plus significatif de
`feature-129` — voir [ADR-0082](../adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md).

C'est notamment aussi le cas des logs du **disjoncteur de boot-loop history**
(v2.10.0), qui s'exécute dans `HistoryManager::begin()` **avant** l'activation
de la persistance (c'est précisément elle qu'il protège) : le CRIT « partition
effacée automatiquement » est visible dans la page Logs (RAM) et sur la série,
et — depuis feature-129 — dans `/system.log` après le premier flush qui suit
`setPersistenceFs()`. Voir [system-health.md](system-health.md#disjoncteur-de-boot-loop-de-linit-history-v2100).

### La découverte HA n'est plus le premier poste du journal (feature-109, 06/09/2026)

`publishDiscovery()` émettait **54 lignes `INFO` de ~98 o par rafale, ≈ 5 292 o**, soit **65 % d'un fichier de 8 192 o** et **86 % des 6 144 o que la rotation conserve** — plus 54 des 100 entrées du tampon RAM `_persistBuffer`. Ce n'est pas un émetteur *périodique* mais un émetteur **par reconnexion MQTT**, et 4 reconnexions ont été observées en 28 minutes le 06/09. Une rafale de synthèse pèse désormais **79 o** : **×67 sur ce poste**. Format et mécanique : [mqtt-manager.md](mqtt-manager.md#journalisation-dune-rafale--une-ligne-jamais-54-feature-109-discovery_report_logich).

| | Découverte HA | Autres sources | **Total** |
|---|---|---|---|
| **Avant** | 756 o/min | 157 o/min | **913 o/min** |
| **Après** | **11 o/min** | 157 o/min | **168 o/min** |

Débit total **divisé par 5,4** ; fenêtre forensique estimée à **≈ 2 h 30** contre **28 minutes** mesurées le 06/09. Ce chiffre est une **estimation de conception** : le critère réel — « le journal couvre des heures » — se mesure sur l'appareil après déploiement, pas en test natif.

> ⚠️ **93 % du débit résiduel vient désormais des AUTRES sources** (157 o/min sur 168). Elles étaient minoritaires, elles sont devenues **dominantes** : la spec de suivi annoncée en « Hors périmètre » de `feature-109` n'est **plus optionnelle**. Tout gain supplémentaire sur la rétention passe par elles, et par elles seules.

#### Émetteur non authentifié : le motif du throttle de calibration (v2.46.1)

La garde d'authentification du corps des routes `/calibrate_*` ([sensors.md](sensors.md#authentification-du-corps-des-routes-de-calibration-v2461)) journalise les tentatives refusées. Ce WARN est throttlé à **1 par 60 s et par route** (`kCalibrationAuthWarnThrottleMs`), et le throttle **fait partie du correctif**, pas de son confort — pour une raison qui est celle de toute cette page :

- **Le quota HTTP ne borne pas cet émetteur.** `checkRateLimit()` est appelé par `checkAuth()` dans le handler de **requête**, donc **après** le handler de corps. Le corps s'exécute pour **chaque** requête reçue, quota dépassé ou non : un émetteur qui journalise depuis un handler de corps est, par construction, **non borné**.
- **Chiffrage.** Une ligne persistée de ce type pèse **~230 o**. Aux valeurs en vigueur — fichier `kMaxLogFileBytes` = **8 192 o**, `kRotateKeepBytes` = **6 144 o** conservés à la rotation — un émetteur non throttlé purge le journal en **~27 requêtes**, soit **~13 s** au seul quota autorisé de 120 req/min. La fenêtre forensique de **≈ 2 h 30** gagnée par `feature-109`, et dont `bug-080` dépend pour tracer un redémarrage, serait **effaçable à volonté** par un client anonyme sur le réseau local.
- **Coût résiduel avec throttle** : ≤ 1 ligne/min/route, et le **chemin muet n'alloue rien** (sortie avant toute concaténation `String`) — condition non négociable puisque ce chemin est atteignable sans authentification pendant une famine mémoire active.

> ⚠️ **L'émetteur non borné PRÉEXISTE à ce cycle.** `AuthManager::sendAuthRequired()` ([`auth.cpp:231`](../../src/auth.cpp:231)) émet un `WARNING "Accès non autorisé depuis <ip> vers <url>"` **sans throttle**, sur **toute** route protégée, depuis toujours. Le cycle 2.46.1 ne l'a **pas créé** : sans throttle, sa ligne de ~230 o serait venue s'ajouter à ce WARN existant sur trois routes, **multipliant par ~2,7** le coût journal d'une requête non authentifiée. Le throttle annule cet ajout ; il ne borne pas l'émetteur préexistant, qui reste le vrai poste. Le traitement de cet émetteur préexistant fait l'objet d'une **spec dédiée ouverte**, [`bug-112`](../../specs/features/todo/bug-112-journal-non-borne-sur-route-protegee.md) — son périmètre n'est pas arrêté ici.

> ℹ️ **Troisième site du même throttle (v2.46.2).** La garde d'authentification du handler d'**upload** de `POST /update` ([ota-manager.md](ota-manager.md#authentification-du-handler-dupload-v2462), `bug-117`) réutilise **la même constante**, avec sa propre statique locale : ≤ 1 `WARNING`/60 s, et **une seule fois par upload** (au premier fragment, jamais aux ~1 000 suivants). Le raisonnement ci-dessus s'applique tel quel — **y compris sa limite** : sur cette route aussi, `sendAuthRequired()` émet un WARN non throttlé par requête, donc la fenêtre forensique reste purgeable sans authentification. Le throttle réduit la pression, il ne ferme pas le vecteur ; c'est `bug-112` qui traite l'émetteur générique.

> 📋 **Écart de chiffrage à corriger dans le code.** Le commentaire de `kCalibrationAuthWarnThrottleMs` ([`constants.h:153`](../../src/constants.h:153)) raisonne sur « 24 Ko conservés » et « ~105 requêtes » : ce sont les tailles **historiques** du journal (32 Ko de fichier, antérieures à l'[ADR-0033](../adr/0033-budget-espace-partition-history.md)). Aux valeurs réelles (8 192 / 6 144 o), la purge est **~4 fois plus rapide** que ce que dit le commentaire. La conclusion n'est pas affaiblie, elle est renforcée — seul le chiffre du commentaire est à reprendre.

#### Coût RÉSIDUEL d'un détail passé en DEBUG (feature-109)

Basculer un émetteur verbeux de `INFO` vers `DEBUG` épargne le **fichier** — et **seulement** le fichier. À écrire franchement, parce que le raccourci « DEBUG = gratuit » est faux :

| Chemin | Épargné par `DEBUG` ? | Pourquoi |
|---|---|---|
| `/system.log` (partition `history`) | ✅ **Oui** | `logShouldPersist()` ([`log_arena_logic.cpp`](../../src/log_arena_logic.cpp)) exclut `DEBUG` — une entrée `DEBUG` n'est **jamais** copiée dans `sFlushLine` au flush (feature-129 : plus de `_persistBuffer` intermédiaire, filtrage direct depuis l'arène). **C'est le gain qui compte** pour la fenêtre forensique. |
| Buffer circulaire RAM (`kMaxLogEntries = 200`) | ❌ **Non** | L'entrée est poussée comme n'importe quelle autre. 54 lignes `DEBUG` évincent toujours 54 entrées. |
| Push WebSocket (page Logs) | ❌ **Non** | Le callback est appelé pour tous les niveaux. |
| Sortie Serial | ❌ **Non** | Émise même quand le mutex n'a pas pu être pris. |

Le seul rempart sur ces trois derniers chemins est le toggle `authCfg.debugLogsEnabled`, **à `false` par défaut** : en production le détail des 54 entités n'est effectivement produit nulle part, mais **activer le switch « Logs DEBUG activés » rétablit intégralement la pression sur le ring RAM, le WebSocket et la console** — pas sur le fichier. Une session de diagnostic prolongée le paie.

**Placement de la garde `DEBUG` — le piège.** `Logger::debug()` court-circuite en première ligne (early return), ce qui suffit à éviter le *log* mais **pas** la construction de ses arguments : `systemLogger.debug("Discovery " + topic + …)` concatène **avant** d'appeler. La garde doit donc être posée **au site d'appel, autour de la concaténation** — sinon les 54 allocations `String` sont payées à chaque rafale, DEBUG désactivé compris. C'est le patron retenu dans `publishDiscovery()`.

## Push WebSocket

`setLogCallback(cb)` permet à `ws_manager` de s'abonner. À chaque `log()`, le callback est invoqué **immédiatement, hors mutex**, avec un `LogRecord` dont `message` pointe dans le tampon de l'**appelant** de `log()` — jamais dans l'arène (condition C3, feature-129) — ce qui déclenche un push WS `{type: "log", timestamp, level, message}` ([`WsManager::broadcastLog()`](../../src/ws_manager.cpp), détail : [ws-manager.md](ws-manager.md#broadcastlog-et-le-type-logrecord-feature-129)).

⚠️ **Le flux WebSocket porte le message INTÉGRAL, jamais tronqué, même quand la copie stockée dans l'arène l'a été.** `broadcastLog()` lit le tampon de l'appelant (le texte complet passé à `log()`), pas la copie de `sLogArena` — celle-là seule peut être coupée à `kLogMessageMaxBytes`. C'est délibéré : le drapeau de troncature (`trunc`) ne concerne que `/get-logs`, qui lit l'arène. Afficher « tronqué » sur un refus d'injection complet ferait chercher une information qui est déjà là.

## Émission console

Chaque `log()` se termine par l'émission de la ligne sur la série (UART0), format `[NIVEAU] message` + CRLF — **inchangé** depuis toujours côté contenu.

**Depuis la v2.10.0** (correctif `bug-spam-sensor-problem-serial`), la ligne est composée dans un **buffer pile** (`kLoggerConsoleLineMax = 512`, [`constants.h`](../../src/constants.h)) puis émise en **un seul** `Serial.write()` :

- L'ancienne implémentation faisait **4 `Serial.print` séparés, hors mutex** : chaque `print` est atomique au niveau HAL (`UART_MUTEX_LOCK`), mais la **séquence** pouvait s'entrelacer entre tâches (mqttTask core 0 / loopTask core 1) — lignes mélangées à granularité ligne, observé sur bench.
- Un `write` unique étant atomique au HAL, l'entrelacement inter-tâches est supprimé **sans mutex supplémentaire** ni risque de blocage.
- **Troncature** : si la ligne dépasse 512 octets, elle est tronquée avec **fin de ligne CRLF garantie** (les 2 derniers octets émis sont toujours `\r\n`).
- **Buffer pile** (pas de heap) : la console reste fonctionnelle même sous heap critique.

> Les troncatures **intra-chaîne** et répétitions de fragments observées lors de l'incident du 2026-07-22 relevaient, elles, d'un **rejeu matériel/driver du FIFO TX UART0** (bench, ESP32 hors PCB) — pas d'un défaut logiciel. Analyse complète : `specs/features/todo/bug-spam-sensor-problem-serial.md`.

## Concurrence

Mutex FreeRTOS (`_mutex`) protège **tout l'état de l'arène** (feature-129) :
- `sLogArena` / `sLogSlots` (lecture/écriture concurrente entre tâches et handlers web)
- Les index `_arenaHead`/`_arenaTail`/`_arenaUsed`/`_slotOldestIdx`/`_slotWriteIdx`/`_slotLiveCount`/`_nextSeq`/`_flushCursor`

`sFlushLine`, lui, n'est **pas** protégé par ce mutex : c'est la propriété **exclusive** du flusher (`flushToDisk()`), garantie par le drapeau `_flushInFlight` (anti-chevauchement) plutôt que par le mutex du journal.

### Timeouts mutex bornés (feature-027, v2.11.1)

Les prises de mutex du logger sont bornées par `kLoggerMutexTimeoutMs = 100 ms` ([`constants.h`](../../src/constants.h)) — valeur courte car toutes les sections critiques du logger sont en **RAM pure** (l'arène circulaire), jamais d'I/O sous mutex.

**Politique silencieuse anti-récursion** : les chemins d'échec du logger n'appellent **JAMAIS** `systemLogger` (un log qui échoue à logger son propre échec = récursion). Seul un `Serial.printf` de secours existe sur le chemin de réinsertion de `flushToDisk()`.

Politique d'échec par site :

| Site | Sur timeout |
|---|---|
| `log()` | Entrée perdue (RAM + persistance) + `_droppedLogs++`. Le push WS n'a pas lieu (callback non capturé) |
| `forEachRecentLog()` | Aucune itération (le `visitor` n'est pas appelé — le client réessaiera) |
| `copyEntryByIndex()` | Retourne `false` — l'export chunked `/download-logs` termine sa section RAM sans erreur (best-effort) |
| `clear()` | No-op — rien n'est perdu, l'utilisateur peut re-cliquer. **Ne compte pas dans `_droppedLogs`** (corrigé post-revue : le compteur ne trace que des *entrées* perdues) |
| `clearAll()` | Return **avant** toute modification — le fichier `/system.log` n'est pas supprimé si la RAM n'a pas pu être vidée (pas d'état incohérent) |
| `getLogCount()` | Retourne `0` |
| `flushToDisk()` — lecture du prochain créneau à flusher (`_flushOnePass`) | Sort de la boucle de drain : le curseur pointe déjà sur la suite, rien n'est perdu, on retentera au prochain passage |

**`_droppedLogs`** (`uint32_t`, membre privé) : compteur diagnostic des entrées perdues sur timeout. Il est **volontairement write-only** — aucun endpoint, aucun topic MQTT, aucun getter ne l'expose (mineure consignée en revue, assumée) : l'exposer créerait une API pour un événement qui ne doit jamais se produire en nominal ; il reste lisible au debugger / dans un coredump si un diagnostic de contention devient nécessaire.

## Endpoint HTTP

| Action | Endpoint | Auth |
|--------|----------|------|
| Lire les logs récents (paginé v2.10.4 — `?since=`, `truncated`) | `GET /get-logs` | WRITE |
| Télécharger les logs (fichier persistant + RAM, flux chunked v2.10.4) | `GET /download-logs` | WRITE |
| Effacer les logs côté ESP32 (RAM + fichier persistant) | `DELETE /logs` | WRITE |

`DELETE /logs` invoque `systemLogger.clearAll()` puis émet un INFO `Logs effacés (RAM + fichier persistant)` pour tracer l'action. Réponse JSON : `{"success": true}`.

## Logs WiFi

`setupWiFi()` ([`src/main.cpp`](../../src/main.cpp)) installe un handler `WiFi.onEvent()` après `WiFi.setSleep(false)` qui émet les messages suivants via `systemLogger` :

| Événement Arduino | Niveau | Message |
|---|---|---|
| `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` | WARN | `WiFi déconnecté (reason=N)` |
| `ARDUINO_EVENT_WIFI_STA_CONNECTED` | INFO | `WiFi associé à l'AP` |
| `ARDUINO_EVENT_WIFI_STA_GOT_IP` | INFO | `WiFi IP obtenue: x.x.x.x RSSI=-NNdBm` |
| `ARDUINO_EVENT_WIFI_STA_LOST_IP` | WARN | `WiFi IP perdue` |

Ces traces servent au diagnostic des blackouts réseau (ping qui ne répond plus, app web injoignable). La séquence observée renseigne sur la cause :

- **Drop Wi-Fi côté AP/routeur** : `DISCONNECTED → CONNECTED → GOT_IP`.
- **DHCP renew** : `LOST_IP → GOT_IP` (l'IP peut être identique ou différente).
- **Problème côté broker MQTT, firewall ou applicatif** : aucun log WiFi pendant le blackout, mais `mqtt_manager` enchaîne ses tentatives de reconnexion.

### Codes de raison Wi-Fi courants

Le `reason=N` du message `WiFi déconnecté` correspond aux constantes `WIFI_REASON_*` de l'IDF :

| Code | Constante | Cause typique |
|---|---|---|
| 4 | `AUTH_EXPIRE` | Timeout authentification |
| 8 | `ASSOC_LEAVE` | ESP32 a quitté volontairement (rare) |
| 15 | `4WAY_HANDSHAKE_TIMEOUT` | Problème WPA |
| 200, 201, 202 | `BEACON_TIMEOUT`, `NO_AP_FOUND`, `AUTH_FAIL` | RSSI faible / AP perdu |

Liste complète : [`esp_wifi_types.h`](https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_wifi_types.h).

### Ré-annonce mDNS sur changement d'IP

Depuis la v2.8.1, `poolcontroller.local` est ré-annoncé sur **tout changement d'IP**, pas seulement lors d'une reconnexion manuelle. Le handler `ARDUINO_EVENT_WIFI_STA_GOT_IP` compare la nouvelle IP à la dernière annoncée (`mdnsLastAnnouncedIp`) ; si elle diffère, il pose le drapeau `mdnsReannounceRequested`. Ce drapeau est consommé dans `checkSystemHealth()` (contexte `loopTask`, car MDNS/lwIP n'est pas thread-safe vis-à-vis de la tâche événements WiFi), qui exécute `MDNS.end()` + `begin()` + `addService(http)`. Une déconnexion (`STA_DISCONNECTED`) invalide l'IP mémorisée.

Ceci couvre le **roaming CPL/mesh sans perte de WiFi** : auparavant, la ré-annonce n'avait lieu que sur reconnexion, si bien que `poolcontroller.local` pouvait pointer vers une IP périmée (appareil injoignable en HTTP alors que MQTT continuait de fonctionner).

**Debounce (v2.9.1)** : la ré-annonce n'est exécutée que si l'IP a **réellement** changé (`currentIp != mdnsLastAnnouncedIp`) **et** au plus une fois toutes les `kMdnsReannounceMinIntervalMs = 30 s`. Objectif : supprimer le churn `MDNS.end()/begin()` provoqué par les oscillations d'IP du **double NAT CPL**, qui était coûteux et **accélérait la fragmentation du tas** (cf. gel 2.9.1, [system-health.md](system-health.md)). Les oscillations ignorées sont tracées en DEBUG.

| Message | Niveau | Quand |
|---|---|---|
| `mDNS ré-annoncé sur x.x.x.x (changement d'IP)` | INFO | Après ré-annonce effective dans `checkSystemHealth()` |
| `Échec ré-annonce mDNS` | WARN | `MDNS.begin()` a échoué lors de la ré-annonce |
| `Ré-annonce mDNS débouncée (IP=x.x.x.x)` | DEBUG | Oscillation d'IP ignorée (IP inchangée ou intervalle < 30 s) |

### Instrumentation du tas (v2.9.1 — trace INFO désactivée en 2.10.2)

`checkSystemHealth()` échantillonne le tas à **chaque** tick (60 s) pour l'auto-guérison (échantillonnage toujours actif) :

| Message | Niveau | Quand |
|---|---|---|
| `Heap: libre=<n> maxBloc=<n> minLibre=<n>` | INFO | **Désactivé depuis 2.10.2** — bloc mis en commentaire dans `checkSystemHealth()` (`main.cpp:782`), ré-activable en le décommentant. Émettait toutes les `kHeapLogIntervalMs` (5 min) ou sur chute de `maxBloc` > `kHeapLogDropBytes` (8 Ko) |
| `Heap fragmenté sous seuil d'alerte: maxBloc=<n> bytes` | WARN | **Inchangé** — premier franchissement à la baisse de `kLowHeapWarnBytes` (20 Ko), suivi d'un `flushToDisk()` **one-shot** |

Le flush one-shot capture la descente sur disque même si le redémarrage d'auto-guérison n'est jamais atteint ; il est ré-armé quand `maxBloc` repasse au-dessus du seuil. Mécanisme complet (auto-guérison / mode maintenance) : [system-health.md](system-health.md).

> Le getter `WsManager::clientCount()` (introduit en v2.8.1) reste disponible. Le log de diagnostic périodique `[Diag]` qui l'utilisait a été **retiré en v2.8.2** (temporaire, il avait servi au diagnostic du crash `/logs`).

## Logs MQTT

`MqttManager` ([`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp)) émet les messages suivants pour le diagnostic des reconnexions :

| Message | Niveau | Quand |
|---|---|---|
| `MQTT déconnecté détecté — état=N` | WARN | Edge-triggered — au **front** de la transition `connecté → déconnecté`. `N` = `PubSubClient::state()` |
| `Tentative connexion MQTT (délai=Ns)...` | INFO | Avant chaque tentative `connect()` (rate-limit 5 s mini, jusqu'à 120 s en backoff) |
| `MQTT connecté !` | INFO | Connexion réussie |
| `MQTT échec DNS pour 'host' — prochaine tentative dans Ns` | ERROR | `WiFi.hostByName()` a échoué (hostname uniquement, pas IP littérale) |
| `MQTT échec, code=N — prochaine tentative dans Ns` | ERROR | Handshake CONNECT applicatif refusé (auth, protocole, etc.) |
| `MQTT déconnecté` | INFO | Appel explicite à `disconnect()` |

Le log `MQTT déconnecté détecté` permet de cibler immédiatement la cause d'une perte de connexion. Voir [`mqtt-manager.md`](mqtt-manager.md) section « Diagnostic — Codes d'état PubSubClient » pour la table des codes.

## Usage typique

```cpp
systemLogger.info("Filtration started");
systemLogger.warning("pH out of range: " + String(ph, 2));
systemLogger.error("I2C timeout on ADS1115");
systemLogger.critical("Heap below 10KB, forcing restart");
```

## Cas limites

- **Mutex non initialisé** : `log()` avant `begin()` est silencieusement ignoré.
- **Partition history pleine** : `flushToDisk()` échoue, les logs restent dans l'arène jusqu'à rotation.
- **Arène saturée sans flush** (feature-129) : au-delà de `kMaxLogEntries = 200` entrées ou `kLogArenaBytes = 16 384` octets, les créneaux les plus anciens sont évincés — comptés par `log_arena_evictions`, et par `log_persist_overrun` (C6) si un créneau évincé n'avait pas encore été persisté. `_persistBuffer` n'existe plus : cette éviction est désormais l'unique file d'attente vers `/system.log`.
- **Callback non défini** : simplement pas de push WS, logs conservés dans l'arène quand même.
- **Heap critique** : le journal reste fonctionnel **sans dégradation** — `log()` n'alloue plus rien sur le tas depuis feature-129, il ne dépend donc plus de l'état de fragmentation qu'il sert justement à diagnostiquer.

## Fichiers liés

- [`src/logger.h`](../../src/logger.h), [`src/logger.cpp`](../../src/logger.cpp)
- [`src/log_arena_logic.h`](../../src/log_arena_logic.h) / [`.cpp`](../../src/log_arena_logic.cpp) — module **pur** de l'arène circulaire (feature-129, ADR-0017) : `logArenaPlan`, `logTruncatePlan`, `logRingEvictCount`, `logFormatHeader`, `logShouldPersist`, `logNextFlushSlot`, `logArenaWriteOverlapsLive`, `logLevelName` (feature-130, remplace `Logger::getLevelString`)
- [`tools/check_memory_discipline.sh`](../../tools/check_memory_discipline.sh) — sentinelle CI qui refuse toute réintroduction du motif corrigé par feature-129 (feature-130, job `memory-discipline` de `hygiene-specs.yml`) ; voir [`docs/BUILD.md`](../BUILD.md#toolscheck_memory_disciplinesh--la-discipline-mémoire-du-journal-reste-vraie-adr-0082-feature-130)
- [`src/utf8_clamp.h`](../../src/utf8_clamp.h) / [`.cpp`](../../src/utf8_clamp.cpp) — clamp UTF-8 partagé (`copyEntryByIndex`, `logTruncatePlan`)
- [`src/constants.h`](../../src/constants.h) (section « JOURNAL — arène circulaire ») — `kMaxLogEntries`, `kLogArenaBytes`, `kLogMessageMaxBytes`, `kLogTruncTail`, `kLogPersistLineMax`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — abonnement callback (`broadcastLog(const LogRecord&)`)
- [`src/logger_logic.h`](../../src/logger_logic.h) / [`.cpp`](../../src/logger_logic.cpp) — module **pur** de réconciliation du `.tmp` de rotation (v2.48.1)
- [`src/fs_guard.h`](../../src/fs_guard.h) — point d'admission unique des `fopen` internes (v2.43.0)
- [acces-fichiers.md](acces-fichiers.md) — recensement des sites d'ouverture, dont ceux du journal
- [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) — garde mémoire avant tout `fopen` interne
- [ADR-0076](../adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md) — réconciliation du `.tmp` de rotation au démarrage, contrainte d'ordre
- [ADR-0082](../adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md) — arène circulaire, alternatives écartées (dont la PSRAM), C6/C7, mutant non couvrable
