# Subsystem — `logger`

- **Fichiers** : [`src/logger.h`](../../src/logger.h), [`src/logger.cpp`](../../src/logger.cpp)
- **Singleton** : `extern Logger systemLogger;`


> ⚠️ **Accès fichiers — 7 des 14 sites `fopen` du firmware sont ici**
> (l. **230**, **276-277**, **299**, **307**, **317**, **329**), et ils sont tous
> classés **contention TCP** : `flushToDisk()` s'exécute **sur la tâche qui a émis
> le log** — `loopTask`, `mqttTask` ou `async_tcp` — parce que `ERROR` et
> `CRITICAL` déclenchent un flush **synchrone** ([`logger.cpp:71-73`](../../src/logger.cpp:71)).
> Il n'existe pas de « tâche du logger ». L'échec du `malloc` du verrou newlib
> appelle **`abort()`**. Voir [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md)
> (dont la décision **(f)** lève la contradiction de l'interdit `critical()` posé
> par [ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md)) et le
> recensement [acces-fichiers.md](acces-fichiers.md). ⚠️ **Garde non posée à ce
> jour** — cycle 2 de `bug-100`.

## Rôle

Log central du firmware avec buffer circulaire en RAM + persistance sur la partition `history` + push temps réel via WebSocket. Niveaux : `DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL`.

## API publique

```cpp
void begin();
void log(LogLevel level, const String& message);
void debug(const String& message);
void info(const String& message);
void warning(const String& message);
void error(const String& message);
void critical(const String& message);

void setLogCallback(std::function<void(const LogEntry&)>);  // push WS temps réel

// Itération en place des logs récents, SANS copie (v2.8.2) ; le visiteur
// retourne false pour arrêter l'itération (v2.10.4 — pagination /get-logs)
void forEachRecentLog(size_t count, unsigned long sinceTimestamp,
                      const std::function<bool(const LogEntry&)>& visitor) const;

// Copie UNE entrée sous mutex, best-effort (v2.10.4 — export chunked /download-logs)
bool getLogEntryByIndex(size_t chronoIdx, LogEntry& out) const;

void clear();      // vide uniquement le buffer RAM circulaire
void clearAll();   // vide RAM + _persistBuffer + supprime /system.log et /system.log.tmp
size_t getLogCount();

// Persistance
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

Activation utilisateur : Paramètres → Avancé → card Logs → switch « Logs DEBUG activés ». Effet immédiat (pas de redémarrage). La valeur est lue à chaque appel à `debug()` depuis la variable globale `authCfg`, sans mutex (lecture booléenne atomique sur ESP32).

Default `false` choisi pour alléger le buffer en production : les logs `DEBUG` (`Diagnostic publié`, `Consolidation terminée: N points`, `MQTT publish drop`, etc.) ne saturent plus les 200 entrées de `kMaxLogEntries`. L'utilisateur active explicitement le switch en cas de session de diagnostic.

Le filtre UI `#log_level_debug` de la page Logs reste indépendant : il filtre l'affichage navigateur des entrées DEBUG déjà produites par le firmware. Les deux mécanismes sont complémentaires :

| Mécanisme | Effet |
|---|---|
| Switch « Logs DEBUG activés » (firmware) | Décide si `Logger::debug()` **produit** les entrées |
| Filtre `#log_level_debug` (UI) | Décide si l'UI **affiche** les entrées DEBUG produites |

## Buffer circulaire RAM

`kMaxLogEntries = 200` ([`constants.h:46`](../../src/constants.h:46)). Stocké dans un `std::vector<LogEntry>` préalloué. Index circulaire `currentIndex`, flag `bufferFull` pour savoir si le buffer a fait au moins un tour.

## Lecture des logs récents — `forEachRecentLog` (v2.8.2, visiteur `bool` en v2.10.4)

```cpp
void forEachRecentLog(size_t count, unsigned long sinceTimestamp,
                      const std::function<bool(const LogEntry&)>& visitor) const;
```

Itère les `count` entrées les plus récentes (dont le `timestamp > sinceTimestamp`, `0` = pas de filtre) **en place, sous mutex, sans copie**. Le `visitor` est appelé pour chaque entrée dans l'ordre chronologique, avec une **référence const** sur l'entrée stockée dans le buffer circulaire — aucune allocation interne, aucun `std::vector<LogEntry>` intermédiaire.

**Retour du visiteur (v2.10.4, bug-crash-getlogs-badalloc)** : `false` = **arrêt anticipé** de l'itération. C'est le mécanisme de pagination de `/get-logs` : la page complète est construite en **une seule prise de mutex** (jamais de double passage), l'arrêt survient quand la page est pleine (règles d'admission : [`src/logs_page.h`](../../src/logs_page.h)).

**Contrat mutex** : le `_mutex` est tenu pendant toute l'itération. Le `visitor` s'exécute donc **sous mutex** ; il ne doit faire que des écritures mémoire rapides — **allocations BORNÉES tolérées** (ex. le `JsonDocument` d'une page ≤ ~4 Ko de `/get-logs`), pas d'I/O bloquante, pas de yield, et ne **jamais** rappeler `systemLogger` (risque de deadlock / récursion).

### Contexte : correctif du crash `/logs` (`std::bad_alloc`)

Cette méthode **remplace** l'ancienne `std::vector<LogEntry> getRecentLogs(size_t)`. L'ancienne API **deep-copiait** tout le buffer (y compris les `String`) dans un vecteur retourné par valeur : sous fragmentation du tas, `operator new` levait `std::bad_alloc`, non rattrapée → `abort()` → PANIC → reboot. L'appareil devenait injoignable en HTTP alors que MQTT continuait de fonctionner (diagnostiqué via coredump). En itérant sans copie et en streamant la réponse, `forEachRecentLog` supprime cette allocation massive.

### Gardes heap des routes logs (v2.10.4, [ADR-0032](../adr/0032-reponses-http-bornees.md))

La constante `kMinHeapForLogsResponse` (24 Ko, v2.8.2) est **supprimée** : un seuil FIXE indépendant de la taille réelle de la réponse ne protégeait pas (2 coredumps `bad_alloc` → `abort`, 2026-07-23 `/debug/sensor_trace` et 2026-07-24 `/get-logs` — le commentaire v2.8.2 « le stream grandit par realloc, jamais d'abort » était faux : le `cbuf` d'`AsyncResponseStream` est un `new char[]` throwant). Remplacée par :

- **`/get-logs`** : page bornée (30 entrées, hard cap 60), `measureJson()` de la page réelle + marge `kHttpHeapMarginBytes` (4 Ko) comparés à `ESP.getMaxAllocHeap()` → **503** `{"error":"heap fragmenté, réessayez"}` ou stream **pré-dimensionné exact**.
- **`/download-logs`** : flux **chunked zéro-RAM** (aucune garde nécessaire — rien de proportionnel à l'export n'est alloué), voir `getLogEntryByIndex` ci-dessous.

Contrats détaillés : [`docs/API.md`](../API.md#get-get-logs--write).

## Copie unitaire — `getLogEntryByIndex` (v2.10.4)

```cpp
bool getLogEntryByIndex(size_t chronoIdx, LogEntry& out) const;
```

Copie **UNE** entrée sous mutex (`chronoIdx` chronologique, `0` = la plus ancienne). Retourne `false` si l'index dépasse le nombre d'entrées **OU** sur timeout mutex (`kLoggerMutexTimeoutMs`) — les deux cas sont indistinguables pour l'appelant : contrat **best-effort** assumé. Consommée par le callback chunked de `/download-logs` (une entrée par itération, une seule `String` copiée à la fois) ; le ring peut tourner entre deux chunks (doublon/saut tolérés — export de diagnostic). Le report de ligne entre deux chunks est géré par le module pur [`src/logs_export_chunk.h`](../../src/logs_export_chunk.h) (`LogLineCarry<kLogExportLineMaxBytes>` / `emitCarry`, garantie de progression — jamais `0` avant la vraie fin ; lignes > 256 o tronquées « … »).

## Persistance sur LittleFS (partition history)

Activable via `setPersistenceFs(LittleFS*)` après montage de la partition `history`. Paramètres :
- Flush périodique : **10 min** (`kFlushIntervalMs = 600000`) — le flush immédiat sur ERROR/CRITICAL et le coredump couvrent les crashes.
- **Flush immédiat** sur `ERROR` et `CRITICAL` : `flushToDisk()` est appelé directement sans attendre l'intervalle périodique.
- Taille max du fichier : **8 KB** (`kMaxLogFileBytes = 8192`) — réduit de 16 → 8 KB en v2.11.0 ([ADR-0033](../adr/0033-budget-espace-partition-history.md)) pour tenir le budget d'espace garanti de la partition `history` (64 KB, pire cas log 2-3 blocs au lieu de 4-5). Historique : 32 → 16 KB antérieurement.
- Rotation : quand le fichier dépasse 8 KB, tronqué aux **6 derniers KB** (`kRotateKeepBytes = 6144`, était 12288) — le `.tmp` de rotation ne pèse plus que 2 blocs.

> ⚠️ Ces deux plafonds **font partie du budget d'espace** de la partition `history` (pire cas 13/16 blocs, [ADR-0033](../adr/0033-budget-espace-partition-history.md)) : ne pas les ré-augmenter sans recalculer le budget.

### 🔒 Un log `ERROR` depuis un chemin anti-`fopen` déclenche l'`abort()` qu'on prévient (v2.41.0)

**Trouvaille du lot B-3 de bug-080** ([ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md)). Le flush immédiat décrit ci-dessus n'est pas seulement « immédiat » : il est **synchrone, dans la tâche appelante**.

```
systemLogger.error(...)          logger.cpp:71-73  →  si ERROR ou CRITICAL
  → flushToDisk()                logger.cpp:276-277
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

`logger.cpp` ouvre `/system.log` (ou son `.tmp` de rotation) en **7 endroits**.
Depuis la v2.43.0, les 7 passent par `openIfHeapAllows()`
([`src/fs_guard.h`](../../src/fs_guard.h)) — plus aucun `fopen` nu.

**Les 7 sites sont tous de CLASSE A (seuil 8 192)**, y compris celui du boot. Ce
n'est pas une précaution : `flushToDisk()` s'exécute **sur la tâche qui a émis le
log**, il n'existe pas de « tâche du logger ». Une ligne `ERROR` émise depuis un
handler `async_tcp` ouvre le fichier **pendant** qu'une réponse est en vol, donc
avec des pbuf lwip alloués.

#### Une seule mesure par flush, six ouvertures ensuite

`flushToDisk()` prend **une** admission
([`logger.cpp:304`](../../src/logger.cpp:304)) puis ouvre jusqu'à six fois avec
`kFsGuardAlreadyAdmitted` : append, repli `"w"`, relecture de taille, source de
rotation, `.tmp` en écriture, `.tmp` en relecture. **6 mesures deviennent 1.**

Ce n'est pas une optimisation de confort : le parcours TLSF qui mesure le tas
coûte **d'autant plus cher que le tas est fragmenté**, c'est-à-dire exactement
dans le régime pour lequel la garde existe. Et la rotation est une séquence
**indissociable** — la re-mesurer à mi-parcours autoriserait un `.tmp` sans
pouvoir le relire.

Le 7ᵉ site, [`logger.cpp:246`](../../src/logger.cpp:246) (marqueur
`--- DÉMARRAGE ---` de `setPersistenceFs()`), prend **sa propre** mesure : il est
hors du flush.

#### Ce que devient le journal sous famine

Sur refus d'admission, `flushToDisk()` **réinsère le buffer non flushé en tête**
de `_persistBuffer` (sous mutex, `kLoggerMutexTimeoutMs` — jamais de réinsertion
sans mutex) et repousse `_lastFlushMs`. Conséquences, dans l'ordre :

1. **Aucune ligne n'est perdue tant que le buffer tient.** Le flush est
   **reporté**, retenté au prochain `kFlushIntervalMs` (10 min). Sans le
   `_lastFlushMs = millis()`, `update()` rappellerait `flushToDisk()` **à la
   fréquence de la loop** pendant toute la famine.
2. **Au-delà, la perte est bornée et par le HAUT.** `_persistBuffer` plafonne à
   **100 entrées en éviction FIFO** : sous famine prolongée, ce sont les lignes
   **les plus anciennes** qui sortent — y compris le WARN de boot du tripwire de
   l'historique.
3. **Le refus n'est jamais silencieux**, mais son canal durable n'est **pas** le
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

> **Règle générale** : avant d'ajouter une trace **périodique** persistée, chiffrer son débit en Ko/h et vérifier la rétention résultante. Une trace de 100 octets toutes les 30 s consomme **à elle seule** 12 Ko/h, soit plus que la taille totale du fichier — elle rend le journal inutilisable pour tout post-mortem.

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

Le tampon `_persistBuffer` (std::vector<String>) accumule entre deux flushes. Sa capacité est **bornée à 100 entrées** (`kMaxPersistBufferEntries`) et pré-allouée dans le constructeur (`reserve(kMaxPersistBufferEntries)`) pour éviter les `std::bad_alloc` en cas de heap fragmenté en production. Les entrées au-delà de 100 sont silencieusement ignorées (les logs restent dans le buffer RAM circulaire).

**Logs pré-persistance** : tant que `setPersistenceFs()` n'a pas été appelé (`_persistEnabled == false`), les entrées ne sont **pas** accumulées pour `/system.log` — elles partent en buffer RAM + console uniquement. C'est notamment le cas des logs du **disjoncteur de boot-loop history** (v2.10.0), qui s'exécute dans `HistoryManager::begin()` **avant** l'activation de la persistance (c'est précisément elle qu'il protège) : le CRIT « partition effacée automatiquement » est visible dans la page Logs (RAM) et sur la série, mais jamais dans `/system.log`. Voir [system-health.md](system-health.md#disjoncteur-de-boot-loop-de-linit-history-v2100).

## Push WebSocket

`setLogCallback(cb)` permet à `ws_manager` de s'abonner. À chaque `log()`, le callback est invoqué **immédiatement** avec l'entrée, ce qui déclenche un push WS `{type: "log", timestamp, level, message}`.

## Émission console

Chaque `log()` se termine par l'émission de la ligne sur la série (UART0), format `[NIVEAU] message` + CRLF — **inchangé** depuis toujours côté contenu.

**Depuis la v2.10.0** (correctif `bug-spam-sensor-problem-serial`), la ligne est composée dans un **buffer pile** (`kLoggerConsoleLineMax = 512`, [`constants.h`](../../src/constants.h)) puis émise en **un seul** `Serial.write()` :

- L'ancienne implémentation faisait **4 `Serial.print` séparés, hors mutex** : chaque `print` est atomique au niveau HAL (`UART_MUTEX_LOCK`), mais la **séquence** pouvait s'entrelacer entre tâches (mqttTask core 0 / loopTask core 1) — lignes mélangées à granularité ligne, observé sur bench.
- Un `write` unique étant atomique au HAL, l'entrelacement inter-tâches est supprimé **sans mutex supplémentaire** ni risque de blocage.
- **Troncature** : si la ligne dépasse 512 octets, elle est tronquée avec **fin de ligne CRLF garantie** (les 2 derniers octets émis sont toujours `\r\n`).
- **Buffer pile** (pas de heap) : la console reste fonctionnelle même sous heap critique.

> Les troncatures **intra-chaîne** et répétitions de fragments observées lors de l'incident du 2026-07-22 relevaient, elles, d'un **rejeu matériel/driver du FIFO TX UART0** (bench, ESP32 hors PCB) — pas d'un défaut logiciel. Analyse complète : `specs/features/todo/bug-spam-sensor-problem-serial.md`.

## Concurrence

Mutex FreeRTOS (`_mutex`) protège :
- `logs` vector (lecture/écriture concurrente entre tâches et handlers web)
- `_persistBuffer` (flush concurrent)

### Timeouts mutex bornés (feature-027, v2.11.1)

Les **8 prises de mutex** du logger (7 depuis feature-027, +1 avec `getLogEntryByIndex` en v2.10.4) sont bornées par `kLoggerMutexTimeoutMs = 100 ms` ([`constants.h`](../../src/constants.h)) — valeur courte car toutes les sections critiques du logger sont en **RAM pure** (buffer circulaire, `_persistBuffer`), jamais d'I/O sous mutex.

**Politique silencieuse anti-récursion** : les chemins d'échec du logger n'appellent **JAMAIS** `systemLogger` (un log qui échoue à logger son propre échec = récursion). Seul un `Serial.printf` de secours existe sur le chemin de réinsertion de `flushToDisk()`.

Politique d'échec par site :

| Site | Sur timeout |
|---|---|
| `log()` | Entrée perdue (RAM + persistance) + `_droppedLogs++`. Le push WS n'a pas lieu (callback non capturé) |
| `forEachRecentLog()` | Aucune itération (le `visitor` n'est pas appelé — le client réessaiera) |
| `getLogEntryByIndex()` | Retourne `false` — l'export chunked `/download-logs` termine sa section RAM sans erreur (best-effort) |
| `clear()` | No-op — rien n'est perdu, l'utilisateur peut re-cliquer. **Ne compte pas dans `_droppedLogs`** (corrigé post-revue : le compteur ne trace que des *entrées* perdues) |
| `clearAll()` | Return **avant** toute modification — le fichier `/system.log` n'est pas supprimé si la RAM n'a pas pu être vidée (pas d'état incohérent) |
| `getLogCount()` | Retourne `0` |
| `flushToDisk()` — swap du buffer | Return **sans toucher `_lastFlushMs`** → retente naturellement au prochain `update()` |
| `flushToDisk()` — réinsertion après échec d'écriture fichier | Lignes perdues proprement (`_droppedLogs += n`) + message `Serial` — jamais de réinsertion sans mutex |

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
- **Partition history pleine** : `flushToDisk()` échoue, les logs restent en RAM jusqu'à rotation.
- **`_persistBuffer` saturé** : au-delà de `kMaxPersistBufferEntries = 100` entrées, les nouvelles entrées ne sont pas accumulées pour le prochain flush (elles restent dans le buffer RAM circulaire). Situation possible uniquement si le flush est bloqué pendant un temps anormalement long.
- **Callback non défini** : simplement pas de push WS, logs conservés en RAM quand même.
- **Heap critique** : le log reste fonctionnel tant que le vector préalloué peut absorber les `String`.

## Fichiers liés

- [`src/logger.h`](../../src/logger.h), [`src/logger.cpp`](../../src/logger.cpp)
- [`src/constants.h:46`](../../src/constants.h:46) — `kMaxLogEntries`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — abonnement callback
- [`src/fs_guard.h`](../../src/fs_guard.h) — point d'admission unique des `fopen` internes (v2.43.0)
- [acces-fichiers.md](acces-fichiers.md) — recensement des 13 sites d'ouverture, dont les 7 du journal
- [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) — garde mémoire avant tout `fopen` interne
