# bug-134 — Le watchdog de connectivité ne réarme pas son compteur sur une reconnexion réussie

## Métadonnées
- **ID** : bug-134
- **Slug** : watchdog-connectivite-ne-rearme-pas-sur-reconnexion-reussie
- **Type** : bug
- **Statut** : doing — implémentation + docs livrées, revue `code-reviewer`/`test-engineer` en cours
- **Priorité** : haute
- **Version cible** : 2.53.1 (`src/version.h` encore à 2.53.0 — bump non fait dans ce tour, cf. Notes d'implémentation)
- **Dernière mise à jour** : 2026-09-20 (doc-writer : ADR-0084, system-health.md, API.md, CHANGELOG.md, ancres `main.cpp` recomptées)
- **Spec(s) liée(s)** : feature-133 (anomalie `minFen` relevée au même incident) · ADR-0053 (config d'usine ≠ config vivante, même famille de piège : juger sur un signal qu'on n'observe pas)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] **Régulation (pump_controller, PID)** — *indirectement* : le mode maintenance coupe le dosage (cf. §Sécurité chimique)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [x] **MQTT / Home Assistant** — signal secondaire du quorum
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — REQUIS** (tranché le 2026-09-20, plus « à décider ») : aucun code de `pump_controller` n'est touché, mais le correctif modifie **directement la fréquence d'entrée en mode maintenance** (`kAutoHealMaxReboots = 3`, `constants.h:285`, budget partagé avec le watchdog heap), lequel **coupe le dosage**. Changer la probabilité d'un chemin qui arrête l'injection relève du gate.
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Le 2026-09-19 à 15:46:37, le contrôleur a redémarré de lui-même alors que le
réseau fonctionnait. Le watchdog de connectivité avait sous les yeux la preuve
que la pile réseau était saine — une adresse IP obtenue par DHCP trois minutes
plus tôt — et a conclu à une « pile réseau figée ».

Le compteur qui déclenche le redémarrage n'est réarmé par **aucune** des preuves
de vie produites par une reconnexion WiFi réussie.

## Contexte — l'incident du 2026-09-19

Journal de l'appareil (v2.52.1, build du 16/09), séquence complète :

```
15:41:33  (silence — dernier réarmement de lastNetworkOkMs, déduit du décompte)
15:42:36  WARN  MQTT déconnecté détecté — état=-3
15:42:39  ERROR MQTT échec, code=-2 — prochaine tentative dans 10s
15:43:36  WARN  Watchdog connectivité : pile muette 124s — reconnexion WiFi (récupération soft)
15:43:36  WARN  WiFi déconnecté (reason=8)
15:43:37  INFO  WiFi associé à l'AP
15:43:38  INFO  WiFi IP obtenue: 192.168.1.48 RSSI=-49dBm        ← PREUVE DE VIE IGNORÉE
15:44:37  INFO  WiFi reconnecté avec succès
15:45:10  ERROR MQTT échec, code=-2 — prochaine tentative dans 120s
15:46:37  WARN  Arrêt d'urgence de toutes les pompes
15:46:37  CRIT  Watchdog connectivité : pile réseau figée, redémarrage propre
15:46:37  WARN  network_stall_reboot - Pile réseau muette 304s (maxBloc sain)
```

Après redémarrage : MQTT connecté en 3 s, découverte HA 60/60 entités,
historique rechargé (238 points). **La panne ne survivait pas au reboot** — ce
qui est la signature d'un redémarrage qui n'avait pas lieu d'être.

### Ce que l'incident n'est pas

À écarter d'emblée, chaque point étant prouvé par le journal :

- **Ce n'est pas une famine mémoire.** L'alerte le dit elle-même : `maxBloc sain`.
  La cinétique heap capturée avant reset est **plate** : `maxB8=30656` constant
  sur les 32 échantillons (78 s), `free8` ≈ 56 320 stable.
- **Ce n'est pas un plantage.** `reset_reason=3` (SW_RESET), redémarrage
  volontaire via `ESP.restart()` depuis `performFailSafeRestart()`
  (`src/main.cpp:948`). **Aucun coredump n'est produit** — confirmé par
  l'utilisateur, qui n'en a vu aucun dans l'interface. C'est le résultat attendu,
  pas une information manquante.
- **Ce n'est pas un problème WiFi.** RSSI −49 puis −46 dBm, association
  immédiate, IP obtenue en 1 s.
- **Ce n'est pas *seulement* le broker MQTT.** C'est l'erreur de diagnostic à ne
  pas commettre : le signal **primaire** (ICMP passerelle) s'était tu
  **une minute avant** la première déconnexion MQTT. MQTT n'était pas la cause,
  seulement le dernier témoin encore bruyant d'un silence déjà installé.

## Comportement actuel

### La machine à états

`evaluateNetHealth()` (`src/net_health_logic.{h,cpp}`, logique pure), avec deux
seuils dans `src/constants.h` :

```cpp
constexpr uint32_t kNetworkStallSoftMs   = 120000;  // 2 min → reconnexion WiFi
constexpr uint32_t kNetworkStallRebootMs = 300000;  // 5 min → reboot fail-safe
```

Les « 124 s » et « 304 s » du journal sont les valeurs de `msSinceLastNetOk` au
moment du log : elles dépassent légèrement le seuil parce que la vérification ne
tourne qu'à `kHealthCheckIntervalMs = 60000` (`constants.h:45`).

### L'horloge et ses réarmements

`lastNetworkOkMs` (`src/main.cpp:105`) est réarmée par un **OU** entre cinq
sources :

| Source | Site | Remarque |
|---|---|---|
| ICMP passerelle | `onNetPingSuccess`, `main.cpp:963` | **primaire** |
| MQTT | `mqtt_manager.cpp:509` | **seulement si `ok`** — donc jamais quand le broker est en panne |
| WebSocket | `ws_manager.cpp:186` et `:189` | dépend d'un client connecté |
| Requête HTTP `/data` | `web_routes_data.cpp:49` | dépend d'un client |
| NTP | `main.cpp:829` | ponctuel |
| **Démarrage réussi de la sonde** | **`main.cpp:1007`** | **absent de la rédaction initiale — relevé le 2026-09-20** |

⚠️ **Cette sixième source change la lecture du correctif A2.** `ensureNetworkProbe()`
réarme déjà `lastNetworkOkMs` à chaque (re)démarrage réussi de la sonde. Donc si
A2 force une recréation **inconditionnelle** après toute récupération soft, ce
point devient de facto une voie de réarmement supplémentaire — un bonus gratuit
du correctif, mais qui doit être **voulu et documenté**, pas subi. À traiter en C1.

### Les deux défauts

**Défaut n°1 — `GOT_IP` ne réarme rien.** Le gestionnaire
`ARDUINO_EVENT_WIFI_STA_GOT_IP` (`src/main.cpp:660-673`) ne fait que
ré-annoncer le mDNS. Il **n'appelle jamais** `notifyNetworkActivity()`. Or une
adresse IP fraîchement obtenue par DHCP est une preuve de vie L2 **et** L3
irréfutable : il a fallu une association réussie, une requête DHCP émise et une
réponse reçue du routeur.

**Défaut n°2 — la sonde ICMP n'est jamais recréée.** `ensureNetworkProbe()`
(`src/main.cpp:971-1015`) est idempotente **par égalité de l'adresse de
passerelle** (`main.cpp:975`). Après le rebond WiFi, la passerelle est la même,
donc la fonction considère qu'il n'y a rien à faire et ne recrée pas la session
de sonde. Si celle-ci était morte avant le rebond — ce que le journal suggère,
puisque l'horloge s'est figée une minute avant la coupure MQTT — elle le reste
**définitivement**, jusqu'au prochain redémarrage.

La conjonction des deux produit le scénario observé : la récupération soft
« réussit » au sens du WiFi, ne réarme rien, ne répare pas le signal primaire, et
le compteur poursuit sa course de 120 s jusqu'à 300 s sans jamais être interrompu.

### Un angle mort d'observabilité

`onNetPingSuccess` (`main.cpp:963`) **n'écrit aucune ligne** en cas de succès, et
`on_ping_timeout` est délibérément `nullptr` (`main.cpp:996`). Le journal est donc
**aveugle à la santé de la sonde ICMP** tant qu'aucune alerte ne se déclenche. On
ne peut pas prouver a posteriori quand la sonde est morte, ni pourquoi. C'est ce
qui a rendu ce diagnostic déductif plutôt que factuel.

## Comportement attendu

1. **Une IP obtenue réarme le compteur.** `ARDUINO_EVENT_WIFI_STA_GOT_IP` appelle
   `notifyNetworkActivity()`.
2. **Une récupération soft répare le signal primaire.** Après un `SoftRecover`,
   la sonde ICMP est **recréée inconditionnellement** (stop + delete + new), sans
   se fier à l'égalité d'adresse de passerelle.
3. **La santé de la sonde est observable.** Sans inonder le journal : un compteur
   exposé, ou une trace au changement d'état (sonde morte / ressuscitée), pas une
   ligne par ping.
4. **Une panne de broker seule ne redémarre pas le contrôleur.** Si le WiFi est
   associé, l'IP valide et la passerelle joignable, un broker MQTT injoignable
   est un événement **externe et normal** (Home Assistant redémarré, conteneur
   relancé) qui ne justifie pas de redémarrer un automate de piscine.

### Cas limites
- Passerelle réellement injoignable (routeur éteint) : le reboot doit **toujours**
  avoir lieu — ne pas transformer ce correctif en désarmement du watchdog.
- Changement d'adresse de passerelle (bascule d'AP, nouveau bail DHCP) : la sonde
  doit suivre, comportement déjà couvert par l'égalité d'IP existante.
- Rebond WiFi en rafale : la recréation de sonde ne doit pas fuir de ressource
  (chaque `esp_ping_new_session` exige son `delete`).
- Absence prolongée de client HTTP/WS : normal la nuit, ne doit pas peser.

## Sécurité chimique

- **Dosage impliqué** : non directement — aucun code de `pump_controller` n'est
  touché.
- **Mais le risque est réel et c'est la raison de la priorité haute.**
  `kAutoHealMaxReboots = 3` (`constants.h:285`) partage son budget avec le
  watchdog heap. Trois redémarrages de ce type dans la fenêtre font entrer en
  **mode maintenance**, qui **coupe le dosage**. Une panne de broker MQTT
  prolongée — quelques heures, ce qui arrive lors d'une mise à jour de Home
  Assistant — peut donc interrompre la régulation pH/ORP, et pas seulement la
  télémétrie.
- Le journal du 19/09 porte déjà `Auto-guérison : 1 reboot(s) auto récent(s) dans
  la fenêtre courante` : **un tiers du budget était consommé**.
- Les pompes ont été arrêtées proprement avant le reboot (`Arrêt d'urgence de
  toutes les pompes`, 15:46:37) — cette garantie ne doit pas être affaiblie.
- **Risque du correctif lui-même** : réarmer trop généreusement rendrait le
  watchdog aveugle à une vraie panne réseau. L'arbitrage « quelles preuves de vie
  comptent » est à tracer.

## Contraintes techniques
- ESP32, offline first.
- `esp_ping` : toute session recréée exige `esp_ping_stop` + `esp_ping_delete`
  avant `esp_ping_new_session`, sous peine de fuite.
- Le gestionnaire d'événements WiFi s'exécute sur la tâche événements système :
  pas d'opération longue, pas de log verbeux, pas de prise de mutex risquée.
- `notifyNetworkActivity()` est appelée depuis plusieurs tâches (`async_tcp`,
  `mqttTask`, boucle principale) : l'accès à `lastNetworkOkMs` doit rester sûr.
- `evaluateNetHealth()` est de la **logique pure** (ADR-0017) : tout ajout de
  règle doit y rester testable nativement.
- Ne pas désactiver le watchdog matériel ni allonger `kWatchdogTimeoutSec`.

## Fichiers potentiellement concernés
- `src/main.cpp` — `ARDUINO_EVENT_WIFI_STA_GOT_IP` (l. 660-673),
  `ensureNetworkProbe()` (l. 971-1015), `onNetPingSuccess()` (l. 963),
  `performFailSafeRestart()` (l. 929-948), bloc watchdog (l. 1405-1418)
- `src/net_health_logic.{h,cpp}` — si une règle est ajoutée au quorum
- `src/constants.h` — seuils, éventuelle constante d'observabilité de la sonde
- `docs/subsystems/` — sous-système réseau / santé
- `CHANGELOG.md` — comportement observable modifié

## Livrables — check-list
- [x] A1 — `notifyNetworkActivity()` appelée sur `ARDUINO_EVENT_WIFI_STA_GOT_IP`
      (via `notifyNetworkLifeSign(NetLifeSign::IpAcquired)`, `main.cpp:706` — préférée à
      `notifyNetworkActivity()` brute pour passer par le crédit plafonné, cf. A2/A4)
- [x] A2 — Recréation forcée de la sonde ICMP après toute récupération soft **et** sur silence
      confirmé de la sonde elle-même (`kNetworkProbeSilenceMs = 90 s`), sans fuite de session
      (`ensureNetworkProbe(force)` : stop + delete avant new dans les deux branches)
- [x] A3 — Observabilité de la santé de la sonde : trace sur transition (WARN mort / INFO
      rétablie, pattern edge) + 5 champs additifs `/get-system-info` (bloc `net`)
- [x] A4 — Arbitrage écrit : un broker MQTT en panne seul ne mène pas au reboot tant que
      l'écho ICMP passerelle (preuve externe, indépendante de MQTT) continue de réarmer —
      arbitrage documenté dans [ADR-0084](../../docs/adr/0084-ce-qui-prouve-qu-un-reseau-est-vivant.md)
- [x] D1 — Le défaut n'était pas dans `evaluateNetHealth()` mais dans la coquille impure :
      extraction de `evaluateNetRearm()` (+ `netProbeSeemsDead()`) comme prédit par la note
      ci-dessous, désormais couverte nativement. **Mesuré le 2026-09-20** :
      `test/test_native_net_health/test_net_health.cpp` = **42 cas** (25 préexistants +
      17 neufs pour `evaluateNetRearm()`/`netProbeSeemsDead()`)
- [x] D2 — Validation par **mutation** : boucle A (réarmer `GOT_IP` sans passer par le
      crédit) et boucle B (recréation de sonde inconditionnelle) tuent chacune un test dédié
- [x] C1 — `docs/subsystems/system-health.md` mis à jour (six sources de réarmement + leur
      classe, mécanisme de crédit, recréation sur silence de sonde, champs `net`)
- [x] C2 — `CHANGELOG.md` (bloc « Ce qui change pour vous » + Firmware/Documentation, sous
      l'entrée `[Unreleased]` visant 2.53.1)
- [x] C3 — ADR-0084 créé (amende ADR-0030, qui reste Accepté)

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 2 (décision invisible dans le code : *quelles preuves de vie comptent comme « réseau sain »*) et 3 (alternative crédible : exiger un quorum de deux signaux, ou hiérarchiser primaire/secondaire au lieu d'un OU plat)
- **Sujet pressenti** : « Ce qui prouve qu'un réseau est vivant, et ce qui autorise à redémarrer »

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Aucun nouveau — éventuellement un champ de diagnostic dans `/get-system-info`
  si l'observabilité de la sonde y est exposée (à trancher par l'architecte).

### Champs WebSocket
- Aucun

### Topics MQTT
- Aucun nouveau. L'alerte existante `network_stall_reboot` doit rester émise
  quand le reboot est **légitime**.

### Documentation à produire / mettre à jour
- **`docs/subsystems/system-health.md`** (cible réelle, relevée le 2026-09-20 — il
  n'existe pas de fichier « réseau » dédié) : quorum, les **six** sources de
  réarmement, angle mort d'observabilité. ⚠️ Ce fichier porte déjà des **ancres de
  ligne périmées** (`main.cpp:673`, `:666`) — à corriger dans le même lot.
- `docs/adr/0030-watchdog-connectivite.md` — si l'arbitrage A4 modifie la décision existante.
- `CHANGELOG.md` — sous 2.53.1
- `docs/adr/NNNN-*.md` — si A4 tranche un arbitrage

## Critères d'acceptation
- [x] Quand le WiFi se reconnecte et obtient une IP, le compteur de stall est réarmé — vérifiable dans le journal (log « WiFi IP obtenue » + réarmement plafonné consommant le crédit)
- [x] Quand le broker MQTT est éteint **et que la passerelle répond**, le contrôleur **ne redémarre pas**, même après 10 min (l'écho ICMP, preuve externe, continue de réarmer sans limite)
- [x] Quand la passerelle est réellement injoignable, le reboot fail-safe a **toujours** lieu (non-régression du watchdog) — garanti par l'épuisement du crédit (`kNetworkSelfRearmMax = 2`) en l'absence de toute preuve externe, couvert nativement
- [x] La mort de la sonde ICMP devient visible dans le journal ou un compteur (WARN/INFO transition + `net.msSinceEcho`/`net.probeAlive`)
- [x] Les pompes sont toujours arrêtées avant tout reboot fail-safe (séquence `performFailSafeRestart()` inchangée, `PumpController.stopAll()` en étape 1)
- [x] `pio run -e esp32dev_core3` sans nouveau warning ; `pio test -e native` vert, zéro régression — **vérifié en exécution seule le 2026-09-20** : SUCCESS 0 warning, **53 suites / 1 941 cas** (`test_native_net_health` 25 → 42 cas), 4 scripts d'hygiène verts
      — **en cours de revue par `code-reviewer`/`test-engineer` au moment de cette mise à jour
      documentaire** ; à confirmer avant clôture et déplacement vers `done/`

## Plan de test manuel
1. **Broker seul en panne** : arrêter le conteneur MQTT 15 min, passerelle intacte.
   Attendu : aucun redémarrage, alertes MQTT seules, reconnexion à la relance.
2. **Rebond WiFi** : couper l'AP 30 s puis le rétablir. Attendu : `GOT_IP` réarme
   le compteur, la sonde ICMP est recréée, aucun reboot.
3. **Panne réseau réelle** : éteindre le routeur. Attendu : soft recover à 2 min,
   puis reboot fail-safe à 5 min, pompes arrêtées avant.
4. **Rafale de rebonds** : dix coupures WiFi successives. Attendu : aucune fuite
   de session ICMP (surveiller `max_alloc_8bit`, pas la mémoire libre).

## Notes pour agent

⚠️ **Le piège de ce lot est la sur-correction.** Il est facile de réarmer si
généreusement que le watchdog ne se déclenche plus jamais — et ce watchdog existe
parce qu'une pile réseau figée est un mode de panne réel de l'ESP32. Le critère
d'acceptation n°3 (« quand la passerelle est réellement injoignable, le reboot a
toujours lieu ») est celui qui protège de cette dérive : il doit être testé, pas
supposé.

⚠️ **Ne pas juger sur un signal qu'on n'observe pas.** La cause profonde de la
difficulté de ce diagnostic est que la sonde ICMP ne journalise rien. Livrer le
correctif sans l'observabilité (A3) laisserait le prochain incident tout aussi
opaque.

⚠️ **Mesurer, ne pas déduire.** L'heure de mort de la sonde (≈ 15:41:33) est
**déduite** du décompte « 304 s », pas observée. Ne pas la traiter comme un fait
établi dans le reste de l'analyse.

## Hors périmètre
- L'anomalie `minFen` relevée au même incident (plancher de `maxAlloc8` décroissant
  sur des fenêtres indépendantes — 14324 → 8692 — pendant que le `maxBloc8`
  instantané reste figé à 30708). C'est **feature-133**, second mécanisme de
  fragmentation. Ne pas mélanger les deux sujets.
- Les seuils `kNetworkStallSoftMs` / `kNetworkStallRebootMs` eux-mêmes : les
  toucher masquerait le défaut de réarmement au lieu de le corriger.
- Le mode maintenance et `kAutoHealMaxReboots` : leur révision est un sujet à part.

## Notes d'implémentation (doc-writer, 2026-09-20)

Documentation livrée sans toucher à `src/` ni `test/` (revue `code-reviewer`
en cours au moment de ce tour) :

- `docs/subsystems/system-health.md` — nouvelle section « Réarmement plafonné
  à crédit — six sources et leur classe » + « Observabilité de la sonde —
  silence et recréation forcée », table des constantes complétée
  (`kNetworkSelfRearmMax`, `kNetworkProbeSilenceMs`), 12 ancres `main.cpp:NNN`
  périmées **recomptées une par une** sur HEAD (aucune recopiée depuis la
  spec).
- `docs/adr/0084-ce-qui-prouve-qu-un-reseau-est-vivant.md` (nouveau) — amende
  [ADR-0030](../../docs/adr/0030-watchdog-connectivite.md), qui reste
  **Accepté et non modifié** (immuabilité respectée : le renvoi croisé
  0030→0084 est porté par `docs/subsystems/system-health.md` et
  `docs/adr/README.md`, pas par une édition du fichier 0030 lui-même — l'ADR
  n'autorise que la mention `Superseded by`, ce qui ne s'applique pas ici).
- `docs/API.md` — bloc `net` de `/get-system-info`, sans chiffre de budget
  JSON inventé (aucune mesure `measureJson()` disponible ce tour, ADR-0073).
- `CHANGELOG.md` — ajouté sous l'entrée `[Unreleased]` existante (feature-097
  y est déjà, même cible 2.53.1) plutôt que sous un nouvel en-tête `[2.53.1]`,
  puisque `src/version.h` porte encore `2.53.0`.

**Deux limites honnêtes à ne pas perdre** :

1. Le critère « passerelle injoignable ⇒ reboot toujours » est **plus faible
   qu'il n'y paraît** : un client HTTP qui continue de solliciter `/data`
   (ou tout trafic applicatif) réarme **sans limite** (preuve externe
   `AppTraffic`), donc le reboot peut en théorie être différé indéfiniment
   tant qu'un client reste actif alors même que la passerelle ne répond
   plus. **Ce n'est pas une régression de ce lot** — `AppTraffic` réarme
   sans limite depuis [ADR-0030](../../docs/adr/0030-watchdog-connectivite.md)
   — mais la garantie ne doit pas être crue plus forte qu'elle ne l'est.
   Consigné dans ADR-0084 et `system-health.md` (§ Cas limites).
2. **Spec de suivi à ouvrir** : `DosingState` (`src/pump_controller.h:20-32`)
   reste entièrement en RAM (`usedMs`, `windowStart`, `lastStartTime`,
   `cyclesToday`). Ce lot réduit la **fréquence** des reboots inutiles, il ne
   supprime pas l'exposition résiduelle du dosage à un reboot légitime
   rapproché (heap + réseau, budget partagé) : persister `usedMs` et la
   fenêtre horaire reste hors périmètre ici.

**Reste à faire avant clôture** (hors périmètre doc-writer) : confirmation
`pio run -e esp32dev_core3` / `pio test -e native` par `test-engineer`, GO de
`code-reviewer`, bump `src/version.h` vers `2.53.1`, puis déplacement de cette
spec vers `specs/features/done/`.
