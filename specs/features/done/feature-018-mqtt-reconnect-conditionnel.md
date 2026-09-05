## Métadonnées
- **ID** : feature-018
- **Slug** : mqtt-reconnect-conditionnel
- **Type** : feature (optimisation / bug fix)
- **Statut** : done
- **Priorité** : normale
- **Version cible** : 1.0.6
- **Dernière mise à jour** : 2026-04-30
- **Spec(s) liée(s)** : feature-014 (MQTT task dédiée), feature-015 (statut MQTT temps réel UI — a rendu le bug visible), feature-017 (toggle DEBUG — exemple récent de save qui déclenche un reconnect inutile)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket (handler `POST /save-config`)
- [x] MQTT / Home Assistant (réduction des reconnects intempestifs)
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — non applicable (aucun chemin de dosage touché)
- [ ] ux-ui-designer — non applicable (pas de surface UI nouvelle)
- [x] embedded-developer
- [ ] web-ui-developer — non applicable (pas de modif frontend)
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Le firmware déclenche actuellement `mqttManager.requestReconnect()` après **TOUT** save de config via `POST /save-config`, même quand seuls des paramètres non-MQTT ont changé (modes de régulation pH/ORP, NTP, logs sondes, logs DEBUG, écran LVGL, etc.). Conséquence : disconnect MQTT propre + reconnect + republication des **17 messages discovery HA** systématiquement, à chaque save UI.

Cette spec corrige le comportement pour que `requestReconnect()` ne soit déclenché **que si** un champ MQTT a effectivement changé (`server`, `port`, `username`, `password`, `topic`, `enabled`).

## Contexte

Observation utilisateur le 2026-04-30 :
- Avant feature-015 IT1bis (statut MQTT temps réel UI), le badge MQTT lisait l'état uniquement au load `/get-config` → les transitions transitoires (~1s pendant un reconnect) étaient invisibles
- Après feature-015 IT1bis, le badge bascule en temps réel via push WS → l'utilisateur voit désormais chaque mini-déconnexion au save de config
- Logs ESP32 concrets (3 saves successifs sur paramètres NON-MQTT) :
  ```
  15:16:10 INFO: Logs DEBUG: désactivés
  15:16:10 INFO: MQTT déconnecté (reconnect demandé)
  15:16:10 INFO: MQTT connecté !
  ... 17 lignes de Discovery republiées ...
  15:16:44 INFO: Logs DEBUG: activés
  15:16:44 INFO: MQTT déconnecté (reconnect demandé)
  ... 17 lignes de Discovery ...
  15:17:01 INFO: Logs DEBUG: désactivés
  15:17:01 INFO: MQTT déconnecté (reconnect demandé)
  ... 17 lignes de Discovery ...
  ```

Aucun de ces 3 saves n'a touché aux paramètres MQTT. Pourtant la session MQTT a été coupée 3 fois en 51 secondes, avec republication des 17 messages discovery à chaque fois. C'est sub-optimal :
- Charge inutile sur le broker Mosquitto
- Bruit MQTT côté HA (entités potentiellement re-sourcées)
- Bref état « Déconnecté » visible côté UI Paramètres → MQTT
- Latence avant que les états retain ne soient republiés

Avant feature-015, le comportement existait déjà mais n'était pas visible. Maintenant que l'observabilité est correcte (et c'est volontaire), on doit aussi corriger le côté firmware pour ne reconnecter que quand c'est nécessaire.

## Comportement attendu

### Côté firmware (`src/web_routes_config.cpp` handler `POST /save-config`)

1. Avant d'appliquer la config reçue, **snapshotter** les valeurs MQTT actuelles : `mqttCfg.server`, `mqttCfg.port`, `mqttCfg.username`, `mqttCfg.password`, `mqttCfg.topic`, `mqttCfg.enabled`.
2. Appliquer la config (parsing JSON + écriture struct + saveMqttConfig + saveAuthConfig + autres saves).
3. Comparer les valeurs **après** application aux valeurs snapshottées avant.
4. **Si au moins un champ MQTT a changé** : appeler `mqttManager.requestReconnect()` comme aujourd'hui.
5. **Si aucun champ MQTT n'a changé** : ne rien faire (la session MQTT continue).

### Cas limites

- Save où l'utilisateur a touché à un champ MQTT (même typo : tape un caractère puis efface) : si la valeur finale est identique à l'avant, pas de reconnect. Cohérent.
- Save MQTT enabled false → true : reconnect (la connexion doit s'établir).
- Save MQTT enabled true → false : reconnect demandé pour fermer la session proprement (à confirmer par architect : peut-être `mqttManager.disconnect()` directement, sans reconnect).
- Save où un champ MQTT change ET d'autres paramètres aussi : un seul reconnect (même comportement qu'aujourd'hui).
- Save sans aucun champ MQTT dans le payload (cas typique : save logs, NTP, modes régulation) : pas de reconnect.

### Côté UI

Aucun changement nécessaire. Le badge MQTT reste sur « Connecté » sans transition, l'expérience utilisateur est plus fluide.

### Cas particulier `mqtt_enabled` true → false

À arbitrer par architect : faut-il `requestReconnect()` ou `disconnect()` direct ? `requestReconnect()` ne reconnecte pas si `mqttCfg.enabled == false` (cf. `connectInTask()` early return). Donc `requestReconnect()` est suffisant : il ferme la session existante, et la prochaine itération de `taskLoop` ne tentera pas de reconnect.

## Comportement actuel

Dans `src/web_routes_config.cpp`, le handler `POST /save-config` appelle `mqttManager.requestReconnect()` à la fin du save **inconditionnellement** (à confirmer par lecture du code par architect / spec-reader). Conséquence : 3 saves en 51s = 3 reconnects + 51 messages discovery republiés.

## Contraintes techniques

- ESP32, offline first
- Conserver le comportement de `requestReconnect()` lui-même (utilisé par d'autres call sites éventuels)
- Le snapshot doit être fait **avant** parsing JSON / application (sinon les nouvelles valeurs ont déjà écrasé les anciennes)
- Ne pas tenir le `configMutex` sur la durée du snapshot+save+compare (déjà géré par les saveXxxConfig actuels)
- Aucune nouvelle constante de seuil
- Pas de modif de protocole MQTT, de topics, ou de payload
- Pas de modif frontend
- Watchdog 30s respecté

## Fichiers potentiellement concernés

- `src/web_routes_config.cpp` — handler `POST /save-config` : ajout du snapshot + comparaison + appel conditionnel
- `docs/subsystems/mqtt-manager.md` — préciser que `requestReconnect()` n'est plus systématique post-save
- `docs/API.md` — éventuelle mise à jour si le comportement de `/save-config` y est documenté
- `CHANGELOG.md` — entrée Firmware

Pas de modif :
- `src/mqtt_manager.cpp` / `src/mqtt_manager.h` (le `requestReconnect()` reste tel quel)
- `data/index.html` / `data/app.js` (pas de modif UI)
- Pas de nouvelle constante (`src/constants.h` non concerné)

## Livrables — check-list

- [x] A1 — Snapshot des 6 champs MQTT (`server`, `port`, `username`, `password`, `topic`, `enabled`) au début du handler `POST /save-config`
- [x] A2 — Comparaison après application : booléen `mqttChanged`
- [x] A3 — Appel conditionnel `mqttManager.requestReconnect()` uniquement si `mqttChanged == true`
- [x] A4 — Log info FR : si reconnect déclenché : « MQTT reconnect demandé (config MQTT modifiée) » ; sinon ne rien logger (pas de spam)
- [ ] D1 — Test manuel : modifier un champ non-MQTT (ex. mode régulation) → vérifier qu'il n'y a PAS de `MQTT déconnecté (reconnect demandé)` dans les logs
- [ ] D2 — Test manuel : modifier un champ MQTT (ex. topic) → vérifier qu'il y a un `MQTT déconnecté (reconnect demandé)` + reconnect + discovery
- [ ] D3 — Test manuel : modifier un champ MQTT mais avec la même valeur (re-saisir et sauver) → vérifier qu'il n'y a PAS de reconnect
- [x] D4 — `pio run` SUCCESS, RAM/Flash inchangés ou variation négligeable
- [x] C1 — `docs/subsystems/mqtt-manager.md` : préciser le comportement conditionnel
- [x] C2 — `CHANGELOG.md` : entrée Firmware

## Nouvelles signatures / API

- Endpoints HTTP : `POST /save-config` comportement modifié (interne) — réponse identique
- Champs WebSocket : aucun nouveau
- Topics MQTT : aucun nouveau
- Fonctions publiques : aucune nouvelle
- Constantes : aucune nouvelle

## Sécurité chimique

- Dosage impliqué : non
- Limites journalières / horaires concernées : aucune
- Filtration requise avant action : non
- Calibration capteur modifiée : non
- Algorithme PID touché : non
- Risque identifié : aucun (optimisation côté observabilité MQTT)

## ADR attendu ?

- **Probable** : non
- **Critère(s) candidat(s)** : aucun (optimisation locale, pas de décision structurante, pas d'alternative crédible à arbitrer)
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `POST /save-config` — comportement interne modifié, contrat externe identique

### Champs WebSocket
- aucun

### Topics MQTT
- aucun (économie de re-discovery, pas changement de schéma)

### Documentation à produire / mettre à jour

#### Docs vivantes
- `docs/subsystems/mqtt-manager.md` — section reconnect : préciser le comportement conditionnel

#### Docs transverses
- `docs/API.md` — éventuelle note sur `POST /save-config` si le comportement post-save y est listé
- `CHANGELOG.md` — entrée sous la version cible

#### Docs immuables
- aucun ADR

## Critères d'acceptation

- [ ] AC1 — Save d'un champ NON-MQTT ne déclenche AUCUN log `MQTT déconnecté` ni de republication discovery (test humain post-flash)
- [ ] AC2 — Save d'un champ MQTT (au moins un sur les 6 listés) déclenche un seul reconnect comme avant (test humain post-flash)
- [ ] AC3 — Save où l'utilisateur retape la même valeur MQTT (pas de changement réel) ne déclenche pas de reconnect (test humain post-flash)
- [ ] AC4 — Save d'une combinaison (un champ MQTT + plusieurs non-MQTT) déclenche un seul reconnect (test humain post-flash)
- [ ] AC5 — Aucun bug introduit sur la session MQTT (toujours connectée si rien ne bouge) (test humain post-flash)
- [x] AC6 — Tous les libellés utilisateur restent en français (non concerné, pas d'UI ; log INFO firmware FR : « MQTT reconnect demandé (config MQTT modifiée) »)
- [x] AC7 — `pio run` compile sans nouveau warning
- [x] AC8 — Comportement de `requestReconnect()` lui-même inchangé (aucune modif `mqtt_manager.cpp/h`)

## Travail réalisé

- **Snapshot des 6 champs MQTT** (`server`, `port`, `topic`, `username`, `password`, `enabled`) effectué dans le handler `POST /save-config` après `xSemaphoreTakeRecursive(configMutex)` et **avant** parsing JSON, dans des variables locales (`String oldServer`, `uint16_t oldPort`, `bool oldEnabled`, etc.).
- **Comparaison post-application** : booléen `mqttChanged` calculé après écriture des structs et `saveMqttConfig()` / `saveAuthConfig()`, par OR sur les 6 deltas (`oldServer != mqttCfg.server || oldPort != mqttCfg.port || ...`).
- **Appel conditionnel** : `mqttManager.requestReconnect()` désormais déclenché **uniquement si** `mqttChanged == true`. Les saves de paramètres non-MQTT (modes régulation, NTP, logs DEBUG, écran LVGL, calibrations, etc.) n'invoquent plus `requestReconnect()`.
- **Log INFO FR** : `systemLogger.info("MQTT reconnect demandé (config MQTT modifiée)")` ajouté dans la branche conditionnelle. Aucun log dans la branche « pas de changement » (silence par design pour ne pas spammer).
- **Aucune modification** de `mqtt_manager.cpp/h` (le comportement de `requestReconnect()` lui-même est inchangé), ni du frontend, ni du contrat externe `POST /save-config` (mêmes payloads, mêmes réponses).

## Fichiers modifiés

- `src/web_routes_config.cpp` : +24 / -1 lignes (snapshot 6 champs + comparaison + appel conditionnel + log)

Total : **1 fichier code modifié**.

Documentation associée :
- `docs/subsystems/mqtt-manager.md` : nouvelle sous-section « Déclenchement conditionnel post-`POST /save-config` (feature-018) » sous « Reconnexion »
- `CHANGELOG.md` : entrée Firmware + Documentation sous `[Unreleased] - 2026-04-30`

## Build

- `pio run` : **SUCCESS**, 0 nouveau warning
- RAM : **16.4 %** (inchangée)
- Flash : **97.8 %** (inchangée)

## Tests dynamiques restants

Les tests humains AC1–AC5 sont à exécuter après flash sur le matériel réel, en suivant le « Plan de test manuel » (étapes 1 à 6) :

1. AC1 — Save NON-MQTT (toggle Logs DEBUG dans Avancé) → vérifier l'absence de `MQTT déconnecté (reconnect demandé)` dans les logs ESP32 et de transition « Déconnecté » sur le badge UI Paramètres → MQTT.
2. AC2 — Save MQTT (modifier le topic) → vérifier la présence du log INFO `MQTT reconnect demandé (config MQTT modifiée)`, le cycle disconnect/reconnect, et la republication des 17 messages discovery HA.
3. AC3 — Save MQTT identique (retaper la même valeur) → vérifier l'absence de reconnect (les valeurs finales sont identiques).
4. AC4 — Save combiné (mode pH + topic MQTT en une fois) → vérifier qu'un seul reconnect est déclenché.
5. AC5 — Phase de repos (aucun save) → vérifier que la session MQTT reste connectée sans interruption.
6. Cas `enabled` true → false / false → true : vérifier la fermeture propre / la reconnexion.

## Plan de test manuel

1. **Save NON-MQTT** : ouvrir Paramètres → Avancé → cocher/décocher « Logs DEBUG activés » → Enregistrer. Observer les logs ESP32 : doit montrer `Logs DEBUG: activés/désactivés` mais **AUCUN** `MQTT déconnecté (reconnect demandé)`. Le badge MQTT reste sur « Connecté » sans transition.
2. **Save MQTT** : ouvrir Paramètres → MQTT → modifier le topic (ex. `pool/sensors` → `pool/test`) → Enregistrer. Observer : `MQTT déconnecté (reconnect demandé)` + reconnect + 17 messages discovery. Remettre la valeur initiale.
3. **Save MQTT identique** : ouvrir Paramètres → MQTT → cliquer dans le champ topic, taper un caractère, l'effacer (valeur finale identique) → Enregistrer. Observer : pas de reconnect (la valeur n'a pas réellement changé).
4. **Save combiné** : changer le mode régulation pH ET le topic MQTT en une seule fois → Enregistrer. Observer : un seul reconnect.
5. **Save mqtt_enabled true → false** : décocher « Activer MQTT » → Enregistrer. Observer : disconnect propre, pas de tentative de reconnect (puisque enabled=false).
6. **Save mqtt_enabled false → true** : recocher → Enregistrer. Observer : connect, discovery.

## Notes pour agent

- **Lire `web_routes_config.cpp`** pour identifier le call site exact de `mqttManager.requestReconnect()` post-save. Probablement dans le handler `POST /save-config` (ligne approximative à découvrir par lecture).
- **Pattern de snapshot** : copier les 6 champs dans des variables locales (`String oldServer = mqttCfg.server; uint16_t oldPort = mqttCfg.port; ...`) avant le parsing JSON.
- **Comparaison** : booléen `bool mqttChanged = (oldServer != mqttCfg.server) || (oldPort != mqttCfg.port) || ...`.
- **Concurrence** : le snapshot lit `mqttCfg`, l'application écrit dans `mqttCfg`. Tout cela se passe sous le `configMutex` déjà tenu par les `saveXxxConfig`. Mais le snapshot doit être fait **avant** ces appels. Vérifier qu'on lit `mqttCfg` à un moment cohérent (avant l'écriture). Au pire, prendre `configMutex` autour du snapshot + écriture + comparaison.
- **Champs MQTT à comparer (6)** : `server`, `port`, `username`, `password`, `topic`, `enabled`. Vérifier le nom exact dans `MqttConfig` ou équivalent.
- **Cas particulier de l'enabled** : si l'utilisateur passe `enabled` true → false, le `requestReconnect()` ne reconnectera pas mais déconnectera proprement. Cohérent.

## Hors périmètre

- Pas de modification du firmware MQTT (`mqtt_manager.cpp/h`)
- Pas de modification du frontend
- Pas de nouvelle UI ni champ
- Pas de modification du contrat MQTT externe (mêmes topics, mêmes payloads)
- Pas de modification d'autres handlers de routes (calibration, OTA, etc.)
