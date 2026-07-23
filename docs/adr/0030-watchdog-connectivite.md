# ADR-0030 — Watchdog de connectivité (sonde ICMP passerelle + réponse graduée)

- **Statut** : Accepté
- **Date** : 2026-07-15
- **Décideurs** : Nicolas Philippe (architect), debugger (diagnostic du gel réseau), pool-chemistry (réutilisation de la séquence fail-safe C1/C2)
- **Spec(s) liée(s)** : aucune (correctif de robustesse déclenché par un gel réseau récurrent en production)

## Contexte

Après l'ajout de l'auto-guérison mémoire ([ADR-0029](0029-auto-guerison-heap.md), v2.9.1), un **nouveau** gel « appareil injoignable » est survenu — mais cette fois **avec un tas parfaitement sain** : le plus gros bloc contigu libre (`ESP.getMaxAllocHeap()`) restait autour de **37 Ko**, jamais proche du seuil de restart (`kRestartMaxAllocHeapBytes = 8 Ko`). L'auto-guérison mémoire ne pouvait donc **rien** voir.

Diagnostic : c'est la **pile réseau lwIP elle-même qui se fige**. Serveur web injoignable, mDNS muet, et surtout **le ping ICMP lui-même ne passe plus** — alors que le WiFi reste **associé** à l'AP (`WiFi.isConnected()` vrai). Le thread `tcpip`/lwIP n'est **pas** enregistré auprès du watchdog matériel (TWDT), et `loopTask`/`mqttTask` continuaient de tourner et de reset le WDT normalement. Résultat : **ni la TWDT, ni l'auto-guérison mémoire** ne pouvaient sortir l'appareil de ce gel.

C'est le **cousin « en gel »** du crash `tcp_recved` corrigé par la migration fork ([ADR-0028](0028-migration-fork-esp32async.md), v2.9.0) : même famille de bug de course dans AsyncTCP/lwIP, mais qui aboutit ici à un blocage silencieux plutôt qu'à un `assert` → panic. C'est le **4ᵉ mode de défaillance « appareil injoignable »** :

1. `std::bad_alloc` sur `/logs` (2.8.2) — PANIC → reboot.
2. Assert lwIP `tcp_recved` sur close/timeout WebSocket (2.9.0, ADR-0028) — PANIC → reboot.
3. Fragmentation du tas sans crash (2.9.1, ADR-0029) — gel silencieux, heap.
4. **Gel de la pile lwIP avec heap sain** (ce ADR) — gel silencieux, réseau.

Il fallait un **signe de vie du réseau lui-même**, indépendant de la mémoire et **indépendant de MQTT** (le broker MQTT peut être désactivé par l'utilisateur, ou légitimement injoignable).

## Décision

**Ajouter un watchdog de connectivité dans `checkSystemHealth()`** (contexte `loopTask`, tick 60 s), dont le signe de vie primaire est une **sonde ICMP active vers la passerelle** :

1. **Sonde ICMP** (`esp_ping`, session **persistante** unique) vers la passerelle par défaut, cadence `kNetworkProbeIntervalMs` (30 s), timeout `kNetworkProbeTimeoutMs` (2 s). Chaque echo reply reçu prouve que **toute** la chaîne réseau (réception → lwIP → émission) fonctionne, et rafraîchit l'horloge `lastNetworkOkMs` depuis le callback `onNetPingSuccess`. Signe de vie **indépendant de MQTT**.
2. **Réponse graduée** (décision isolée dans le module pur `net_health_logic`, testable en natif) :
   - WiFi associé mais pile muette depuis `kNetworkStallSoftMs` (2 min) **et** aucune tentative soft encore faite sur cet épisode → **SoftRecover** : `WiFi.disconnect(false)` + `WiFi.reconnect()`, alerte MQTT `network_soft_recover`. Tenté **une seule fois** par épisode.
   - Pile muette depuis `kNetworkStallRebootMs` (5 min) → **Restart** : redémarrage propre **fail-safe** réutilisant la séquence C1 (stopAll → saveDailyCounters → log critique → alerte MQTT `network_stall_reboot` → flush → `ESP.restart()`).
3. **Anti-faux-positif** : la décision est **gated sur `WiFi.isConnected()`**. Tant que le WiFi n'est **pas** associé, l'horloge de stall est **gelée** (`lastNetworkOkMs = millis()`) et le palier soft réarmé. Une coupure WiFi légitime, un broker MQTT down ou un reboot de box **ne déclenchent rien** : seul un gel de la pile *alors que le WiFi tient* est traité. La perte de WiFi reste gérée par la logique de reconnexion / AP de secours existante, pas par ce watchdog.
4. **Budget anti-boucle PARTAGÉ** avec l'auto-guérison mémoire : même compteur RTC `rtcAutoHealRebootCount` (plafond `kAutoHealMaxReboots = 3` sur fenêtre glissante `kAutoHealRebootWindowMs = 15 min`). Au-delà → **mode maintenance** (pompes OFF, dosage inhibé, appareil joignable), **quelle que soit** la cause cumulée (heap et/ou réseau).

**Refactor** : l'ancien `performAutoHealRestart` délègue désormais à un `performFailSafeRestart(reason, alertType, alertMsg)` partagé, appelé aussi bien par l'auto-guérison mémoire (`auto_reboot`) que par le watchdog de connectivité (`network_stall_reboot`). Une seule séquence fail-safe, deux déclencheurs.

**Ce correctif est du containment, pas une correction de cause racine.** Le wedge est dans AsyncTCP/lwIP (piste racine identifiée : churn WebSocket/HTTP), à traiter ultérieurement. En attendant, le watchdog sort l'appareil du gel automatiquement.

## Alternatives considérées

- **Option A (rejetée) — Ne rien faire / subir le gel.** Statu quo : coupure secteur manuelle à chaque occurrence. Inacceptable (régulation à l'arrêt sans alerte, appareil censé fonctionner sans surveillance). C'est ce que ce ADR corrige.
- **Option B (rejetée) — Utiliser l'activité MQTT comme signe de vie réseau.** MQTT publie régulièrement ; on pourrait considérer « pas de publish OK depuis N min » comme un stall. Rejeté : **MQTT peut être désactivé** dans la config, et un broker down (cause externe légitime) produirait un faux positif. Le signe de vie doit être **indépendant de MQTT** et prouver la pile réseau de bout en bout, d'où l'ICMP vers la passerelle (toujours présente si le WiFi est associé).
- **Option C (rejetée) — Reboot direct sans palier intermédiaire.** Dès 2 min de stall, rebooter. Rejeté : le reboot est coûteux (perte du contexte de dosage, downtime) et souvent une simple reconnexion WiFi débloque la pile. La réponse **graduée** (soft d'abord, hard seulement si le soft ne suffit pas) minimise les reboots.
- **Option D (rejetée) — Enregistrer le thread tcpip auprès de la TWDT.** Le thread lwIP n'appelle pas de « check-in » applicatif ; l'instrumenter proprement toucherait au framework et resterait fragile (le thread peut « tourner » sans traiter les paquets). Un watchdog applicatif ciblé sur la **vraie** métrique (le réseau répond-il ?) est plus robuste — c'est exactement ce que la sonde ICMP mesure.
- **Option E (retenue) — Sonde ICMP passerelle + réponse graduée soft→hard + budget partagé.** Signe de vie réel et indépendant de MQTT, gradation qui évite les reboots inutiles, anti-faux-positif par gating WiFi, et budget commun avec l'auto-guérison mémoire pour un anti-boucle unifié. Décision isolée et testée en natif.

## Conséquences

### Positives

- **Fin du gel réseau silencieux** : l'appareil se rétablit seul (reconnexion douce, puis reboot si nécessaire), plus de coupure secteur manuelle — y compris quand le heap est sain.
- **Indépendance MQTT** : la détection fonctionne même MQTT désactivé, et ne produit pas de faux positif si le broker est down.
- **Sécurité chimique préservée** : le reboot réutilise la séquence fail-safe C1 validée pool-chemistry (pompes coupées et cumuls persistés avant tout).
- **Anti-boucle unifié** : un compteur unique évite qu'un épisode combinant fragmentation **et** gel réseau enchaîne des reboots sans fin ; l'appareil se stabilise en maintenance joignable.
- **Observabilité** : alertes MQTT `network_soft_recover` / `network_stall_reboot` + logs → la panne réseau devient traçable.
- **Décision testable** : `net_health_logic` couvre gating WiFi, paliers soft/hard, uptime minimal, plafond de reboots (25 tests natifs).

### Négatives / dette assumée

- **Containment, pas cause racine** : le wedge lwIP (AsyncTCP, churn WebSocket/HTTP) subsiste ; le watchdog masque le symptôme. La cause racine reste à traiter.
- **Budget partagé → bascule maintenance plus tôt** : un épisode combinant fragmentation mémoire **et** gel réseau consomme le même quota de 3 reboots, donc bascule en maintenance plus vite qu'un mode de défaillance isolé. **Comportement conservateur volontaire** (mieux vaut dégrader proprement tôt que boucler), mais à connaître au diagnostic.
- **Sonde ICMP permanente** : une session `esp_ping` persistante tourne en fond (thread interne, un echo/30 s). Coût réseau et mémoire négligeable, mais c'est une ressource de plus à maintenir vivante (recréée si besoin par `ensureNetworkProbe()`, idempotent).
- **Dépend d'une passerelle qui répond au ping ICMP** : si la passerelle filtre l'ICMP echo, le signe de vie primaire est perdu et le watchdog pourrait rebooter à tort. Sur un LAN domestique standard, la passerelle répond ; à revoir si un réseau bloque l'ICMP.

### Ce que ça verrouille

- Le signe de vie réseau primaire est une **sonde ICMP active vers la passerelle**, **indépendante de MQTT** : ne pas remplacer par une heuristique basée sur MQTT (contrainte : MQTT désactivable).
- La décision est **gated sur `WiFi.isConnected()`** : toute évolution doit préserver le gel de l'horloge de stall en l'absence de WiFi (anti-faux-positif).
- Le reboot passe **obligatoirement** par `performFailSafeRestart()` (séquence C1 fail-safe pool-chemistry) — comme l'auto-guérison mémoire.
- Le **budget anti-boucle est partagé** (`rtcAutoHealRebootCount`, `kAutoHealMaxReboots`) entre heap et réseau : ne pas séparer les compteurs sans réévaluer l'anti-boucle global.
- La logique de décision reste **pure** (`net_health_logic`, [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) : toute évolution des seuils/priorités passe par ce module et ses tests natifs.

## Références

- Code : [`src/net_health_logic.h`](../../src/net_health_logic.h) / [`src/net_health_logic.cpp`](../../src/net_health_logic.cpp), [`src/main.cpp`](../../src/main.cpp) (`checkSystemHealth`, `ensureNetworkProbe`, `onNetPingSuccess`, `performFailSafeRestart`, `performAutoHealRestart`, `enterMaintenanceMode`), [`src/constants.h`](../../src/constants.h) (`kNetworkProbeIntervalMs`, `kNetworkProbeTimeoutMs`, `kNetworkStallSoftMs`, `kNetworkStallRebootMs`, partagées : `kAutoHealMinUptimeMs`, `kAutoHealMaxReboots`, `kAutoHealRebootWindowMs`)
- Tests : `test/test_native_net_health/` — 25 tests dédiés à `net_health_logic` (265 au total)
- Diagnostic : gel réseau récurrent sans panic/coredump ; WiFi associé mais web/mDNS/ICMP morts ; heap sain (maxBloc ≈ 37 Ko) ; `loopTask`/`mqttTask` vivants
- Docs : [subsystems/system-health.md](../subsystems/system-health.md), [MQTT.md](../MQTT.md)
- Antécédents : [ADR-0028](0028-migration-fork-esp32async.md) (assert lwIP, migration fork), [ADR-0029](0029-auto-guerison-heap.md) (auto-guérison mémoire — séquence fail-safe et budget anti-boucle réutilisés)
