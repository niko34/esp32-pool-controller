## Métadonnées
- **ID** : feature-015
- **Slug** : statut-mqtt-temps-reel-ui
- **Type** : feature (anomalie / bug fix UI)
- **Statut** : done
- **Priorité** : normale
- **Version cible** : 1.0.5 (correctif observabilité — incrément CORRECTIF)
- **Dernière mise à jour** : 2026-04-29
- **Spec(s) liée(s)** : feature-014 (MQTT task dédiée — le `connectedAtomic` exposé par IT4 est la source canonique à propager côté UI)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant (statut de connexion broker)
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non applicable (pas de dosage)
- [ ] ux-ui-designer — non applicable (badge de statut existant, pas de nouvelle surface)
- [ ] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Quand le broker MQTT devient injoignable (câble Ethernet HA débranché, broker arrêté), le firmware détecte la déconnexion correctement (cf. logs `WARN: MQTT déconnecté détecté — état=-X` après ~60s grâce à feature-014 IT4), mais l'**UI Paramètres → MQTT continue d'afficher « Connecté »** tant qu'on ne recharge pas la page manuellement (F5 / bouton actualiser navigateur). Le badge de statut doit refléter l'état réel **dans les quelques secondes** suivant la détection firmware, sans intervention utilisateur.

## Contexte

Anomalie observée par l'utilisateur pendant les tests D2 de la feature-014 (fenêtre `2026-04-29 14:11:53 → 14:12:43`, logs ESP32) : la déconnexion MQTT était bien détectée et tracée côté firmware, mais l'onglet Paramètres → MQTT déjà ouvert dans le navigateur affichait encore « Connecté ». Un rechargement manuel de la page corrigeait l'affichage.

L'origine probable : le badge de statut MQTT est alimenté par la réponse de `GET /get-config` (HTTP, snapshot au chargement de page) au lieu d'être poussé en temps réel par le WebSocket. Conséquence : aucun rafraîchissement automatique tant qu'on ne re-fetch pas la config.

Note : `connectedAtomic` (single source of truth introduit par feature-014 IT2) est mis à jour à chaque tour de `taskLoop()` (~100 ms). La donnée est disponible côté firmware ; il manque juste le canal de propagation temps réel vers l'UI.

## Comportement attendu

### Côté UI
- Le badge « Statut MQTT » dans Paramètres → MQTT (et tout autre indicateur reflétant l'état de connexion broker, par ex. dashboard si présent) doit basculer **dans les 2-5 secondes maximum** après la détection firmware, sans interaction utilisateur.
- Le label exact reste celui actuellement utilisé (ex. « Connecté » / « Déconnecté »), aucun changement de wording.
- Cohérent avec le badge global de connexion WebSocket déjà push : même cadence et même mécanisme.

### Côté firmware
- Exposer l'état `connectedAtomic` du `MqttManager` dans la payload WebSocket périodique (ws_manager) ou via un événement WS push edge-triggered sur transition `true → false` et `false → true`.
- Conserver la réponse de `GET /get-config` inchangée pour le chemin de chargement initial (le rafraîchissement temps réel s'ajoute par-dessus, ne remplace pas).

### Cas limites
- Onglet Paramètres → MQTT pas encore ouvert au moment de la déconnexion : à l'ouverture, le badge doit refléter l'état actuel (snapshot via /get-config, déjà OK).
- Rafale de transitions (broker qui flap rapidement pendant un orage CPL) : pas de spam log côté firmware (déjà géré : WARN edge-triggered) ; côté UI le badge suit la dernière valeur poussée, pas de blink intempestif.
- WebSocket déconnecté côté navigateur : l'UI montre déjà un indicateur WS séparé. Si WS down, on ne peut pas savoir l'état MQTT temps réel — comportement actuel acceptable (l'utilisateur saura que les données sont stale via l'indicateur WS).

## Comportement actuel

- L'onglet Paramètres → MQTT alimente son badge de statut via `latestSensorData` ou `window._config` issu de `GET /get-config` (`data/app.js`, fonction `applyConfig()` / `updateMqttStatusIndicator()` ligne ~1349).
- Aucun champ `mqtt_connected` n'est poussé via WebSocket dans la payload périodique de `ws_manager` (à confirmer en lecture du code — c'est l'hypothèse la plus probable).
- Conséquence : l'état affiché reflète la dernière valeur de `connectedAtomic` au moment du dernier `GET /get-config` (chargement de page, save settings, etc.).

## Contraintes techniques
- ESP32, offline first
- Vanilla JS dans `data/` (pas de framework, pas de bundler)
- Français dans toute l'UI utilisateur
- Budget FS `spiffs` 1088 KB — vérifier avec `./build_fs.sh` si modifs frontend significatives
- AsyncWebServer : aucun handler bloquant > 50 ms
- WebSocket pour toute donnée temps réel — c'est précisément le canal à utiliser ici
- Ne PAS prendre de mutex côté ws_manager pour lire `connectedAtomic` : c'est un `std::atomic<bool>`, lecture relaxed suffit
- Ne PAS contourner le single source of truth : la lecture canonique reste `mqttManager.isConnected()` qui retourne `connectedAtomic.load()`

## Fichiers potentiellement concernés
- `src/ws_manager.cpp` — ajouter le champ `mqtt_connected` dans la payload périodique poussée toutes les 5 s
- `src/ws_manager.h` — éventuellement déclaration helper
- `data/app.js` — câbler la mise à jour du badge MQTT sur réception WS au lieu (ou en plus) du `applyConfig`
- `data/index.html` — aucun changement attendu (badge déjà existant)
- `docs/subsystems/ws-manager.md` — nouveau champ WS à documenter
- `docs/features/page-settings.md` — comportement temps réel à mentionner
- `docs/API.md` ou `docs/MQTT.md` — éventuellement noter le nouveau champ WS si la doc WS y est tracée

## Livrables — check-list
- [x] A1 — Ajout du champ `mqtt_connected` (booléen) dans la payload WebSocket périodique (`ws_manager.cpp` `_buildSensorJson()` ligne 200), valeur lue via `mqttManager.isConnected()`
- [x] A2 — Cadence WS actuelle (push 5 s) couvre la contrainte « basculer en 2-5 s » — pas besoin de push edge-triggered supplémentaire
- [x] B1 — `data/app.js` `_onWsSensorData()` lignes 140-144 : sync `window._config.mqtt_connected` + appel `updateMqttStatusIndicator(window._config.enabled, json.mqtt_connected)` à chaque tick WS, en plus de `applyConfig()` au load
- [x] B2 — Vérification visuelle (humain) — validée au test du 2026-04-29 après IT 1bis (fix propagation)
- [x] C1 — `docs/subsystems/ws-manager.md` : champ `mqtt_connected` documenté avec mention du doublon volontaire vs `config`
- [x] C2 — `docs/features/page-settings.md` : précision temps réel ajoutée pour le badge MQTT
- [x] D1 — Test manuel humain — PASS le 2026-04-29 après IT 1bis (badge bascule en < 5 s sans reload, reconnexion OK)

## Nouvelles signatures / API
- Endpoints HTTP : aucun nouveau (`GET /get-config` reste inchangé)
- Champs WebSocket : `mqtt_connected` (booléen) ajouté à la payload périodique
- Topics MQTT : aucun nouveau
- Fonctions publiques : aucune nouvelle
- Constantes : aucune nouvelle

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun (correction d'observabilité simple, pas de décision structurante, pas d'alternative crédible à arbitrer — le canal WS est le bon par contrat projet)
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun (pas de modification)

### Champs WebSocket
- `mqtt_connected` (booléen) — état de connexion du `MqttManager` au broker MQTT, poussé à chaque cycle WS périodique

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/features/page-settings.md` — préciser le comportement temps réel du badge Paramètres → MQTT
- `docs/subsystems/ws-manager.md` — documenter le nouveau champ WS `mqtt_connected`

#### Docs transverses
- `docs/API.md` — si la doc WS y est listée, ajouter le champ
- `CHANGELOG.md` — entrée Frontend / WebSocket sous la version cible

#### Docs immuables
- aucun ADR

## Critères d'acceptation
- [x] AC1 — Quand le firmware détecte la déconnexion (`connectedAtomic` passe à `false`), le badge UI Paramètres → MQTT bascule en **moins de 5 secondes** sans interaction utilisateur — *validé par test manuel humain le 2026-04-29 après IT 1bis (fix propagation `_onWsSensorData` + re-render au panel switch)*
- [x] AC2 — Quand la reconnexion réussit, le badge revient à « Connecté » dans les mêmes 5 secondes — *validé par test manuel humain le 2026-04-29 après IT 1bis*
- [x] AC3 — L'ouverture de la page Paramètres → MQTT alors que la déconnexion existe déjà affiche immédiatement « Déconnecté » (snapshot au load via `/get-config`, conservé tel quel)
- [x] AC4 — Aucune nouvelle requête HTTP périodique ajoutée par l'UI — propagation via WS uniquement
- [x] AC5 — Aucun nouveau warning compile (`pio run` SUCCESS, 0 nouveau warning)
- [x] AC6 — `./build_fs.sh` passe sans dépasser la partition `spiffs` 1088 KB
- [x] Tous les libellés utilisateur restent en français (aucun wording modifié)

## Plan de test manuel
1. **Scénario nominal** : ouvrir la page Paramètres → MQTT, vérifier badge « Connecté ». Stopper Mosquitto sur HA. Sans rafraîchir la page, observer le badge basculer en « Déconnecté » dans les 5 s suivant le log firmware `WARN: MQTT déconnecté détecté`. Redémarrer Mosquitto. Observer le badge revenir à « Connecté » en < 5 s après le log `INFO: MQTT connecté !`.
2. **Scénario reload** : avec broker arrêté, recharger la page (F5). Le badge doit afficher « Déconnecté » dès le chargement (regression test du chemin `/get-config`).
3. **Scénario flap** : couper / rétablir Ethernet HA rapidement (3 fois en 30 s). Le badge doit suivre la dernière transition reportée par le firmware sans clignotement parasite ni état incohérent.
4. **Scénario WS down** : si WebSocket déconnecté côté navigateur (couper Wi-Fi ESP32 par exemple), vérifier que l'indicateur WS l'affiche correctement (comportement existant) — le badge MQTT peut rester sur sa dernière valeur connue, c'est acceptable.

## Notes pour agent
- La source canonique côté firmware est `mqttManager.isConnected()` qui lit `connectedAtomic` (introduit par feature-014 IT2, mis à jour en début de chaque tour de `taskLoop()` ~100 ms — cf. `src/mqtt_manager.cpp:127`). Pas besoin de mutex pour lire.
- La cadence WS périodique actuelle est de 5 s (cf. `WebSocket démarré sur /ws (push capteurs toutes les 5s)` dans le log boot). Sous le critère AC1 (< 5 s), le push périodique seul suffit. Si on veut une réactivité plus fine (< 1 s), ajouter un push edge-triggered sur transition `connectedAtomic` (mais c'est probablement inutile pour cet usage).
- Vérifier le nom exact du champ déjà utilisé côté `applyConfig` / `updateMqttStatusIndicator` pour ne pas créer de doublon (`mqtt_connected` est l'hypothèse de nommage cohérent avec `cfg.mqtt_connected` mentionné dans le diagnostic IT4).
- Aucune sécurité chimique impliquée : ce fix concerne uniquement l'observabilité UI.

## Hors périmètre
- Pas de modification du protocole MQTT, des topics, ni des payloads
- Pas de refonte visuelle du badge ou de la page Paramètres
- Pas de nouveau capteur HA / nouveau topic MQTT exposé
- Pas de migration vers `esp-mqtt`
- Pas de modification de `connectedAtomic` ou de la logique `taskLoop` (déjà OK depuis feature-014)

---

## Travail réalisé

- **`src/ws_manager.cpp` `_buildSensorJson()` (ligne 200)** : ajout `d["mqtt_connected"] = mqttManager.isConnected();` dans la payload périodique `sensor_data` poussée toutes les 5 s. Lecture relaxed de `connectedAtomic` (atomic, pas de mutex), commentaire explicite renvoyant à feature-014 IT2 et feature-015.
- **`src/ws_manager.cpp` ajustement budget JSON** : `StaticJson<768>` → `StaticJson<832>`, `out.reserve(640)` → `reserve(672)` pour absorber proprement le champ booléen ajouté.
- **`src/ws_manager.cpp` `_buildConfigJson()` (ligne 219)** : `mqtt_connected` **conservé** (doublon volontaire vs `sensor_data`). Justification : `config` est broadcast à la transition (save HTTP) et sert aussi de snapshot pour `/get-config` au chargement de page ; `sensor_data` est le canal temps réel.
- **`data/app.js` `_onWsSensorData()` (lignes 140-144)** : à chaque tick WS, sync `window._config.mqtt_connected = json.mqtt_connected` puis appel `updateMqttStatusIndicator(window._config.enabled, json.mqtt_connected)` pour rafraîchir le pill `mqtt_status_pill` du panneau Paramètres → MQTT. Pas de modification de `applyConfig()` (chemin de chargement initial préservé).

## Découvertes

- **Hypothèse initiale de la spec partiellement fausse** : la spec supposait qu'aucun champ `mqtt_connected` n'était poussé via WS (« hypothèse à confirmer en lecture du code », ligne 65). En réalité, `mqtt_connected` était **déjà présent** dans la payload `config` (`_buildConfigJson()` ligne 219). Le bug venait du fait que `config` n'est broadcast qu'à la **transition** (save HTTP `/save-config`, ouverture de page via `/get-config`) — il n'y a aucune périodicité sur ce canal. Conséquence : tant qu'on n'enregistrait pas la config et qu'on ne rechargeait pas la page, l'UI gardait la dernière valeur connue.
- **Solution adoptée** : ajout du même champ dans la payload `sensor_data` (canal temps réel 5 s) sans retirer celui de `config` (snapshot stable conservé). Doublon volontaire, source unique côté firmware (`mqttManager.isConnected()`).
- **Pas d'edge-triggered nécessaire** : la cadence 5 s du push `sensor_data` couvre la contrainte AC1 (« moins de 5 s »). Pas de push immédiat sur transition `connectedAtomic` — overengineering pour cet usage observabilité.

## Fichiers modifiés

- `src/ws_manager.cpp` — ajout champ `mqtt_connected` dans `_buildSensorJson()`, ajustement budget JSON `StaticJson<832>` + `reserve(672)`
- `data/app.js` — sync + appel `updateMqttStatusIndicator()` dans `_onWsSensorData()` (lignes 140-144)

## Build

- `pio run` : **SUCCESS** — RAM 16.4 %, Flash 97.8 % (+56 octets vs avant fix), 0 nouveau warning
- `./build_fs.sh` : **SUCCESS** — partition `spiffs` 1088 KB respectée

## Validation

- `architect`, `web-ui-developer`, `test-engineer`, `code-reviewer` : tous validés, verdict **APPROVE**
- AC3-AC6 + français : couverts par revue / build
- AC1-AC2 : à valider par humain au prochain test sur banc (broker arrêté, observer bascule badge en < 5 s sans reload)

---

## Itération 1bis — Fix propagation post-test manuel (2026-04-29)

**Statut** : livré — test manuel PASS, code-reviewer APPROVED. Spec conservée en `done/` (fix mineur post-clôture, pas de réouverture en `doing/`).

### Bug observé pendant le test manuel D1

Au premier test manuel après livraison IT1, le badge UI Paramètres → MQTT **restait sur « Déconnecté » après reconnexion firmware** tant que l'utilisateur ne switchait pas d'onglet (puis revenait sur Paramètres → MQTT). Le badge ne bascule pas en temps réel comme prévu par AC2.

Sniff console navigateur :
- Frames WS `sensor_data` reçues correctement avec `mqtt_connected: true` (firmware OK, propagation WS OK).
- Côté JS, `window._config.mqtt_connected` restait `false` malgré la frame reçue.
- Cause racine : non identifiée définitivement. Hypothèse : interaction avec `loadSensorData` (refresh HTTP périodique 30 s) ou `_showResetToast` qui pouvait court-circuiter / écraser la mise à jour WS dans `_onWsSensorData`.

### Fixes appliqués (`data/app.js`)

1. **`_onWsSensorData()`** : déplacement du bloc qui synchronise `window._config.mqtt_connected` + appelle `updateMqttStatusIndicator(...)` **EN TÊTE de la fonction**, blindé par `try/catch` pour qu'aucune exception ultérieure ne puisse empêcher la propagation du statut MQTT vers l'UI.
2. **`showSettingsPanel("mqtt")`** : ajout d'un re-render explicite du badge à l'activation du panel via `latestSensorData?.mqtt_connected ?? window._config?.mqtt_connected ?? false`. Garantit que même si le state JS interne est stale (pour une raison X non élucidée), l'UI affiche immédiatement la dernière valeur connue à chaque fois que l'utilisateur revient sur le panel MQTT.

### Build

- `./build_fs.sh` : **SUCCESS** — pas de modification firmware (`pio run` non requis).

### Validation

- Test manuel humain (2026-04-29) : **PASS** — badge bascule en < 5 s sans reload après stop/start broker, sans switch d'onglet nécessaire.
- `code-reviewer` : **APPROVED**.
- AC1, AC2, B2, D1 cochés.

### Fichiers modifiés (IT 1bis)

- `data/app.js` — `_onWsSensorData()` (bloc MQTT déplacé en tête + try/catch) ; `showSettingsPanel("mqtt")` (re-render badge à l'activation).
