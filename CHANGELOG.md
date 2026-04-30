# Changelog - ESP32 Pool Controller

## [Unreleased] - 2026-04-30

### Firmware
- **IT5 — MQTT — fix déconnexions `exceeded timeout` Mosquitto** : remplacement du mode non-bloquant `O_NONBLOCK` (IT4) par un timeout d'écriture borné `SO_SNDTIMEO=500 ms` posé via `setsockopt()` après chaque `mqtt.connect()` réussi. Le PINGREQ keepalive PubSubClient (2 octets toutes les 60 s) part désormais de manière fiable même quand un publish concurrent occupe le send buffer TCP — avant IT5, un `lwip_send()` qui retournait `EAGAIN` instantanément faisait perdre silencieusement le PINGREQ (PubSubClient n'audite pas le retour de `_client->write` pour le PINGREQ), et Mosquitto coupait la session après 90 s sans paquet reçu. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 5 »
  - **Nouvelle constante** `kMqttSocketSendTimeoutMs = 500` (ms) dans `src/constants.h` avec commentaire d'unité explicite et référence ADR-0011 IT5
  - **`src/mqtt_manager.cpp` `connectInTask()`** : `fcntl(F_GETFL) + fcntl(F_SETFL, O_NONBLOCK)` remplacé par `setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))` avec `tv = {0, 500_000 µs}` ; include `<fcntl.h>` retiré, `<lwip/sockets.h>` ajouté
  - **Wrapper `safePublish()` inchangé runtime** : commentaire d'en-tête mis à jour pour refléter le nouveau mécanisme (`mqtt.publish()` borné à 500 ms par appel via `SO_SNDTIMEO`, plus de retour immédiat `EAGAIN`)
  - **Trade-off** : un publish lent peut prendre jusqu'à 500 ms (vs retour immédiat IT4 sur send buffer plein). Pire cas `publishDiscovery` (17 publishes enchaînés) = 8.5 s, sous le watchdog 30 s avec marge. Imperceptible utilisateur sur LAN sain
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning

### Documentation
- `docs/subsystems/mqtt-manager.md` : section « Garde-fou » renommée « `safePublish()` + socket avec `SO_SNDTIMEO` (IT5, remplace O_NONBLOCK d'IT4) » avec snippet `setsockopt`, explication du side-effect IT4 sur le PINGREQ keepalive et son fix IT5 ; tableau des paramètres tâche enrichi de `kMqttSocketSendTimeoutMs` ; sections « Keepalive », « Bornage TCP côté lwip », « Bascule de dominance entre `-3` et `-4` » et tableaux mis à jour pour refléter le timeout socket borné IT5 au lieu du non-bloquant IT4
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 5 — 2026-04-30 » dans « Évolutions » détaillant la cause racine (PINGREQ silencieusement perdu en `O_NONBLOCK`), le fix `SO_SNDTIMEO=500 ms`, les fixes F17–F20, le trade-off et les tests dynamiques restants. La décision principale de l'ADR (tâche dédiée) reste retenue
- `specs/features/done/feature-014-mqtt-task-dediee.md` : statut passé à `done`, version cible 1.0.5, itération 5 marquée livrée (build vert + revue OK ; AC-IT5-3 et AC-IT5-4 délégués à l'humain post-flash)

---

## [Unreleased] - 2026-04-29

### Frontend
- **UI cards — placement uniformisé des badges d'état (feature-001)** : uniformisation du placement des badges d'état (Marche/Arrêt, Allumé/Éteint, Connecté/Déconnecté) à droite du titre dans le `card__head` des cards Filtration « Contrôle manuel », Éclairage « Contrôle manuel » et MQTT (Paramètres). Suppression des lignes « État actuel » redondantes dans le body des cards Filtration et Éclairage. Cohérence visuelle inter-pages, gain de place vertical. Côté JS, `updateFiltrationBadges()` et `updateLightingStatus()` ont été splittés (page détail = `pill ok/bad/mid` ; dashboard `card--status` = `state-badge--*` inchangé), `getFiltrationState()` expose désormais `pillClass` (mapping `warn → mid`). Règle CSS de garde `.card__head .pill { flex-shrink: 0; white-space: nowrap; }`. Aucun impact sur le dashboard ni sur les autres cards (Wi-Fi, Heure, Sécurité, Régulation, Calibrations, Produits, Historique, Système).
- **Bug fix** : Badge MQTT (Paramètres → MQTT) — propagation fiabilisée vers l'UI : la mise à jour temps réel via WS s'applique désormais en tête de `_onWsSensorData` (blindée par try/catch) et un re-render explicite est déclenché au passage sur le panel MQTT. Corrige un cas où le badge restait à « Déconnecté » après reconnexion firmware sans switch d'onglet.
- **WebSocket** : badge statut MQTT (Paramètres → MQTT) mis à jour en temps réel via push WS toutes les 5 s, sans nécessité de reload page. Quand le broker devient injoignable (câble HA débranché, broker arrêté), le badge bascule sur « Déconnecté » en moins de 5 s suivant la détection firmware ; idem pour la reconnexion. Le champ `mqtt_connected` est désormais inclus dans la payload `sensor_data` (en plus du snapshot `config` déjà présent). Source : `mqttManager.isConnected()` (single source of truth `connectedAtomic` introduit par feature-014 IT2)

### Documentation
- `docs/features/page-filtration.md` : badge Marche/Arrêt désormais documenté dans le `card__head` de la card « Contrôle manuel » ; mention de la suppression de la ligne « État actuel » redondante et du split de `updateFiltrationBadges()` / `getFiltrationState()`
- `docs/features/page-lighting.md` : badge Allumé/Éteint désormais documenté dans le `card__head` de la card « Contrôle manuel » ; mention de la suppression de la ligne « État actuel » redondante et du split de `updateLightingStatus()`
- `docs/features/page-settings.md` : précision du placement DOM uniformisé du badge MQTT (frère direct du `<h2>` dans `card__head`) et de la règle CSS de garde
- `docs/subsystems/ws-manager.md` : nouveau champ `mqtt_connected` documenté dans `sensor_data` avec la précision du doublon volontaire vs `config` (canal temps réel 5 s vs snapshot stable à la transition)
- `docs/features/page-settings.md` : précision sur le comportement temps réel du badge MQTT (Paramètres → MQTT) — bascule en < 5 s sans reload

---

## [Unreleased] - 2026-04-29

### Firmware
- **IT4 — Wrapper `safePublish()` + socket non-bloquante** — corrige un nouveau PANIC watchdog observé au 3ᵉ re-test D2 humain APRÈS le flash IT3. Le point de blocage avait migré vers `drainOutQueue()` (publish ~110 octets `orp_limit`), fonction qui n'avait pas été instrumentée par F8/F9 d'IT3 (oubli). Découverte parallèle : `CONFIG_LWIP_TCP_MAXRTX=5` borne en réalité à ~93 s (et non ~10 s comme estimé en IT3) à cause de `TCP_RTO_INITIAL=3 s` × backoff exponentiel — le bornage seul ne protège pas `mqttTask`. Pivot architectural : socket TCP non-bloquante via `fcntl(F_SETFL, O_NONBLOCK)` après chaque `mqtt.connect()` réussi → tout `WiFiClient::write()` retourne immédiatement avec `EAGAIN` si send buffer plein, plus de blocage dans `lwip_select`. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 4 »
  - **Mode non-bloquant via `fcntl()`** : passage de la socket en `O_NONBLOCK` après chaque connect réussi dans `connectInTask()`, avant `subscribe()`. Includes `<fcntl.h>` et `<errno.h>` ajoutés
  - **Wrapper unique `safePublish(topic, payload, retain)`** : remplace les 24 `mqtt.publish()` directs dans `mqttTask` (status `online` au connect, `drainOutQueue`, 20 publishes de `publishAllStatesInternal`, `publishDiagnosticInternal`, 17 publishes de la lambda `publishConfig` dans `publishDiscovery`). Le wrapper fait `esp_task_wdt_reset()` puis check `mqtt.connected()` avant délégation à `mqtt.publish()`
  - **Suppression des garde-fous IT3 redondants** : ~50 lignes de `esp_task_wdt_reset()` et `if (!mqtt.connected()) return;` éparpillées dans `publishAllStatesInternal()` (20 resets + 5 bail-out) et la lambda `publishConfig` (1 bail-out) ont été supprimées — factorisées dans le wrapper, plus lisible
  - **F12 annulé** : la constante `kMqttPublishHeadersOverhead` envisagée pour un check `availableForWrite()` est **inutile** car `WiFiClient::availableForWrite()` retourne toujours 0 dans Arduino-ESP32 6.9.0 (méthode héritée de `Print::availableForWrite()` non override). Le mode non-bloquant remplace ce mécanisme
  - **Trade-off accepté** : drop silencieux des publish quand le send buffer TCP est plein. Les états retain sont republiés au cycle suivant (10 s), les alertes ponctuelles peuvent être perdues — c'était déjà le cas en mode bloquant pré-IT4 (timeout 30 s puis drop), IT4 rend le drop instantané

- **IT3 — Borne TCP write côté lwip + bail-out anticipé** — corrige un nouveau PANIC watchdog observé au re-test D2 humain APRÈS le flash IT2. Le point de blocage avait migré de `mqtt.connect()` (résolu en IT2) vers `mqtt.publish()` : coredump-5 confirme `mqttTask` bloquée dans `WiFiClient::write` → `lwip_select` lors d'un publish 33 octets ("OFF") sur send buffer TCP saturé. Le reset wdt par groupes de 5 publish d'IT2 ne suffisait pas. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 3 »
  - **`CONFIG_LWIP_TCP_MAXRTX=5` dans `platformio.ini`** (`build_flags`) : limite à 5 retransmissions TCP avant abandon de socket par lwip (~10 s cumulés au lieu de ~75 s avec la valeur par défaut 12). Trade-off : paramètre global à toute la pile lwip → impact aussi AsyncWebServer / OTA HTTP / NTP (abandon socket ~10 s vs ~75 s). Acceptable pour un firmware temps réel ; OTA en réseau très dégradé peut être avorté plus tôt et nécessiter un retry humain
  - **Cadence `esp_task_wdt_reset()` 1:1 dans `publishAllStatesInternal()`** : reset posé **avant chaque** `mqtt.publish()` (20 resets, vs 2 resets par groupes en IT2). Garantit qu'au pire un seul publish reste à l'intérieur de la fenêtre wdt 30 s
  - **Bail-out fail-fast** : 5 `if (!mqtt.connected()) return;` répartis dans `publishAllStatesInternal()` (tous les ~3-4 publish) + 1 bail-out en tête de la lambda `publishConfig` de `publishDiscovery()`. Dès que lwip ferme la socket (cf. `CONFIG_LWIP_TCP_MAXRTX=5`), les publish restants sont court-circuités au lieu d'enchaîner 14 erreurs de ~2 s chacune
  - **F7/F10 annulés** : `setsockopt(TCP_USER_TIMEOUT)` envisagé initialement pour borner par-socket (RFC 5482) — **non supporté par lwip dans ESP-IDF 4.4** (Arduino-ESP32 6.9.0). Le bornage TCP repose donc uniquement sur F6 (paramètre global lwip). Constante `kMqttTcpUserTimeoutMs` non créée

- **Durcissement watchdog `mqttTask` sur broker injoignable** — corrige un PANIC watchdog 30 s observé pendant le test D2 (câble Ethernet HA débranché 2–3 min) où `mqttTask` restait bloquée dans `WiFiClient::connect()` → `lwip_select` sans reset. La régulation pH/ORP, la filtration et les autres tâches n'étaient pas concernées (déjà isolées par ADR-0011), mais le PANIC du core 0 entraînait un reboot complet. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions »
  - **Timeout client TCP corrigé de 5000 s à 2 s** : `WiFiClient::setTimeout()` (Arduino-ESP32 6.9.0) attend des **secondes**, pas des ms — le code historique `setTimeout(5000)` programmait 5000 secondes (~83 min), bug latent qui rendait le timeout côté client TCP totalement inopérant. Nouvelle constante `kMqttClientConnectTimeoutSec = 2` dans `constants.h` avec commentaire d'unité explicite
  - **`esp_task_wdt_reset()` granulaire** : reset ajouté juste après le retour de `mqtt.connect()` (couvre le cas SYN TCP retransmis sur broker injoignable jusqu'à ~75 s avant abandon lwip), reset au milieu de `publishAllStatesInternal()` (≤ 5 publish entre 2 resets), reset après chaque publish individuel dans le helper `publishConfig` de `publishDiscovery()` (17 publishes auto-discovery)
  - **`connectedAtomic` single source of truth** : store canonique posé en début de `taskLoop()` à chaque tour, complété par les transitions explicites en `connectInTask()` (succès) et `disconnect()`. Suppression de 3 stores intermédiaires redondants qui créaient une fenêtre de divergence UI/WARN observée pendant D2 (UI affichait « Déconnecté » sans WARN dans les logs)
  - **Suppression de la race au boot** : `mqttManager.requestReconnect()` retiré de `setup()` dans `main.cpp` — `mqttTask` se connecte déjà toute seule au premier tour de `taskLoop()`. Élimine le double publish d'auto-discovery au démarrage (32 messages au lieu de 17)

### Documentation
- `docs/subsystems/mqtt-manager.md` + `specs/features/done/feature-014-mqtt-task-dediee.md` (D1/D2) : précision du comportement réel de détection des déconnexions MQTT après validation D2 humaine du 2026-04-29 14:12:43 — avec le mode non-bloquant IT4, l'état dominant lors d'une coupure réseau est `-4` (`MQTT_CONNECTION_TIMEOUT`, keepalive PubSubClient, ~60 s) au lieu de `-3` (`MQTT_CONNECTION_LOST`, abandon TCP lwip, 90–180 s). La dominance documentée pré-IT4 (`-3` majoritaire) est inversée. Bénéfice indirect : détection plus précoce et plus propre. Tableau « États observés selon le scénario », tableau des codes d'état et workflow troubleshooting mis à jour en conséquence ; D2 marqué VALIDÉ dans la spec avec logs de référence
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Garde-fou : `safePublish()` + socket non-bloquant (IT4) » (snippet du wrapper, `fcntl(F_SETFL, O_NONBLOCK)` après chaque connect, tableau des 24 call sites, trade-off drop silencieux) ; AVERTISSEMENT en tête de section sur `WiFiClient::availableForWrite()` qui retourne toujours 0 dans Arduino-ESP32 6.9.0 ; section « Watchdog dans `mqttTask` » mise à jour (les `esp_task_wdt_reset()` IT3 ont été factorisés dans `safePublish()`) ; section « Bornage TCP côté lwip » corrigée — `CONFIG_LWIP_TCP_MAXRTX=5` borne à ~93 s (RTO_INITIAL × backoff), pas 10 s. Le mode non-bloquant IT4 est le vrai mécanisme de protection ; section « Bail-out fail-fast pendant les salves de publish » mise à jour pour pointer vers le wrapper
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 4 — 2026-04-29 » dans « Évolutions » détaillant le pivot socket non-bloquante, le wrapper `safePublish()` sur 24 call sites, l'annulation de F12 (`availableForWrite()` cassé dans Arduino-ESP32 6.9.0), la découverte du bornage réel à ~93 s pour `MAXRTX=5`, le trade-off drop silencieux. La décision principale de l'ADR (tâche dédiée) reste retenue
- `docs/subsystems/mqtt-manager.md` : section « Watchdog dans `mqttTask` » mise à jour (cadence 1:1 dans `publishAllStatesInternal()`, 5 bail-out répartis, rationnel IT3) ; section « Détection des déconnexions » étendue avec sous-section « Bail-out fail-fast pendant les salves de publish (IT3) » ; nouvelle section « Bornage TCP côté lwip » documentant `CONFIG_LWIP_TCP_MAXRTX=5` et son impact global (AsyncWebServer / OTA / NTP) avec mention de l'absence de support `TCP_USER_TIMEOUT` dans lwip ESP-IDF 4.4
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 3 — 2026-04-29 » dans « Évolutions » détaillant les 3 fixes appliqués (F6, F8, F9), l'annulation de F7/F10 (TCP_USER_TIMEOUT non supporté par lwip ESP-IDF 4.4), le coredump-5 confirmant le déplacement du blocage de connect vers publish, et le trade-off du paramètre lwip global sur AsyncWebServer / OTA / NTP. La décision principale de l'ADR (tâche dédiée) reste retenue
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Watchdog dans `mqttTask` » (tableau des 10 emplacements de `esp_task_wdt_reset()` avec cadence garantie ≤ 5 publish entre 2 resets) ; nouvelle section « Timeout client TCP — UNITÉ EN SECONDES » avec avertissement visible pour le bug latent `setTimeout(5000)` ; section « Détection des déconnexions » étendue avec le store canonique `connectedAtomic` en début de `taskLoop()` et la suppression des stores intermédiaires ; section « Reconnexion » étendue avec la sous-section « Connexion initiale au boot » documentant la suppression du `requestReconnect()` dans `setup()`
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la section « Évolutions » → « Itération 2 — 2026-04-29 » détaillant les 5 fixes appliqués, le bug latent `WiFiClient::setTimeout()` et le coredump confirmant la cause racine. La décision principale de l'ADR (tâche dédiée) reste retenue — c'est un durcissement post-test, pas un revirement

---

## [Unreleased] - 2026-04-27

### Firmware
- **MQTT déplacé dans une tâche FreeRTOS dédiée** (`mqttTask`, core 0, prio 2, stack 8 KB) — corrige les crashes `PANIC IntegerDivideByZero` watchdog 30 s observés en production lorsqu'une publication MQTT (de 33 octets dans le 3ᵉ crash) bloquait `loopTask` dans `lwip_select` à cause de la saturation du TCP send window sur CPL bruyant. La régulation pH/ORP, la filtration et le watchdog continuent désormais indépendamment de l'état du réseau MQTT. Comportement utilisateur strictement identique : mêmes topics, mêmes payloads, mêmes intervalles, même auto-discovery HA. API publique de `MqttManager::publishXxx()` inchangée — les méthodes deviennent simplement non-bloquantes (producteurs sur queue FreeRTOS). Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md)
- **Arrêt propre OTA** : publication `status=offline` synchrone (timeout 1 s) avant `ESP.restart()` sur tous les sites concernés (OTA firmware, redémarrage mode AP, factory reset bouton, factory reset HTTP). Les clients HA voient désormais le passage à `offline` immédiatement, sans attendre les 90 s de timeout broker
- **Stabilité MQTT** : fin des déconnexions périodiques (toutes les 1–2 h) — désactivation du WiFi power save (`WiFi.setSleep(false)`), pré-résolution DNS explicite avant `WiFiClient::connect`, `setSocketTimeout` réduit à 2 s, suppression du `requestReconnect()` périodique qui réinitialisait le backoff exponentiel
- **Stabilité réseau** : latence LAN ramenée sous 10 ms (était 90–260 ms à cause du DTIM WiFi) ; loop principale jamais bloquée plus de ~7 s par opération réseau ; backoff MQTT effectif jusqu'à 120 s sur broker injoignable
- **Coredump** : ajout de la partition `coredump` dédiée (64 KB, offset `0x3F0000`) — les crashes `PANIC` sont désormais persistés en flash et accessibles via l'API HTTP ou l'UI
- **Partitions** : partition `history` réduite de 128 KB à 64 KB pour libérer l'espace nécessaire au coredump ; flash USB obligatoire pour cette migration (OTA insuffisant)
- **History** : `kMaxHourlyDataPoints` réduit de 360 à 168 (7 jours de rétention horaire, contre 15 jours précédemment) pour respecter le budget de la partition réduite
- **History** : `HistoryManager::begin()` détecte un redimensionnement de partition via NVS (`hist_meta/part_sz`) et efface le filesystem avant montage si la taille a changé — évite un crash `IntegerDivideByZero` dans `lfs_alloc`
- **Régulation** : reset journalier des compteurs `dailyPhInjectedMl` / `dailyOrpInjectedMl` désormais indépendant de l'état de la filtration — extraction de la logique de bascule de date dans `tickDailyRollover()`, appelée depuis `update()` **avant** le check `canDose()`. Le bug rendait le compteur figé sur la valeur de la veille tant que la filtration n'avait pas tourné dans la journée. La transition `currentDayDate` vide → date NTP valide remet aussi `dayStartTimestamp = 0` pour invalider tout timer fallback `millis()` accumulé depuis le boot
- **Régulation** : ajout de `saveDailyCounters()` + `armStabilizationTimer()` dans la branche fallback `millis()` du reset journalier (manquaient avant le fix)
- **Régulation** : warnings/criticals du mode `scheduled` (capteur pH/ORP hors plage, daily target plafonné, débit pompe à 0) passés en **edge-triggered** — un seul log à l'entrée dans l'état + un INFO de recovery au retour à la normale. Stoppe le spam de centaines de lignes par seconde quand l'état persistait
- **Historique** : suppression de l'appel `saveToFile()` redondant dans `HistoryManager::update()` (`consolidateData()` l'appelle déjà en interne) — moitié moins d'écritures flash sur la partition `history` toutes les 5 min
- **Historique** : trace `Consolidation terminée: N points` rétrogradée de INFO à DEBUG, suppression du marqueur `DEBUG: Début consolidation historique` ; commentaire corrigé (consolidation effective toutes les 5 min, pas « toutes les heures »)
- **Logger** : intervalle de flush ramené de 60 s à **10 min** (`kFlushIntervalMs = 600000`) — le flush immédiat sur ERROR/CRITICAL et la persistance du coredump couvrent les crashes, l'écriture flash périodique en INFO/DEBUG n'apporte rien
- **Logger** : nouvelle méthode `clearAll()` qui vide RAM + `_persistBuffer` + supprime `/system.log` et `/system.log.tmp` (la méthode existante `clear()` ne touche que le buffer RAM)
- **Diagnostic réseau** : ajout d'un handler `WiFi.onEvent()` dans `setupWiFi()` qui logge les événements `STA_DISCONNECTED` (WARN, avec `reason=N`), `STA_CONNECTED` (INFO), `STA_GOT_IP` (INFO, avec IP et RSSI), `STA_LOST_IP` (WARN). Permet de distinguer un drop Wi-Fi (séquence `DISCONNECTED → CONNECTED → GOT_IP`), un DHCP renew (`LOST_IP → GOT_IP`) ou un problème non-Wi-Fi (broker, firewall) lors d'un blackout réseau
- **Diagnostic MQTT** : log de la cause de déconnexion au front de transition `connecté → déconnecté` (`WARN: MQTT déconnecté détecté — état=N` où `N` est le code `PubSubClient::state()` : `-4` timeout keepalive, `-3` TCP fermé, `-2` TCP refusé, `-1` déconnexion propre). Court-circuit DNS quand `mqttCfg.server` est déjà une IP littérale (`IPAddress::fromString()` avant `WiFi.hostByName()`) — élimine tout cycle DNS pour les installations LAN par IP fixe
- **Stabilité MQTT** : keepalive PubSubClient relevé de 30 s à **60 s** (`mqtt.setKeepAlive(60)` dans `MqttManager::begin()`). La tolérance broker passe corrélativement de 45 s à 90 s (1.5 × keepalive côté Mosquitto), ce qui absorbe les microcoupures réseau prolongées sans déclencher de `état=-4`. Cible : chemins instables type CPL/Powerline où des publishes et PINGREQ se perdent sporadiquement à cause du bruit électrique secteur. Trade-off accepté : une vraie déconnexion (broker arrêté, WiFi coupé) est détectée en 90 s au lieu de 45 s — sans impact sur la régulation pH/ORP qui ne dépend pas d'une latence sub-minute

### Fonctionnalités
- **Paramètres → Avancé** : nouvelle card "Diagnostic crash" — statut coredump (tâche, exception, PC), boutons Actualiser / Télécharger / Effacer, hint de décodage `./tools/decode_coredump.sh`
- **Paramètres → Avancé → Logs** : bouton existant « Effacer » renommé **« Effacer (écran) »** (vide uniquement la vue navigateur) ; nouveau bouton **« Effacer (firmware) »** en rouge `btn--danger` qui appelle `DELETE /logs` après confirmation et purge intégralement les logs côté ESP32 (RAM + fichier persistant)
- **Script** : `tools/decode_coredump.sh` — décode un `coredump.bin` avec `xtensa-esp32-elf-gdb` et `esp_coredump` du penv PlatformIO

### API
- `GET /coredump/info` : résumé JSON du dernier crash (tâche, PC, cause exception)
- `GET /coredump/download` : téléchargement du binaire brut `coredump.bin` (streamé, pas d'allocation 64 KB)
- `DELETE /coredump` : effacement de la partition pour le prochain crash
- `DELETE /logs` (WRITE) : efface intégralement les logs côté ESP32 (RAM + tampon de flush + `/system.log` + `/system.log.tmp`). Réponse `{"success": true}`. Une entrée INFO `Logs effacés (RAM + fichier persistant)` est écrite immédiatement après pour tracer l'action

### Documentation
- `docs/adr/0011-mqtt-task-dediee.md` : ADR créé — déplacement de toute la logique MQTT (publish, connect, loop, callback) dans une tâche FreeRTOS dédiée `mqttTask` ; producer/consumer via deux queues (`outQueue` 32 entrées, `inQueue` 16 entrées) ; arrêt propre `status=offline` avant `ESP.restart()`. Décision motivée par 3 crashes production confirmant que `setSocketTimeout(2)` (ADR-0010) ne couvre pas `lwip_write` sur réseau lossy
- `docs/adr/0010-stabilite-mqtt-reseau.md` : note de mise à jour — l'alternative « Tâche FreeRTOS dédiée pour MQTT » écartée à l'origine est désormais retenue par ADR-0011 (Superseded for MQTT decoupling). Les fixes synchrones D1–D5 restent valides et sont préservés intégralement dans la nouvelle tâche dédiée
- `docs/subsystems/mqtt-manager.md` : refonte majeure — section « Architecture producer/consumer », règles d'or (aucun `mqtt.publish()` depuis `loopTask`, aucun acteur direct depuis `mqttTask`, aucun appel `Async*` depuis `mqttTask`), tableau des paramètres `mqttTask`, refonte gestion des commandes HA (queue entrante drainée par `loopTask`), section « Arrêt propre OTA », troubleshooting drops `outQueue`
- `docs/UPDATE_GUIDE.md` : note user-facing — stabilité réseau améliorée (la régulation continue de tourner même en cas de microcoupures broker MQTT)
- `docs/adr/0010-stabilite-mqtt-reseau.md` : ADR créé — décisions de stabilité réseau (WiFi sans power save, pré-résolution DNS, backoff non réinitialisé)
- `docs/adr/0009-partition-coredump.md` : ADR créé — table de partitions avec coredump + conséquences de migration
- `docs/adr/0007-table-partitions-custom.md` : statut mis à jour → `Superseded by ADR-0009`
- `docs/subsystems/history.md` : partition 64 KB, rétention 7 jours, section protection redimensionnement, budget partition ; clarification que `consolidateData()` appelle `saveToFile()` en interne et que la trace de fin est en DEBUG
- `docs/subsystems/logger.md` : flush 10 min, flush immédiat ERROR/CRITICAL, `_persistBuffer` borné, fichier log 16 KB ; ajout de `clearAll()` et de l'endpoint `DELETE /logs`
- `docs/subsystems/pump-controller.md` : section "Reset journalier" décrivant `tickDailyRollover()` et son emplacement dans `update()` avant `canDose()` ; section "Warnings edge-triggered" listant les six logs concernés et le pattern `static bool`
- `docs/subsystems/mqtt-manager.md` : section "Pattern de connexion (DNS séparé du TCP)" ; clarification que la reconnexion est 100 % pilotée par `update()` ; lien vers ADR-0010
- `docs/API.md` : section "Diagnostic crash (coredump)" avec les 3 endpoints + nouvelle entrée `DELETE /logs`
- `docs/features/page-settings.md` : card "Diagnostic crash" dans le panneau Avancé + section dédiée à la card Logs (4 boutons : Actualiser, Effacer (écran), Télécharger, Effacer (firmware))
- `docs/subsystems/logger.md` : nouvelle section « Logs WiFi » documentant les 4 messages émis par le handler `WiFi.onEvent()` et les codes de raison Wi-Fi courants
- `docs/subsystems/mqtt-manager.md` : note de troubleshooting — vérifier d'abord les logs WiFi (`WARN: WiFi déconnecté (reason=...)`) avant de chercher la cause d'une reconnexion MQTT répétée
- `docs/subsystems/mqtt-manager.md` : section « Diagnostic — Codes d'état PubSubClient » (codes `-4` à `-1` du log `MQTT déconnecté détecté`), tableau du chemin DNS selon IP littérale vs hostname, workflow de troubleshooting basé sur le code d'état
- `docs/subsystems/logger.md` : nouvelle section « Logs MQTT » listant les 6 messages émis par `MqttManager` (incluant le nouveau `MQTT déconnecté détecté — état=N` edge-triggered)
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Keepalive » (60 s côté client, 90 s de tolérance broker, trade-off de détection des vraies déconnexions) ; précision dans le tableau des codes d'état (`-4` = sans PINGREQ/PONG dans la fenêtre 90 s) ; ajout d'une étape 3 au workflow troubleshooting couvrant les chemins physiques instables (CPL/Powerline, RSSI marginal) quand WiFi et broker sont innocentés
- `docs/subsystems/mqtt-manager.md` + `specs/features/done/feature-014-mqtt-task-dediee.md` (D1/D2) : précision du comportement réel de détection des déconnexions MQTT — état dominant `-3` (`MQTT_CONNECTION_LOST`, socket TCP invalide), `-4` (timeout keepalive) quasi-inatteignable car `mqtt.connected()` est testé avant `mqtt.loop()` dans `taskLoop()` ; délai typique 100–180 s sur arrêt brutal du broker (RTO TCP lwip, pas keepalive applicatif), quasi-immédiat sur fermeture propre TCP. Nouvelle section « Détection des déconnexions » dans la doc subsystem

---

## [Unreleased] - 2026-04-24

### Firmware
- **Persistance compteurs journaliers** : `dailyPhInjectedMl` et `dailyOrpInjectedMl` sont désormais persistés en NVS (namespace `pool-daily`) — les compteurs survivent aux reboots ESP32 et sont restaurés si le jour calendaire est identique
- **Reset journalier** : aligné sur minuit local (RTC/NTP) au lieu d'une fenêtre glissante 24 h ; `armStabilizationTimer()` est armé au passage de minuit (mitigation double quota)
- **`kMinValidEpoch`** : constante consolidée dans `src/constants.h` (valeur : 1700000000, 14 nov. 2023)
- **Raison du dernier reboot** : champ `reset_reason` ajouté dans le payload WebSocket `sensor_data` — valeurs possibles : `POWER_ON`, `SW_RESET`, `WATCHDOG`, `BROWNOUT`, `PANIC`, `DEEP_SLEEP`, `EXTERNAL`, `UNKNOWN` ; constant pendant le runtime

### Fonctionnalités
- **Pages /ph et /orp** : les blocs Statistiques sont grisés (`opacity: 0.5`) lors d'une déconnexion WebSocket — indique visuellement que les données affichées ne sont plus à jour
- **Toast reboot inattendu** : un toast dismissable s'affiche une fois par session si le champ `reset_reason` indique un reboot inattendu (`WATCHDOG`, `BROWNOUT` ou `PANIC`) — libellé : « Redémarrage inattendu détecté (raison : X) »
- **Régulation pH** : remplacement du toggle binaire `ph_enabled` par un sélecteur de mode à 3 valeurs (`automatic` / `scheduled` / `manual`)
- **Régulation pH** : mode Programmée — volume quotidien configurable (mL), injecté pendant les plages de filtration jusqu'au quota journalier
- **Régulation pH** : migration automatique au premier boot : `ph_enabled=true` → `automatic`, `ph_enabled=false` → `manual`
- **Limites horaires** : renommage `phInjectionLimitSeconds` → `phInjectionLimitMinutes` (idem ORP) — les limites sont désormais saisies en minutes (1–60) au lieu de secondes ; migration NVS transparente au boot (`ph_limit_sec` → `ph_limit_min`)
- **Protection pompes** : suppression de `minPauseBetweenMs` — la pause inter-injections configurable est retirée ; la protection contre le short-cycling reste assurée par `minInjectionTimeMs` (30 s) et `maxCyclesPerDay` (20/24 h)
- **MQTT** : publication des champs `ph_regulation_mode` et `ph_daily_target_ml` dans `publishTargetState()`
- **Sécurité** : suppression du log du mot de passe WiFi en clair dans les traces de reconnexion
- **Régulation pH (Programmée)** : refonte de l'algorithme d'injection — la pompe injecte librement pendant la filtration jusqu'à atteindre le quota journalier (`phDailyTargetMl`), sans répartition sur 24 h ; la limite horaire (`phInjectionLimitMinutes`) reste la seule barrière contre l'injection rapide
- **Régulation ORP** : remplacement du toggle binaire `orp_enabled` par un sélecteur de mode à 3 valeurs (`automatic` / `scheduled` / `manual`)
- **Régulation ORP** : mode Programmée — volume quotidien de chlore configurable (mL), aveugle au capteur ORP, borné par `maxChlorineMlPerDay` ; PID réinitialisé au retour en mode automatique
- **Régulation ORP** : migration automatique au premier boot : `orp_enabled=true` → `automatic`, `orp_enabled=false` → `manual` ; champ `orp_enabled` conservé comme miroir pour compatibilité HA
- **MQTT** : publication des champs `orp_regulation_mode` et `orp_daily_target_ml` dans `publishTargetState()`

### API
- `GET /get-config` / `POST /save-config` : `ph_limit_seconds` → `ph_limit_minutes`, `orp_limit_seconds` → `orp_limit_minutes` ; suppression de `min_pause_between_min`
- WebSocket config : mêmes renommages (`ph_limit_minutes`, `orp_limit_minutes`)
- `GET /get-config` : ajout des champs `orp_regulation_mode`, `orp_daily_target_ml`, `max_orp_ml_per_day`, `orp_cal_valid`
- `POST /save-config` : validation de `orp_regulation_mode` (enum), `orp_daily_target_ml` (borné par `max_orp_ml_per_day`, HTTP 400 si dépassé)

### Fonctionnalités
- **Page pH** : sélecteur de mode régulation (Automatique / Programmée / Manuelle) avec sous-blocs conditionnels
- **Page pH** : mode Programmée avec saisie du volume quotidien (mL) borné par la limite journalière configurée
- **Paramètres** : champs durée max pH/ORP en minutes (1–60 min/h) au lieu de secondes ; suppression du champ « Pause entre deux injections »
- **Page ORP** : refonte complète — architecture 4 cartes (Statistiques compact / Régulation / Historique / Calibration conditionnelle)
- **Page ORP** : sélecteur de mode régulation (Automatique / Programmée / Manuelle) avec sous-blocs conditionnels (symétrie avec page pH)
- **Page ORP** : mode Programmée avec saisie du volume quotidien de chlore (mL), borné par la limite journalière de sécurité
- **Page ORP** : calibration accessible uniquement en mode Automatique (bouton Calibrer dans le sous-bloc Automatique) ; carte Calibration en superposition pendant le protocole
- **Page ORP** : bloc Statistiques compact (ORP actuelle + Dosage du jour) en en-tête de page, hors carte

---

## [1.1.0] - 2026-03-29

### Firmware
- **MQTT** : ajout des topics publiés `ph_dosing`, `orp_dosing`, `ph_limit`, `orp_limit`, `ph_target`, `orp_target`
- **MQTT** : ajout des topics de commande `ph_target/set` et `orp_target/set` (modification des consignes pH et ORP depuis HA ou MQTT)
- **MQTT** : correction du switch "Filtration Marche/Arrêt" — la commande `OFF` forçait l'arrêt de la filtration mais elle redémarrait immédiatement selon le planning
- **Home Assistant Auto-Discovery** : ajout de 6 nouvelles entités (Dosage pH Actif, Dosage Chlore Actif, Limite Journalière pH, Limite Journalière Chlore, Consigne pH, Consigne ORP)

### Documentation
- `docs/MQTT.md` : documentation complète des topics publiés, commandes et entités Home Assistant avec les noms tels qu'ils apparaissent dans l'interface HA
- `docs/API.md` : réécriture complète — tous les endpoints documentés (30+)
- `docs/UPDATE_GUIDE.md` : mise à jour avec les modes OTA de `deploy.sh`
- `deploy.sh` : ajout des modes `ota-firmware`, `ota-fs`, `ota-all` (compile + envoi OTA en une commande)
- Renommage `quick_update.sh` → `ota_update.sh`

---

## [1.0.3] - 2026-03-27

### Firmware
- **Factory reset** : détection par appui long 10s pendant le fonctionnement normal — plus besoin de couper l'alimentation
- Suppression des constantes `PH_SENSOR_PIN` / `ORP_SENSOR_PIN` (vestiges ADC interne non utilisés depuis le passage à l'ADS1115 I2C)

### Documentation
- Procédure factory reset mise à jour (fonctionnement runtime)
- Section Matériel Requis : schéma électronique et PCB illustrés, liens vers fichiers Gerber et STL
- `build_all.sh` documenté dans BUILD.md et UPDATE_GUIDE.md

---

## [1.0.1] - 2026-03-26

### Firmware
- **Bouton factory reset (GPIO32)** : appui de 10 secondes au démarrage pour réinitialisation usine complète
  - LED intégrée clignote pendant l'appui pour indiquer la progression
  - Efface la partition NVS (mot de passe, WiFi, MQTT, calibrations)
  - Préserve les consignes, limites et l'historique des mesures
  - L'ESP32 redémarre en mode AP avec l'assistant de configuration

### Hardware
- Ajout des fichiers Gerber (fabrication PCB) dans le dossier `hardware/`
- Ajout des fichiers STL du boîtier v3 (corps + couvercle) dans le dossier `hardware/`

---

## [1.0.0] - 2026-03-24 — Première release publique

### Fonctionnalités
- Régulation automatique pH et ORP (chlore) via algorithme PID
- Gestion filtration (auto / manuel / off) avec programmation horaire
- Contrôle éclairage avec programmation horaire
- Interface web avec tableau de bord temps réel (graphiques pH, ORP, température)
- Intégration Home Assistant via MQTT Auto-Discovery
- Mises à jour OTA via interface web (firmware et filesystem)
- Assistant de configuration au premier démarrage (mot de passe, WiFi, heure)
- Protocole UART pour écran LVGL externe
- Historique des mesures sur partition dédiée (préservé lors des mises à jour)
- Alertes MQTT en cas d'anomalie (valeurs aberrantes, limites atteintes, mémoire faible)
- Factory reset via bouton physique GPIO32
