## Métadonnées
- **ID** : feature-009
- **Slug** : ha-enable-regulation
- **Type** : feature
- **Statut** : done
- **Priorité** : normale
- **Version cible** : 2.7.0 (livrée)
- **Dernière mise à jour** : 2026-07-04 (livraison v2.7.0 — voir Notes d'implémentation)
- **Doc(s) liée(s)** : [docs/MQTT.md](../../docs/MQTT.md), [docs/subsystems/mqtt-manager.md](../../docs/subsystems/mqtt-manager.md), [docs/features/page-ph.md](../../docs/features/page-ph.md), [docs/features/page-orp.md](../../docs/features/page-orp.md)

> **Annotations d'audit (2026-07-04)** — spec confrontée au code v2.6.0, prémisses exactes, non supersédée :
> 1. **Modèle d'implémentation** : suivre le `select` du **mode filtration** (`mqtt_manager.cpp` l.1006-1010 : discovery + `command_topic` + souscription + handler `InboundCmdType::FiltrationMode` drainé sur loopTask sous `configMutex`) — modèle plus direct que le `number.*_ph_target` cité plus bas.
> 2. **pool-chemistry requalifié : passe légère obligatoire** (et non « non ») : la feature crée un nouveau chemin de commande à distance qui bascule le mode de régulation (active/désactive le dosage). Aucune garde modifiée, même effet que l'UI web — mais la règle projet sur les nouveaux chemins vers la régulation s'applique. Point à valider : changement de mode pendant injection → l'injection en cours se termine naturellement.
> 3. **Valeur invalide → log `warning`** : les handlers existants ignorent silencieusement ; appliquer le warning demandé par la spec (A5) sans modifier le comportement des handlers existants (hors périmètre).

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **passe légère** (requalifié à l'audit 2026-07-04 : nouveau chemin de commande à distance vers le mode de régulation, cf. annotation 2)
- [ ] ux-ui-designer — non (aucune surface UI web modifiée)
- [x] embedded-developer
- [x] web-ui-developer — MQTT + auto-discovery HA
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Exposer dans Home Assistant des entités `select` permettant de changer le `ph_regulation_mode` et `orp_regulation_mode` (`automatic` / `scheduled` / `manual`). Aujourd'hui, le champ est publié en MQTT state, mais pas exposé comme entité commandable.

## Contexte
Le topic `{base}/ph_regulation_mode` est déjà publié ([docs/features/page-ph.md](../../docs/features/page-ph.md#interaction-mqtt)), mais non exposé comme entité HA → l'utilisateur ne peut pas créer un automatisme « passer en mode manuel quand la piscine est en hivernage ».

Cas d'usage type : scripts HA qui activent/désactivent la régulation en fonction de la saison, d'une détection d'absence longue, ou d'un basculement manuel via un dashboard HA.

## Comportement attendu

### Côté firmware
- Ajouter dans l'auto-discovery MQTT deux entités `select` :
  - `select.{name}_ph_regulation_mode` :
    - `state_topic = {base}/ph_regulation_mode`
    - `command_topic = {base}/ph_regulation_mode/set`
    - `options = ["automatic", "scheduled", "manual"]`
  - Symétrique ORP
- Handler MQTT sur le topic `set` :
  - Valider la valeur reçue (enum) → sinon ignorer + log `warning`
  - Appliquer via la même voie que `/save-config` (persistance NVS, `configMutex`)
  - Republier le nouvel état (feedback HA)
- Ne rien changer à l'UI web : la carte « Régulation » déjà en place reste la source principale.

### Cas limites
- HA envoie une valeur inconnue → ignorer, log.
- Le mode change pendant une injection en cours → pas d'interruption forcée, l'injection se termine, le nouveau mode prend effet au cycle suivant.
- Perte MQTT → la dernière valeur retenue (`retain=true` sur state, pas sur command) reste visible dans HA.

## Comportement actuel
- `ph_regulation_mode` et `orp_regulation_mode` sont publiés en MQTT state (voir `src/mqtt_manager.cpp`).
- Pas d'auto-discovery `select`, pas de topic de commande.
- L'utilisateur doit passer par l'UI web pour changer le mode.

## Contraintes techniques
- ESP32, offline first
- Ne pas casser la compatibilité avec les dashboards HA existants (topic state inchangé)
- Auto-discovery retain true, commande non retain
- Validation stricte : rejeter toute valeur hors enum

## Fichiers potentiellement concernés
- `src/mqtt_manager.cpp` / `src/mqtt_manager.h` — ajouter les 2 entités select dans l'auto-discovery + handler de commande
- `src/constants.h` — éventuels topics si pas déjà centralisés
- `docs/MQTT.md` — documenter les 2 nouvelles entités HA
- `docs/features/page-ph.md`, `page-orp.md` — mettre à jour le tableau MQTT

## Livrables — check-list
- [x] A1 — Auto-discovery `select.{name}_ph_regulation_mode` (state + command + options)
- [x] A2 — Auto-discovery `select.{name}_orp_regulation_mode` (symétrique)
- [x] A3 — Handler MQTT sur `{base}/ph_regulation_mode/set` : validation enum + persistance via voie `saveConfig` + republish
- [x] A4 — Idem pour `{base}/orp_regulation_mode/set`
- [x] A5 — Valeur invalide → ignorée + log `warning`
- [x] C1 — `docs/MQTT.md` : 2 nouvelles entités HA `select`
- [x] C2 — `docs/features/page-ph.md`, `page-orp.md` : tableau MQTT complété
- [x] C3 — `docs/subsystems/mqtt-manager.md` : nouveau handler + auto-discovery
- [x] C4 — `CHANGELOG.md` : entrée
- [x] D1 — `pio run` sans nouveau warning

## Nouvelles signatures / API
- Topics MQTT : `{base}/ph_regulation_mode/set`, `{base}/orp_regulation_mode/set` (commande)
- Auto-discovery HA : 2 entités `select` (`select.*_ph_regulation_mode`, `select.*_orp_regulation_mode`)

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun — l'implémentation suit le pattern `number.*_ph_target` déjà en place (état retain + commande non retain).

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Aucun nouveau

### Champs WebSocket
- Aucun nouveau (déjà présents)

### Topics MQTT
- `{base}/ph_regulation_mode/set` (commande, non retain) + auto-discovery `select`
- `{base}/orp_regulation_mode/set` (symétrique)

### Documentation à produire / mettre à jour

#### Docs vivantes (1 par surface touchée)
- `docs/features/page-ph.md`, `docs/features/page-orp.md` — tableau MQTT complété
- `docs/subsystems/mqtt-manager.md` — handler + auto-discovery select

#### Docs transverses (si contrat externe modifié)
- `docs/MQTT.md` — 2 nouvelles entités HA select
- `CHANGELOG.md` — entrée

#### Docs immuables
- aucune ADR (voir §ADR attendu)

## Critères d'acceptation
- [ ] Dans HA (via MQTT integration), les entités `select.*_ph_regulation_mode` et `select.*_orp_regulation_mode` apparaissent automatiquement après boot *(à valider on-target — cf. check-list §1)*
- [x] Changer la valeur dans HA → mode modifié côté ESP32, persisté en NVS, UI web reflète le changement au rafraîchissement *(voie identique à l'UI web vérifiée en revue ; confirmation matérielle : check-list §2)*
- [x] Envoi d'une valeur hors enum (`"foo"`) → ignoré, log `warning` *(implémenté et revu ; confirmation matérielle : check-list §3)*
- [x] Les topics state existants (`{base}/ph_regulation_mode`) ne changent pas de format
- [ ] Après reboot ESP32, HA retrouve la bonne valeur (retain state) *(à valider on-target — cf. check-list §6)*
- [x] `pio run` compile sans nouveau warning

## Plan de test manuel
1. Observer l'auto-discovery dans HA → 2 entités select apparaissent avec les 3 options.
2. Dans HA, changer en `manual` → vérifier dans l'UI web que le mode a basculé, et en NVS après reboot.
3. Publier manuellement un payload invalide via mosquitto_pub → pas de changement, log ESP32 `warning`.
4. Injection en cours, changement de mode → injection continue, cycle suivant respecte le nouveau mode.
5. HA redémarré → retrouve la valeur via retain sur state_topic.

## Notes pour agent
- **Cohérence avec l'UI web** : la même route de persistance (`saveConfig()`) doit être utilisée pour que le changement soit identique.
- **Futur** : exposer aussi `ph_daily_target_ml` / `orp_daily_target_ml` comme `number` HA → hors périmètre ici (spec séparée si demandée).
- **Attention** : `ph_enabled` / `orp_enabled` sont des miroirs booléens (voir [ADR-0004](../../docs/adr/0004-mode-regulation-enum-3-valeurs.md)) : ne pas exposer de `switch` HA séparé pour éviter la confusion. Le `select` est la source de vérité.

## Notes d'implémentation (2026-07-04)

**Livré en v2.7.0.**

### Fichiers modifiés
- `src/mqtt_manager.h` : topics `phRegulationModeCommand` / `orpRegulationModeCommand` dans `MqttTopics` ; nouveaux `InboundCmdType::PhRegulationMode` / `OrpRegulationMode`.
- `src/mqtt_manager.cpp` : construction des topics `.../set`, souscription à la connexion, discovery des 2 `select` (icônes `mdi:ph` / `mdi:flash`, options `["automatic","scheduled","manual"]`, state_topic = topics state existants inchangés), routage `messageCallback()` (enqueue seul), handlers dans `drainCommandQueue()`.
- `src/version.h` : `2.6.0` → `2.7.0`.

### Choix techniques
- **Pattern du select `filtration_mode`** repris intégralement (discovery + command_topic + souscription + `InboundCmdType` drainé sur loopTask sous `configMutex`) — annotation d'audit n°1 respectée, aucun pattern nouveau.
- **Validation stricte après `toLowerCase()`** (cohérent avec le select filtration) ; valeur hors enum → `systemLogger.warning("Mode régulation pH/ORP invalide (MQTT): ...")` + aucun effet (A5).
- **Miroir ADR-0004** : `phEnabled/orpEnabled = (mode != "manual")`, voie strictement identique au handler `POST /save-config` de l'UI web ; persistance via `saveMqttConfig()`.
- **`publishTargetState()` pour le feedback HA** : republication immédiate du state (inconditionnelle sur commande valide, y compris no-op) — HA voit le select se caler sans attendre le cycle 10 s.
- **Flag `changed`** : `saveMqttConfig()` et log `info` « Mode régulation pH/ORP changé (MQTT): ... » exécutés **uniquement si changement réel**, log hors section critique — *correction apportée après revue (1 remarque mineure code-reviewer)* : évite l'usure NVS et le spam de logs sur commandes répétées.
- L'UI web reflète un changement HA via les canaux existants (broadcast WS `ph/orp_regulation_mode` + `/get-config`) — **aucun code UI ajouté**.
- Changement de mode pendant injection : l'injection en cours se termine naturellement, le nouveau mode prend effet au cycle de régulation suivant.

### Verdicts
- **pool-chemistry** : GO (passe légère — nouveau chemin de commande à distance, aucune garde modifiée, comportement injection en cours validé).
- **code-reviewer** : Approved (après correction d'1 remarque mineure — save/log conditionnels via flag `changed`).
- **test-engineer** : `pio run` SUCCESS, 0 warning nouveau ; 135/135 tests natifs verts.
- **doc-writer** : `docs/MQTT.md`, `docs/features/page-ph.md`, `page-orp.md`, `docs/subsystems/mqtt-manager.md`, `CHANGELOG.md` [2.7.0]. Aucun ADR (pattern existant, cf. §ADR attendu).

⚠️ **Reste à faire par l'utilisateur** : exécuter la « Check-list de validation on-target » ci-dessous (rédigée par test-engineer) sur matériel réel avec broker MQTT + HA — notamment les 2 critères d'acceptation laissés décochés (entités visibles dans HA après boot, retain retrouvé après reboot).

## Check-list de validation on-target

> Rédigée par test-engineer (2026-07-04, firmware 2.7.0). À exécuter par l'utilisateur sur matériel réel avec broker MQTT accessible. Remplacer `{base}` par le topic configuré (champ « Topic » de la config MQTT) et `<broker>` par l'IP du broker. Ajouter `-u user -P pass` si le broker est authentifié.

### Préparation
```bash
# Terminal 1 : observer les états et la discovery
mosquitto_sub -h <broker> -v -t "{base}/ph_regulation_mode" -t "{base}/orp_regulation_mode" -t "homeassistant/select/+/config"
# Terminal 2 : logs ESP32 (ou page Logs de l'UI web)
pio device monitor -b 115200
```

### 1. Auto-discovery HA
- [ ] Redémarrer l'ESP32 (ou reconnecter MQTT). Attendu : 2 payloads discovery `select` reçus (`..._ph_regulation_mode/config`, `..._orp_regulation_mode/config`) avec `options = ["automatic","scheduled","manual"]`, `command_topic` en `.../set`, `unique_id` distincts.
- [ ] Dans HA : les entités `select.*_ph_regulation_mode` et `select.*_orp_regulation_mode` apparaissent avec les 3 options.

### 2. Changement de mode valide + persistance NVS
- [ ] Dans HA, passer le select pH sur `manual`. Attendu : republish immédiat de `{base}/ph_regulation_mode` = `manual`, log `info` « Mode régulation pH changé (MQTT): manual », UI web (carte Régulation pH) reflète le changement au rafraîchissement.
- [ ] Rebooter l'ESP32. Attendu : le mode `manual` est conservé (NVS) et republié au boot.
- [ ] Répéter avec le select ORP (ex. `scheduled`) pour vérifier la symétrie.

### 3. Payload invalide → warning, aucun changement (recommandé pool-chemistry)
```bash
mosquitto_pub -h <broker> -t "{base}/ph_regulation_mode/set" -m "foo"
mosquitto_pub -h <broker> -t "{base}/ph_regulation_mode/set" -m "auto"      # valeur filtration, invalide ici
mosquitto_pub -h <broker> -t "{base}/ph_regulation_mode/set" -m ""
mosquitto_pub -h <broker> -t "{base}/orp_regulation_mode/set" -m "foo"
```
- [ ] Attendu pour chaque envoi : log `warning` « Mode régulation pH/ORP invalide (MQTT): ... », AUCUN republish du state topic, mode inchangé dans l'UI web et HA.
- [ ] Casse différente : `mosquitto_pub ... -m "MANUAL"` → accepté et normalisé en `manual` (cohérent avec le select mode filtration).

### 4. manual → automatic avec filtration OFF → aucun dosage (recommandé pool-chemistry)
- [ ] Filtration OFF (mode filtration `off` ou hors plage horaire). Passer le select pH de `manual` à `automatic` via HA.
- [ ] Attendu : le mode bascule (state republié) mais AUCUNE injection ne démarre tant que la filtration est arrêtée (garde présence d'eau du pump_controller). Vérifier l'absence de log d'injection et pompes doseuses à l'arrêt.

### 5. Changement de mode pendant injection en cours
- [ ] Filtration ON, injection pH en cours (forcer via une consigne éloignée si besoin). Passer le select pH sur `manual`.
- [ ] Attendu : l'injection en cours se termine naturellement (pas d'interruption forcée), puis aucune nouvelle injection ne démarre au cycle suivant.

### 6. Retain / redémarrage HA
- [ ] Redémarrer Home Assistant (ou l'intégration MQTT). Attendu : les 2 selects retrouvent leur valeur via le retain sur les state topics, sans intervention de l'ESP32.

## Hors périmètre
- Exposer `ph_daily_target_ml` / `orp_daily_target_ml` en `number` HA
- Exposer la cible pH/ORP en `number` HA (déjà fait : `number.*_ph_target`, `number.*_orp_target`)
- Écran LVGL : couvert par une spec séparée de l'écran si besoin
