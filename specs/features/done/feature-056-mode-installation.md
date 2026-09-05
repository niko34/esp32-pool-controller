## Métadonnées
- **ID** : feature-056
- **Slug** : mode-installation
- **Type** : feature
- **Statut** : done — pool-chemistry GO (2 passages), code-reviewer Approved, 214 tests natifs verts, build SUCCESS (cf. notes ; AC on-target restants)
- **Priorité** : haute
- **Version cible** : 2.19.0 (change de contrat config + migration NVS → bump mineur)
- **Dernière mise à jour** : 2026-07-08
- **Spec(s) liée(s)** : feature-053/054/055 (Boost — dépend de « filtration gérée ? »)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [x] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance (migration NVS)
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **DOUBLE passage** (avant conception dosage + après implémentation) : nouveau chemin vers la garde « présence d'eau »
- [x] ux-ui-designer — nouvelle section « Installation » + étape assistant
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Remplacer le paramètre `regulationMode` (Continu/Piloté), aujourd'hui mal placé sous « Régulation pH/ORP », par un **« Mode d'installation »** unique à 3 choix qui décrit le câblage réel et pilote de façon cohérente la présence d'eau, le pilotage du relais filtration et la régulation. Absorbe aussi `filtrationCfg.enabled`. Ajoute un 3ᵉ cas jusqu'ici non géré : filtration pilotée par un système externe qui signale son état via API HTTP/MQTT.

## Contexte
`regulationMode` (`config.h:45`, défaut `"pilote"`) ne pilote aucune régulation : son seul effet est de court-circuiter la garde « présence d'eau » du dosage (`dosing_logic.cpp:15`). Il recouvre partiellement `filtrationCfg.enabled` (« PoolController gère la filtration »). L'installateur doit croiser deux réglages mal nommés et dispersés.

Analyse des câblages réels → 3 archétypes (la 4ᵉ combinaison est un non-sens matériel : un PC alimenté par la filtration ne peut pas piloter la filtration sans se couper l'alimentation) :

| Choix | Câblage | Eau présente si… | Filtration pilotée par PC ? | Ancien équivalent |
|---|---|---|---|---|
| 1. PoolController pilote la filtration | alim permanente + sortie 12 V→contacteur | PC a commandé la filtration ON | oui (relais GPIO 26) | `pilote` + `enabled=true` |
| 2. Alimenté par le circuit de filtration | alim = phase filtration | PC est allumé (par construction) | non | `continu` |
| 3. Filtration externe signalée *(NOUVEAU)* | alim permanente, filtration tierce | dernier signal externe = ON **et récent** | non | *aucun (trou dangereux)* |

Trou actuel : dans le cas 3, choisir `pilote` → PC ne dose jamais ; choisir `continu` → PC suppose de l'eau 24/7 alors que la filtration externe peut être coupée → **injection dans une eau stagnante = danger chimique**.

## Comportement attendu

### Paramètre « Mode d'installation »
- Choix unique parmi les 3 archétypes ci-dessus, exposé dans une **nouvelle section « Installation »** (app web) + une **étape de l'assistant 1ᵉʳ lancement** (contrôleur ET écran LVGL via UART).
- Remplace `regulationMode` **et** `filtrationCfg.enabled` (fusion). Ces deux réglages disparaissent de l'UI.

### Effets par choix
- **Choix 1** : la programmation filtration (auto/horaire/manuel/off/force) et son widget/relais sont actifs. Présence d'eau = `filtration.isRunning()` (état commandé). Comportement identique à l'actuel `pilote` + filtration gérée.
- **Choix 2** : relais filtration non piloté ; programmation filtration masquée/désactivée ; widget filtration masqué. Présence d'eau = vrai en permanence (PC vivant ⟺ eau). Timer de stabilisation pompes désactivé (comme `continu` actuel).
- **Choix 3** : relais filtration non piloté ; programmation masquée. Présence d'eau = dernier état signalé par l'extérieur **si récent** (cf. timeout sécurité). Par défaut au boot : OFF (fail-safe).

### API filtration externe (choix 3)
- `POST /filtration/external-state {running: true|false}` (auth WRITE) → met à jour l'état externe + horodatage.
- MQTT `{base}/filtration_state/set` (payload `ON`/`OFF`) → idem, pour automation Home Assistant.
- L'état signalé n'est valide que `kExternalFiltrationStaleMs` après réception ; au-delà → considéré OFF.

### Cas limites
- **Signal externe périmé (choix 3)** : dosage refusé (fail-safe OFF), message clair.
- **Boot / redémarrage (choix 3)** : état externe inconnu → OFF jusqu'au 1ᵉʳ signal.
- **Injection manuelle en cours + perte d'eau (choix 3)** : interruption CRITICAL (comme la garde `filtrationOkForInjection` actuelle).
- **Migration** : install existant `pilote` → choix 1 ; `continu` → choix 2 ; aucun n'est en choix 3.
- **Choix 2/3 + tentative de forçage filtration (Boost, UI, MQTT)** : sans effet sur le relais (non piloté) ; signalé clairement (cf. feature-054).
- **Écran LVGL déconnecté** pendant changement de mode : cohérence config à la reconnexion.

## Comportement actuel
- `regulationMode` (`continu`/`pilote`) sous « Régulation pH/ORP » ; unique effet = court-circuit garde présence d'eau (`dosing_logic.cpp:15`, `pump_controller.cpp:371,812,1029,1299`, `web_routes_control.cpp:38,65,170`).
- `filtrationCfg.enabled` = « PoolController gère la filtration » (widget/relais).
- Aucun endpoint pour signaler l'état réel de la filtration ; `filtration.isRunning()` = état commandé par PC uniquement.

## Contraintes techniques
- ESP32, offline first ; Vanilla JS ; UI en français.
- Migration NVS sans perte (clé `reg_mode` → nouveau schéma, avec fallback de lecture de l'ancienne clé).
- La garde « présence d'eau » reste une **règle de sécurité non négociable** : tout nouveau chemin passe par `pool-chemistry` + `code-reviewer`.
- Logique décidable en pur (ADR-0017) : la résolution « eau présente ? » selon (mode, filtration commandée, état externe, fraîcheur) doit être testable en natif (`test/`).
- AsyncWebServer : handler `/filtration/external-state` non bloquant.
- Watchdog actif sur tout chemin de dosage.

## Fichiers potentiellement concernés
- `src/config.h` / `src/config.cpp` — remplacer `regulationMode` + `filtrationCfg.enabled` par `installMode` (enum) ; migration NVS `reg_mode`.
- `src/constants.h` — `kExternalFiltrationStaleMs` (timeout fraîcheur), valeurs enum.
- `src/dosing_logic.*` — remplacer `continuousMode`/`filtrationRunning` par une résolution « eau présente ? » basée sur le mode (logique pure testable).
- `src/pump_controller.cpp` — points de garde présence d'eau (`:371,812,1029,1299`) + horizon scheduled.
- `src/filtration.*` — pilotage relais conditionné au choix 1 ; état externe (choix 3) ; timer stabilisation.
- `src/web_routes_config.cpp` — get/set du nouveau mode ; retrait `regulation_mode`/`filtration_enabled`.
- `src/web_routes_control.cpp` — nouveau `POST /filtration/external-state` ; gardes injection.
- `src/mqtt_manager.cpp` — topic `filtration_state/set` + auto-discovery HA ; publication mode.
- `src/ws_manager.cpp` — champ(s) mode + source présence d'eau + fraîcheur signal.
- `src/uart_commands.cpp` — `set_config`/`get_config` : nouveau mode ; étape assistant écran.
- `data/index.html` / `data/app.js` / `data/app.css` — section « Installation », retrait ancien select, masquage conditionnel programmation filtration.
- `docs/*` — voir section doc.

## Livrables — check-list
- [ ] A1 — Enum `installMode` en config + migration NVS depuis `reg_mode` (+ absorption `filtration_enabled`)
- [ ] A2 — Résolution « eau présente ? » en logique pure testable (3 modes + fraîcheur signal externe)
- [ ] A3 — Pilotage relais filtration conditionné au choix 1 uniquement
- [ ] A4 — `POST /filtration/external-state` (auth WRITE) + horodatage + timeout `kExternalFiltrationStaleMs`
- [ ] A5 — MQTT `{base}/filtration_state/set` + auto-discovery HA + publication du mode d'installation
- [ ] A6 — Câblage garde dosage (auto/manuel/scheduled/injection en cours) sur la nouvelle résolution
- [ ] A7 — UART : mode d'installation dans set_config/get_config + étape assistant écran
- [ ] B1 — Section « Installation » (app web) : sélecteur 3 choix + aides + masquage conditionnel filtration
- [ ] B2 — Étape « Installation » dans l'assistant 1ᵉʳ lancement
- [ ] C1 — Docs (API, MQTT, subsystems, features, ADR migration, CHANGELOG)
- [ ] D1 — Tests natifs : résolution présence d'eau (3 modes, fraîcheur, boot) — zéro régression sur les 199 existants

## Nouvelles signatures / API
- Endpoints HTTP : `POST /filtration/external-state {running:bool}` (auth WRITE) ; `regulation_mode` retiré de get/set-config, remplacé par `install_mode`.
- Champs WebSocket : `install_mode`, source/état présence d'eau (ex. `water_present`, `filtration_state_source`, `filtration_state_stale`).
- Topics MQTT : `{base}/filtration_state/set` (commande, non-retain) ; publication `install_mode` + auto-discovery HA (select/état).
- Constantes : `constexpr unsigned long kExternalFiltrationStaleMs = ...;` (à arbitrer par `pool-chemistry`).

## Sécurité chimique (obligatoire)
- Dosage impliqué : **oui** — pH- (acide) et chlore.
- Limites journalières / horaires concernées : inchangées ; c'est la **garde présence d'eau** qui est refactorée.
- Filtration requise avant action : **oui** — cœur de la feature. Choix 1 = état commandé ; choix 2 = présumé par câblage ; choix 3 = signal externe **avec timeout fail-safe OFF**.
- Calibration capteur modifiée : non.
- Algorithme PID touché : non (seule la condition d'exécution change).
- Risque identifié : **injection dans une eau stagnante** si le signal externe (choix 3) est cru alors qu'il est périmé → exigence forte de timeout de fraîcheur + fail-safe OFF au boot et à l'expiration. `pool-chemistry` doit fixer `kExternalFiltrationStaleMs` et valider chaque chemin.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contrainte structurante), 2 (compromis 3 archétypes vs 2 axes), 4 (contrat config/MQTT + migration NVS)
- **Sujet pressenti** : « Mode d'installation : 3 archétypes de câblage et résolution de la présence d'eau » (remplace de facto la sémantique continu/pilote).

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `POST /filtration/external-state` (nouveau)
- `/get-config` / `/save-config` : `regulation_mode` → `install_mode`

### Champs WebSocket
- `install_mode`, `water_present` (+ éventuels `filtration_state_source`, `filtration_state_stale`)

### Topics MQTT
- `{base}/filtration_state/set` (nouveau, commande) + auto-discovery HA ; publication du mode d'installation (select HA)

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-settings.md` — nouvelle section « Installation », retrait mode pilote/continu
- `docs/features/page-filtration.md` — visibilité conditionnelle selon le mode
- `docs/subsystems/pump-controller.md` — refonte de la garde présence d'eau
- `docs/subsystems/filtration.md` — pilotage relais conditionné + état externe
- `docs/subsystems/mqtt-manager.md` / `ws-manager.md` — nouveaux champs/topics

#### Docs transverses
- `docs/API.md` — endpoint external-state, install_mode
- `docs/MQTT.md` — topic filtration_state, discovery HA
- `docs/UPDATE_GUIDE.md` — **migration NVS** + note installateur (choisir le mode d'installation)
- `README.md` — description du mode d'installation
- `CHANGELOG.md` — entrée [2.19.0]

#### Docs immuables
- `docs/adr/NNNN-mode-installation.md` — à créer

## Critères d'acceptation
- [x] Choix 1 (managed) : dosage autorisé uniquement quand PC commande la filtration ON ; programmation filtration active. *(logique pure + tests ; validation nominale on-target)*
- [x] Choix 2 (powered) : dosage autorisé en permanence ; relais filtration non piloté ; programmation masquée. *(logique + UI ; on-target)*
- [x] Choix 3 (external) : dosage autorisé seulement si dernier signal externe = ON et < `kExternalFiltrationStaleMs` ; sinon refusé. *(couvert par test_native_water_presence)*
- [x] Choix 3 au boot (aucun signal) : dosage refusé (fail-safe OFF). *(test known=false → absent)*
- [ ] Signal externe reçu via HTTP **et** via MQTT met à jour l'état de façon identique. *(on-target : HTTP + MQTT réels)*
- [ ] Migration : install `pilote` → managed, `continu` → powered, sans perte de config. *(migrateInstallMode testé ; flash réel on-target)*
- [ ] Le mode est modifiable depuis l'app web (section Installation) ET l'assistant, cohérent via UART/WS. *(on-target : écran LVGL)*
- [x] Tous les libellés utilisateur sont en français.
- [x] `pio run` compile sans nouveau warning.
- [x] `./build_fs.sh` passe sans dépasser la partition (320 KB).
- [x] `pio test -e native` : 214/214 verts (199 existants + 15 nouveaux présence d'eau/migration).

## Plan de test manuel
1. Choix 1, filtration auto : vérifier dosage suspendu hors filtration, repris pendant.
2. Choix 2 : vérifier dosage permanent, relais filtration inerte, programmation masquée.
3. Choix 3 : publier `filtration_state ON` (MQTT) → dosage autorisé ; ne plus publier > timeout → dosage refusé ; publier `OFF` → refusé immédiatement.
4. Choix 3, injection manuelle volumée en cours puis signal OFF/périmé → interruption CRITICAL.
5. Migration : flasher par-dessus une config `continu`/`pilote` existante → vérifier le mapping.
6. Changement de mode depuis l'assistant écran LVGL → cohérence app web (WS) et NVS.

## Notes pour agent
- **Ne pas** relâcher la garde présence d'eau : le refactor doit être iso-sécurité ou plus sûr. `pool-chemistry` DOUBLE passage obligatoire.
- Le cas 3 est le seul chemin réellement nouveau ; concentrer la vigilance sécurité dessus (timeout, fail-safe, injection en cours).
- Réutiliser l'infra commande existante (drainCommandQueue, broadcast WS config) pour propager le signal externe et le changement de mode vers l'UI.
- Interaction Boost (feature-053/054) : « filtration gérée » = choix 1 ; en choix 2/3 le Boost ne prolonge pas la filtration (déjà signalé par le widget Effet).

## Hors périmètre
- Capteur de débit physique (mesure réelle du débit) — non couvert ; la présence d'eau reste déduite (commande / câblage / signal externe).
- Pilotage de plusieurs circuits de filtration indépendants.

## Notes d'implémentation (v2.19.0, 2026-07-08)
- **Enum unique** `InstallMode { ManagedFiltration=0, PoweredByFiltration=1, ExternalFiltration=2 }` (dosing_logic.h), sérialisé `managed`/`powered`/`external`. Remplace ET fusionne `regulationMode` (continu/pilote) + `filtrationCfg.enabled`.
- **Logique pure** (ADR-0017) : `resolveWaterPresent(WaterPresenceInputs) → {waterPresent, source, stale}` dans dosing_logic.*, source UNIQUE consommée par tous les sites de garde (pump_controller canDose/scheduled/horizons, web_routes_control injection + interruption). `DoseInputs`/`ManualInjectInputs` : champ unique `waterPresent`. Ordre des gardes INCHANGÉ.
- **Cas external** : état `{on, lastMs, known}` en RAM sous `portMUX` (spinlock, pas de mutex FreeRTOS), écrit par `POST /filtration/external-state?running=…` (auth WRITE, param form/query) ET MQTT `{base}/filtration_external_state/set`. Âge = `(now-lastMs)` wrap-safe hors lock. **Fail-safe OFF** : boot (known=false), périmé (`age > kExternalFiltrationStaleMs = 180000` = 3 min, fixé par pool-chemistry), payload OFF. Aucune persistance NVS de l'état externe.
- **Migration NVS** write-back one-shot : `install_mode` sinon `migrateInstallMode(reg_mode, filt_enabled)` (`continu`→powered, sinon→managed, jamais external). `migrateInstallMode` ignore volontairement `filt_enabled` (combinaison alim-filtration + PC-pilote = non-sens matériel) → cas de bord documenté dans UPDATE_GUIDE.
- **Contrats** : `regulation_mode`/`filtration_enabled` retirés (config JSON/WS/UART) ; ajout WS `install_mode`, `water_present`, `filtration_ext_known/_on/_age_s` (buffer 1664→1920) ; MQTT `install_mode`(/set, select HA FR) + `filtration_external_state/set` (switch optimiste HA). `boost_filtration_extended` (feature-055) re-sourcé sur managed sans changer sa sémantique.
- **UI** : section « Installation » 1er onglet (3 cartes radio), étape assistant entre Wi-Fi et Heure ; programmation/widget filtration masqués hors managed. Suppression du toggle filtration_enabled et du select regulation_mode.
- **Validation** : pool-chemistry GO ×2, code-reviewer Approved (2 mineurs corrigés : dead code app.js switchMap, commentaires « continu » obsolètes), 214/214 tests natifs, `pio run` SUCCESS 0 warning, build_fs 320 KB.
- **Reste on-target** : signal HTTP+MQTT réel, migration par flash USB sur config existante, cohérence écran LVGL, interruption CRITICAL injection en cours sur perte d'eau (external).

## Finitions post-clôture (2026-07-08, même version 2.19.0)
- **Harmonisation nommage** : topic de commande MQTT renommé `filtration_state/set` → `filtration_external_state/set` (cohérence avec l'endpoint HTTP `external-state` et distinction du topic d'état publié `filtration_state` = relais). Membre C++ `filtrationStateCommand` → `filtrationExternalStateCommand`. Migration nulle (feature non commitée, aucune install en mode external). `pio run` + `build_fs` verts.
- **Aide intégrée UI** : bloc repliable `<details class="tips">` (`#install-ext-tips`) dans le callout du mode external, avec exemples prêts à l'emploi (curl HTTP, `mosquitto_pub`, automatisation Home Assistant heartbeat 1 min). `fillExternalTips()` renseigne l'URL réelle (`location.host`) et le topic MQTT réel (`window._config.topic`). UI seule, firmware non touché. `node --check` + `build_fs` verts. Docs : page-settings.md + CHANGELOG.
