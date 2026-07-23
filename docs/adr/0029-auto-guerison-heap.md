# ADR-0029 — Auto-guérison sur fragmentation du tas (redémarrage propre) + mode maintenance anti-boucle

- **Statut** : Accepté
- **Date** : 2026-07-15
- **Décideurs** : Nicolas Philippe (architect), debugger (diagnostic du gel), pool-chemistry (séquence fail-safe C1/C2)
- **Spec(s) liée(s)** : aucune (correctif de robustesse déclenché par un gel récurrent en production)

## Contexte

L'appareil se **figeait** de façon récurrente : serveur web injoignable, écriture des logs sur disque arrêtée, mais **appareil toujours sous tension**. Seule une **coupure secteur** manuelle le débloquait. Contrairement aux deux modes de défaillance précédents, il n'y avait **ni panic, ni coredump, ni reboot automatique** :

- **1ʳᵉ cause** (corrigée en 2.8.2) — `std::bad_alloc` sur `/logs` sous fragmentation du tas : PANIC → reboot.
- **2ᵉ cause** (corrigée en 2.9.0, [ADR-0028](0028-migration-fork-esp32async.md)) — assert lwIP `tcp_recved` sur close/timeout WebSocket : PANIC → reboot.
- **3ᵉ cause** (ce ADR) — **épuisement / fragmentation du tas sans crash** : le plus gros bloc contigu libre (`ESP.getMaxAllocHeap()`) s'effondre alors que le heap **total** (`ESP.getFreeHeap()`) reste correct. Le serveur web asynchrone et la persistance des logs ne peuvent plus allouer et s'arrêtent silencieusement — mais **aucune exception n'est levée**, donc aucun PANIC.

Point clé : les tâches surveillées par la TWDT (`loopTask`, `mqttTask`) **restaient vivantes** (elles reset le watchdog normalement). Le chien de garde ne pouvait donc **jamais** se déclencher : c'est précisément le type de panne qu'un watchdog par tâche ne détecte pas. L'alerte `low_memory` existante (`kMinFreeHeapBytes = 10 Ko`, comparée au heap **total**) ne voyait rien non plus, la fragmentation frappant le **plus gros bloc** et non le total.

Déclencheur externe aggravant : l'installation réseau de l'utilisateur (double NAT via prise **CPL**) fait sauter l'IP de l'ESP32. Chaque changement d'IP rejouait `MDNS.end()/begin()`, ce qui accélérait la fuite/fragmentation mémoire et rapprochait l'échéance du gel.

Il fallait trancher : sans détection dédiée du plus gros bloc contigu, aucune couche existante (watchdog, alerte heap total, coredump) ne pouvait sortir l'appareil de ce gel.

## Décision

**Ajouter une auto-guérison mémoire dans `checkSystemHealth()`** (contexte `loopTask`, tick 60 s) surveillant le **plus gros bloc contigu libre** (`ESP.getMaxAllocHeap()`), distinct du heap total :

1. **Instrumentation** : log INFO persisté du tas à chaque tick + flush disque one-shot au premier franchissement à la baisse de `kLowHeapWarnBytes` (20 Ko).
2. **Redémarrage propre** (auto-guérison) : si le plus gros bloc reste sous `kRestartMaxAllocHeapBytes` (8 Ko) pendant `kAutoHealDebounceChecks` (3) ticks consécutifs **et** après `kAutoHealMinUptimeMs` (3 min) d'uptime → séquence **fail-safe** validée pool-chemistry (condition C1, ordre imposé) : `stopAll()` pompes → `saveDailyCounters()` → log critical → alerte MQTT `auto_reboot` → flush disque → `ESP.restart()`.
3. **Mode maintenance anti-boucle** (condition pool-chemistry C2) : un compteur de reboots en **RAM RTC** (survit au reboot logiciel), sur une fenêtre glissante de 15 min (`kAutoHealRebootWindowMs`), plafonné à `kAutoHealMaxReboots` (3). Au-delà, l'appareil **cesse d'enchaîner les reboots** et bascule en maintenance : pompes OFF, dosage inhibé sur **toutes** les voies (`PumpController.setMaintenanceInhibit(true)`), auto-guérison désactivée, alerte MQTT `maintenance_mode`, **appareil maintenu joignable** pour diagnostic. Sortie : redémarrage manuel.

La **décision** (Continue / Restart / EnterMaintenance) est isolée dans un **module pur** `self_heal_logic` ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)), testable en natif (17 tests, total 240) ; la collecte des métriques et les effets restent dans la coquille impure `main.cpp`.

En complément, la ré-annonce mDNS est **debouncée** : au plus une fois toutes les `kMdnsReannounceMinIntervalMs` (30 s) et seulement si l'IP a réellement changé, pour supprimer le churn `MDNS.end/begin` en roaming.

**Reboot = pompes OFF = état fail-safe** (validé pool-chemistry). Le redémarrage n'ouvre aucune fenêtre de dosage : les pompes sont coupées avant le reboot, et l'état sûr au boot est garanti par un **pulldown de gate MOSFET (10 kΩ, GPIO25 pH / GPIO33 ORP) confirmé présent sur le PCB v2** (cf. Conséquences).

## Alternatives considérées

- **Option A (rejetée) — Ne rien faire / subir le gel.** Statu quo : l'utilisateur coupe le secteur à la main à chaque occurrence. Inacceptable pour un appareil censé fonctionner sans surveillance, et dangereux (régulation à l'arrêt pendant une durée indéterminée sans alerte).
- **Option B (rejetée) — Augmenter le heap disponible sans borne** (réduire buffers, alléger les libs). Repousse l'échéance sans traiter la cause ; la fragmentation finit toujours par gagner sur un uptime long. Ne fournit aucune sortie de secours quand elle survient malgré tout.
- **Option C (rejetée) — Watchdog plus agressif.** Inefficace : les tâches surveillées **tournent** pendant le gel (elles reset le WDT). Un WDT par tâche ne détecte pas une panne où les tâches vivent mais ne peuvent plus allouer. Aurait fallu un watchdog applicatif ad hoc — c'est exactement ce que l'auto-guérison implémente, mais ciblé sur la vraie métrique (plus gros bloc contigu).
- **Option D (retenue) — Auto-reboot sur seuil de plus gros bloc contigu + mode maintenance anti-boucle.** Traite le mode de défaillance réel (fragmentation, pas heap total), sort l'appareil du gel automatiquement, borne le risque de boucle de reboot, et dégrade proprement en maintenance joignable. Décision isolée et testée en natif.

## Conséquences

### Positives

- **Fin du gel silencieux** : l'appareil se rétablit seul (redémarrage propre), plus de coupure secteur manuelle.
- **Sécurité chimique préservée** : la séquence de reboot coupe et persiste avant tout ; le reboot n'est jamais une fenêtre de sur-dosage. Le mode maintenance inhibe **toutes** les voies de dosage.
- **Anti-boucle borné** : une cause persistante (matériel, réseau, fuite non corrigée) ne provoque pas un cycle infini de reboots — l'appareil se stabilise en maintenance et reste diagnosticable (web + MQTT vivants).
- **Observabilité** : instrumentation heap persistée + alertes MQTT `auto_reboot` / `maintenance_mode` → la panne devient traçable a posteriori.
- **Décision testable** : `self_heal_logic` couvre les seuils, le debounce, l'uptime minimal et le plafond de reboots sans matériel.

### Négatives / dette assumée

- **Dépend du réglage réseau côté utilisateur** : le déclencheur aggravant (double NAT CPL, IP instable) reste externe. L'auto-guérison masque le symptôme mais ne corrige pas l'instabilité réseau amont ; un réseau plus stable réduit la fréquence des reboots.
- **Pulldown sur la gate des MOSFET pompes (GPIO25 pH / GPIO33 ORP)** requis pour un fail-safe **total** au boot : sans lui, la gate flotte pendant le reset et pourrait énergiser brièvement une pompe avant que le firmware n'impose 0. Condition matérielle **hors périmètre logiciel** — **vérifiée satisfaite sur le PCB v2 (résistance 10 kΩ gate→GND présente sur les deux voies, confirmé 2026-07-15)**. Toute révision matérielle future doit préserver ce pulldown.
- **Instrumentation ajoutée** : une ligne INFO de heap toutes les 60 s consomme des entrées du buffer de logs (borné à `kMaxLogEntries = 200`, rotation existante — impact maîtrisé).
- **Compteur en RAM RTC** : la fenêtre anti-boucle repose sur la RAM RTC, réinitialisée en cas de brownout/power-on (magic word) — un vrai power-cycle repart donc de zéro (comportement voulu : on ne veut pas verrouiller en maintenance après une simple coupure secteur).

### Ce que ça verrouille

- La métrique de décision est le **plus gros bloc contigu libre** (`ESP.getMaxAllocHeap()`), **distinct** du heap total (`kMinFreeHeapBytes`, alerte `low_memory` conservée telle quelle). Les deux gardes coexistent et ne doivent pas être fusionnées.
- La **séquence de reboot** (stopAll → saveDailyCounters → log → alerte → flush → restart) est **fail-safe et ordonnée** (pool-chemistry C1) : tout nouveau chemin d'auto-reboot doit la respecter.
- Le mode maintenance doit **inhiber toutes les voies de dosage** (auto, scheduled, manuel web, manuel UART, test) via le point unique `PumpController.setMaintenanceInhibit()` et rester **joignable** (pas de restart en maintenance).
- La logique de décision reste **pure** (`self_heal_logic`, [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) : toute évolution des seuils/priorités passe par ce module et ses tests natifs.

## Références

- Code : [`src/self_heal_logic.h`](../../src/self_heal_logic.h) / [`src/self_heal_logic.cpp`](../../src/self_heal_logic.cpp), [`src/main.cpp`](../../src/main.cpp) (`checkSystemHealth`, `performAutoHealRestart`, `enterMaintenanceMode`, `initAutoHealRtcState`), [`src/pump_controller.h`](../../src/pump_controller.h) (`setMaintenanceInhibit` / `isMaintenanceInhibited`), [`src/constants.h`](../../src/constants.h) (`kRestartMaxAllocHeapBytes`, `kLowHeapWarnBytes`, `kAutoHealDebounceChecks`, `kAutoHealMinUptimeMs`, `kAutoHealMaxReboots`, `kAutoHealRebootWindowMs`, `kMdnsReannounceMinIntervalMs`)
- Tests : `test/test_native_*` — 17 tests dédiés à `self_heal_logic` (240 au total)
- Diagnostic : gel récurrent sans panic/coredump ; `loopTask`/`mqttTask` vivants ; effondrement du plus gros bloc contigu ; déclencheur externe double NAT CPL
- Docs : [subsystems/system-health.md](../subsystems/system-health.md), [subsystems/pump-controller.md](../subsystems/pump-controller.md), [subsystems/logger.md](../subsystems/logger.md), [MQTT.md](../MQTT.md)
- Antécédents : CHANGELOG `[2.8.2]` (1ʳᵉ cause), `[2.9.0]` / [ADR-0028](0028-migration-fork-esp32async.md) (2ᵉ cause)
