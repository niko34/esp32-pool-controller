# Page ORP — `/orp`

- **Fichier UI** : [`data/index.html`](../../data/index.html) (section `#view-orp`)
- **URL** : `http://poolcontroller.local/#/orp`

## Rôle

Point d'entrée unique pour **tout** ce qui concerne l'ORP (potentiel redox, piloté par injection de chlore liquide) : mesure, mode de régulation, injection manuelle, historique, calibration. Structure **symétrique** de la page [pH](page-ph.md).

## Structure

En mode nominal, quatre zones :

1. **Bloc Statistiques** (bandeau compact, sans titre) — valeur ORP courante (mV) + dosage du jour (barre de progression, borne `max_orp_ml_per_day`).
2. **Carte Régulation ORP** — sélecteur de mode : `Automatique`, `Programmée`, `Manuelle`. Sous-blocs :
   - **Automatique** : ORP cible (mV) + bouton Sauvegarder. Affichage du **statut de calibration EZO** (cf. [Statut de calibration](#statut-de-calibration-ezo)) :
     - **Callout vert** « Calibré ✓ » si `orpCalPoints >= 1`.
     - **Chip ambrée** « Régulation ORP inhibée — non calibré » si `orpCalPoints == 0`.
     - **Chip rouge** « EZO ORP injoignable » si `orpCalPoints == -1`.
   - **Programmée** : volume quotidien de chlore en mL + bouton Sauvegarder.
   - **Manuelle** : bloc Injection manuelle.
3. **Carte Historique** — graphique Chart.js, plages `Tout` / `30j` / `7j` / `24h`, zone ombrée 600–750 mV indiquant la plage de désinfection.
4. **Carte Calibration** — **remplace** Régulation + Historique pendant une session. **1 sous-bloc unique** `.cal-point-block` (architecture symétrique de la page pH, sans le sélecteur 1pt/2pts du firmware v1).

Le bloc Statistiques reste visible en permanence.

## Statut de calibration EZO

Champ source : `orpCalPoints` (WebSocket / `GET /data`). L'EZO ORP n'accepte qu'**un seul point de calibration** côté Atlas, contrairement au pH (mid + low + high).

| `orpCalPoints` | UI | Régulation auto |
|----------------|-----|-----------------|
| `-1` | Chip rouge « EZO ORP injoignable » | Inhibée (cond #5) |
| `0` | Chip ambrée « Régulation ORP inhibée — non calibré » | Inhibée |
| `1` | Callout vert « Calibré ✓ » | Active |

## Workflow calibration

### Architecture UI

1 seul bloc `.cal-point-block` dans `#orp-card-calibration`, contenant :

- **Champ d'entrée** `orp-cal-reference` : valeur de référence en mV (range `0..1000`, défaut `470`). Standards usuels : 225 mV (kit Hanna), 470 mV (kit Atlas), 650 mV (rare).
- **Micro-étapes** : « Plonger la sonde dans la solution standard » + « Attendre 1 min ».
- **Readout live** : `<div class="readout">` affichant la valeur ORP brute lue toutes les 5 s.
- **Bouton « Calibrer »**.

### Workflow temporel

1. L'utilisateur saisit la valeur de référence de sa solution standard dans le champ.
2. Il prépare la sonde (rinçage eau distillée, plongée dans le tampon, attente 1 min).
3. Clic sur **« Calibrer »** → POST `/calibrate_orp {reference: <mV>}`.
4. Réponse 200 immédiate `{success:true, queued:true, reference:470}` → toast info **« Calibration en cours… »**.
5. Le firmware exécute `Cal,<reference>` via la queue (~900 ms transaction I²C).
6. **Polling 15 s** sur `latestSensorData.orpCalPoints` :
   - Si `orpCalPoints` augmente (passe de `0` à `1`) → toast succès **« Calibration ORP réussie »**.
   - Si `orpCalPoints` était déjà à `1` (recalibration) → fallback succès silencieux après 5 s sans transition de compteur (l'EZO ne change pas le compteur lors d'une recalibration sur un point existant).
   - Si timeout 15 s → toast warning **« Calibration : pas de retour »**.

### Cas d'erreur

| Réponse HTTP | Toast affiché |
|--------------|---------------|
| 400 `reference must be 0..1000 mV` | « Valeur de référence hors plage (0–1000 mV) » |
| 503 `calibration queue saturée — réessayer dans 1s` | « Calibration en cours, réessayer dans 1 seconde » |

## Données consommées (WebSocket `/ws`)

**Mesure** : `orp` (entier mV)
**Calibration** : `orpCalPoints` (`-1..1`) — voir [Statut de calibration](#statut-de-calibration-ezo)
**Dosage** : `orp_dosing`, `orp_used_ms`, `orp_daily_ml`, `orp_limit_reached`
**Mode** : `orp_regulation_mode`, `orp_daily_target_ml`, `orp_enabled` (miroir — voir [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md))
**Config** : `orp_target`, `max_orp_ml_per_day` (= `max_chlorine_ml_per_day`)
**Injection manuelle** : `orp_inject_remaining_s`
**Stabilisation** : `stabilization_remaining_s`

> Champs **supprimés** depuis feature-021 : `orp_raw`, `orp_voltage_mv`, `orp_calibration_offset`, `orp_calibration_slope`, `orp_calibration_reference`, `orp_calibration_temp`, `orp_calibration_date` (calibration mémorisée dans le module EZO, pas en NVS ESP32). Le champ `orp_cal_valid` est conservé en miroir pour rétrocompat HA (`true` ssi `orpCalPoints >= 1`).

## Actions

| Action | Endpoint | Payload | Auth |
|--------|----------|---------|------|
| Sauvegarder config (mode, cible, volume) | `POST /save-config` | JSON config complète | CRITICAL |
| Injection manuelle start | `POST /orp/inject/start?volume=N` | querystring | WRITE |
| Injection manuelle stop | `POST /orp/inject/stop` | — | WRITE |
| Calibration EZO ORP | `POST /calibrate_orp` | `{"reference": <mV float>}` | WRITE |
| Effacer calibration EZO | `POST /calibrate_clear` | `{"sensor":"orp"}` | WRITE |

> Avant feature-021 : la calibration ORP se faisait entièrement côté UI client (calcul offset/slope, persistance via `POST /save-config` avec `orp_calibration_*`) — voir [ADR-0003](../adr/0003-calibration-orp-cote-client.md). Cette voie est **supersédée** par la calibration interne au module Atlas EZO ORP. ADR-0003 reste applicable historiquement (PCB v1).

Validation côté firmware :
- `orp_regulation_mode` rejeté si hors de l'enum.
- `orp_daily_target_ml` > `max_orp_ml_per_day` → HTTP 400.
- `reference` hors plage `0..1000` mV → HTTP 400.

## Règles firmware appliquées

Voir [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md), notamment [Garde-fous `canDose`](../subsystems/pump-controller.md#garde-fous-candose). Résumé ORP :

- **Mode `automatic`** : PID vers `orp_target`, actif uniquement pendant filtration (mode `pilote`), bloqué pendant stabilisation. **Inhibé tant que `orpCalPoints < 1`** (calibration EZO incomplète).
- **Mode `scheduled`** : injecte jusqu'à `orp_daily_target_ml` pendant filtration, **intentionnellement aveugle à la valeur ORP mesurée**. Voir [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md).
- **Mode `manual`** : seule l'injection manuelle est autorisée.
- **Limites** : `orp_limit_minutes` (défaut 10 min/h glissante) et `max_chlorine_ml_per_day` (défaut 500 mL/j).
- **Anti-rafale court terme** : ≤ 6 cycles/min ET ≤ 20 cycles/15 min (correctif Pass 3.5).
- **Stabilisation post-cal ORP** : 3 min (`kStabilizationDurationOrpMs`) après chaque calibration EZO réussie.
- ⚠️ **Injection manuelle — garde filtration uniquement (v2.1.2)** : les endpoints `/orp/inject/*` vérifient **uniquement** que la filtration est active (sauf mode `continu`). Refus HTTP 409 au démarrage si filtration arrêtée + arrêt cyclique automatique si la filtration tombe pendant l'injection (alerte MQTT `orp_injection_aborted` sur `{base}/alerts`). Voir [Comportement UI injection manuelle](#comportement-ui-injection-manuelle-v212) et [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md#garde-filtration-sur-linjection-manuelle-v212).
- ⚠️ **Limites volumétriques toujours non gardées** : `canDose()`, `orp_limit_minutes`, `max_chlorine_ml_per_day`, stabilisation et mode de régulation **ne sont pas vérifiés**. Le volume injecté est compté dans `orp_daily_ml`. Responsabilité opérateur.
- **Bornage durée** : `duration` plafonné à 600 s (`kManualInjectMaxDurationS`) au lieu de 3600 s avant v2.1.2.

### Comportement UI injection manuelle (v2.1.2)

| Situation | Réaction UI |
|-----------|-------------|
| Clic « Injecter » avec filtration **arrêtée** (sauf mode `continu`) | Bouton restauré + toast rouge : « Injection refusée : la filtration doit être active avant d'injecter (sécurité chimique : pas de circulation = surdosage local). » |
| Filtration **s'arrête en cours** d'injection | Toast rouge capté via WS log critical (`[Injection] ORP INTERROMPUE`) : « Injection ORP/chlore interrompue : la filtration s'est arrêtée. Relancez l'injection après reprise de la filtration. » |
| Erreur HTTP autre (4xx/5xx) | Lecture du body texte affiché si court (< 200 chars), sinon message générique. |

**Comportement attendu utilisateur** : démarrer la filtration **avant** d'injecter, ou relancer manuellement après reprise. Pas de reprise automatique — l'injection en cours est perdue.

## Cas limites

- **EZO ORP injoignable** : chip rouge « EZO ORP injoignable » sur la carte Régulation.
- **Lecture ORP stale > 20 s** : valeur en `is-stale` + alerte MQTT `pool/alerts/sensor_stale`. Régulation auto inhibée.
- **Recalibration sur un capteur déjà à `orpCalPoints == 1`** : fallback succès UI après 5 s (le compteur EZO ne change pas, le polling de transition échoue → on bascule sur un succès optimiste).
- **WebSocket déconnecté** : éléments capteur taggés `.is-stale`. Reset à la reconnexion.
- **Reboot inattendu** : toast à la première réception WS si `reset_reason` ∈ {`WATCHDOG`, `BROWNOUT`, `PANIC`, `EXTERNAL`, `UNKNOWN`}.
- **Mode Programmée sans sonde** : fonctionne — c'est justement un cas d'usage (utilisateur sans sonde ORP). Aucune dépendance sur `orpCalPoints` dans cette branche.

## Interaction MQTT

| Entité HA | Topic | Commande |
|-----------|-------|----------|
| `sensor.*_orp` | `{base}/orp` | — |
| `sensor.*_orp_cal_points` | `{base}/orp_cal_points` (`-1..1`) | — |
| `number.*_orp_target` | `{base}/orp_target` | `{base}/orp_target/set` |
| `binary_sensor.*_orp_dosing` | `{base}/orp_dosing` | — |
| `binary_sensor.*_orp_limit` | `{base}/orp_limit` | — |
| (alerte) | `{base}/alerts/calibration_required` | — |

## Fichiers

- [`data/index.html`](../../data/index.html) — structure HTML (carte Régulation + carte Calibration 1 bloc)
- [`data/app.js`](../../data/app.js) — logique mode / calibration EZO (POST + polling) / injection manuelle
- [`data/app.css`](../../data/app.css) — classe `.cal-point-block` réutilisée
- [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — `canDose(1)` 10 garde-fous
- [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp) — pilotage Atlas EZO ORP
- [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp)
- [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) — endpoints `/calibrate_orp`, `/calibrate_clear`
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — validation `orp_regulation_mode` et `orp_daily_target_ml`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — diffusion champs ORP + `orpCalPoints`
- [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) — publication `orp_regulation_mode`, `orp_daily_target_ml`, `orp_cal_points`, `orp_cal_valid`

## Documentation liée

- [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) — règles de régulation, garde-fous, anti-rafale.
- [docs/subsystems/sensors.md](../subsystems/sensors.md) — lecture EZO ORP, queue, cache cal_points.
- [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md) — mode `scheduled` aveugle au capteur.
- [ADR-0003](../adr/0003-calibration-orp-cote-client.md) — calibration ORP côté client (historique PCB v1, supersédée).
- [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md) — enum à 3 valeurs et booléen miroir `orp_enabled`.
- [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md) — persistance NVS du cumul journalier.
- [ADR-0014](../adr/0014-migration-atlas-ezo.md) — migration Atlas EZO (supersedes ADR-0001).
