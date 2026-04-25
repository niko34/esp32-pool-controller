# Page pH — `/ph`

- **Fichier UI** : [`data/index.html:580`](../../data/index.html:580) (section `#view-ph`)
- **URL** : `http://poolcontroller.local/#/ph`

## Rôle

Point d'entrée unique pour **tout** ce qui concerne le pH : mesure, mode de régulation, injection manuelle, historique, calibration.

## Structure

En mode nominal, quatre zones :

1. **Bloc Statistiques** (bandeau compact, sans titre) — valeur pH courante + dosage du jour (barre de progression).
2. **Carte Régulation pH** — sélecteur de mode : `Automatique`, `Programmée`, `Manuelle`. Sous-blocs conditionnels :
   - **Automatique** : pH cible + bouton Sauvegarder ; + informations de calibration (date, bandeau « Calibré ») + bouton **Calibrer**.
   - **Programmée** : volume quotidien en mL (borné par `max_ph_ml_per_day`) + bouton Sauvegarder.
   - **Manuelle** : bloc Injection manuelle (volume + bouton ▶ Injecter / ⏹ Stopper).
3. **Carte Historique** — graphique Chart.js, plages `Tout` / `30j` / `7j` / `24h`.
4. **Carte Calibration** — **remplace** les cartes Régulation + Historique pendant une session de calibration. Assistant guidé : point neutre (pH 7.0) seul, ou neutre + acide (pH 4.0). Protocole en 3 à 4 étapes avec boutons Étape suivante / Calibrer / Annuler.

Le bloc Statistiques reste visible **en permanence**, y compris pendant la calibration.

## Données consommées (WebSocket `/ws`)

**Mesure** : `ph`, `ph_raw`, `ph_voltage_mv` (tension brute ADS1115 canal A0)
**Dosage** : `ph_dosing`, `ph_used_ms` (durée d'injection en cours), `ph_daily_ml`, `ph_limit_reached`
**Mode** : `ph_regulation_mode` (`automatic` / `scheduled` / `manual`), `ph_daily_target_ml`, `ph_enabled` (miroir booléen, voir [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md))
**Config** : `ph_target`, `ph_correction_type` (`ph_minus` / `ph_plus`), `max_ph_ml_per_day`
**Calibration** : `ph_cal_valid`, `ph_calibration_date`, `ph_calibration_temp`
**Injection manuelle** : `ph_inject_remaining_s`
**Stabilisation** : `stabilization_remaining_s`

## Actions

| Action | Endpoint | Auth |
|--------|----------|------|
| Sauvegarder config (mode, cible, volume) | `POST /save-config` | CRITICAL |
| Injection manuelle start | `POST /ph/inject/start?volume=N` (ou `?duration=N`) | WRITE |
| Injection manuelle stop | `POST /ph/inject/stop` | WRITE |
| Calibration pH 7.0 | `POST /calibrate_ph_neutral` | CRITICAL |
| Calibration pH 4.0 | `POST /calibrate_ph_acid` | CRITICAL |
| Effacer calibration | `POST /clear_ph_calibration` | CRITICAL |

Validation côté firmware :
- `ph_regulation_mode` rejeté si hors de l'enum → valeur ignorée.
- `ph_daily_target_ml` > `max_ph_ml_per_day` → HTTP 400.

## Règles firmware appliquées

Voir [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) pour le contrat complet. Résumé :

- **Mode `automatic`** : PID vers `ph_target`, actif uniquement pendant filtration (mode `pilote`), bloqué pendant stabilisation.
- **Mode `scheduled`** : injecte jusqu'à `ph_daily_target_ml` pendant les plages de filtration, **intentionnellement aveugle à la valeur mesurée du pH** ([`pump_controller.cpp:496`](../../src/pump_controller.cpp:496)) — un capteur déréglé ou en cours de remplacement n'arrête pas l'injection. Seules les gardes volumétriques (`ph_limit_minutes`, `max_ph_ml_per_day`) et structurelles (`canDose()`, débit configuré) s'appliquent. `ph_target` n'est pas consulté dans cette branche. Voir [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md).
- **Mode `manual`** : aucune régulation automatique, seule l'injection manuelle (`/ph/inject/*`) pilote la pompe.
- **Limites** : `ph_limit_minutes` (défaut 5 min/h glissante) et `max_ph_ml_per_day` (défaut 300 mL/j) — voir [docs/subsystems/pump-controller.md#paramètres-en-dur-récapitulatif](../subsystems/pump-controller.md#paramètres-en-dur-récapitulatif).
- **Anti-cycling** : `minInjectionTimeMs = 30s`, `maxCyclesPerDay = 20` — en dur dans `PumpProtection` ([`config.h:152`](../../src/config.h:152)).
- **Cumul journalier** : persisté en NVS, reset à minuit local — voir [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md).
- ⚠️ **Injection manuelle non gardée** : le bloc Injection manuelle (et les endpoints `/ph/inject/*`) **ignorent** `canDose()`, la limite horaire, la limite journalière, le délai de stabilisation et l'état de la filtration. Le volume injecté est compté dans `ph_daily_ml` et peut le faire dépasser `max_ph_ml_per_day`. Responsabilité opérateur — voir [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md).

## Cas limites

- **Sonde non calibrée** : bandeau global affiché + carte calibration accessible.
- **WebSocket déconnecté** : éléments dépendants des données capteur taggés `.is-stale` (opacité 0.5, transition 300 ms — [`app.css:3066`](../../data/app.css:3066)). Reset automatique à la reconnexion.
- **Reboot inattendu** : à la première réception WS après un reboot dont `reset_reason` ∈ {`WATCHDOG`, `BROWNOUT`, `PANIC`, `EXTERNAL`, `UNKNOWN`}, un toast d'alerte est affiché une seule fois par session ([`app.js:132`](../../data/app.js:132)). Les redémarrages volontaires (`POWER_ON`, `SW_RESET`, `DEEP_SLEEP`) ne déclenchent pas le toast.
- **Limite journalière atteinte** : message « Limite atteinte — dosage suspendu jusqu'à minuit », barre en rouge.
- **Limite horaire atteinte** : pas d'affichage dédié à date (voir [docs/subsystems/pump-controller.md#paramètres-en-dur-récapitulatif](../subsystems/pump-controller.md#paramètres-en-dur-récapitulatif)).
- **Pendant la calibration** : le bouton Calibrer est désactivé si une injection pH est en cours.

## Interaction MQTT

| Entité HA | Topic | Commande |
|-----------|-------|----------|
| `sensor.*_ph` | `{base}/ph` | — |
| `number.*_ph_target` | `{base}/ph_target` | `{base}/ph_target/set` |
| `binary_sensor.*_ph_dosing` | `{base}/ph_dosing` | — |
| `binary_sensor.*_ph_limit` | `{base}/ph_limit` | — |
| — | `{base}/ph_regulation_mode` (publié, pas exposé comme switch/select HA à date) | — |

## Fichiers

- [`data/index.html:580`](../../data/index.html:580) — structure HTML
- [`data/app.js`](../../data/app.js) — logique mode / sauvegarde / injection manuelle / calibration
- [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — PID + anti-cycling + persistance journalière
- [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp) — `DFRobot_PH`, calibration EEPROM
- [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) — endpoints `/calibrate_ph_*`
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — endpoints `/ph/inject/*`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — diffusion des champs

## Documentation liée

- [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) — règles de régulation, anti-cycling, gardes, paramètres en dur.
- [docs/subsystems/sensors.md](../subsystems/sensors.md) — lecture pH et calibration.
- [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md) — mode `scheduled` aveugle au capteur.
- [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md) — enum à 3 valeurs et booléen miroir `ph_enabled`.
- [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md) — persistance NVS du cumul journalier.
