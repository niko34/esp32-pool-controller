# ADR-0026 — Mode d'installation : 3 archétypes de câblage et résolution unique de la présence d'eau

- **Statut** : Accepté
- **Date** : 2026-07-08
- **Décideurs** : Nicolas Philippe (architect), pool-chemistry (GO pré + post)
- **Spec(s) liée(s)** : feature-056 (mode-installation) ; interagit avec feature-053/054/055 (Mode Boost — « filtration gérée » = `managed`)

## Contexte

Deux réglages dispersés et mal nommés décrivaient auparavant le câblage réel de l'installation :

- `regulationMode` (`"pilote"` / `"continu"`, sous l'onglet « Régulation pH/ORP ») — son **seul** effet était de court-circuiter la garde « présence d'eau » du dosage : en `continu`, on présumait de l'eau en permanence.
- `filtrationCfg.enabled` (« PoolController gère la filtration ») — pilotait le relais de filtration (GPIO 26) et la visibilité du widget/programmation.

L'installateur devait croiser ces deux réglages, et une combinaison n'avait aucun sens matériel (un contrôleur **alimenté par** le circuit de filtration ne peut pas **piloter** ce même circuit sans se couper l'alimentation). Surtout, un **troisième câblage réel restait dangereux** : une filtration pilotée par un système tiers (domotique, coffret existant). En `pilote`, le contrôleur ne dosait jamais ; en `continu`, il présumait de l'eau 24/7 alors que la filtration externe pouvait être coupée → **risque d'injection dans une eau stagnante**.

La garde « présence d'eau » est une **règle de sécurité chimique non négociable** du projet. Refondre sa source d'information est une décision structurante : elle contraint le dosage, la filtration, le contrat de config, le contrat MQTT et impose une migration NVS. Elle doit être formalisée et bornée.

## Décision

**Un enum unique `InstallMode` à 3 valeurs remplace `regulationMode` ET `filtrationCfg.enabled`.** Il décrit le câblage réel et pilote de façon cohérente la présence d'eau, le pilotage du relais et l'horizon de dosage. La sérialisation (config JSON / WS / MQTT / UART) est **stable et ne doit jamais être renumérotée** :

| `InstallMode` | Sérialisé | Câblage | Eau présente si… | Relais piloté par PC ? | Ancien équivalent |
|---|---|---|---|---|---|
| `ManagedFiltration` | `managed` | alim permanente + sortie 12 V → contacteur | PC commande la filtration ON (`filtration.isRunning()`) | **oui** (GPIO 26) | `pilote` + `enabled=true` |
| `PoweredByFiltration` | `powered` | alim = phase filtration | vrai en permanence (PC vivant ⟺ eau) | non | `continu` |
| `ExternalFiltration` | `external` | alim permanente, filtration tierce | dernier signal externe = ON **et** reçu il y a < `kExternalFiltrationStaleMs` (180 s) | non | *aucun (trou dangereux)* |

**La présence d'eau devient une fonction pure unique** `resolveWaterPresent(WaterPresenceInputs)` ([`src/dosing_logic.cpp`](../../src/dosing_logic.cpp), pattern Humble Object [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)), **source unique** consommée par TOUS les sites de garde (régulation auto `canDose`, injection manuelle `evaluateManualInject`, monitor d'injection en cours). L'ordre des gardes de dosage est **inchangé** : seule la source du booléen « eau présente » est unifiée. La fonction est **fail-closed strict** (condition pool-chemistry #2 : doute = refus) :

- `managed` → `filtrationCommandedOn` ;
- `powered` → `true` ;
- `external` → `externalSignalKnown && (age <= externalStaleMs) && externalSignalOn` (la connaissance du signal est testée **avant** son âge : un signal jamais reçu au boot n'a pas d'âge signifiant → refus) ;
- mode inconnu → `false`.

**Signal de filtration externe (mode `external`)** : deux canaux équivalents, non bloquants, **sans persistance** (au boot, état inconnu → fail-safe OFF) :
- `POST /filtration/external-state?running=true|false` (auth WRITE, paramètre form OU query, **pas** de corps JSON) ;
- MQTT `{base}/filtration_external_state/set` (payload `ON`/`OFF`, switch optimiste HA « Signal filtration externe »).

L'état est stocké sous **portMUX** (`setExternalState` / `getExternalState`), sûr depuis un handler HTTP ou un callback MQTT async. Au-delà de 180 s sans nouveau signal → présence d'eau `false` (fail-safe), dosage suspendu.

**Effets dérivés du mode** :
- Relais de filtration piloté **uniquement** en `managed` ; retour anticipé dans `filtration.update()` sinon.
- Programmation filtration masquée en `powered` / `external`.
- Timer de stabilisation au démarrage filtration armé **uniquement** en `managed`.
- Horizon de répartition du mode scheduled = `remainingRangeMinutes(...)` en `managed`, `1440 − nowMin` sinon (borné par la garde présence d'eau).

**Migration NVS one-shot** : `migrateInstallMode(regMode, filtrationEnabled)` — `"continu"` → `powered`, sinon → `managed`. Écriture immédiate de la clé `install_mode`, l'ancien schéma n'est plus relu ensuite. `external` n'est **jamais** produit par migration (mode nouveau, choisi explicitement).

## Alternatives considérées

- **Option A (rejetée) — Ajouter une 3ᵉ valeur à `regulationMode`** (`pilote` / `continu` / `externe`). N'absorbe **pas** `filtrationCfg.enabled` : il resterait deux réglages à croiser, et la combinaison contradictoire (`continu` + filtration gérée) demeurerait exprimable — donc un état invalide persistant.
- **Option B (rejetée) — Conserver deux axes booléens** (« PC gère la filtration ? » × « eau présumée en permanence ? »). Les 2 axes offrent 4 combinaisons dont **une est un non-sens matériel** (alimenté par la filtration + PC pilote la filtration). Un état invalide représentable est une source de bug et de mauvaise configuration installateur ; l'enum à 3 valeurs le rend **inexprimable**.
- **Option C (retenue) — Enum unique à 3 archétypes de câblage.** Chaque valeur correspond à un câblage physique réel et déduit sans ambiguïté la présence d'eau, le pilotage relais et l'horizon. La 4ᵉ combinaison impossible est éliminée par construction.

## Conséquences

### Positives

- **Un seul réglage** décrit le câblage ; l'état invalide (alim-filtration + PC-pilote) est inexprimable.
- **Troisième câblage enfin sûr** : la filtration externe signalée dose **seulement** sur un signal récent (< 180 s), fail-safe OFF au boot et à l'expiration → plus de risque d'injection en eau stagnante.
- **Source unique de présence d'eau** (`resolveWaterPresent`), testable en natif, consommée à l'identique par tous les sites de garde → moins de risque de divergence entre chemins de dosage.
- **Contrat cohérent** config / WS / MQTT / UART (sérialisation `managed`/`powered`/`external`), select HA en libellés FR.

### Négatives / dette assumée

- **Migration ignore `filt_enabled`** : `migrateInstallMode` ne regarde que `reg_mode`. Deux combinaisons héritées changent donc de comportement relais :
  - `pilote` + `enabled=false` → `managed` : le relais devient **piloté** (avant : inerte) ;
  - `continu` + `enabled=true` → `powered` : le relais devient **inerte** (avant : piloté).
  Conforme à l'intention (la combinaison alim-filtration + PC-pilote est un non-sens matériel), mais à **signaler à l'installateur** (voir [UPDATE_GUIDE.md](../UPDATE_GUIDE.md)).
- **Le mode `external` dépend d'un signal tiers** : si l'automation HA/domotique cesse de publier, le dosage se suspend au bout de 180 s. C'est le comportement fail-safe voulu, mais il impose une intégration fiable côté tiers.

### Ce que ça verrouille

- La sérialisation `managed` / `powered` / `external` est **publique** (config JSON, WS, MQTT, UART) — ne jamais renuméroter l'enum ni renommer les chaînes.
- La présence d'eau passe **exclusivement** par `resolveWaterPresent()` : tout nouveau chemin de dosage doit consommer cette source unique (validation `pool-chemistry` + `code-reviewer`).
- Le contrat externe est verrouillé : route `POST /filtration/external-state` (param `running`), topics `{base}/install_mode` (+ `/set`) et `{base}/filtration_external_state/set`, champs WS `install_mode` / `water_present` / `filtration_state_source` / `filtration_state_stale` / `filtration_ext_*`.
- `regulation_mode` et `filtration_enabled` sont **retirés** de la config JSON / WS / UART.

## Références

- Code : [`src/dosing_logic.h`](../../src/dosing_logic.h) / [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) (`InstallMode`, `resolveWaterPresent`, `migrateInstallMode`), [`src/config.h`](../../src/config.h) / [`src/config.cpp`](../../src/config.cpp) (`installMode`, migration NVS, `installModeToString`/`installModeFromString`), [`src/filtration.h`](../../src/filtration.h) / [`src/filtration.cpp`](../../src/filtration.cpp) (`setExternalState`/`getExternalState`/`resolveWaterPresence`, relais conditionné), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (gardes présence d'eau + horizon scheduled), [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) (`POST /filtration/external-state`), [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) (topics + discovery), [`src/ws_manager.cpp`](../../src/ws_manager.cpp), [`src/uart_commands.cpp`](../../src/uart_commands.cpp), [`src/constants.h`](../../src/constants.h) (`kExternalFiltrationStaleMs = 180000`)
- Spec : `specs/features/doing/feature-056-mode-installation.md`
- Docs : [API.md](../API.md), [MQTT.md](../MQTT.md), [UPDATE_GUIDE.md](../UPDATE_GUIDE.md), [subsystems/filtration.md](../subsystems/filtration.md), [subsystems/pump-controller.md](../subsystems/pump-controller.md), [subsystems/ws-manager.md](../subsystems/ws-manager.md), [subsystems/mqtt-manager.md](../subsystems/mqtt-manager.md), [features/page-settings.md](../features/page-settings.md), [features/page-filtration.md](../features/page-filtration.md)
- ADR liés : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure Humble Object — patron de `resolveWaterPresent`), [ADR-0025](0025-mode-boost.md) (Mode Boost — « filtration prolongée » n'a d'effet qu'en `managed`), [ADR-0021](0021-repartition-scheduled.md) (horizon de répartition scheduled)
