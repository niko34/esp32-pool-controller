# Page pH — `/ph`

- **Fichier UI** : [`data/index.html`](../../data/index.html) (section `#view-ph`)
- **URL** : `http://poolcontroller.local/#/ph`

## Rôle

Point d'entrée unique pour **tout** ce qui concerne le pH : mesure, mode de régulation, injection manuelle, historique, calibration.

## Structure

En mode nominal, quatre zones :

1. **Bloc Statistiques** (bandeau compact, sans titre) — valeur pH **filtrée** courante en grand (1 décimale, cohérent avec le tableau de bord) + **ligne brute discrète** « brut · médiane · maj » + **rangée de chips** : chip d'état filtre cliquable (cf. [Chip d'état filtre](#chip-détat-filtre-feature-025)) et **chip unique sonde + calibration** cliquable (cf. [Chip d'état sonde](#chip-détat-sonde-feature-024)) + dosage du jour (barre de progression). Depuis feature-034, le chip de calibration distinct est **supprimé** : son information est fusionnée dans le chip sonde.
2. **Carte Régulation pH** — sélecteur de mode : `Automatique`, `Programmée`, `Manuelle`. Sous-blocs conditionnels :
   - **Automatique** : pH cible + bouton Sauvegarder.
   - **Programmée** : volume quotidien en mL (borné par `max_ph_ml_per_day`) + bouton Sauvegarder. Depuis la v2.8.0 (feature-011) : ligne « **Débit calculé : X,X mL/min** » (`#ph_scheduled_flow_line` / `#ph_scheduled_flow_value`, alimentée par le champ WS `ph_scheduled_flow_ml_per_min` ; affiche « — » avec `title` « Hors plage de filtration ou données indisponibles » quand la valeur est `null`) + hint statique « Le volume non injecté avant minuit est perdu (pas de report au lendemain). ». Volontairement **pas** de « sur Y h restantes » côté client : l'horloge du navigateur peut différer de celle de l'ESP32, seul le débit (source firmware) est affiché.
   - **Manuelle** : bloc Injection manuelle (volume + bouton ▶ Injecter / ⏹ Stopper).
3. **Carte Historique** — graphique uPlot (feature-043, ex-Chart.js), plages `Tout` / `30j` / `7j` / `24h`.
4. **Carte Calibration** — **remplace** Régulation + Historique pendant une session de calibration. Écran **guidé par stepper** (feature-034) — voir [Workflow calibration](#workflow-calibration).

Le bloc Statistiques reste visible **en permanence**, y compris pendant la calibration.

> **Calibration accessible dans tous les modes (feature-034)** : le bouton « Calibrer la sonde » (`#ph_cal_trigger_btn`) est désormais affiché et fonctionnel quel que soit le mode de régulation — **automatique, programmée et manuelle**. Depuis l'itération 3, ce bouton est placé **sous la rangée de chips** du bloc Statistiques (et non plus en bas de la carte de régulation dans `#ph-calibration-info`, supprimé). La session de calibration ne modifie pas `ph_regulation_mode` : à la fermeture (`#ph_cal_close_btn`), on **revient au mode précédemment sélectionné** (aucune bascule forcée en automatique).

## Affichage mesure filtrée / brute (feature-025)

La valeur affichée **en grand** dans le bloc Statistiques est la mesure **filtrée** (`phFiltered`, médiane + EMA). C'est aussi la valeur utilisée par la régulation. La fonction `_renderFilterSub('ph', json)` ([`data/app.js`](../../data/app.js)) rend une **ligne discrète** `#ph-filter-sub` :

```
brut 7.24 · médiane 7.24 · maj à l'instant
```

- **brut** = `phRaw` (dernière mesure Atlas non filtrée — diagnostic EMI) ;
- **médiane** = `phMedian` (médiane glissante fenêtre 7) ;
- **maj** = âge de la dernière mesure brute valide (`à l'instant` / `il y a N s/min`).

`--` est affiché si la valeur est `null` (filtre non amorcé / EZO injoignable).

## Chip d'état filtre (feature-025)

À côté de la valeur filtrée, un **chip cliquable** `#ph-filter-chip` reflète l'état du filtre. Classification **côté UI** par `_classifyFilterState(json, 'ph', st)` ([`data/app.js`](../../data/app.js)), évaluée dans cet ordre :

| Condition (ordre de priorité) | Label chip | Classe / couleur |
|---|---|---|
| `phRaw` invalide (`null`) | « EZO indisponible » | `unknown` (gris) |
| `phFilterUnstable === true` | « Capteur instable » | `bad` (rouge) |
| `phFilterReady === false` | « Stabilisation… » | `warn` (ambré) |
| `phRejectedCount` a augmenté récemment (`< FILTER_REJECT_WINDOW_MS`) | « Pics rejetés » | `warn2` (ambré) |
| sinon | « Mesure stable » | `good` (vert) |

> Le compteur de rejets étant **cumulatif** côté firmware, l'UI ne signale « Pics rejetés » que si `phRejectedCount` a augmenté dans la fenêtre récente (`st.lastRejectAt`), pas tant que le total reste non nul.

### Modal détail filtre

Clic sur le chip → `<dialog id="ph-filter-modal">` (`État filtre pH`). `_renderFilterModalValues('ph', json)` remplit : valeur brute, médiane, filtrée, **Pics rejetés** (`phRejectedCount`), âge dernière mesure, **Filtre prêt** (Oui/Non), et une ligne conditionnelle **raison de blocage dosage** (`phDoseBlockedReason`, traduite en clair par `_doseBlockReasonFr()` ; affiche aussi « Mélange en cours » si `phMixingDelayActive`).

## Chip d'état sonde (feature-024)

Sous la valeur pH dans le bloc Statistiques, un **chip cliquable** affiche en permanence l'état diagnostique de la sonde pH (pente Atlas EZO + décalage zéro). L'évaluation est faite **côté UI** — le firmware expose les valeurs brutes (cf. [docs/subsystems/sensors.md](../subsystems/sensors.md#pente-sonde-ph--feature-024)).

### Placement

Élément `<button id="ph-probe-chip" class="chip chip--probe ...">` à l'intérieur du bloc Statistiques pH (`#ph-card-stats`), sous l'affichage pH (1 décimale). Le chip est focusable (rôle bouton, `Enter`/`Espace` ouvrent le modal).

### Classification (UI)

Source : champs WS `phSlopeAcid`, `phSlopeBase`, `phSlopeZero`, `phSlopeAgeMs`, `phCalPoints`. Les seuils sont **dans `data/app.js`** (constantes `PH_PROBE_*` + fonction `classifyPhProbe()`) — modifier sans reflasher le firmware.

> **feature-034 — chip unique sonde + calibration** : ce chip est désormais l'**unique** indicateur « sonde + calibration » du bloc Statistiques pH (le chip de calibration séparé `#ph-cal-chip` a été supprimé). Pour `phCalPoints < 2`, il affiche l'**état de calibration** ; pour `≥ 2`, il affiche le **diagnostic de pente** (santé sonde).

| `phCalPoints` | min(acide, base) | \|zéro\| (mV) | Variante CSS | Libellé | Couleur |
|---|---|---|---|---|---|
| `null` ou `< 0` | — | — | `chip--probe-unknown` | « EZO indisponible » | gris |
| `0` | — | — | `chip--probe-bad` | « Calibration requise » | rouge |
| `1` | — | — | `chip--probe-warn` | « Calibration 1/2 » | ambré |
| `≥ 2` | NaN / `null` | — | `chip--probe-unknown` | « Pente non disponible » | gris |
| `≥ 2` | `≥ 95 %` | `≤ 15` | `chip--probe-good` | « Sonde excellente · 98 % » | vert (`var(--success)`) |
| `≥ 2` | `90–95 %` | `≤ 30` | `chip--probe-warn` | « Sonde correcte · 92 % » | ambré clair (`var(--warn)`) |
| `≥ 2` | `85–90 %` | `≤ 30` | `chip--probe-warn2` | « Sonde usée · 87 % » | ambré (`var(--warn)`) |
| `≥ 2` | `< 85 %` OU `> 30` | — | `chip--probe-bad` | « Sonde à remplacer · 81 % » | rouge (`var(--danger)`, point pulsant) |

### État stale (âge > 36 h)

Si `phSlopeAgeMs > 36 h` (`PH_PROBE_STALE_MS`), la classe `chip--probe-stale` est ajoutée (encadré jaune) en plus de la variante de couleur. Indique que la dernière mesure de pente date — la valeur affichée est toujours valide mais l'utilisateur peut forcer un refresh.

### Modal détails

Au clic (ou `Enter`/`Espace`), un `<dialog id="ph-probe-modal">` s'ouvre :

- **Pente acide** — `<id="ph-probe-acid">` au format `99.7 %` (1 décimale). `--` si NaN ou `phCalPoints < 2`.
- **Pente base** — `<id="ph-probe-base">` au format `100.3 %`.
- **Décalage zéro** — `<id="ph-probe-zero">` au format `-0.89 mV` (2 décimales). `--` si firmware EZO ancien ne le rapporte pas.
- **Vérifié** — `<id="ph-probe-age">` libellé lisible : « à l'instant », « il y a 14 h », « il y a 2 j 3 h », « jamais ».
- **Bouton « Rafraîchir »** (`#ph-probe-refresh`) → `POST /debug/ph_slope_refresh`. Texte bascule en « Rafraîchissement… » jusqu'à ce que `phSlopeAgeMs` redescende sous 60 s (succès → toast « Pente sonde pH rafraîchie ») ou timeout 8 s (`PH_PROBE_REFRESH_TIMEOUT_MS` → toast « Sonde non joignable »).
- **Bouton « Fermer »** (`#ph-probe-close`) ou clic backdrop ou `ESC`.

### Fallback `<dialog>` non supporté

Si `dialog.showModal` est absent (vieux navigateur), le chip est rendu non cliquable (`aria-disabled="true"`, `cursor: default`) avec un `console.warn` — la valeur reste affichée mais le détail n'est pas accessible. Aucun crash, aucun fallback DOM custom.

### Cas particuliers

- **WebSocket déconnecté** : le chip ne change pas immédiatement (dernière classification mémorisée par `latestSensorData`). À la reconnexion, l'UI re-classifie sur le payload reçu.
- **EZO pH débranché en runtime** : après 2 échecs consécutifs (`kEzoBusFailMaxConsecutive`), les 4 champs WS passent à `null` → chip gris « Pente non disponible » — pas de crash, pas de toast.
- **Refresh en cours et payload WS qui apporte une nouvelle valeur** : si `phSlopeAgeMs < 60 s`, l'état refresh est résolu en succès (toast).

## État de calibration & bouton « Calibrer la sonde » (feature-034)

Depuis l'itération 2 de feature-034, l'état de calibration EZO n'est plus présenté par trois callouts noyés dans la carte Régulation (supprimés). L'itération suivante a **fusionné** le chip de calibration séparé (`#ph-cal-chip`, supprimé) dans le **chip unique sonde + calibration** `#ph-probe-chip` (cf. [Chip d'état sonde](#chip-détat-sonde-feature-024)) : pour `phCalPoints < 2`, ce chip affiche directement l'état de calibration. Il ne reste donc plus qu'**un seul chip** côté sonde/calibration pH.

Le **bouton de calibration** (`#ph_cal_trigger_btn`) est rendu **sous la rangée de chips** avec un **libellé fixe « Calibrer la sonde »**, accompagné d'un **hint** texte (`#ph_cal_hint`). Le bouton/hint sont pilotés par `renderCalibrationStatus()` ([`data/app.js`](../../data/app.js)) à partir de `phCalPoints` (WebSocket / `GET /data`, voir [API.md](../API.md)) :

| `phCalPoints` | Bouton « Calibrer la sonde » | Hint | Régulation auto |
|---------------|---------------------|------|-----------------|
| `null` (avant données) | actif | — | — |
| `-1` | **désactivé** | « EZO pH injoignable — vérifiez le câblage I²C et l'alimentation. » | Inhibée (cond #5 pool-chemistry) |
| `0` | actif | « Régulation auto inhibée tant que non calibré. » | Inhibée |
| `1` | actif | « Régulation auto inhibée (1/2 point). » | Inhibée |
| `≥ 2` | actif | — | Active |

- Le **libellé du bouton est fixe** (« Calibrer la sonde ») quel que soit l'état (itération 3) ; le bouton est **sous la rangée de chips** (`#ph_cal_trigger_btn`), **toujours accessible** dans tous les modes — y compris quand la sonde est calibrée (recalibration possible).
- On ne désactive le bouton que sur **preuve d'EZO injoignable** (`phCalPoints < 0`). Le garde « injection en cours » reste **prioritaire** et peut le désactiver indépendamment.
- L'état de calibration ORP, lui, conserve son **chip dédié** `#orp-cal-chip` (l'ORP n'a pas de chip sonde) piloté par `setCalChip('orp', …)`.
- ⚠️ Quand `phCalPoints == 1`, le chip ne distingue pas mid seul de low seul (l'EZO ne renvoie que le compteur via `Cal,?`). Risque résiduel mineur — l'utilisateur sait normalement quel point il vient d'effectuer.
- **Garde injection prioritaire** : le bouton « Calibrer la sonde » est forcé **désactivé** pendant une injection pH en cours, indépendamment de l'état de calibration (cf. [Cas limites](#cas-limites)).

## Workflow calibration

### Architecture UI — écran guidé (feature-034)

La carte `#ph-card-calibration` présente un **stepper** (`<ol class="calibration-steps" id="ph-cal-steps">`, réutilisant le pattern `.step` / `.calibration-steps` déjà employé pour la calibration température). Les étapes pour le pH (2 points) :

| Idx | Étape | Action UI |
|-----|-------|-----------|
| 0 | Rincer la sonde, plonger en solution **pH 7.00** | bouton « C'est fait → » (`.cal-step-advance`) |
| 1 | Attendre la stabilisation + **calibrer le point milieu** | minuterie + bouton « Calibrer le point 7.0 » (`#btn-cal-ph-mid`) → `Cal,mid,7.00` |
| 2 | Rincer, plonger en solution **pH 4.00** | bouton « C'est fait → » |
| 3 | Attendre la stabilisation + **calibrer le point bas** | minuterie + bouton « Calibrer le point 4.0 » (`#btn-cal-ph-low`) → `Cal,low,4.00` |
| 4 | Terminé | — |

**États visuels du stepper** (`setCalStepState()` dans [`data/app.js`](../../data/app.js)) : chaque `.step` reçoit une classe selon sa position relative à l'étape courante —

| État | Classe CSS | `aria-current` |
|------|-----------|----------------|
| Faite (✓) | `is-completed` | — |
| En cours | `is-active` | `step` |
| Restante | `is-upcoming` | — |

Le focus est déplacé sur le contrôle de l'étape active (`focusActiveStep()`) pour l'accessibilité clavier.

**Minuterie de stabilisation** (aide **non bloquante**) : chaque étape d'attente propose un bouton « Démarrer la minuterie de stabilisation (60 s) » (`.cal-timer-start`). Le compte à rebours est géré **côté client** par timestamp (`startCalTimer()`, constante `CAL_STAB_DURATION_S = 60`), affiche `mm:ss` + une barre de progression dans `#ph-cal-timer` (`role="timer" aria-live="polite"`), et bascule en « Stabilisation atteinte, vous pouvez calibrer » à 0. L'utilisateur **peut calibrer avant la fin** — la minuterie n'inhibe pas le bouton « Calibrer ». Annulation par « Annuler » (`.cal-timer-cancel`).

**Indicateur de stabilité Δ60 s** (purement **indicatif**) : `renderStability()` calcule l'amplitude `max − min` de la mesure **brute** sur une fenêtre glissante de ~60 s (`CAL_STAB_WINDOW_MS`) et affiche `Δ60 s : <valeur> — stable/en cours`. Seuils cosmétiques `CAL_STAB_THRESHOLD = { ph: 0.05, orp: 5 }`. La fenêtre est réinitialisée à chaque changement de solution (avance de stepper / calibration réussie).

Après une calibration EZO réussie, `completeCalStep()` marque l'étape « calibrer » comme faite (✓) et avance le stepper. La fermeture de la carte (`#ph_cal_close_btn`) annule toute minuterie en cours et restaure la carte Régulation dans le mode initial.

> **Readout de calibration = valeur brute.** `updateCalibrationReadouts()` alimente `#cal-ph-mid-readout` et `#cal-ph-low-readout` avec la mesure **brute** (`json.phRaw`, repli sur `json.ph` si absent), **pas** la valeur lissée affichée ailleurs dans l'UI. Raison : en changeant de solution étalon (ex. pH 7 → pH 4, saut ≈ 3.0 > `maxStep` du filtre), le filtre rejette les mesures pendant ~1 min (12 lectures × 5 s, `kSensorFilterResyncRejects`, feature-033) avant re-sync ; le lissé resterait figé sur l'ancienne valeur, rendant la calibration impraticable. Le brut suit le potentiel réel de l'électrode, indispensable pour juger la stabilité avant de calibrer. Le reste de l'UI (dashboard, valeur principale pH, MQTT) continue d'afficher le lissé.

L'ordre d'exécution est libre (mid puis low, ou inverse). En pratique, les utilisateurs commencent par le milieu (référence neutre) puis enchaînent avec le bas.

### Workflow temporel

1. L'utilisateur prépare la sonde (rinçage eau distillée, plongée dans le tampon, attente 1 min).
2. Clic sur **« Calibrer le point 7.0 »** → POST `/calibrate_ph {step:"mid"}`.
3. Réponse 200 immédiate `{success:true, queued:true, step:"mid"}` → toast info **« Calibration en cours… »**.
4. Le firmware exécute la commande EZO via la queue `_ezoQueue` (~900 ms transaction I²C bloquante dans `loopTask`).
5. Après calibration, `_phCalCachedPoints` est rafraîchi automatiquement → publié sur le WS → l'UI observe la transition.
6. **Polling 15 s** côté UI sur `latestSensorData.phCalPoints` :
   - Si `phCalPoints` augmente → toast succès **« Calibration mid réussie (N points) »** + mise à jour des badges.
   - Si timeout 15 s sans transition → toast warning **« Calibration : pas de retour, vérifier la connexion EZO »**.
7. Idem pour le point bas (clic sur l'autre sous-bloc).

### Cas d'erreur

| Réponse HTTP | Toast affiché |
|--------------|---------------|
| 400 `step must be 'mid' or 'low'` | « Erreur : payload invalide » (bug client) |
| 503 `calibration queue saturée — réessayer dans 1s` | « Calibration en cours, réessayer dans 1 seconde » |
| Erreur réseau | « Erreur de communication, vérifier le réseau » |

## Données consommées (WebSocket `/ws`)

**Mesure** : `ph` (= valeur **filtrée**, 3 décimales) ; `phRaw`, `phMedian`, `phFiltered` (feature-025)
**État filtre** : `phFilterReady`, `phFilterUnstable`, `phRejectedCount`, `phMixingDelayActive`, `phDoseBlockedReason` (feature-025) — voir [Chip d'état filtre](#chip-détat-filtre-feature-025)
**Calibration** : `phCalPoints` (`-1..3`) — voir [Chip d'état de calibration](#chip-détat-de-calibration-feature-034)
**Pente sonde** : `phSlopeAcid`, `phSlopeBase`, `phSlopeZero`, `phSlopeAgeMs` (feature-024) — voir [Chip d'état sonde](#chip-détat-sonde-feature-024)
**Dosage** : `ph_dosing`, `ph_used_ms`, `ph_daily_ml`, `ph_limit_reached`
**Mode** : `ph_regulation_mode` (`automatic` / `scheduled` / `manual`), `ph_daily_target_ml`, `ph_enabled` (miroir, voir [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md))
**Répartition scheduled (feature-011, v2.8.0)** : `ph_scheduled_flow_ml_per_min` (float 1 décimale, mL/min — débit moyen planifié restant ; `null` hors mode `scheduled`, hors plage de filtration ou heure ESP32 invalide → l'UI affiche « — »)
**Config** : `ph_target`, `ph_correction_type` (`ph_minus` / `ph_plus`), `max_ph_ml_per_day`
**Injection manuelle** : `ph_inject_remaining_s`
**Stabilisation** : `stabilization_remaining_s`

> Champs **supprimés** depuis feature-021 : `ph_raw`, `ph_voltage_mv`, `ph_cal_valid` (remplacé par `phCalPoints`), `ph_calibration_date`, `ph_calibration_temp` (calibration mémorisée dans le module EZO, pas en NVS ESP32).

## Actions

| Action | Endpoint | Payload | Auth |
|--------|----------|---------|------|
| Sauvegarder config (mode, cible, volume) | `POST /save-config` | JSON config complète | CRITICAL |
| Injection manuelle start | `POST /ph/inject/start?volume=N` | querystring | WRITE |
| Injection manuelle stop | `POST /ph/inject/stop` | — | WRITE |
| Calibration EZO (mid ou low) | `POST /calibrate_ph` | `{"step":"mid"}` ou `{"step":"low"}` | WRITE |
| Effacer calibration EZO | `POST /calibrate_clear` | `{"sensor":"ph"}` | WRITE |
| Refresh pente sonde (feature-024) | `POST /debug/ph_slope_refresh` | — | aucune |

> Routes legacy supprimées (404) : `/calibrate_ph_neutral`, `/calibrate_ph_acid`, `/clear_ph_calibration`. Voir [ADR-0014](../adr/0014-migration-atlas-ezo.md).

Validation côté firmware :
- `ph_regulation_mode` rejeté si hors de l'enum → valeur ignorée.
- `ph_daily_target_ml` > `max_ph_ml_per_day` → HTTP 400.

## Règles firmware appliquées

Voir [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) pour le contrat complet, notamment la section [Garde-fous `canDose`](../subsystems/pump-controller.md#garde-fous-candose). Résumé pH :

- **Mode `automatic`** : PID vers `ph_target`, actif uniquement pendant filtration (mode `pilote`), bloqué pendant stabilisation. **Inhibé tant que `phCalPoints < 2`** (calibration EZO incomplète).
- **Mode `scheduled`** : injecte jusqu'à `ph_daily_target_ml` pendant les plages de filtration, **intentionnellement aveugle à la valeur mesurée du pH** — un capteur déréglé ou en cours de remplacement n'arrête pas l'injection. Voir [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md).
  - ✅ **Répartition 24 h (v2.8.0, feature-011,** [ADR-0021](../adr/0021-repartition-scheduled.md)**)** : le volume quotidien n'est plus injecté d'un bloc mais **réparti par fenêtres de 15 min** sur la plage de filtration restante, **bornée à minuit**. Le volume de fenêtre (`restant / fenêtres restantes`, recalculé à chaque fenêtre — donc auto-corrigé après changement de cible, injection manuelle ou reboot) est borné par le **budget horaire partagé** (`ph_limit_minutes`, [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)) et **reporté** à la fenêtre suivante si l'injection correspondante durerait moins de 30 s (anti short-cycling). **Pas de rattrapage J+1** : le reliquat non injecté à minuit est perdu (log `info`). Heure ESP32 invalide → dosage suspendu. **Nuance pompe rapide** : avec un débit élevé (ex. 60 mL/min) et une petite cible, les premières fenêtres sont reportées (volume < équivalent 30 s) — les injections démarrent quand l'horizon se resserre ; c'est le fonctionnement voulu du report.
- **Mode `manual`** : aucune régulation automatique, seule l'injection manuelle pilote la pompe.
- **Limites** : `ph_limit_minutes` (défaut 5 min/h glissante) et `max_ph_ml_per_day` (défaut 300 mL/j).
- **Anti-rafale court terme** : ≤ 6 cycles/min ET ≤ 20 cycles/15 min (correctif Pass 3.5).
- **Stabilisation post-cal pH** : 5 min (`kStabilizationDurationPhMs`) après chaque calibration EZO réussie. Le dosage est refusé pendant cette fenêtre (cond #3 pool-chemistry).
- **Cumul journalier** : persisté en NVS, reset à minuit local — voir [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md).
- ✅ **Injection manuelle gardée (v2.6.0, feature-006)** : `POST /ph/inject/start` passe par les gardes de `evaluateManualInject()` — dans l'ordre : watchdog, filtration (sauf mode `continu`), stabilisation post-cal **de la pompe pH**, double démarrage, **limite journalière prédictive** (frontière `==` acceptée ; depuis la **v2.9.2**, une demande supérieure au reliquat est **écrêtée automatiquement au reliquat** au lieu d'être refusée — le refus `daily_limit` ne subsiste que si le reliquat est nul ou < 1 s de pompe, avec `remaining_ml` arrondi vers le bas), **limite horaire partagée avec l'auto** (`ph_limit_minutes`, [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)), cycles/jour (20) et anti-rafale (6/min, 20/15 min — ring partagé avec l'auto). Refus = **409 JSON** `{"error","message","seconds_remaining"?,"remaining_ml"?}` — voir [Comportement UI injection manuelle](#comportement-ui-injection-manuelle-v260) et [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md#gardes-des-injections-manuelles-feature-006). Le mode de régulation et la mesure capteur ne sont volontairement **pas** vérifiés (le manuel est disponible dans tous les modes et aveugle à la mesure). Arrêt cyclique automatique conservé si la filtration s'arrête pendant l'injection. `POST /ph/inject/stop` n'est **jamais** gardé.
- **Bornage durée** : `duration` plafonné à 600 s (10 min, `kManualInjectMaxDurationS`) au lieu de 3600 s avant v2.1.2. Les gardes évaluent le volume **post-plafonnement** (ce qui sera réellement injecté).

### Comportement UI injection manuelle (v2.6.0)

**Blocage proactif** : à chaque push WS, `_updateInjectButtons()` → `getInjectBlockReason()` ([`data/app.js`](../../data/app.js)) désactive le bouton « ▶ Injecter » (`#ph_inject_btn`) et affiche la raison dans le hint `#ph_inject_block_hint` (+ `title`) quand un blocage est **déjà connu côté client** :

| Blocage connu (ordre d'évaluation) | Hint affiché |
|---|---|
| `ph_limit_reached === true` | « Limite journalière atteinte — injection impossible jusqu'à minuit » |
| `stabilization_remaining_s > 0` | « Stabilisation post-calibration en cours — réessayer dans N s/min » |
| Filtration arrêtée en mode `pilote` | « Filtration arrêtée — injection impossible (sécurité chimique) » |

Le firmware reste l'**autorité** : miroir best-effort seulement (WS déconnecté → bouton cliquable, le 409 tranche). Le bouton « ⏹ Arrêter » n'est **jamais** désactivé (coupe-circuit opérateur).

**Toasts sur refus 409** (mapping `INJECT_REFUSAL_MESSAGES` par code `error`) :

| Situation | Réaction UI |
|-----------|-------------|
| 409 JSON (8 codes : `watchdog_inactive`, `filtration_off`, `stabilization_in_progress`, `already_injecting`, `daily_limit`, `hourly_limit`, `max_cycles`, `burst_limit`) | Bouton restauré + toast rouge français par code ; `daily_limit` ajoute « Reste disponible aujourd'hui : N mL » (`remaining_ml`), `stabilization_in_progress` ajoute « Réessayer dans N s/min » (`seconds_remaining`) |
| 409 non JSON (firmware ancien / proxy) | Fallback : toast filtration (seule cause 409 de l'ancien format) |
| Filtration **s'arrête en cours** d'injection | Toast rouge capté via WS log critical (`[Injection] pH INTERROMPUE`) : « Injection pH interrompue : la filtration s'est arrêtée. Relancez l'injection après reprise de la filtration. » |
| Erreur HTTP autre (4xx/5xx) | Lecture du body texte affiché si court (< 200 chars), sinon message générique. |

**Comportement attendu utilisateur** : démarrer la filtration **avant** d'injecter, ou relancer manuellement après reprise. Pas de reprise automatique — l'injection en cours est perdue.

## Cas limites

- **EZO pH injoignable** : chip rouge « EZO pH injoignable » sur la carte Régulation. Régulation auto inhibée (cond #5).
- **Lecture pH stale > 20 s** : valeur du bloc Statistiques en `is-stale` (opacité 0.5). Régulation auto inhibée (cond #1) + alerte MQTT `pool/alerts/sensor_stale`.
- **Calibration en cours et utilisateur clique 2× sur le même bouton** : queue à 4 slots accepte sans erreur les 4 premières mises en file. Au-delà : 503 + toast « Calibration en cours, réessayer dans 1 seconde ».
- **WebSocket déconnecté** : éléments dépendants des données capteur taggés `.is-stale` (transition 300 ms). Reset automatique à la reconnexion.
- **Reboot inattendu** : à la première réception WS après un reboot dont `reset_reason` ∈ {`WATCHDOG`, `BROWNOUT`, `PANIC`, `EXTERNAL`, `UNKNOWN`}, un toast d'alerte est affiché une seule fois par session.
- **Limite journalière atteinte** : message « Limite atteinte — dosage suspendu jusqu'à minuit », barre en rouge.
- **Pendant la calibration** : le bouton « Calibrer le point » de l'étape concernée affiche un spinner. Les autres actions UI restent disponibles.
- **Injection pH en cours (feature-034)** : le clic sur « Calibrer » (`#ph_cal_trigger_btn`) est **bloqué** si une injection pH est active — condition `(ph_inject_remaining_s > 0) || (ph_dosing === true)`, toast « Calibration impossible pendant une injection pH ». Ce garde existait déjà côté ORP ; il est désormais ajouté côté pH (symétrie de sécurité).

## Interaction MQTT

| Entité HA | Topic | Commande |
|-----------|-------|----------|
| `sensor.*_ph` | `{base}/ph` (3 décimales depuis 2.0.0) | — |
| `sensor.*_ph_cal_points` | `{base}/ph_cal_points` (`-1..3`) | — |
| `number.*_ph_target` | `{base}/ph_target` | `{base}/ph_target/set` |
| `select.*_ph_regulation_mode` | `{base}/ph_regulation_mode` | `{base}/ph_regulation_mode/set` (feature-009, v2.7.0) |
| `binary_sensor.*_ph_dosing` | `{base}/ph_dosing` | — |
| `binary_sensor.*_ph_limit` | `{base}/ph_limit` | — |
| `sensor.*_ph_slope_acid` | `{base}/ph_slope_acid` (% — feature-024) | — |
| `sensor.*_ph_slope_base` | `{base}/ph_slope_base` (% — feature-024) | — |
| `sensor.*_ph_slope_zero` | `{base}/ph_slope_zero` (mV — feature-024) | — |
| (alerte) | `{base}/alerts/calibration_required` | — |

> **Mode de régulation commandable depuis HA (feature-009, v2.7.0)** : l'entité `select.*_ph_regulation_mode` (options `automatic` / `scheduled` / `manual`) permet de changer le mode depuis Home Assistant, par la **même voie** que le sélecteur de la carte Régulation (miroir `ph_enabled`, [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md), persistance NVS). La **carte Régulation de l'UI web reste la source principale** ; un changement initié par HA se reflète dans l'UI via les canaux existants (broadcast WS `ph_regulation_mode` + `/get-config`) — aucun code UI ajouté. Valeur hors enum → ignorée + log `warning`. Un changement pendant une injection en cours ne l'interrompt pas : le nouveau mode prend effet au cycle de régulation suivant. Voir [docs/MQTT.md](../MQTT.md#entités-ajoutées-en-feature-009-modes-de-régulation-commandables-v270).

## Fichiers

- [`data/index.html`](../../data/index.html) — structure HTML (carte Régulation + carte Calibration 2 sous-blocs)
- [`data/app.js`](../../data/app.js) — logique mode / sauvegarde / injection manuelle / calibration EZO (POST + polling)
- [`data/app.css`](../../data/app.css) — classe `.cal-point-block` introduite pour les sous-blocs parallèles
- [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — `canDose(0)` 10 garde-fous + stabilisation par pompe
- [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp) — pilotage Atlas EZO pH + queue commandes
- [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp) — mini-classe pilote EZO
- [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) — endpoints `/calibrate_ph`, `/calibrate_clear`
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — endpoints `/ph/inject/*` (coquille des gardes : collecte + 409 JSON, feature-006)
- [`src/dosing_logic.h`](../../src/dosing_logic.h), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) — `evaluateManualInject()` : gardes d'injection manuelle pures (feature-006)
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — diffusion `ph` (= filtré) + `phRaw/Median/Filtered/FilterReady/...` (feature-025)
- [`src/sensor_filter.h`](../../src/sensor_filter.h), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) — filtre médiane + EMA (feature-025)

## Documentation liée

- [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) — règles de régulation, garde-fous `canDose`, anti-rafale, pause mélange.
- [docs/subsystems/sensors.md](../subsystems/sensors.md) — lecture EZO pH, queue, cache cal_points, chaîne de filtrage.
- [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md) — mode `scheduled` aveugle au capteur.
- [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md) — enum à 3 valeurs et booléen miroir `ph_enabled`.
- [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md) — persistance NVS du cumul journalier.
- [ADR-0014](../adr/0014-migration-atlas-ezo.md) — migration Atlas EZO (supersedes ADR-0001).
- [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) — régulation P temporisée sur mesure filtrée (feature-025).
- [ADR-0021](../adr/0021-repartition-scheduled.md) — répartition scheduled par fenêtres de 15 min (feature-011, v2.8.0).
