# Page ORP — `/orp`

- **Fichier UI** : [`data/index.html`](../../data/index.html) (section `#view-orp`)
- **URL** : `http://poolcontroller.local/#/orp`

## Rôle

Point d'entrée unique pour **tout** ce qui concerne l'ORP (potentiel redox, piloté par injection de chlore liquide) : mesure, mode de régulation, injection manuelle, historique, calibration. Structure **symétrique** de la page [pH](page-ph.md).

## Structure

En mode nominal, quatre zones :

1. **Bloc Statistiques** (bandeau compact, sans titre) — valeur ORP **filtrée** courante en grand (mV) + **ligne brute discrète** « brut · médiane · maj » + **rangée de chips** : chip d'état filtre cliquable (cf. [Chip d'état filtre](#chip-détat-filtre-feature-025)) et **chip d'état de calibration** (cf. [Chip d'état de calibration](#chip-détat-de-calibration-feature-034)) + dosage du jour (barre de progression, borne `max_orp_ml_per_day`).
2. **Carte Régulation ORP** — sélecteur de mode : `Automatique`, `Programmée`, `Manuelle`. Sous-blocs :
   - **Automatique** : ORP cible (mV) + bouton Sauvegarder.
   - **Programmée** : volume quotidien de chlore en mL + bouton Sauvegarder. Depuis la v2.8.0 (feature-011) : ligne « **Débit calculé : X,X mL/min** » (`#orp_scheduled_flow_line` / `#orp_scheduled_flow_value`, alimentée par le champ WS `orp_scheduled_flow_ml_per_min` ; affiche « — » avec `title` « Hors plage de filtration ou données indisponibles » quand la valeur est `null`) + hint statique « Le volume non injecté avant minuit est perdu (pas de report au lendemain). ». Volontairement **pas** de « sur Y h restantes » côté client : l'horloge du navigateur peut différer de celle de l'ESP32, seul le débit (source firmware) est affiché.
   - **Manuelle** : bloc Injection manuelle.
3. **Carte Historique** — graphique uPlot (feature-043, ex-Chart.js), plages `Tout` / `30j` / `7j` / `24h`, zone ombrée 600–800 mV (`ORP_MIN`/`ORP_MAX`, app.js) indiquant la plage de désinfection.
4. **Carte Calibration** — **remplace** Régulation + Historique pendant une session. Écran **guidé par stepper** (feature-034), 1 seul point de calibration (architecture symétrique de la page pH) — voir [Workflow calibration](#workflow-calibration).

Le bloc Statistiques reste visible en permanence.

> **Calibration accessible dans tous les modes (feature-034)** : le bouton « Calibrer la sonde » (`#orp_cal_trigger_btn`) est désormais affiché et fonctionnel en **automatique, programmée et manuelle**. Depuis l'itération 3, ce bouton est placé **sous la rangée de chips** du bloc Statistiques (et non plus en bas de la carte de régulation dans `#orp-calibration-info`, supprimé). La session de calibration ne modifie pas `orp_regulation_mode` : à la fermeture, on **revient au mode précédemment sélectionné**.

## Affichage mesure filtrée / brute (feature-025)

La valeur affichée **en grand** est l'ORP **filtré** (`orpFiltered`, médiane + EMA), également utilisé par la régulation. La ligne discrète `#orp-filter-sub` (`_renderFilterSub('orp', json)`) affiche :

```
brut 720 mV · médiane 720 mV · maj à l'instant
```

- **brut** = `orpRaw` ; **médiane** = `orpMedian` ; **maj** = âge de la dernière mesure brute valide. `--` si `null`.

## Chip d'état filtre (feature-025)

Chip cliquable `#orp-filter-chip` reflétant l'état du filtre ORP. Classification UI par `_classifyFilterState(json, 'orp', st)` (même logique que la page pH) :

| Condition (ordre) | Label | Classe / couleur |
|---|---|---|
| `orpRaw` invalide | « EZO indisponible » | `unknown` (gris) |
| `orpFilterUnstable === true` | « Capteur instable » | `bad` (rouge) |
| `orpFilterReady === false` | « Stabilisation… » | `warn` (ambré) |
| `orpRejectedCount` augmenté récemment | « Pics rejetés » | `warn2` (ambré) |
| sinon | « Mesure stable » | `good` (vert) |

Clic → `<dialog id="orp-filter-modal">` (`État filtre ORP`) : brut, médiane, filtré, **Pics rejetés**, âge, **Filtre prêt**, et raison de blocage dosage conditionnelle (`orpDoseBlockedReason`, ou « Mélange en cours » si `orpMixingDelayActive`). La pause mélange ORP est plus longue (20 min vs 15 min pour le pH).

## Chip d'état de calibration (feature-034)

Depuis l'itération 2 de feature-034, l'état de calibration EZO n'est plus présenté par trois callouts noyés dans la carte Régulation (supprimés). Il est condensé dans un **chip d'état de calibration** placé dans la **rangée de chips** du bloc Statistiques (à côté du chip filtre) + un **hint** texte. Depuis l'itération 3, le **bouton de calibration** (`#orp_cal_trigger_btn`) est rendu **sous la rangée de chips** avec un **libellé fixe « Calibrer la sonde »** (le libellé adaptatif de l'itération 2 est abandonné) ; l'état de calibration reste entièrement porté par le chip + le hint. L'EZO ORP n'accepte qu'**un seul point de calibration** côté Atlas (pas d'état « 1/2 » comme le pH).

**Élément** : `<span id="orp-cal-chip" role="status" aria-live="polite">` — **non cliquable** (annonce d'état, pas d'action). Classification côté UI par `renderCalibrationStatus()` / `setCalChip('orp', …)` ([`data/app.js`](../../data/app.js)) à partir de `orpCalPoints` (WebSocket / `GET /data`).

| `orpCalPoints` | Libellé chip | Variante CSS | Bouton « Calibrer la sonde » | Hint | Régulation auto |
|----------------|--------------|--------------|---------------------|------|-----------------|
| `null` (avant données) | « Calibration — » | `chip--probe-unknown` (gris) | actif | — | — |
| `-1` | « EZO indisponible » | `chip--probe-unknown` (gris) | **désactivé** | « EZO ORP injoignable — vérifiez le câblage I²C et l'alimentation. » | Inhibée (cond #5) |
| `0` | « Calibration requise » | `chip--probe-bad` (rouge) | actif | « Régulation auto inhibée tant que non calibré. » | Inhibée |
| `≥ 1` | « Calibré » | `chip--probe-good` (vert) | actif | — | Active |

- Le **libellé du bouton est fixe** (« Calibrer la sonde ») quel que soit l'état (itération 3) ; le bouton est **sous la rangée de chips** (`#orp_cal_trigger_btn`), **toujours accessible** dans tous les modes — y compris une fois la sonde calibrée (recalibration possible).
- Tant que les données ne sont pas reçues (`orpCalPoints == null`) et hors EZO injoignable, le chip affiche « Calibration — » et le bouton reste **actif**.
- **Garde injection prioritaire** : le bouton « Calibrer la sonde » est forcé **désactivé** pendant une injection ORP en cours, indépendamment de l'état de calibration (cf. [Cas limites](#cas-limites)).

## Workflow calibration

### Architecture UI — écran guidé (feature-034)

La carte `#orp-card-calibration` présente un **stepper** (`<ol class="calibration-steps" id="orp-cal-steps">`, même pattern `.step` / `.calibration-steps` que le pH). Étapes ORP (1 point) :

| Idx | Étape | Action UI |
|-----|-------|-----------|
| 0 | Saisir la référence + rincer, plonger en **solution étalon** | champ `#orp-cal-reference` + bouton « C'est fait → » (`.cal-step-advance`) |
| 1 | Attendre la stabilisation + **calibrer** | minuterie + bouton « Calibrer » (`#btn-cal-orp`) → `Cal,<reference>` |
| 2 | Terminé | — |

- **Champ d'entrée** `orp-cal-reference` : valeur de référence en mV (range `0..1000`, défaut `470`). Standards usuels : 225 mV (kit Hanna), 470 mV (kit Atlas), 650 mV (rare).
- **États visuels du stepper** : `is-completed` (✓) / `is-active` (`aria-current="step"`) / `is-upcoming`, identiques à la page pH (voir [page-ph.md](page-ph.md#architecture-ui--écran-guidé-feature-034)).
- **Minuterie de stabilisation** (aide non bloquante, `CAL_STAB_DURATION_S = 60`) : bouton « Démarrer la minuterie (60 s) » → compte à rebours `mm:ss` + barre dans `#orp-cal-timer` (`role="timer"`), message « Stabilisation atteinte » à 0. On peut calibrer avant la fin.
- **Indicateur de stabilité Δ60 s** (indicatif) : amplitude `max − min` de la mesure brute sur ~60 s, seuil cosmétique `5` mV.
- **Readout live** : `<div class="readout">` (`#cal-orp-readout`) affichant la valeur ORP **brute** lue toutes les 5 s. `updateCalibrationReadouts()` utilise `json.orpRaw` (repli sur `json.orp` si absent), **pas** la valeur lissée. Raison : en changeant de solution étalon (saut > `maxStep` du filtre), le lissé reste figé ~1 min (12 lectures × 5 s, `kSensorFilterResyncRejects`, feature-033) avant re-sync ; le brut suit le potentiel réel de l'électrode, indispensable pour juger la stabilité avant de calibrer. Le reste de l'UI (valeur principale ORP, MQTT) continue d'afficher le lissé.

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

**Mesure** : `orp` (= valeur **filtrée**, entier mV) ; `orpRaw`, `orpMedian`, `orpFiltered` (feature-025)
**État filtre** : `orpFilterReady`, `orpFilterUnstable`, `orpRejectedCount`, `orpMixingDelayActive`, `orpDoseBlockedReason` (feature-025) — voir [Chip d'état filtre](#chip-détat-filtre-feature-025)
**Calibration** : `orpCalPoints` (`-1..1`) — voir [Chip d'état de calibration](#chip-détat-de-calibration-feature-034)
**Dosage** : `orp_dosing`, `orp_used_ms`, `orp_daily_ml`, `orp_limit_reached`
**Mode** : `orp_regulation_mode`, `orp_daily_target_ml`, `orp_enabled` (miroir — voir [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md))
**Répartition scheduled (feature-011, v2.8.0)** : `orp_scheduled_flow_ml_per_min` (float 1 décimale, mL/min — débit moyen planifié restant ; `null` hors mode `scheduled`, hors plage de filtration ou heure ESP32 invalide → l'UI affiche « — »)
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
  - ✅ **Répartition 24 h (v2.8.0, feature-011,** [ADR-0021](../adr/0021-repartition-scheduled.md)**)** : le volume quotidien de chlore n'est plus injecté d'un bloc mais **réparti par fenêtres de 15 min** sur la plage de filtration restante, **bornée à minuit**. Le volume de fenêtre (`restant / fenêtres restantes`, recalculé à chaque fenêtre — donc auto-corrigé après changement de cible, injection manuelle ou reboot) est borné par le **budget horaire partagé** (`orp_limit_minutes`, [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)) et **reporté** à la fenêtre suivante si l'injection correspondante durerait moins de 30 s (anti short-cycling). **Pas de rattrapage J+1** : le reliquat non injecté à minuit est perdu (log `info`). Heure ESP32 invalide → dosage suspendu. **Nuance pompe rapide** : avec un débit élevé (ex. 60 mL/min) et une petite cible, les premières fenêtres sont reportées (volume < équivalent 30 s) — les injections démarrent quand l'horizon se resserre ; c'est le fonctionnement voulu du report.
- **Mode `manual`** : seule l'injection manuelle est autorisée.
- **Limites** : `orp_limit_minutes` (défaut 10 min/h glissante) et `max_chlorine_ml_per_day` (défaut 500 mL/j).
- **Anti-rafale court terme** : ≤ 6 cycles/min ET ≤ 20 cycles/15 min (correctif Pass 3.5).
- **Stabilisation post-cal ORP** : 3 min (`kStabilizationDurationOrpMs`) après chaque calibration EZO réussie.
- **Pause mélange ORP** : 20 min (`kOrpMixingDelayMs`) **après chaque injection** — le dosage suivant est différé le temps que le chlore s'homogénéise et que la sonde reflète l'effet (anti-surdosage) ; la régulation ORP est donc plafonnée à ~1 injection / 20 min. Depuis **v2.19.1**, ce délai est affiché sur le widget du tableau de bord (« Pause mélange : … »), plus seulement dans le modal « État filtre ». Chronologie complète : [pump-controller.md § Cycle de régulation](../subsystems/pump-controller.md#cycle-de-régulation--chronologie-stabilisation--pause-mélange).
- ✅ **Injection manuelle gardée (v2.6.0, feature-006)** : `POST /orp/inject/start` passe par les gardes de `evaluateManualInject()` — dans l'ordre : watchdog, filtration (sauf mode `continu`), stabilisation post-cal **de la pompe ORP**, double démarrage, **limite journalière prédictive** (frontière `==` acceptée ; depuis la **v2.9.2**, une demande supérieure au reliquat est **écrêtée automatiquement au reliquat** au lieu d'être refusée — le refus `daily_limit` ne subsiste que si le reliquat est nul ou < 1 s de pompe, avec `remaining_ml` arrondi vers le bas), **limite horaire partagée avec l'auto** (`orp_limit_minutes`, [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)), cycles/jour (20) et anti-rafale (6/min, 20/15 min — ring partagé avec l'auto). Refus = **409 JSON** `{"error","message","seconds_remaining"?,"remaining_ml"?}` — voir [Comportement UI injection manuelle](#comportement-ui-injection-manuelle-v260) et [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md#gardes-des-injections-manuelles-feature-006). Le mode de régulation et la mesure capteur ne sont volontairement **pas** vérifiés. Arrêt cyclique automatique conservé si la filtration tombe pendant l'injection (alerte MQTT `orp_injection_aborted` sur `{base}/alerts`). `POST /orp/inject/stop` n'est **jamais** gardé.
- **Bornage durée** : `duration` plafonné à 600 s (`kManualInjectMaxDurationS`) au lieu de 3600 s avant v2.1.2. Les gardes évaluent le volume **post-plafonnement**.

### Comportement UI injection manuelle (v2.6.0)

**Blocage proactif** : identique à la page pH (`_updateInjectButtons()` → `getInjectBlockReason()`) — bouton « ▶ Injecter » (`#orp_inject_btn`) désactivé + raison dans le hint `#orp_inject_block_hint` quand le blocage est connu côté client : `orp_limit_reached`, `stabilization_remaining_s > 0`, ou filtration arrêtée en mode `pilote`. Le firmware reste l'autorité (WS déconnecté → bouton cliquable, le 409 tranche). Le bouton « ⏹ Arrêter » n'est **jamais** désactivé.

**Toasts sur refus 409** :

| Situation | Réaction UI |
|-----------|-------------|
| 409 JSON (8 codes : `watchdog_inactive`, `filtration_off`, `stabilization_in_progress`, `already_injecting`, `daily_limit`, `hourly_limit`, `max_cycles`, `burst_limit`) | Bouton restauré + toast rouge français par code ; `daily_limit` ajoute le reliquat (`remaining_ml`), `stabilization_in_progress` le compte à rebours (`seconds_remaining`) |
| 409 non JSON (firmware ancien / proxy) | Fallback : toast filtration (seule cause 409 de l'ancien format) |
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
- **Injection ORP en cours (feature-034)** : le clic sur « Calibrer » (`#orp_cal_trigger_btn`) est **bloqué** si une injection ORP est active — condition `(orp_inject_remaining_s > 0) || (orp_dosing === true)`, toast « Calibration impossible pendant une injection ORP ».

## Interaction MQTT

| Entité HA | Topic | Commande |
|-----------|-------|----------|
| `sensor.*_orp` | `{base}/orp` (= filtré) | — |
| `sensor.*_orp_raw` | `{base}/orp_raw` (feature-025) | — |
| `sensor.*_orp_filtered` | `{base}/orp_filtered` (feature-025) | — |
| `binary_sensor.*_orp_filter_ready` | `{base}/orp_filter_ready` (feature-025) | — |
| `sensor.*_orp_cal_points` | `{base}/orp_cal_points` (`-1..1`) | — |
| `number.*_orp_target` | `{base}/orp_target` | `{base}/orp_target/set` |
| `select.*_orp_regulation_mode` | `{base}/orp_regulation_mode` | `{base}/orp_regulation_mode/set` (feature-009, v2.7.0) |
| `binary_sensor.*_orp_dosing` | `{base}/orp_dosing` | — |
| `binary_sensor.*_orp_limit` | `{base}/orp_limit` | — |
| (alerte) | `{base}/alerts/calibration_required` | — |

> **Mode de régulation commandable depuis HA (feature-009, v2.7.0)** : l'entité `select.*_orp_regulation_mode` (options `automatic` / `scheduled` / `manual`) permet de changer le mode depuis Home Assistant, par la **même voie** que le sélecteur de la carte Régulation (miroir `orp_enabled`, [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md), persistance NVS). La **carte Régulation de l'UI web reste la source principale** ; un changement initié par HA se reflète dans l'UI via les canaux existants (broadcast WS `orp_regulation_mode` + `/get-config`) — aucun code UI ajouté. Valeur hors enum → ignorée + log `warning`. Un changement pendant une injection en cours ne l'interrompt pas : le nouveau mode prend effet au cycle de régulation suivant. Voir [docs/MQTT.md](../MQTT.md#entités-ajoutées-en-feature-009-modes-de-régulation-commandables-v270).

## Fichiers

- [`data/index.html`](../../data/index.html) — structure HTML (carte Régulation + carte Calibration 1 bloc)
- [`data/app.js`](../../data/app.js) — logique mode / calibration EZO (POST + polling) / injection manuelle
- [`data/app.css`](../../data/app.css) — classe `.cal-point-block` réutilisée
- [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — `canDose(1)` 10 garde-fous
- [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp) — pilotage Atlas EZO ORP
- [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp)
- [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) — endpoints `/calibrate_orp`, `/calibrate_clear`
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — endpoints `/orp/inject/*` (coquille des gardes : collecte + 409 JSON, feature-006)
- [`src/dosing_logic.h`](../../src/dosing_logic.h), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) — `evaluateManualInject()` : gardes d'injection manuelle pures (feature-006)
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — validation `orp_regulation_mode` et `orp_daily_target_ml`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — diffusion `orp` (= filtré) + `orpRaw/Median/Filtered/FilterReady/...` (feature-025)
- [`src/sensor_filter.h`](../../src/sensor_filter.h), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) — filtre médiane + EMA (feature-025)
- [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) — publication `orp_regulation_mode`, `orp_daily_target_ml`, `orp_cal_points`, `orp_cal_valid`, topics filtre (feature-025)

## Documentation liée

- [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) — règles de régulation, garde-fous, anti-rafale, pause mélange.
- [docs/subsystems/sensors.md](../subsystems/sensors.md) — lecture EZO ORP, queue, cache cal_points, chaîne de filtrage.
- [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) — régulation P temporisée sur mesure filtrée (feature-025).
- [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md) — mode `scheduled` aveugle au capteur.
- [ADR-0003](../adr/0003-calibration-orp-cote-client.md) — calibration ORP côté client (historique PCB v1, supersédée).
- [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md) — enum à 3 valeurs et booléen miroir `orp_enabled`.
- [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md) — persistance NVS du cumul journalier.
- [ADR-0014](../adr/0014-migration-atlas-ezo.md) — migration Atlas EZO (supersedes ADR-0001).
- [ADR-0021](../adr/0021-repartition-scheduled.md) — répartition scheduled par fenêtres de 15 min (feature-011, v2.8.0).
