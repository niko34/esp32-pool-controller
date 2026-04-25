# Page ORP — `/orp`

- **Fichier UI** : [`data/index.html:744`](../../data/index.html:744) (section `#view-orp`)
- **URL** : `http://poolcontroller.local/#/orp`

## Rôle

Point d'entrée unique pour **tout** ce qui concerne l'ORP (potentiel redox, piloté par injection de chlore liquide) : mesure, mode de régulation, injection manuelle, historique, calibration. Structure **symétrique** de la page [pH](page-ph.md).

## Structure

En mode nominal, quatre zones :

1. **Bloc Statistiques** (bandeau compact, sans titre) — valeur ORP courante (mV) + dosage du jour (barre de progression, borne `max_orp_ml_per_day`).
2. **Carte Régulation ORP** — sélecteur de mode : `Automatique`, `Programmée`, `Manuelle`. Sous-blocs :
   - **Automatique** : ORP cible (mV) + bouton Sauvegarder ; + informations de calibration + bouton **Calibrer**.
   - **Programmée** : volume quotidien de chlore en mL + bouton Sauvegarder.
   - **Manuelle** : bloc Injection manuelle.
3. **Carte Historique** — graphique Chart.js, plages `Tout` / `30j` / `7j` / `24h`, zone ombrée 600–750 mV indiquant la plage de désinfection.
4. **Carte Calibration** — **remplace** Régulation + Historique pendant une session. Choix 1 point (offset) ou 2 points (offset + slope). Protocole en 4 ou 8 étapes. **Calcul réalisé côté client**, persisté via `POST /save-config` — voir [ADR-0003](../adr/0003-calibration-orp-cote-client.md).

Le bloc Statistiques reste visible en permanence.

## Données consommées (WebSocket `/ws`)

**Mesure** : `orp`, `orp_raw`, `orp_voltage_mv`
**Dosage** : `orp_dosing`, `orp_used_ms`, `orp_daily_ml`, `orp_limit_reached`
**Mode** : `orp_regulation_mode`, `orp_daily_target_ml`, `orp_enabled` (miroir — voir [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md))
**Config** : `orp_target`, `max_orp_ml_per_day` (= `max_chlorine_ml_per_day`)
**Calibration** : `orp_cal_valid`, `orp_calibration_date`, `orp_calibration_offset`, `orp_calibration_slope`, `orp_calibration_reference`, `orp_calibration_temp`
**Injection manuelle** : `orp_inject_remaining_s`
**Stabilisation** : `stabilization_remaining_s`

## Actions

| Action | Endpoint | Auth |
|--------|----------|------|
| Sauvegarder config (mode, cible, volume) | `POST /save-config` | CRITICAL |
| Injection manuelle start | `POST /orp/inject/start?volume=N` | WRITE |
| Injection manuelle stop | `POST /orp/inject/stop` | WRITE |
| Calibration 1 point | `POST /save-config` avec `orp_calibration_offset`, `orp_calibration_date`, `orp_calibration_reference` | CRITICAL |
| Calibration 2 points | `POST /save-config` avec `orp_calibration_offset`, `orp_calibration_slope`, `orp_calibration_date`, `orp_calibration_reference` | CRITICAL |

**Pas d'endpoint firmware `/calibrate_orp_*`** (différence avec pH). Voir [ADR-0003](../adr/0003-calibration-orp-cote-client.md).

Validation côté firmware :
- `orp_regulation_mode` rejeté si hors de l'enum.
- `orp_daily_target_ml` > `max_orp_ml_per_day` → HTTP 400.

## Règles firmware appliquées

Voir [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md). Résumé :

- **Mode `automatic`** : PID vers `orp_target`, actif uniquement pendant filtration (mode `pilote`), bloqué pendant stabilisation.
- **Mode `scheduled`** : injecte jusqu'à `orp_daily_target_ml` pendant filtration, **intentionnellement aveugle à la valeur ORP mesurée** ([`pump_controller.cpp:614`](../../src/pump_controller.cpp:614)) — un capteur déréglé ou en cours de remplacement n'arrête pas l'injection. Seules les gardes volumétriques (`orp_limit_minutes`, `max_chlorine_ml_per_day`) et structurelles (`canDose()`, débit configuré) s'appliquent. `orp_target` n'est pas consulté dans cette branche. PID réinitialisé au retour en mode automatique. Voir [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md).
- **Mode `manual`** : seule l'injection manuelle est autorisée.
- **Formule de mesure calibrée** : `ORP_final = ORP_brut × slope + offset` (voir [`sensors.cpp`](../../src/sensors.cpp)).
- **Limites** : `orp_limit_minutes` (défaut 10 min/h glissante) et `max_chlorine_ml_per_day` (défaut 500 mL/j) — voir [docs/subsystems/pump-controller.md#paramètres-en-dur-récapitulatif](../subsystems/pump-controller.md#paramètres-en-dur-récapitulatif).
- **Anti-cycling** : `minInjectionTimeMs = 30s`, `maxCyclesPerDay = 20` — en dur dans `PumpProtection`, mêmes valeurs que pour le pH.
- ⚠️ **Injection manuelle non gardée** : le bloc Injection manuelle (et les endpoints `/orp/inject/*`) **ignorent** `canDose()`, les limites horaire/journalière, le délai de stabilisation et l'état de la filtration. Le volume injecté est compté dans `orp_daily_ml` et peut le faire dépasser `max_chlorine_ml_per_day`. Responsabilité opérateur — voir [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md).

## Cas limites

- **Sonde absente** : bloc Statistiques affiche `--`, carte calibration reste accessible.
- **WebSocket déconnecté** : éléments dépendants des données capteur taggés `.is-stale` (opacité 0.5, transition 300 ms — [`app.css:3066`](../../data/app.css:3066)). Reset automatique à la reconnexion.
- **Reboot inattendu** : à la première réception WS après un reboot dont `reset_reason` ∈ {`WATCHDOG`, `BROWNOUT`, `PANIC`, `EXTERNAL`, `UNKNOWN`}, un toast d'alerte est affiché une seule fois par session ([`app.js:132`](../../data/app.js:132)).
- **Calibration pendant injection en cours** : bouton Calibrer désactivé.
- **Tension brute figée ou NAN** : bouton « Calibrer » de l'étape finale désactivé.
- **Mode Programmée sans sonde** : fonctionne — c'est justement un cas d'usage (utilisateur sans sonde ORP).

## Interaction MQTT

| Entité HA | Topic | Commande |
|-----------|-------|----------|
| `sensor.*_orp` | `{base}/orp` | — |
| `number.*_orp_target` | `{base}/orp_target` | `{base}/orp_target/set` |
| `binary_sensor.*_orp_dosing` | `{base}/orp_dosing` | — |
| `binary_sensor.*_orp_limit` | `{base}/orp_limit` | — |
| — | `{base}/orp_regulation_mode` (publié, pas encore exposé comme select HA) | — |
| — | `{base}/orp_daily_target_ml` (publié) | — |

## Fichiers

- [`data/index.html:744`](../../data/index.html:744) — structure HTML
- [`data/app.js`](../../data/app.js) — logique mode / calibration calculée / injection manuelle
- [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — branche mode `scheduled` ORP
- [`src/sensors.h`](../../src/sensors.h) — lecture ADS1115 canal ORP, application offset/slope
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — endpoints `/orp/inject/*`
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — validation `orp_regulation_mode` et `orp_daily_target_ml`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — diffusion champs ORP
- [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) — publication `orp_regulation_mode`, `orp_daily_target_ml`

## Documentation liée

- [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) — règles de régulation, anti-cycling, gardes, paramètres en dur.
- [docs/subsystems/sensors.md](../subsystems/sensors.md) — lecture ORP et formule de calibration.
- [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md) — mode `scheduled` aveugle au capteur.
- [ADR-0003](../adr/0003-calibration-orp-cote-client.md) — calibration ORP côté client.
- [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md) — enum à 3 valeurs et booléen miroir `orp_enabled`.
- [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md) — persistance NVS du cumul journalier.
