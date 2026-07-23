# Subsystem — `system-health` (santé système & auto-guérison)

- **Fichiers** : [`src/main.cpp`](../../src/main.cpp) (`checkSystemHealth`, `performFailSafeRestart`, `performAutoHealRestart`, `enterMaintenanceMode`, `initAutoHealRtcState`, `ensureNetworkProbe`, `onNetPingSuccess`), [`src/self_heal_logic.h`](../../src/self_heal_logic.h) / [`.cpp`](../../src/self_heal_logic.cpp), [`src/net_health_logic.h`](../../src/net_health_logic.h) / [`.cpp`](../../src/net_health_logic.cpp), [`src/history_guard_logic.h`](../../src/history_guard_logic.h) / [`.cpp`](../../src/history_guard_logic.cpp) (+ coquille dans [`src/history.cpp`](../../src/history.cpp))
- **Pas de singleton** : logique portée par des fonctions libres de `main.cpp` + trois modules purs `self_heal_logic` (heap), `net_health_logic` (connectivité) et `history_guard_logic` (disjoncteur de boot-loop history).
- **Décisions architecturales** : [ADR-0029](../adr/0029-auto-guerison-heap.md) (auto-guérison mémoire), [ADR-0030](../adr/0030-watchdog-connectivite.md) (watchdog de connectivité), [ADR-0031](../adr/0031-disjoncteur-bootloop-history.md) (disjoncteur de boot-loop history).

## Rôle

Surveiller périodiquement la santé du firmware et **sortir automatiquement l'appareil d'un gel ou d'une boucle « appareil injoignable »**. Trois volets d'auto-guérison **distincts** cohabitent :

- **Watchdog mémoire (heap)** — gel par **fragmentation du tas** : web injoignable + logging arrêté, mais heap **total** correct (le plus gros bloc contigu s'effondre). Voir [ADR-0029](../adr/0029-auto-guerison-heap.md).
- **Watchdog de connectivité (réseau)** — gel de la **pile lwIP** : web/mDNS/ICMP morts alors que le WiFi reste **associé** et que le **heap est sain** (maxBloc ≈ 37 Ko lors du gel diagnostiqué). Le thread `tcpip`/lwIP n'est pas surveillé par la TWDT. Voir [ADR-0030](../adr/0030-watchdog-connectivite.md).
- **Disjoncteur de boot-loop history (v2.10.0)** — boot-loop par **partition history corrompue** : panic CPU non rattrapable (`IntegerDivideByZero` dans `lfs_alloc`) à la 1ʳᵉ écriture dans `/system.log`, **avant la montée du WiFi** → appareil définitivement injoignable. Détection **inter-boot** via NVS + effacement automatique de la partition. Voir [ADR-0031](../adr/0031-disjoncteur-bootloop-history.md) et la [section dédiée](#disjoncteur-de-boot-loop-de-linit-history-v2100).

Pour les deux watchdogs (heap, réseau), les tâches surveillées par le watchdog matériel restent **vivantes**, donc la TWDT ne se déclenche jamais ; pour le boot-loop history, le crash a lieu **pendant `setup()`**, avant tout mécanisme runtime.

Responsabilités :

1. **Instrumentation** du tas (trace persistée + flush one-shot).
2. **Auto-guérison mémoire** : redémarrage propre fail-safe sur fragmentation critique confirmée.
3. **Watchdog de connectivité** : sonde ICMP passerelle + réponse graduée (reconnexion WiFi → reboot fail-safe) sur gel de la pile réseau.
4. **Disjoncteur de boot-loop history** : effacement automatique de la partition `history` après 2 crashs consécutifs pendant sa fenêtre d'init.
5. **Mode maintenance** anti-boucle : au-delà d'un plafond de reboots **partagé** (heap + réseau), on cesse d'enchaîner et on dégrade proprement en restant joignable.

`checkSystemHealth()` est appelée depuis `loop()` toutes les `kHealthCheckIntervalMs = 60 s` ([`main.cpp:299`](../../src/main.cpp:299)), en contexte `loopTask`. Elle porte **les deux watchdogs runtime** (heap + réseau) ; le disjoncteur history, lui, s'exécute **une seule fois au boot**, dans `HistoryManager::begin()`.

## Pourquoi deux gardes dédiées

| Mécanisme existant | Ce qu'il détecte | Pourquoi il ne voyait ni le gel heap, ni le gel réseau |
|---|---|---|
| Watchdog TWDT (`kWatchdogTimeoutSec`) | Tâche bloquée qui ne reset pas le WDT | `loopTask` / `mqttTask` **tournaient** pendant les deux gels → WDT jamais déclenché. Le thread `tcpip`/lwIP n'est de toute façon **pas** surveillé par la TWDT. |
| Alerte `low_memory` (`kMinFreeHeapBytes = 10 Ko`) | Heap **total** trop bas | La fragmentation frappe le **plus gros bloc contigu**, pas le total. Et lors du gel réseau, le heap était **sain**. |
| Coredump | Crash / panic | **Aucun panic** dans les deux cas : pas d'exception levée, blocage silencieux. |
| Auto-guérison mémoire (watchdog heap) | Fragmentation du tas | Ne voit rien lors du gel réseau : `maxBloc` ≈ 37 Ko, loin du seuil de 8 Ko. |

D'où **deux** gardes complémentaires, chacune sur sa vraie métrique :

- **Heap** : `ESP.getMaxAllocHeap()` (plus gros bloc contigu libre), **distinct** de l'alerte heap total.
- **Connectivité** : sonde ICMP active vers la passerelle (le réseau répond-il de bout en bout ?), **indépendante de MQTT**.

Historique des modes de défaillance « appareil injoignable » (5 traités) : `std::bad_alloc` sur `/logs` (2.8.2, PANIC) → assert lwIP WebSocket (2.9.0, [ADR-0028](../adr/0028-migration-fork-esp32async.md), PANIC) → fragmentation du tas sans crash (2.9.1, [ADR-0029](../adr/0029-auto-guerison-heap.md), gel heap) → gel de la pile lwIP avec heap sain (2.9.2, [ADR-0030](../adr/0030-watchdog-connectivite.md), gel réseau) → **boot-loop sur partition history corrompue** (2.10.0, [ADR-0031](../adr/0031-disjoncteur-bootloop-history.md), panic à chaque boot avant WiFi).

## Instrumentation du tas (v2.9.1)

À **chaque** tick, sans dépendre d'aucun toggle de logs :

```
Heap: libre=<getFreeHeap> maxBloc=<getMaxAllocHeap> minLibre=<getMinFreeHeap>
```

INFO persisté — permet de voir la fragmentation (`maxBloc` s'effondre alors que `libre` reste correct). Au **premier** franchissement à la baisse de `kLowHeapWarnBytes = 20 Ko` (plus gros bloc), un WARN est émis **et** un `flushToDisk()` **one-shot** capture la descente sur disque même si le redémarrage n'est jamais atteint. Le déclencheur est **ré-armé** dès que `maxBloc` repasse au-dessus du seuil ([`main.cpp:657`](../../src/main.cpp:657)).

## Machine à états auto-guérison / maintenance

```
                 maxBloc < kRestartMaxAllocHeapBytes (8 Ko)
                 pendant kAutoHealDebounceChecks (3) ticks consécutifs
   [ NORMAL ] ───ET uptime ≥ kAutoHealMinUptimeMs (3 min)──────────► [ RESTART propre ]
       ▲                                                                     │
       │                                                                     │ reboots comptés
       │ maxBloc ≥ seuil (compteur remis à 0)                               │ en RAM RTC
       │                                                                     ▼
       │                          rebootCountInWindow ≥ kAutoHealMaxReboots (3)
       └──────────────────────────────────────────────────► [ MAINTENANCE (joignable) ]
                                   (fenêtre glissante kAutoHealRebootWindowMs = 15 min)
```

### Redémarrage propre — séquence partagée `performFailSafeRestart()` ([`main.cpp:632`](../../src/main.cpp:632))

Depuis v2.9.2, la séquence de reboot est **factorisée** dans `performFailSafeRestart(reason, alertType, alertMsg)`, appelée par **les deux** watchdogs :

- `performAutoHealRestart(maxAllocHeap)` ([`main.cpp:656`](../../src/main.cpp:656)) → délègue avec `alertType = "auto_reboot"` (gel heap).
- watchdog de connectivité → appelle directement avec `alertType = "network_stall_reboot"` (gel réseau).

Séquence **fail-safe, ordre imposé par pool-chemistry (condition C1) — NE PAS réordonner** :

1. `PumpController.stopAll()` — coupe les 2 pompes (sécurité chimique avant tout).
2. `saveDailyCounters()` — persiste les cumuls journaliers de dosage (anti-sur-dosage au reboot).
3. Incrément du compteur RTC + ancrage de la fenêtre glissante au 1er reboot.
4. `systemLogger.critical(reason)` — log persisté.
5. `mqttManager.publishAlert(alertType, alertMsg)` — alerte MQTT (tolère l'échec, ne bloque pas).
6. `systemLogger.flushToDisk()` — flush (tolère un échec silencieux si heap trop bas).
7. `ESP.restart()` — **ne retourne jamais**.

Le compteur RTC incrémenté à l'étape 3 est **partagé** entre heap et réseau : tout reboot fail-safe, quelle qu'en soit la cause, rapproche du plafond `kAutoHealMaxReboots`.

> **Reboot = pompes OFF = état fail-safe.** Le fail-safe **total** au boot suppose un **pulldown sur la gate des MOSFET** (GPIO25 pH / GPIO33 ORP) — condition matérielle hors périmètre logiciel ([ADR-0029](../adr/0029-auto-guerison-heap.md)).

### Mode maintenance — `enterMaintenanceMode()` ([`main.cpp:626`](../../src/main.cpp:626))

Condition pool-chemistry **C2** : quand `kAutoHealMaxReboots` (3) reboots ont eu lieu dans la fenêtre — **toutes causes confondues** (heap et/ou réseau, cf. budget partagé) — on **n'enchaîne plus** de reboot. Idempotent (flag `maintenanceMode`) :

- `PumpController.stopAll()` + `PumpController.setMaintenanceInhibit(true)` → dosage inhibé sur **toutes** les voies (voir [pump-controller.md](pump-controller.md)).
- `systemLogger.critical(...)` + `mqttManager.publishAlert("maintenance_mode", ...)`.
- **Aucun restart** : l'appareil reste **joignable** (web + MQTT) pour diagnostic.
- **Sortie** : redémarrage **manuel** du contrôleur.

### Compteur anti-boucle en RAM RTC — `initAutoHealRtcState()` ([`main.cpp:570`](../../src/main.cpp:570))

- `rtcAutoHealRebootCount` / `rtcAutoHealWindowStartEpoch` en **RAM RTC** (survivent au reboot logiciel), protégés par un magic word `kAutoHealRtcMagic`.
- **Power-on / brownout** (magic absent) → compteur remis à zéro : un vrai power-cycle ne verrouille jamais en maintenance.
- **Reboot logiciel** → compteur conservé, mais **purgé** si la fenêtre glissante (`kAutoHealRebootWindowMs`) a expiré → autorise à nouveau l'auto-guérison après une période saine.

## Décision pure — `self_heal_logic`

Style Humble Object ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) : la **décision** est extraite dans un module pur (`<stdint.h>` seul, sans Arduino/FreeRTOS/String) → **testable en natif**. La collecte (`getMaxAllocHeap`, `millis`, RAM RTC) et les effets restent dans la coquille `main.cpp`.

```cpp
enum class SelfHealDecision : uint8_t { Continue, Restart, EnterMaintenance };

struct SelfHealInputs {
  uint32_t maxAllocHeap;        // plus gros bloc contigu libre
  uint32_t restartThreshold;    // kRestartMaxAllocHeapBytes
  uint8_t  consecutiveBelow;    // ticks consécutifs sous le seuil
  uint8_t  debounceChecks;      // kAutoHealDebounceChecks
  uint32_t uptimeMs;            // millis()
  uint32_t minUptimeMs;         // kAutoHealMinUptimeMs
  uint8_t  rebootCountInWindow; // reboots auto déjà faits dans la fenêtre
  uint8_t  maxReboots;          // kAutoHealMaxReboots
};

SelfHealDecision evaluateSelfHeal(const SelfHealInputs& in);
```

**Ordre des règles (priorité décroissante)** :

1. `rebootCountInWindow >= maxReboots` → `EnterMaintenance` (**priorité HAUTE** : jamais un reboot de plus une fois le plafond atteint).
2. `uptimeMs >= minUptimeMs` **ET** `maxAllocHeap < restartThreshold` **ET** `consecutiveBelow >= debounceChecks` → `Restart`.
3. sinon → `Continue`.

Le compteur `consecutiveBelow` (statique dans `checkSystemHealth`) est incrémenté tant que `maxBloc < seuil`, remis à 0 sinon. Aucune décision n'est évaluée en mode maintenance (`if (!maintenanceMode)`, [`main.cpp:678`](../../src/main.cpp:678)).

**Tests** : 17 tests natifs dédiés (seuils, debounce, uptime minimal, plafond de reboots, priorité maintenance). Voir [BUILD.md](../BUILD.md) pour `pio test -e native`.

## Watchdog de connectivité (v2.9.2)

Second watchdog applicatif, **indépendant du watchdog heap**, ciblant le gel de la **pile lwIP** (web/mDNS/ICMP morts, WiFi associé, heap sain). Décision architecturale : [ADR-0030](../adr/0030-watchdog-connectivite.md).

### Signe de vie — sonde ICMP passerelle

Le signe de vie **primaire** est une **sonde ICMP active** vers la passerelle par défaut (`esp_ping`, header `ping/ping_sock.h`), **indépendante de MQTT** (contrainte : MQTT peut être désactivé) :

- **Session persistante unique** (`netPingHandle`), (re)créée de façon **idempotente** par `ensureNetworkProbe()` ([`main.cpp:673`](../../src/main.cpp:673)) tant que le WiFi est associé. Cadence `kNetworkProbeIntervalMs = 30 s`, timeout `kNetworkProbeTimeoutMs = 2 s`.
- Chaque echo reply prouve que **toute** la chaîne (RX → lwIP → TX) fonctionne. Le callback `onNetPingSuccess()` ([`main.cpp:666`](../../src/main.cpp:666)), invoqué par le thread interne `esp_ping` (**hors** `loopTask`), rafraîchit l'horloge `lastNetworkOkMs` (`volatile`, accès 32 bits aligné).
- L'horloge de stall mesurée dans `checkSystemHealth()` est `msSinceNetOk = millis() - lastNetworkOkMs`.

### Anti-faux-positif — gating WiFi

Tant que `WiFi.isConnected()` est **faux**, l'horloge de stall est **gelée** (`lastNetworkOkMs = millis()`) et le palier soft **réarmé** ([`main.cpp:812`](../../src/main.cpp:812)). Conséquence : une coupure WiFi légitime, un broker MQTT down ou un reboot de box **ne déclenchent rien**. La perte de WiFi reste gérée par la logique de reconnexion / AP de secours existante, **pas** par ce watchdog.

### Machine à états `net_health` — réponse graduée

```
        WiFi associé + pile muette (aucun echo ICMP)
                          │
   msSinceNetOk ≥ kNetworkStallSoftMs (2 min) ET soft pas encore tenté
                          ▼
              [ SoftRecover ] ── WiFi.disconnect(false) + WiFi.reconnect()
                          │       alerte MQTT network_soft_recover  (1×/épisode)
   (le soft n'a pas débloqué)
                          │
   msSinceNetOk ≥ kNetworkStallRebootMs (5 min)
                          ▼
              [ Restart ] ── performFailSafeRestart(..., "network_stall_reboot", ...)

   rebootCountInWindow ≥ kAutoHealMaxReboots (3, PARTAGÉ heap+réseau)
                          ▼
              [ EnterMaintenance ]  (priorité HAUTE)
```

Le flag `softRecoverAttempted` (statique dans `checkSystemHealth`) borne le SoftRecover à **une tentative par épisode** ; il est réarmé dès que le réseau redevient sain (`msSinceNetOk < kNetworkStallSoftMs`) ou que le WiFi tombe. Aucune décision réseau n'est évaluée en mode maintenance (`if (!maintenanceMode)`).

### Décision pure — `net_health_logic`

Même style Humble Object ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) que `self_heal_logic` : la **décision** est extraite dans un module pur (`<stdint.h>` seul), la collecte (`esp_ping`, `millis`, RAM RTC) et les effets (`WiFi.reconnect`, `performFailSafeRestart`, `enterMaintenanceMode`) restent dans `main.cpp`.

```cpp
enum class NetHealthDecision : uint8_t { Continue, SoftRecover, Restart, EnterMaintenance };

struct NetHealthInputs {
  bool     wifiAssociated;       // WiFi.isConnected()
  uint32_t msSinceLastNetOk;     // ms depuis le dernier signe de vie réseau
  uint32_t softStallMs;          // kNetworkStallSoftMs
  uint32_t hardStallMs;          // kNetworkStallRebootMs
  bool     softRecoverAttempted; // soft déjà tenté sur cet épisode
  uint32_t uptimeMs;             // millis()
  uint32_t minUptimeMs;          // kAutoHealMinUptimeMs
  uint8_t  rebootCountInWindow;  // reboots auto dans la fenêtre (PARTAGÉ heap+réseau)
  uint8_t  maxReboots;           // kAutoHealMaxReboots
};

NetHealthDecision evaluateNetHealth(const NetHealthInputs& in);
```

**Ordre des règles (priorité décroissante)** :

1. `!wifiAssociated` → `Continue` (horloge de stall gelée par la coquille : on ne juge pas la pile lwIP sans lien WiFi).
2. `uptimeMs < minUptimeMs` → `Continue` (laisser le démarrage se terminer).
3. `rebootCountInWindow >= maxReboots` → `EnterMaintenance` (**anti-boucle prioritaire**).
4. `msSinceLastNetOk >= hardStallMs` → `Restart` (gel confirmé).
5. `msSinceLastNetOk >= softStallMs` ET `!softRecoverAttempted` → `SoftRecover`.
6. sinon → `Continue`.

**Tests** : 25 tests natifs dédiés (gating WiFi, paliers soft/hard, uptime minimal, plafond partagé, réarmement du soft).

## Budget anti-boucle PARTAGÉ (heap + réseau)

Les deux watchdogs partagent **le même** compteur RTC `rtcAutoHealRebootCount` et la même fenêtre `kAutoHealRebootWindowMs`. Un reboot heap **et** un reboot réseau incrémentent le **même** quota (`kAutoHealMaxReboots = 3`). Conséquence assumée ([ADR-0030](../adr/0030-watchdog-connectivite.md)) : **un épisode combinant fragmentation mémoire ET gel réseau bascule en maintenance plus tôt** qu'un mode de défaillance isolé — comportement conservateur volontaire (dégrader proprement tôt plutôt que boucler). À connaître au diagnostic.

## Disjoncteur de boot-loop de l'init history (v2.10.0)

Troisième volet d'auto-guérison, **hors** `checkSystemHealth()` : il s'exécute une seule fois par boot, au début de `HistoryManager::begin()` ([`src/history.cpp`](../../src/history.cpp)), **avant** le montage de la partition `history`. Décision architecturale : [ADR-0031](../adr/0031-disjoncteur-bootloop-history.md).

### Déclencheur — incident réel (2026-07-22, v2.9.2)

Boot-loop `IntegerDivideByZero` dans `lfs_alloc` (littlefs) à la **première écriture** dans `/system.log` : superblock/métadonnées de la partition `history` corrompus. LittleFS **monte sans erreur** un filesystem corrompu puis panique à l'écriture — une **exception CPU non rattrapable** : aucun `try`/test d'écriture ne peut l'attraper. Le crash survenant dans `setup()` **avant la montée du WiFi**, l'appareil est resté injoignable **une semaine**, jusqu'à récupération manuelle en USB. Le garde-fou `.fscheck` existant (write/read 2 octets post-montage) ne voyait rien : un fichier de 2 octets est stocké *inline* dans les métadonnées littlefs et n'exerce **jamais** `lfs_alloc`. La seule défense viable est de détecter la boucle de panics **entre** les boots.

### Machine drapeau / compteur NVS

Le drapeau est un compteur `crash_cnt` dans le namespace NVS `hist_meta` (déjà utilisé pour `part_sz`) :

1. **Pose** : `crash_cnt` est écrit (≥ 1) **juste avant** `historyFs.begin()`. La fenêtre de détection couvre montage + `.fscheck` + `setPersistenceFs()` + `loadFromFile()`.
2. **Levée** : `crash_cnt = 0` sur **toutes** les sorties de `HistoryManager::begin()` (init complète, échec de montage, échec de remontage). Un panic **ailleurs** dans le firmware ne peut donc jamais déclencher l'effacement : le drapeau n'existe que pendant la fenêtre d'init.
3. **Au boot suivant**, avant le montage, la décision pure `evaluateHistoryGuard()` est évaluée à partir de `crash_cnt` (NVS) et de `esp_reset_reason()`.

```
   [ boot ]
      │  crash_cnt == 0 (init précédente propre / 1er boot)
      ├────────────────────────────────► Proceed (réarmement : crash_cnt→1, montage)
      │  crash_cnt > 0 mais reset PROPRE (POWERON/SW/EXT/BROWNOUT/DEEPSLEEP)
      ├────────────────────────────────► Proceed (réarmement SANS compter)
      │  crash_cnt ≥ kHistoryInitCrashThreshold (2) et reset CRASH
      ├────────────────────────────────► ErasePartition (esp_partition_erase_range
      │                                   vérifié ESP_OK + log CRIT, puis montage
      │                                   normal qui reformate — le boot CONTINUE)
      │  sinon (crash sous le seuil)
      └────────────────────────────────► CountCrash (crash_cnt+1, WARN
                                          « x/2 — nouvelle tentative », montage)
```

### Mapping des causes de reset

`esp_reset_reason()` est mappé vers l'enum pur `HistoryResetKind` par la coquille (`mapResetReason()`, [`src/history.cpp`](../../src/history.cpp)) :

| Comptés comme **crash** | Exclus (reset **propre**, réarmement sans compter) |
|---|---|
| `PANIC` (le cas `IntegerDivideByZero`), `INT_WDT`, `TASK_WDT`, `WDT` | `POWERON`, `SW` (ESP.restart, OTA), `EXT`, **`BROWNOUT`**, `DEEPSLEEP` |

Le **brownout est exclu volontairement** : c'est une cause électrique (alimentation faible), pas un symptôme de corruption FS — le compter risquerait d'effacer l'historique sur une simple alim défaillante.

### Indépendance vis-à-vis du budget anti-boucle et du mode maintenance

Le disjoncteur est **indépendant** du budget `kAutoHealMaxReboots` et du mode maintenance — c'est voulu, pour deux raisons :

- **Il ne redémarre jamais** : il répare (efface) puis **laisse le boot continuer**. Il ne consomme donc aucun quota de reboot ; c'est le crash lui-même qui reboote, pas le disjoncteur.
- **Il agit avant tout le reste** : pendant `setup()`, avant WiFi/MQTT/maintenance. Le bloquer en mode maintenance n'aurait aucun sens — le mode maintenance existe pour garder un appareil **joignable** ; ici l'appareil est **mort avant d'être joignable**. Conditionner la réparation au mode maintenance laisserait l'appareil définitivement injoignable, exactement ce qu'on corrige.

### Alerte MQTT différée

L'effacement a lieu dans `history.begin()`, **avant** `mqttManager.begin()` (la file de publication n'existe pas encore). Un flag `_partitionAutoErased` + getter `HistoryManager::partitionWasAutoErased()` permettent à `setup()` ([`src/main.cpp`](../../src/main.cpp)) de publier l'alerte `history_partition_reset` de manière **différée**, après `mqttManager.begin()`, via le `publishAlert()` existant. De même, la persistance des logs n'étant pas encore active à cet instant, le log CRIT de l'effacement part en **RAM + Serial uniquement** (voir [logger.md](logger.md#émission-console)) — c'est suffisant et c'est justement ce qu'on protège.

### Décision pure — `history_guard_logic`

Même style Humble Object ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) que `self_heal_logic` / `net_health_logic` : la décision est extraite dans un module pur (`<stdint.h>` seul) ; la collecte (`esp_reset_reason`, lecture NVS) et les effets (`esp_partition_erase_range`, écriture NVS, `systemLogger.critical`) restent dans la coquille `history.cpp`.

```cpp
enum class HistoryGuardDecision : uint8_t { Proceed, CountCrash, ErasePartition };

struct HistoryGuardInputs {
  uint8_t crashCount;      // crash_cnt lu en NVS ; 0 = pas de drapeau
  bool    resetWasCrash;   // isCrashResetKind(reset précédent)
  uint8_t eraseThreshold;  // kHistoryInitCrashThreshold
};

bool isCrashResetKind(HistoryResetKind kind);
HistoryGuardDecision evaluateHistoryGuard(const HistoryGuardInputs& in);
```

**Ordre des règles (priorité décroissante)** : 1. `crashCount == 0` → `Proceed` ; 2. `!resetWasCrash` → `Proceed` (réarmement sans compter) ; 3. `crashCount >= eraseThreshold` → `ErasePartition` ; 4. sinon → `CountCrash`.

**Tests** : 24 tests natifs dédiés (`test/test_native_history_guard/` — mapping des reset reasons, seuil, réarmement, cas limites) — **289 tests au total**.

### Trade-off assumé et récupération manuelle

- **Disponibilité > données** : 2 crashs consécutifs pendant la fenêtre d'init — même de cause **non liée** au FS — effacent l'historique. Coût accepté : l'historique repart de zéro, mais l'appareil redevient joignable sans intervention.
- **NVS indisponible** : dégradation silencieuse vers le comportement historique (pas de disjoncteur, jamais de blocage du boot).
- **Récupération manuelle de secours** (reste valable si le disjoncteur lui-même est hors jeu) :

```bash
esptool.py erase_region 0x3E0000 0x10000   # efface la partition history (64 Ko)
```

## Debounce de la ré-annonce mDNS (v2.9.1)

`checkSystemHealth()` consomme aussi le drapeau `mdnsReannounceRequested` (posé par le handler d'événement WiFi `STA_GOT_IP`) pour ré-annoncer `poolcontroller.local` en contexte `loopTask` (mDNS/lwIP non thread-safe). Depuis v2.9.1, la ré-annonce est **debouncée** ([`main.cpp:708`](../../src/main.cpp:708)) :

- exécutée **uniquement** si l'IP a **réellement** changé (`currentIp != mdnsLastAnnouncedIp`),
- **et** au plus une fois toutes les `kMdnsReannounceMinIntervalMs = 30 s`.

Sinon, un `debug(...)` « Ré-annonce mDNS débouncée » trace l'oscillation ignorée. Objectif : supprimer le churn `MDNS.end()/begin()` provoqué par les oscillations d'IP du **double NAT CPL**, qui accélérait la fragmentation du tas. Détail des logs mDNS : [logger.md](logger.md#ré-annonce-mdns-sur-changement-dip).

## Constantes ([`constants.h`](../../src/constants.h))

| Constante | Valeur | Rôle |
|---|---|---|
| `kHealthCheckIntervalMs` | `60000` (60 s) | Période d'appel de `checkSystemHealth()` |
| `kMinFreeHeapBytes` | `10000` (10 Ko) | Seuil heap **total** → alerte `low_memory` (**inchangé**, distinct de l'auto-guérison) |
| `kLowHeapWarnBytes` | `20000` (20 Ko) | Plus gros bloc sous ce seuil → WARN + flush one-shot |
| `kRestartMaxAllocHeapBytes` | `8192` (8 Ko) | Plus gros bloc sous ce seuil → candidat au restart |
| `kAutoHealDebounceChecks` | `3` | Ticks consécutifs sous seuil avant restart (anti-oscillation, ≈ 3 min) |
| `kAutoHealMinUptimeMs` | `180000` (3 min) | Uptime minimal avant tout restart auto |
| `kAutoHealMaxReboots` | `3` | Plafond de reboots dans la fenêtre → mode maintenance (**partagé heap + réseau**) |
| `kAutoHealRebootWindowMs` | `900000` (15 min) | Fenêtre glissante anti-boucle de reboot (partagée) |
| `kNetworkProbeIntervalMs` | `30000` (30 s) | Cadence de la sonde ICMP vers la passerelle |
| `kNetworkProbeTimeoutMs` | `2000` (2 s) | Timeout d'un echo ICMP |
| `kNetworkStallSoftMs` | `120000` (2 min) | Pile muette depuis ce délai → SoftRecover (reconnexion WiFi) |
| `kNetworkStallRebootMs` | `300000` (5 min) | Pile muette depuis ce délai → Restart fail-safe |
| `kMdnsReannounceMinIntervalMs` | `30000` (30 s) | Intervalle mini entre 2 ré-annonces mDNS |
| `kHistoryInitCrashThreshold` | `2` | Crashs consécutifs pendant l'init history → effacement de la partition (**hors** budget `kAutoHealMaxReboots`) |
| `kHistoryCrashCountNvsKey` | `"crash_cnt"` | Clé NVS du compteur du disjoncteur (namespace `hist_meta`, clé `part_sz` intacte) |

## Alertes MQTT émises

| Type (`{base}/alerts`) | Quand |
|---|---|
| `auto_reboot` | Juste avant le redémarrage propre pour **heap critique** (watchdog mémoire) |
| `network_soft_recover` | Pile réseau muette ≥ 2 min, WiFi associé → reconnexion WiFi tentée (watchdog connectivité, 1×/épisode) |
| `network_stall_reboot` | Juste avant le redémarrage propre pour **gel réseau** confirmé ≥ 5 min (watchdog connectivité) |
| `maintenance_mode` | Bascule en mode maintenance (plafond de reboots **partagé** atteint) |
| `low_memory` | Heap **total** sous `kMinFreeHeapBytes` (garde existante, conservée) |
| `history_partition_reset` | Partition history effacée automatiquement par le disjoncteur de boot-loop — publication **différée** depuis `setup()` après `mqttManager.begin()` (v2.10.0) |

Voir [MQTT.md](../MQTT.md#alertes).

## Cas limites

- **MQTT injoignable au moment du reboot** : `publishAlert(...)` échoue silencieusement (heap comme réseau), le reboot a lieu quand même (best-effort).
- **Heap trop bas pour le flush** : `flushToDisk()` peut échouer sans bloquer la séquence de reboot.
- **Power-cycle manuel** : la RAM RTC est réinitialisée → l'appareil ne repart jamais directement en maintenance après une coupure secteur.
- **Fenêtre expirée** : après 15 min sans reboot, le compteur (partagé) est purgé au boot suivant → les deux auto-guérisons redeviennent disponibles.
- **Coupure WiFi légitime** : ne déclenche **aucune** action du watchdog de connectivité (horloge de stall gelée tant que le WiFi n'est pas associé).
- **Passerelle qui filtre l'ICMP echo** : le signe de vie primaire serait perdu → risque de reboot à tort. Sur LAN domestique standard, la passerelle répond ([ADR-0030](../adr/0030-watchdog-connectivite.md)).
- **Épisode combiné heap + réseau** : les reboots des deux causes se cumulent dans le **même** budget → bascule en maintenance plus tôt (voulu).
- **NVS indisponible au boot** : le disjoncteur history dégrade silencieusement (compteur lu à 0 → `Proceed`) — jamais de blocage du boot.
- **Échec de l'effacement de la partition** (`esp_partition_erase_range != ESP_OK`) : log CRIT « échec de l'effacement », pas d'alerte MQTT (flag non posé), le montage est tenté quand même.
- **Crash non-FS pendant la fenêtre d'init history** : 2 occurrences consécutives effacent l'historique à tort — trade-off assumé ([ADR-0031](../adr/0031-disjoncteur-bootloop-history.md)).

## Fichiers liés

- [`src/self_heal_logic.h`](../../src/self_heal_logic.h) / [`.cpp`](../../src/self_heal_logic.cpp) — décision pure watchdog **heap**
- [`src/net_health_logic.h`](../../src/net_health_logic.h) / [`.cpp`](../../src/net_health_logic.cpp) — décision pure watchdog **connectivité**
- [`src/history_guard_logic.h`](../../src/history_guard_logic.h) / [`.cpp`](../../src/history_guard_logic.cpp) — décision pure disjoncteur **boot-loop history**
- [`src/main.cpp`](../../src/main.cpp) — coquille impure (collecte ICMP/heap + effets, `performFailSafeRestart`, alerte différée `history_partition_reset`)
- [`src/history.cpp`](../../src/history.cpp) — coquille impure du disjoncteur (NVS `hist_meta`, `esp_reset_reason`, `esp_partition_erase_range`)
- [`src/pump_controller.h`](../../src/pump_controller.h) — `setMaintenanceInhibit()` / `isMaintenanceInhibited()`
- [ADR-0029](../adr/0029-auto-guerison-heap.md) — auto-guérison mémoire
- [ADR-0030](../adr/0030-watchdog-connectivite.md) — watchdog de connectivité
- [ADR-0031](../adr/0031-disjoncteur-bootloop-history.md) — disjoncteur de boot-loop history
