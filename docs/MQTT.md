# MQTT - ESP32 Pool Controller

## Configuration

| Paramètre | Description | Défaut |
|-----------|-------------|--------|
| Serveur | Adresse du broker MQTT | — |
| Port | Port du broker | 1883 |
| Topic de base | Préfixe de tous les topics | `pool/sensors` |
| Utilisateur / Mot de passe | Authentification broker (optionnel) | — |

Configuration via **Paramètres → MQTT** dans l'interface web ou via `POST /save-config`.

---

## Topics publiés

Tous les topics utilisent le préfixe configurable (ex: `pool/sensors`). Les valeurs sont publiées avec **rétention** (retain=true) sauf indication contraire.

### Capteurs

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/temperature` | `24.5` | Température de l'eau (°C) |
| `{base}/ph` | `7.234` | **Valeur OFFICIELLE** pH (médiane 9 + lissage τ=45 s + offset logiciel `ph_offset`), **CORRIGÉE** (**3 décimales** depuis 2.0.0 — voir [ADR-0014](adr/0014-migration-atlas-ezo.md)). Depuis la v2.68.0 (feature-152) : **absente (non publiée, `null`) pendant la montée en régime, sonde figée ou mesure périmée — plus de repli silencieux** sur une autre valeur (ni brut, ni dernière mesure). Topics de diagnostic séparés ci-dessous. |
| `{base}/orp` | `720` | **Valeur OFFICIELLE** ORP (mV), même règle que `{base}/ph` depuis la v2.68.0 : plus de repli sur le brut pendant la montée en régime. |
| `{base}/ph_cal_points` | `2` | Points de calibration EZO pH (entier `-1..3`, `-1` = EZO injoignable). Retain. Voir [feature-021](../specs/features/done/feature-021-migration-atlas-ezo.md). |
| `{base}/orp_cal_points` | `1` | Points de calibration EZO ORP (entier `-1..1`, `-1` = EZO injoignable). Retain. |
| `{base}/ph_slope_acid` | `99.7` | Pente acide sonde pH EZO en % (1 décimale). Retain. Edge-triggered ([feature-024](../specs/features/done/feature-024-pente-sonde-ph.md)). |
| `{base}/ph_slope_base` | `100.3` | Pente base sonde pH EZO en % (1 décimale). Retain. Edge-triggered. |
| `{base}/ph_slope_zero` | `-0.89` | Décalage zéro sonde pH EZO en mV (2 décimales). Retain. Non publié si firmware EZO ancien. |

> **Sémantique brut / corrigé du pH (feature-067, v2.20.0)** — l'offset logiciel `ph_offset` (`pH corrigé = pH brut EZO + ph_offset`) n'a provoqué **aucun renommage d'entité, aucun nouveau topic, aucune suppression**. Seule la **valeur** de deux topics existants change :
>
> | Topic | `unique_id` HA | Contenu | Offset appliqué ? |
> |-------|----------------|---------|-------------------|
> | `{base}/ph` | `poolcontroller_ph` | **valeur officielle** (lissée, **corrigée**) — celle qui décide ET s'affiche | ✅ oui |
> | ~~`{base}/ph_filtered`~~ | ~~`poolcontroller_ph_filtered`~~ | **RETIRÉ en v2.68.0** (feature-152) — `{base}/ph` porte désormais directement la valeur officielle, ce topic était redondant | — |
> | `{base}/ph_median` | *(pas d'entité HA)* | médiane **corrigée**, **diagnostic seul** depuis v2.68.0 | ✅ oui |
> | `{base}/ph_raw` | `poolcontroller_ph_raw` | pH **brut Atlas EZO**, jamais corrigé | ❌ non |
>
> ⚠️ **Conséquence côté Home Assistant** : `sensor.*_ph` garde son `entity_id` et son historique, mais **change de valeur** le jour où un offset non nul est enregistré → une **marche** apparaît dans les graphiques HA. C'est le comportement voulu ([ADR-0039](adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)) : renommer les entités aurait cassé toutes les automatisations et tous les dashboards existants pour un simple décalage de mesure. Pour comparer brut et corrigé, superposer `sensor.*_ph_raw` et `sensor.*_ph`.
>
> **Aucune entité `ph_offset` n'est publiée** : l'offset est un paramètre de configuration, exposé par `GET /get-config` et le message WebSocket `config`, pas une mesure. En créer une entité HA aurait ajouté un point de commande sur l'**entrée de la régulation chimique** hors du chemin validé. La valeur de l'offset apparaît dans les messages d'alerte `ph_abnormal`, `ph_guard` et `orp_ph_inhibit` (voir [Alertes](#alertes)).
>
> Le test de vivacité du capteur (`phStale` de l'alerte `{base}/alerts/sensor_stale`) consomme le **brut** : un offset ne peut ni masquer ni provoquer une détection de sonde muette.

### Filtration

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/install_mode` | `managed` / `powered` / `external` | **Mode d'installation** (feature-056, v2.19.0). Retain. Décrit le câblage : `managed` = PoolController pilote la filtration ; `powered` = alimenté par le circuit de filtration ; `external` = filtration tierce signalée. Valeurs brutes sur le fil ; le select HA les affiche traduites (voir feature-056 ci-dessous). Voir [ADR-0026](adr/0026-mode-installation.md). |
| `{base}/filtration_state` | `ON` / `OFF` | État du relais de filtration |
| `{base}/filtration_mode` | `auto` / `manual` / `force` / `off` | Mode de filtration courant — **la seule autorité qui décide si la pompe tourne** (feature-146, v2.63.0, [ADR-0093](adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)). Valeurs brutes sur le fil, **inchangées** ; le select HA les affiche traduites — voir ci-dessous. Sémantique : `auto` = **Auto**, créneau calculé selon la température, ajusté quand elle change ; `manual` = **Programme**, créneau fixé par l'utilisateur, jamais recalculé ; `force` = **Marche**, la pompe tourne sans expiration jusqu'au prochain changement de mode ; `off` = **Arrêt**, la pompe est arrêtée sans expiration jusqu'au prochain changement de mode. ⚠️ L'ancien « ordre direct » (`forceOn`/`forceOff`, minuteur 4 h) qui pouvait rendre un changement de mode sans effet est **supprimé**. |
| `{base}/filtration_start` | `HH:MM` (ex. `08:00`) | Heure de début du créneau de filtration (feature-051, v2.16.0). Retain. En mode Auto, calculée à partir de la **moyenne** de température du cycle précédent et appliquée uniquement **à minuit** (bug-126, [ADR-0079](adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)) — elle ne change plus en cours de journée. En mode **Programme** (`manual`), heure fixée par l'utilisateur. |
| `{base}/filtration_end` | `HH:MM` (ex. `20:00`) | Heure de fin du créneau de filtration (feature-051, v2.16.0). Retain. Même sémantique. |

### Éclairage

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/lighting_state` | `ON` / `OFF` | État du relais d'éclairage (contrôle manuel) |

### Boost (feature-053, v2.18.0)

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/boost` | `ON` / `OFF` | État du **Mode Boost** (surchloration temporaire du jour). Retain. `ON` tant que le boost est actif ; retombe automatiquement à `OFF` au prochain minuit local (expiration). Voir [ADR-0025](adr/0025-mode-boost.md). |
| `{base}/rescue` | `ON` / `OFF` | État de la **Chloration de secours** (feature-140, v2.55.0) — surcouche aveugle à l'ORP, auto-expirante 48 h glissantes. Retain. Voir [ci-dessous](#topic-et-entité-ajoutés-en-feature-140-chloration-de-secours-v2550). |
| `{base}/rescue_lift_reason` | `0` – `3` | Raison de la dernière fin de la Chloration de secours (feature-140). Retain. Voir [ci-dessous](#topic-et-entité-ajoutés-en-feature-140-chloration-de-secours-v2550). |
| `{base}/rescue_ml` | entier (mL) | **v2.56.0 (feature-141)** — Volume quotidien de la Chloration de secours **ARMÉ** si active, **PROPOSÉ** sinon. Retain, dédupliqué. Voir [ci-dessous](#topic-et-entité-ajoutées-en-feature-141-volume-de-secours-saisi-v2560). |
| `{base}/lighting_schedule` | `ON` / `OFF` | Programmation horaire de l'éclairage active/inactive (feature-052, v2.17.0). Retain. Reflète le booléen `lightingCfg.scheduleEnabled`. Indépendant du relais manuel. Exposé en HA comme `select` « Mode Éclairage » (Programmation/Désactivé) via templates — le fil reste `ON`/`OFF` (bug-ha-eclairage-select, v2.17.2). |
| `{base}/lighting_start` | `HH:MM` (ex. `20:00`) | Heure de début du créneau d'éclairage (feature-052, v2.17.0). Retain. |
| `{base}/lighting_end` | `HH:MM` (ex. `23:30`) | Heure de fin du créneau d'éclairage (feature-052, v2.17.0). Retain. |

### Dosage

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/ph_dosage` | `45.2` | Volume dosé pH aujourd'hui (ml) |
| `{base}/orp_dosage` | `120.5` | Volume dosé ORP/chlore aujourd'hui (ml) |
| `{base}/ph_dosing` | `ON` / `OFF` | **Session de dosage pH ouverte** — ⚠️ **sens changé en v2.36.0**, voir la note ci-dessous : la pompe peut être **à l'arrêt** |
| `{base}/orp_dosing` | `ON` / `OFF` | Pompe doseuse ORP en cours d'injection (**inchangé**) |
| `{base}/ph_limit` | `ON` / `OFF` | Limite journalière pH atteinte |
| `{base}/orp_limit` | `ON` / `OFF` | Limite journalière ORP atteinte |
| `{base}/ph_stock_low` | `ON` / `OFF` | Volume pH restant sous le seuil d'alerte |
| `{base}/orp_stock_low` | `ON` / `OFF` | Volume chlore restant sous le seuil d'alerte |
| `{base}/ph_remaining_ml` | `1500` | Volume de produit pH restant dans le bidon (ml) |
| `{base}/orp_remaining_ml` | `3200` | Volume de produit chlore restant dans le bidon (ml) |
| `{base}/ph_daily_ml` | `45.2` | Cumul journalier injecté pH (mL) — retain, dédup au cycle 10 s, retombe à 0 à minuit (feature-050). Alimente l'entité HA `sensor` « Dosage pH aujourd'hui » |
| `{base}/orp_daily_ml` | `120.5` | Cumul journalier injecté chlore (mL) — retain, dédup au cycle 10 s, retombe à 0 à minuit (feature-050). Alimente l'entité HA `sensor` « Dosage Chlore aujourd'hui » |
| `{base}/ph_integral` | `7450.2` | Terme intégral pH accumulé (pH·s, **signé**) — retain, dédup (v2.26.0). Voir [ci-dessous](#topics-et-entités-ajoutés-en-v2260-terme-intégral-ph) |
| `{base}/ph_integral_term` | `2.759` | Contribution du terme intégral au débit demandé (mL/min) — retain, dédup (v2.26.0) |
| `{base}/ph_integral_pct` | `46.0` | Remplissage de l'anti-windup (% du plafond, valeur absolue) — retain, dédup (v2.26.0) |
| `{base}/ph_ff_ml_per_min` | `1.30` | Dose de base apprise appliquée à la session auto (mL/min, 2 décimales, 0 hors session) — retain, dédup (v2.28.0). Voir [ci-dessous](#topics-et-entités-ajoutés-en-v2280-dose-de-base-apprise--feedforward-ph) |
| `{base}/ph_ff_status` | `1` | Code d'état du feedforward pH (0-4, contrat figé) — retain, dédup (v2.28.0) |
| `{base}/ph_ff_learned_ml_per_min` | `1.41` | Dose de base **apprise** (mL/min) — **conserve sa valeur hors session**, à ne pas confondre avec `ph_ff_ml_per_min` — retain, dédup (v2.46.0). Voir [ci-dessous](#topics-et-entités-ajoutés-en-v2460-composition-du-débit-dinjection) |
| `{base}/ph_p_term_ml_per_min` | `0.35` | Écart courant pH `kp·e` (mL/min, **signé**) — retain, dédup (v2.46.0) |
| `{base}/ph_demand_ml_per_min` | `4.33` | Demande totale bornée pH (mL/min) — **pas** `min(demande, soutenable)` — retain, dédup (v2.46.0) |
| `{base}/orp_p_term_ml_per_min` | `1.20` | Écart courant ORP (mL/min, signé) — retain, dédup (v2.46.0) |
| `{base}/orp_demand_ml_per_min` | `1.20` | Demande ORP (= terme P borné, régulation P-pure) — retain, dédup (v2.46.0) |

> ⚠️ **v2.36.0 (feature-094, [ADR-0062](adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md)) — `ph_dosing` change de SENS.** **Aucune entité nouvelle, aucun topic nouveau, aucun renommage** : c'est la **signification** de la valeur qui change.
>
> | | Avant (≤ 2.35.0) | Depuis 2.36.0 |
> |---|---|---|
> | `{base}/ph_dosing` (et `ph_dosing_active` du JSON [`{base}/diagnostic`](#diagnostic)) | `ON` ⇔ un débit est commandé (≈ la pompe tourne) | `ON` ⇔ une **session de dosage est ouverte** — **la pompe peut être à l'arrêt** |
>
> Le verrou de session pH suit désormais la **porte** (décidée sur une moyenne glissante de 10 min de la mesure filtrée) et non plus le débit instantané. Pendant une **prolongation** (durée minimale de session de 30 min murales, débit plafonné au feedforward puis nul), `ph_dosing` **reste `ON` pompe à l'arrêt** — jusqu'à 3 h au pire cas.
>
> **Impact Home Assistant** : le `binary_sensor` « Dosage pH Actif » restera donc `on` plus longtemps et plus continûment. Une automatisation qui en déduisait « la pompe tourne » doit être revue — l'information « pompe alimentée maintenant » n'est **pas** publiée en MQTT ; elle se lit sur le calendrier d'impulsion de `/data` ou du WebSocket ([ADR-0045](adr/0045-etat-impulsion-expose-en-calendrier.md) : pompe alimentée ⇔ `ph_pulse_phase_ms < ph_pulse_ms`). **`{base}/orp_dosing` et `orp_dosing_active` sont inchangés.**

### Consignes

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/ph_target` | `7.4` | Consigne pH cible (toujours ∈ [7.20 ; 7.50], pas 0,10, depuis la v2.25.0) |
| `{base}/orp_target` | `700` | Consigne ORP cible (mV) |
| `{base}/ph_regulation_mode` | `automatic` / `scheduled` / `manual` | Mode de régulation pH actif — commandable via `{base}/ph_regulation_mode/set` depuis v2.7.0 (feature-009) |
| `{base}/ph_daily_target_ml` | `150` | Volume quotidien programmé pH (mL) — mode Programmée uniquement. Commandable via `{base}/ph_daily_target_ml/set` depuis v2.14.0 (feature-050) |
| `{base}/orp_regulation_mode` | `automatic` / `scheduled` / `manual` | Mode de régulation ORP actif — commandable via `{base}/orp_regulation_mode/set` depuis v2.7.0 |
| `{base}/orp_daily_target_ml` | `200` | Volume quotidien programmé chlore (mL) — mode Programmée uniquement. Commandable via `{base}/orp_daily_target_ml/set` depuis v2.14.0 (feature-050) |

### Système

| Topic | Payload | Rétention | Description |
|-------|---------|:---------:|-------------|
| `{base}/status` | `online` / `offline` | Oui | Disponibilité (LWT) |
| `{base}/alerts` | JSON | Non | Alertes en temps réel |
| `{base}/logs` | Texte | Non | Messages de log |
| `{base}/diagnostic` | JSON | Oui | Snapshot complet du système |

> **`reset_reason` (raison du dernier reboot) :** ce champ est disponible uniquement via le **WebSocket** (`/ws`, champ `reset_reason` dans le message `sensor_data`). Il n'est pas publié via MQTT. Voir [`docs/API.md`](API.md#ws-ws--write) pour les valeurs possibles.

---

## Topics de commande (souscription)

| Topic | Payload accepté | Action |
|-------|----------------|--------|
| `{base}/filtration/set` | `ON` / `OFF` | **Change le mode de filtration** (feature-146, v2.63.0) : `ON` → Marche (jeton `force`), `OFF` → Arrêt (jeton `off`). Ne pose plus un ordre direct séparé — le mode affiché devient réellement Marche/Arrêt, appliqué immédiatement (`filtration.update()`), et republié. |
| `{base}/install_mode/set` | `managed` / `powered` / `external` | Change le **mode d'installation** (feature-056, v2.19.0). Valeurs brutes attendues par le handler ; depuis HA le `command_template` retraduit le libellé français choisi. Valeur hors enum → ignorée + resync HA sur l'état réel. Appliqué sous `configMutex`, persisté NVS, republié (état + broadcast WS config). |
| `{base}/filtration_external_state/set` | `ON` / `OFF` | **Signal de filtration externe** (feature-056, v2.19.0). Non retain. Alimente la garde « présence d'eau » du dosage en mode `external` (équivalent de `POST /filtration/external-state`). `ON`/`OFF` insensible à la casse. **Aucune persistance** : au boot, état inconnu → dosage suspendu (fail-safe OFF) ; au-delà de 180 s sans nouveau signal → périmé → dosage suspendu. Voir [ADR-0026](adr/0026-mode-installation.md). |
| `{base}/filtration_mode/set` | `auto` / `manual` / `force` / `off` | Change le mode de filtration — validé par `parseFiltrationMode()` (valeurs brutes attendues par le handler ; depuis HA le `command_template` retraduit le libellé français choisi vers ces valeurs). **S'applique immédiatement** (feature-146, v2.63.0) : la commande appelle désormais `filtration.update()` avant de republier l'état, pour la même voie que `/save-config` — c'est le correctif du défaut du 2026-10-09 (un changement de mode MQTT pouvait auparavant rester sans effet tant qu'un ordre direct restait actif, supprimé par ce même lot). |
| `{base}/filtration_start/set` | `HH:MM` (ex. `07:30`) | Change l'heure de début du créneau (feature-051, v2.16.0). Validée `HH:MM` via `timeStringToMinutes` (format invalide → ignoré + `warning` + resync HA sur la valeur réelle). Applique via `filtration.update()`, persisté NVS, republié. **Effet réel en mode Programme.** ⚠️ Depuis bug-126 ([ADR-0079](adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)) : **en mode Auto, c'est l'inverse de l'ancien comportement** — l'heure n'est plus recalculée au prochain `update()`, donc la saisie **n'est pas écrasée**. C'est elle qui **annule** un éventuel planning auto en attente (l'instantané de base comparé à la promotion diverge), qui sera alors rejeté silencieusement plutôt que d'écraser la saisie utilisateur. ⚠️ **Correction (feature-146, v2.63.0)** : cette commande n'efface plus les overrides `forceOn`/`forceOff` — ces champs n'existent plus, supprimés avec le reste de l'ordre direct. |
| `{base}/filtration_end/set` | `HH:MM` (ex. `20:30`) | Change l'heure de fin du créneau (symétrique, feature-051, v2.16.0) |
| `{base}/lighting/set` | `ON` / `OFF` | Allume/éteint l'éclairage (contrôle manuel du relais) |
| `{base}/boost/set` | `ON` / `OFF` | Active / désactive le **Mode Boost** (feature-053, v2.18.0). `ON` → active la surchloration temporaire du jour (créneau de filtration étendu jusqu'à minuit + cible/limite chlore effectives relevées, cf. [ADR-0093](adr/0093-le-mode-de-filtration-est-la-seule-autorite.md), remplace [ADR-0025](adr/0025-mode-boost.md)) ; `OFF` → désactive. **L'activation est refusée** si l'heure n'est pas synchronisée (expiration à minuit indéterminable) **ou** si le mode de filtration est **Arrêt** (feature-146, v2.63.0 : pas de circulation, pas de surchloration possible) → aucun changement + resync HA sur l'état réel. Payload invalide ignoré. Expire automatiquement au prochain minuit local ; passer en mode Arrêt pendant que le Boost est actif l'**annule** également. |
| `{base}/rescue/set` | `ON` / `OFF` | Active / désactive la **Chloration de secours** (feature-140, v2.55.0). `ON` → active le dosage programmé aveugle à l'ORP pour 48 h glissantes au maximum. **v2.56.0 (feature-141)** : le volume quotidien n'est **plus** fixé à 1500 mL/j — le switch HA ne transportant aucun champ numérique, `ON` résout le volume via `orp_daily_target_ml` (NVS, si > 0) sinon un repli de 1500 mL/j, **sans jamais consulter la taille du bassin** (invariant feature-090 : `PoolSize` ne doit jamais déterminer un quota sans intervention humaine visible). Plafond dur 2000 mL/j inchangé — cf. [ADR-0087](adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md). Le volume résolu est republié sur `{base}/rescue_ml` (voir [ci-dessous](#topic-et-entité-ajoutées-en-feature-141-volume-de-secours-saisi-v2560)) ; `OFF` → désactive. **Refusée** si la régulation ORP (NVS) est en mode Manuel, ou si l'heure n'est pas synchronisée → aucun changement + resync HA sur l'état réel + `warning` journalisé. Payload invalide ignoré. **Ne modifie jamais** `{base}/orp_regulation_mode`. |
| `{base}/lighting_schedule/set` | `ON` / `OFF` | Active/désactive la programmation horaire (feature-052, v2.17.0). Payload validé `ON`/`OFF` (insensible à la casse) → `lightingCfg.scheduleEnabled` ; invalide → ignoré + `warning` + resync HA. Persisté NVS, appliqué via `lighting.update()`, republié. Broadcast WS config vers l'UI web |
| `{base}/lighting_start/set` | `HH:MM` (ex. `20:00`) | Change l'heure de début du créneau (feature-052, v2.17.0). Validée `HH:MM` via `timeStringToMinutes` (format invalide → ignoré + `warning` + resync HA). Pilote `lightingCfg.startTime`, persisté NVS, appliqué, republié |
| `{base}/lighting_end/set` | `HH:MM` (ex. `23:30`) | Change l'heure de fin du créneau (symétrique, `lightingCfg.endTime`, feature-052, v2.17.0) |
| `{base}/ph_target/set` | `7.4` (**7.20 – 7.50**, pas 0,10) | Change la consigne pH. ⚠️ **v2.25.0** : bornes `kPhTargetMin`/`kPhTargetMax`, **identiques à celles de `POST /save-config`** (avant : littéraux `6.0 – 8.5` propres à ce chemin). Valeur hors bornes ou non numérique → **refusée** (aucune écriture, aucune persistance) + log `warning` + `publishTargetState()` qui **resynchronise Home Assistant** sur la consigne réelle. **Jamais d'écrêtage silencieux** — voir [Refus de consigne pH hors bornes](#refus-de-consigne-ph-hors-bornes-v2250) |
| `{base}/orp_target/set` | `700` (400 – 900) | Change la consigne ORP (mV) |
| `{base}/ph_regulation_mode/set` | `automatic` / `scheduled` / `manual` | Change le mode de régulation pH (feature-009, v2.7.0). Insensible à la casse. Valeur hors enum → ignorée + log `warning`. ⚠️ **v2.25.0** : **refusé** tant qu'une bascule de sécurité pH n'est pas acquittée — voir [Refus du changement de mode pH sous trace de sécurité](#refus-du-changement-de-mode-ph-sous-trace-de-sécurité-v2250) |
| `{base}/orp_regulation_mode/set` | `automatic` / `scheduled` / `manual` | Change le mode de régulation ORP (idem) |
| `{base}/ph_daily_target_ml/set` | `150` (0 – 2000 mL, entier) | Change le volume quotidien programmé pH (feature-050, v2.14.0). Payload non numérique → ignoré + log `warning` ; négatif → clampé à 0 ; **valeur > limite journalière VIVE `maxPhMlPerDay` → refusée** + `warning` + resync HA. Persisté NVS, republié |
| `{base}/orp_daily_target_ml/set` | `500` (0 – 2000 mL, entier) | Change le volume quotidien programmé chlore (symétrique pH). Refus si > limite journalière VIVE `maxChlorineMlPerDay` |
| `{base}/reboot/set` | `PRESS` (tout payload accepté) | Redémarre le contrôleur (feature-050, v2.14.0). Non retain. Redémarrage **différé propre** : flush MQTT `offline` puis restart, même séquence que la route `POST /reboot` |

### Refus de consigne pH hors bornes (v2.25.0)

La consigne pH est bornée à **[7,20 ; 7,50]**, par pas de **0,10** (`kPhTargetMin` / `kPhTargetMax` / `kPhTargetStep`), **mêmes bornes sur les trois chemins d'écriture** — HTTP, MQTT, chargement NVS — et **identiques** pour `ph_minus` et `ph_plus`. Motifs : sous 7,2 l'eau devient agressive (liner, joints, parties métalliques), inconfortable, et consomme le TAC ; à **7,60 pile** la consigne atteindrait `kPhOrpInhibit` et **inhiberait la régulation du chlore en permanence** dès que la régulation tient sa consigne — panne sanitaire silencieuse. D'où une borne haute à **7,50** et non 7,60 ([ADR-0048](adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md), verrouillé par `static_assert`).

Jusqu'à la v2.25.0, ce chemin appliquait ses **propres** littéraux `6,0 – 8,5` — donc deux portes avec deux règles, la porte HTTP n'ayant elle aucune validation. Ces littéraux sont supprimés.

Sur une valeur hors bornes (ou non numérique) publiée sur `{base}/ph_target/set` :

1. **refus** — `mqttCfg.phTarget` reste inchangé, aucune écriture NVS ;
2. log `warning` : `Consigne pH REFUSÉE (MQTT) : 8.20 hors plage autorisée 7,20 à 7,50 — en dessous l'eau devient agressive, au-dessus la régulation du chlore serait inhibée en permanence.` ;
3. `publishTargetState()` **republie la consigne réelle** : le `number` Home Assistant revient de lui-même sur la valeur effective.

**Refus plutôt qu'écrêtage** : un `number` HA est un curseur **distant**. Écrêter en silence laisserait HA afficher 8,5 pendant que le contrôleur régule à 7,5, et une automation rejouerait indéfiniment la valeur écrêtée. La republication rend le refus **visible**.

L'**auto-discovery** publie ces bornes (`min` = 7,20, `max` = 7,50, `step` = 0,10 du `number`), `unique_id` **inchangé** : l'entité existante est réalignée sans rupture. ⚠️ Une consigne persistée hors bornes — **y compris une consigne légitime à 7,60 enregistrée avant ce resserrement** — est **clampée au chargement NVS**, journalisée et **réécrite immédiatement** en NVS.

### Refus du changement de mode pH sous trace de sécurité (v2.25.0)

Quand le firmware a basculé la régulation pH en `manual` par sécurité (3 refus de garde pH en 24 h en mode `automatic`), il **persiste une trace** (`ph_auto_manual_epoch` / `_reason`). Tant qu'elle n'est pas acquittée, une commande `{base}/ph_regulation_mode/set` qui tente de **sortir** du mode `manual` est :

1. **refusée** — `mqttCfg.phRegulationMode` reste `manual`, rien n'est écrit en NVS ;
2. journalisée en `warning` : `Mode régulation pH REFUSÉ (MQTT → automatic) : bascule de sécurité pH non acquittée — acquittez l'alerte sur la page pH de l'interface web avant de réactiver la régulation. Trace active : …` ;
3. suivie d'un `publishTargetState()` qui **resynchronise Home Assistant** sur le mode **réel** — le select HA revient donc de lui-même sur « Arrêt » (libellé révisé en feature-161, v2.77.0 — était « Manuelle »), il ne reste pas figé sur une valeur fantôme.

La trace n'est **jamais** effacée par MQTT (comportement changé : en v2.20.3, une commande HA acquittait la trace). **Motif** : un select Home Assistant est un clic **distant** qui n'affiche ni la cause de la bascule ni le refus ; pire, une automation ou une simple **restauration d'état HA au redémarrage** rejouerait `automatic` et désarmerait la sécurité **sans témoin**. L'acquittement passe donc obligatoirement par le bouton de la bannière de la [page pH](features/page-ph.md) (`ph_auto_manual_ack`), seul endroit où la cause est affichée. Voir [ADR-0048](adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md).

Entrer ou rester en `manual` reste toujours possible depuis HA.

### Refus du changement de mode pH/ORP pendant un étalonnage, désormais aussi côté MQTT (bug-161, lot C)

⚠️ **Comportement nouveau et visible (v2.77.0, correctif post-livraison).** Signalé puis instruit
par le gate `pool-chemistry` : le verrou étalonnage (bug-161, gate C2) n'était vérifié qu'au chemin
HTTP `/save-config` — une commande MQTT `{base}/ph\|orp_regulation_mode/set` reçue **pendant** une
session d'étalonnage en cours était acceptée puis **écrasée en silence** à la sortie de la session
(bug-160 restaure le mode antérieur mémorisé). Fermé par le **même** verrou pur
(`calSwitchBlocksModeChange()`) que le chemin HTTP, appelé au **même rang** (après le gate
`phAutoManual*` existant, inchangé) dans `drainCommandQueue()`.

**Comportement pendant une session d'étalonnage ouverte** (`calSwitchActive`) :

1. Une commande qui tenterait de **sortir** de `manual` (ex. `automatic`, `scheduled`) est
   **refusée** — `mqttCfg.ph\|orpRegulationMode` reste `manual`, rien n'est écrit en NVS ;
2. journalisée en `warning` : `Mode régulation pH|ORP REFUSÉ (MQTT → <mode>) : étalonnage en cours — le mode est rétabli à la fin de l'étalonnage.` ;
3. suivie d'un `publishTargetState()` qui **resynchronise Home Assistant** sur le mode réel
   (`manual`, affiché « Arrêt ») — le select HA revient de lui-même, il ne reste pas figé.
4. Une commande entrante `manual` reste **acceptée** (no-op souhaitable : elle ne change rien
   puisque le mode est déjà `manual` pendant la session).

⚠️ **Une automation Home Assistant qui rejoue `automatic` (ex. restauration d'état au redémarrage
de HA, **sans aucune action humaine**) reste donc sans effet pendant un étalonnage** — c'est
précisément le scénario qui a motivé cette garde : une sonde pH ou ORP trempe dans un tampon
d'étalonnage, et rien ne doit pouvoir y faire croire que c'est l'eau du bassin.

⛔ **Ce verrou refuse, il ne réconcilie jamais** : il ne met **jamais** à jour la mémoire de mode
antérieur à l'étalonnage (`ph/orpCalSwitchPrevMode`) — un second chemin d'écriture vers cette
mémoire de sécurité serait exactement le piège qu'il ferme. Détail et motif général :
[pump-controller.md](subsystems/pump-controller.md#verrou-étalonnage-sur-le-changement-de-mode-bug-161-gate-c2).

---

## Alertes

Topic : `{base}/alerts` — QoS 0, sans rétention.

```json
{
  "type": "ph_abnormal",
  "message": "pH=4.8",
  "timestamp": 12345678
}
```

> **Troncature des messages longs (v2.25.0)** — le transport borne la charge utile à `kMaxPayloadLen` = **128 octets**. Les messages d'escalade récents (bascule pH, consigne inatteignable) dépassaient cette limite et sortaient **coupés en pleine chaîne**, donc en **JSON invalide** : Home Assistant rejetait l'alerte entière — précisément celle qu'il fallait voir arriver. `publishAlert()` raccourcit désormais le champ `message` **avant** sérialisation pour que l'enveloppe reste valide, en respectant les **frontières de caractères UTF-8** (accents des textes français) et en ajoutant le suffixe `... (voir journal)`. Le texte **intégral** n'est jamais perdu : il part dans le journal système (`systemLogger`), et la bascule pH le persiste en NVS (`ph_auto_manual_reason`, affiché par la bannière de la page pH).

| Type | Condition |
|------|-----------|
| `ph_limit` | Limite journalière de dosage pH atteinte — émise **une seule fois, à la transition** vers la limite (v2.10.1 ; avant : répétée à chaque health check de 60 s). Événement non-retained : l'**état permanent** pour HA est porté par le binary_sensor `{base}/ph_limit` (retain). Libellé dynamique « pH+ » / « pH- » selon le produit configuré (`phCorrectionType`). Après un reboot avec limite déjà latchée : une alerte unique au premier health check (~60 s), comportement voulu. |
| `orp_limit` | Limite journalière de dosage ORP atteinte — même sémantique de transition que `ph_limit` (v2.10.1) ; état permanent : binary_sensor `{base}/orp_limit` (retain). |
| `ph_abnormal` | pH **corrigé** < 5.0 ou > 9.0. Depuis la v2.20.0 le message cite **toujours** le brut et l'offset : `pH=5.30 (brut=4.90, offset=0.40)` — sans quoi on ne peut pas distinguer une sonde à la dérive d'un offset mal saisi. |
| `orp_abnormal` | ORP < 400 mV ou ORP > 900 mV |
| `temp_abnormal` | Température < 5°C ou > 40°C |
| `low_memory` | Heap **total** disponible sous le seuil (`kMinFreeHeapBytes = 10 Ko`) |
| `ph_injection_aborted` | Injection manuelle pH interrompue par la sécurité chimique : la filtration s'est arrêtée pendant l'injection (v2.1.2). Pas de reprise auto, l'utilisateur doit relancer manuellement après reprise filtration. |
| `orp_injection_aborted` | Injection manuelle ORP/chlore interrompue par la sécurité chimique (mêmes conditions que `ph_injection_aborted`). |
| `auto_reboot` | **Auto-guérison mémoire (v2.9.1)** : redémarrage propre déclenché par la fragmentation critique du tas (plus gros bloc contigu sous `kRestartMaxAllocHeapBytes = 8 Ko` confirmé sur 3 vérifications). Émise **juste avant** le reboot, après coupure des pompes et sauvegarde des cumuls. Message : `Redémarrage auto-guérison (heap critique), maxBloc=<n>`. Best-effort (le reboot a lieu même si la publication échoue). Voir [`docs/subsystems/system-health.md`](subsystems/system-health.md). |
| `maintenance_mode` | **Auto-guérison (v2.9.1)** : bascule en **mode maintenance** après `kAutoHealMaxReboots = 3` redémarrages auto dans une fenêtre de 15 min. Le compteur est **partagé** entre l'auto-guérison mémoire et l'auto-guérison réseau (v2.9.2). Le dosage est **suspendu sur toutes les voies**, l'auto-guérison est désactivée, l'appareil reste **joignable** pour diagnostic. Message : `Auto-guérison désactivée après <n> reboots — dosage suspendu`. Sortie : redémarrage manuel du contrôleur. |
| `network_soft_recover` | **Watchdog de connectivité (v2.9.2)** : la pile réseau lwIP est **muette depuis `kNetworkStallSoftMs = 2 min`** (aucun echo ICMP vers la passerelle, aucun autre signe de vie) alors que le WiFi reste **associé**. Une **reconnexion WiFi douce** (`WiFi.disconnect()/reconnect()`) est tentée **une seule fois** par épisode de gel. Best-effort : l'alerte est publiée si la pile réseau répond encore assez pour le faire. Voir [`docs/subsystems/system-health.md`](subsystems/system-health.md). |
| `network_stall_reboot` | **Watchdog de connectivité (v2.9.2)** : gel de la pile réseau confirmé **depuis `kNetworkStallRebootMs = 5 min`** (WiFi associé mais pile muette, la reconnexion douce n'a pas suffi). Déclenche un **redémarrage propre fail-safe** (même séquence que `auto_reboot` : pompes coupées → cumuls sauvegardés → reboot). Émise **juste avant** le reboot, best-effort. Le reboot compte dans le **budget anti-boucle partagé** (`kAutoHealMaxReboots`) → peut mener à `maintenance_mode`. |
| `history_partition_reset` | **Disjoncteur de boot-loop history (v2.10.0)** : la partition `history` a été **effacée automatiquement** au boot après `kHistoryInitCrashThreshold = 2` crashs consécutifs pendant sa fenêtre d'init (corruption littlefs → panic non rattrapable à la 1ʳᵉ écriture). Événement de **transition** (retain=false, comme `network_stall_reboot` / `auto_reboot`), publié de manière **différée** depuis `setup()` après `mqttManager.begin()` (l'effacement a lieu avant WiFi/MQTT). Message : `Partition historique corrompue effacée automatiquement (boot-loop détecté) — l'historique repart de zéro`. Voir [`docs/subsystems/system-health.md`](subsystems/system-health.md) et [ADR-0031](adr/0031-disjoncteur-bootloop-history.md). |
| `heap_corruption` | **Corruption du tas détectée (v2.18.8, [ADR-0037](adr/0037-postmortem-oom-et-integrite-tas.md))** : le contrôle d'intégrité périodique (`heap_caps_check_integrity_all()`, toutes les 5 min, uniquement pompes à l'arrêt et hors OTA/maintenance) a échoué. **Type unique** publié par les **deux** branches de sortie : (1) **redémarrage propre** via `performFailSafeRestart()` — message `Corruption du tas détectée — dosage suspendu, redémarrage propre` ; (2) **mode maintenance** quand le plafond `kAutoHealMaxReboots` est atteint — message `Corruption du tas détectée — dosage suspendu, mode maintenance (plafond de redémarrages)`. Dans les deux cas, les pompes sont **coupées et le dosage inhibé AVANT** la publication. Événement non-retained, best-effort (l'action a lieu même si la publication échoue) ; le reboot compte dans le **budget anti-boucle partagé**. Voir [`docs/subsystems/system-health.md`](subsystems/system-health.md#contrôle-dintégrité-périodique-du-tas-v2188). |
| `ph_guard` | **Garde de sécurité pH (feature-067, v2.20.0 ; refondue v2.25.0 — ex-`ph_raw_floor`)** : un seuil de dosage pH a été franchi. **v2.25.0** — les seuils sont **FIXES** et dérivés de la plage de consigne ; le plancher asservi à l'offset est **supprimé** ([ADR-0048](adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md)). Quatre gardes émettent sur ce type, selon le produit déclaré : refus d'**ACIDIFIER** sous **6,70** [pH CORRIGÉ] et butée basse sous **6,20** [pH BRUT] pour `ph_minus` ; refus d'**ALCALINISER** au-dessus de **7,60** [CORRIGÉ] et butée haute au-dessus de **8,10** [BRUT] pour `ph_plus` (le pH+ n'était gardé par aucun seuil avant la v2.25.0). Le dosage est refusé tant que la condition dure. Alerte **edge-triggered** (une fois par transition), non-retained, log `CRITICAL`. Exemples : `pH 6.65 sous le plancher de sécurité 6.70 (offset=0.10) — injection d'acide refusée` ; `pH BRUT 6.18 sous la butée de sécurité 6.20 (offset=0.50) — injection d'acide refusée quelle que soit la valeur corrigée : offset positif probablement erroné, contrôler le pH au réactif`. **Hystérésis de relâche 0,05 pH** : la sortie de blocage n'a lieu qu'à `seuil + 0,05` — plus de rafale de refus sur une mesure oscillante. Un **second** message de même type est émis si **3 refus surviennent dans une fenêtre glissante de 24 h EN MODE `automatic`** (bascule fail-closed **persistée** en NVS) ; son texte est **conditionné au diagnostic EZO** — si les pentes sont dans [90 ; 110] % et \|zéro\| ≤ 15 mV, il indique explicitement que **la sonde n'est pas en cause** et oriente vers un décalage de référence externe. Message tronqué à 128 o sur le bus (voir ci-dessous), texte intégral dans le journal. Le retour dans le domaine sûr est journalisé en `INFO`, sans alerte. ⚠️ Les alertes de garde partent **hors de toute conjonction d'éligibilité** : quota journalier épuisé et mode `manual` compris. Voir [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#gardes-de-sécurité-ph-refonte-v2250). |
| `ph_low_warning` | **Avertissement d'eau acide, NON BLOQUANT (v2.25.0)** : le pH **CORRIGÉ** est passé sous `kPhLowWarn` = **7,00**. Signal **précoce** conservé (condition du gate `pool-chemistry`) malgré l'abaissement du seuil de blocage à 6,70 : sous 7,0 l'eau commence à être agressive pour l'installation bien avant qu'il faille refuser de doser. **Ne bloque aucun dosage et n'incrémente aucun compteur de refus.** Émise dans les deux cas de produit. Alerte **edge-triggered**, non-retained, log `WARNING`. Message : `pH 6.95 sous 7.00 — eau agressive (liner, joints, parties métalliques). Dosage encore autorisé jusqu'à 6.70 ; contrôler le pH au réactif.` La levée est journalisée en `INFO`, sans alerte. |
| `ph_guard_scheduled` | **Dosage pH PROGRAMMÉ refusé par une garde (v2.25.0)** : une garde pH bloque alors que le mode de régulation est `scheduled`. Ce mode **ne compte pas ses refus et n'escalade jamais** — une consigne volumétrique choisie par l'utilisateur ne doit pas fermer la régulation. Sans cette alerte, un refus permanent y serait un no-op **silencieux** après la première transition. Réémise à **débit limité** (`kPhGuardScheduledAlertIntervalMs` = **1 h**) tant que la garde tient ; log `WARNING`, non-retained. Le message réutilise telle quelle la **cause latchée** exposée par `setGuardRefusalCause()` (`phDoseBlockedReason`) : depuis la **v2.65.0** (feature-149), celle-ci cite un **seuil**, plus une mesure (voir [`pump-controller.md`](subsystems/pump-controller.md#règle--une-cause-latchée-ne-porte-jamais-une-mesure-vive-v2571)). Message : `Dosage pH programmé refusé — pH sous le plancher de sécurité (6.7) — dosage acide suspendu.. Le volume quotidien ne sera pas injecté tant que la garde tient ; la régulation n'est PAS basculée en manuel dans ce mode. Contrôler le pH au réactif, l'offset et le produit déclaré.` ⚠️ Point consigné ici faute de mieux (hors périmètre feature-149) : la cause latchée se terminant désormais par un point, ce message affiche un **double point** (`…suspendu.. Le volume…`) — défaut cosmétique introduit par le changement de format, à router vers `code-reviewer`/`embedded-developer`. |
| `orp_ph_inhibit` | **Inhibition du dosage chlore sur pH élevé (feature-067, v2.20.0 ; ex-`orp_ph_raw_inhibit`)** : le dosage ORP est suspendu tant que la condition dure. **v2.25.0 — le seuil porte sur le pH CORRIGÉ** (`kPhOrpInhibit` = **7,60**, relâche ≈ 7,55) et non plus sur le brut : la fraction d'acide hypochloreux (HClO, forme active du chlore) dépend du pH **réel** de l'eau. Au-delà, l'ORP mesuré chute à concentration de chlore constante et la régulation ORP compenserait en **surdosant**. Une **butée haute sur le pH BRUT** (**8,10**) s'y ajoute en OU logique, contre un offset négatif saisi à tort qui masquerait une eau réellement haute. Alerte **edge-triggered**, non-retained, log `CRITICAL`. Messages : `pH 7.68 > 7.60 (offset=-0.20) — chlore peu actif à ce pH : dosage ORP suspendu (faire baisser le pH)` ; `pH BRUT 8.14 au-dessus de la butée 8.10 (offset=-0.20) — dosage ORP suspendu quelle que soit la valeur corrigée : offset négatif probablement erroné, contrôler le pH au réactif`. Le correctif est de **faire baisser le pH**, pas de relever la consigne ORP. **Anti-rafale (v2.25.0)** : une seule alerte **par gravité et par épisode** (butée brute > seuil corrigé > mesure absente), jamais de réémission en dégradation — sans quoi un bassin oscillant sur un seuil produisait deux messages contradictoires en alternance. Quand la mesure est retombée dans la **bande de relâche**, le message le dit (« maintenu dans la bande d'hystérésis ») plutôt que d'affirmer un dépassement ; et l'offset n'est mis en cause que s'il est réellement **négatif**. ⚠️ Le cas « pH **indisponible** » (sonde muette / filtre non amorcé) suspend **aussi** le dosage chlore **automatique** (fail-closed) mais est journalisé en `INFO` **sans alerte MQTT** — situation transitoire normale au démarrage ; le motif « mesures non représentatives » (purge de la chambre) en est distingué. Le dosage **programmé** reste aveugle à la mesure, par conception. |
| ~~`ph_filter_not_ready` / `orp_filter_not_ready`~~ | **RETIRÉ en v2.68.0** (feature-152) — le mécanisme « filtre non prêt durablement » a disparu avec son verrou. Description historique (v2.20.3 à v2.67.x) : signalait un filtre resté non prêt > 5 min, filtration tournant et mesures représentatives. |
| ~~`ph_filter_rearm_denied` / `orp_filter_rearm_denied`~~ | **RETIRÉ en v2.68.0** — la sortie de secours de ré-amorçage et ses gardes ont disparu avec le filtre collant qu'elles tentaient de débloquer. Description historique : ré-amorçage refusé par cooldown ou valeur hors bande admissible. |
| `ph_dose_sensor_fault` / `orp_dose_sensor_fault` | **PANNE DE MESURE bloquant le dosage (v2.25.1)** — **conservée, codes renommés en v2.68.0** (feature-152) : la cause de refus courante du dosage est une **panne** de la chaîne de mesure — **sonde figée** (`DoseRefusal::ReadingNaNFrozen`) ou **plus aucune mesure acceptée** (`ReadingNaNStale`) — et cet état dure **sans interruption** depuis `kDoseSensorFaultAlertDelayMs` = **60 s** (12 cycles de lecture EZO). La régulation de ce paramètre est à l'arrêt. Alerte **une seule fois par épisode**, log `warning` (émis par `publishAlert()`, donc présent au journal même broker déconnecté), non-retained. Message : `[Dosage pH] Refus : sonde figée (valeur constante) — valeur officielle NaN — régulation pH à l'arrêt depuis 65 s : vérifier la sonde`. **L'épisode n'est PAS redaté quand la panne change de nature** (`Frozen` ↔ `Stale`) : un EZO agonisant alternant les deux par phases courtes n'aurait jamais déclenché l'alerte. Les états **normaux** (fenêtre de lissage en cours, mesures non représentatives) n'émettent **jamais** sur ce type. ⚠️ **La mention « complémentaire de `ph_filter_not_ready` »** disparaît : ce dernier type est retiré ci-dessus, `ph_dose_sensor_fault` reste la **seule** alerte qualifiant la nature d'une panne de mesure. Voir [ADR-0049](adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md) et [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#alerte-de-panne-de-mesure-v2251). |
| `ph_daily_limit_streak` / `orp_daily_limit_streak` | **Plafond journalier atteint J jours consécutifs (v2.68.0, feature-152)** — alerte **nouvelle**, log `critical` + `publishAlert()`, non-retained. Se lève quand `safetyLimits.ph/orpLimitReached` a été vrai à la clôture de **3 jours calendaires consécutifs** (`kDailyLimitStreakAlertDays`). Ne bloque rien (pas un verrou, un signal) : sous les deux hypothèses possibles (sonde qui ment / eau réellement mauvaise), poursuivre le goutte-à-goutte au-delà de 3 jours est faux dans les deux cas — sous le breakpoint ORP il fabrique des chloramines. Message : `Plafond journalier chlore atteint 3 jours consécutifs — sous le seuil de redox, poursuivre le goutte-à-goutte fabrique des chloramines : contrôler le chlore et le pH au réactif.` ⚠️ **Le compteur n'est PAS persisté** (variable RAM, `static` locale) : il repart à 0 à chaque redémarrage de l'appareil. Un appareil qui redémarre plus souvent que tous les 3 jours **n'atteindra jamais** ce seuil — choix assumé, voir [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#alerte-plafond-journalier-atteint-j-jours-consécutifs). |
| `ph_integral_saturated` | **Régulation pH saturée sans atteindre la consigne (v2.26.0, [ADR-0052](adr/0052-terme-integral-ph-dimensionne-par-autorite.md))** : le correcteur demande **son maximum** et l'erreur reste **au-dessus du seuil d'arrêt**. Le prédicat porte sur le rapport **demande PID / débit moyen soutenable** (`ampFlow × budget_horaire / 60`), **pas** sur le remplissage de `integralMax` — le gel d'anti-windup immobilise l'intégrale bien en dessous du plafond (~34 % avec 5 min/h), un seuil sur le plafond n'aurait **jamais** été atteint. Hystérésis **95 % / 85 %**. La tenue se compte en **temps de branche automatique** (un passage en `manual`, une garde bloquante ou un hiver sans filtration **suspendent** l'épisode au lieu de le faire mûrir). **Deux paliers, un seul type d'alerte** — la gravité est portée par le message et le niveau de journal : `warning` après **2 h**, `critical` après **6 h** (= une constante d'intégration entière à autorité pleine). Le `critical` est **RÉPÉTÉ toutes les 12 h** tant que l'anomalie dure : un latch « une alerte par épisode » laissait un silence **permanent** sur un état qui, lui, ne cessait pas. Causes physiques typiques : **bidon vide, tube désamorcé, fuite du circuit de dosage, sonde figée, limite horaire trop basse**. Message `critical` (tronqué à 128 o sur le bus, intégral au journal) : `Régulation pH saturée depuis plus de 6 h (2.10 mL/min demandés pour 2.15 mL/min soutenables, terme intégral 2.05 mL/min) — la régulation demande son maximum SANS atteindre la consigne : vérifier le niveau et l'amorçage du bidon, l'étanchéité du circuit de dosage, la sonde pH et la limite horaire d'injection`. La **sortie d'hystérésis** est journalisée en `info`, sans alerte. Aucun équivalent ORP (`Ki_ORP` reste 0). |
| ~~`measures_purge_timeout`~~ | **RETIRÉ en v2.62.0** (feature-139, [ADR-0092](adr/0092-retrait-analyse-stabilite-porte-mesures.md)) — voir la note dédiée ci-dessous. Description historique (v2.34.0 à v2.61.x) conservée pour archive : signalait la purge des mesures ouverte au PLAFOND de 90 min sans stabilité des bruts pH/ORP atteinte. |
| `oom_reserve_absent` | **Parachute OOM sans réserve (bug-080 lot B-1, v2.35.0, [ADR-0061](adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md))** : **aucun** palier de réserve — ni nominal (12 288 o) ni de repli (1 024 o) — n'a pu être armé depuis `kOomNoReserveCriticalDelayMs` = **5 min**, soit **30 tentatives** de réarmement consécutives (`kOomRearmCheckIntervalMs` = 10 s). Il n'y a alors **plus aucun filet anti-`bad_alloc`** : c'est exactement le régime des **heures** de vol à nu des 13 et 15/08/2026, qu'aucun signal ne dénonçait. Alerte **one-shot par épisode** (le compteur repart dès qu'une réserve est de nouveau armée), non-retained, log **`CRITICAL`** : `Parachute OOM SANS RÉSERVE depuis 5 min (aucun palier armable) — plus aucun filet anti-bad_alloc, maxBloc8=<n> o, libre8=<n> o`. Message publié : `Parachute OOM sans réserve depuis plusieurs minutes — plus aucun filet anti-bad_alloc ; voir le journal pour les valeurs de tas`. ⚠️ **Point d'architecture** : `oom_guard` **ne publie PAS lui-même** — il lève un **drapeau**, drainé par `mqtt_manager` **depuis sa propre tâche** (`oomGuardConsumeNoReserveAlert()`). Motifs : `publishAlert()` **alloue** (String + JsonDocument) au moment précis où le tas vient de refuser 1 024 o trente fois de suite, il ré-écrirait le **même** message une **seconde** fois dans un journal de 8 Ko dont la rétention est de 25-35 min (fenêtre forensique divisée par deux), et `oom_guard` est une **feuille** armée avant tout le reste au `setup()` : la faire dépendre du sous-système applicatif le plus haut inverserait les couches. **Conséquence assumée : si MQTT est désactivé ou déconnecté, l'alerte est PERDUE** — le `critical()` du journal reste la **source de vérité**. L'état instantané est lisible à tout moment via `oom_reserve_bytes` / `oom_reserve_internal` de [`/get-system-info`](API.md#get-get-system-info--write). Voir [`docs/subsystems/oom-guard.md`](subsystems/oom-guard.md#politique-dallocation-par-défaut--bascule-psram-v2350). |
| `pump_unusable` | **Pompe doseuse hors service (feature-064, v2.16.0 — exigence pool-chemistry P1, porte de bascule [ADR-0034](adr/0034-migration-core3-idf5.md))** : une pompe a échoué son attache PWM au boot (`ledcAttachChannel`, **core Arduino 3 uniquement** — donc **réellement armée en production depuis la v2.20.4**, le core 3 étant l'environnement par défaut, [ADR-0042](adr/0042-core3-environnement-par-defaut.md) ; dormante en repli core 2) et a été mise en sécurité fail-closed (`pumpUsable`, feature-063) : **aucune injection possible jusqu'au reboot**. Une alerte **par pompe** inutilisable, **re-publiée à CHAQUE (re)connexion MQTT** (arbitrage Q1, décision pool-chemistry P1) et non pas one-shot au boot : l'alerte n'étant **pas retained** et l'état étant **permanent jusqu'au reboot**, un broker absent au boot ne la perd plus — la 1ʳᵉ connexion couvre le boot, les reconnexions couvrent le reste. Limitation résiduelle assumée : un HA qui redémarre **sans** reconnexion de l'ESP32 ne revoit pas l'alerte. Message : `Pompe 1 (pH) HORS SERVICE : échec init PWM (GPIO=25, canal LEDC=0) — aucune injection possible jusqu'au reboot` (idem `Pompe 2 (Chlore/ORP)`, GPIO=33, canal 1). Voir [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md) et [`docs/subsystems/mqtt-manager.md`](subsystems/mqtt-manager.md). |
| `filtration_schedule_invalid` | **Plage auto nulle réparée (bug-126, [ADR-0079](adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md))** : `filtrationCfg.start == filtrationCfg.end` a été détecté en mode `auto` (NVS corrompue, ancien défaut de calcul, saisie incohérente) — sans réparation, ce cas arrête la filtration **pour toujours en silence** (`isMinutesInRange` rend `false` en permanence). Réparé automatiquement sur `08:00`-`20:00`, log `CRITICAL` immédiat (`ensureTimesValid()`, y compris au boot). L'alerte MQTT, elle, est **différée** jusqu'à la première connexion si l'appel a lieu avant `mqttManager.begin()`. Non-retained. Message : `Plage auto nulle : repli 08:00-20:00`. |
| `filtration_temp_implausible` | **Référence de température hors bornes physiques (bug-126, [ADR-0079](adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md))** : la moyenne du cycle (ou la mesure instantanée à la mise en service) sort de `[kFiltrationRefTempMinC = 4 ; kFiltrationRefTempMaxC = 40]` °C **par le haut**, ou n'est pas finie — aucune eau de bassin n'atteint 40 °C, c'est le signe d'une panne de sonde (repli sur une sonde `Circuit`, sonde déconnectée). **Le planning courant est conservé**, aucun planning en attente n'est écrit. Log `CRITICAL`, non-retained. Message : `Réf. 85.0°C hors bornes : planning conservé`. ⚠️ **Asymétrique** : une référence **sous** 4 °C est un hivernage normal, **pas** ce type d'alerte — voir ci-dessous. |
| `filtration_mode_force_migrated` | **Migration `force` → Marche au boot (feature-146, v2.63.0, [ADR-0093](adr/0093-le-mode-de-filtration-est-la-seule-autorite.md))** : un appareil qui redémarre avec `filtrationCfg.mode == "force"` passait auparavant par le `return false` par défaut de l'ancien `decideFiltrationRun()` (pompe **arrêtée**, un bug) ; il bascule désormais en mode **Marche** (pompe **tournante**) — changement de comportement réel au boot, signalé pour que l'utilisateur ne soit pas surpris. Détecté dans `begin()`, alerte **différée** jusqu'à la première connexion MQTT (`outQueue` pas encore créée au boot). Log `warning`. Message : `Mode 'force' : filtration désormais ACTIVE (mode Marche), et non plus arrêtée`. |

> ℹ️ **bug-126** — les types `filtration_schedule_invalid` et `filtration_temp_implausible` sont **ajoutés** à la liste ci-dessus. Ils transitent par le topic d'alertes générique `{base}/alerts` et **ne créent AUCUNE entité d'auto-discovery Home Assistant** — l'auto-discovery existante est **strictement inchangée**. Une automation HA qui veut réagir doit filtrer sur `type == "filtration_schedule_invalid"` ou `type == "filtration_temp_implausible"` dans la charge utile de `{base}/alerts`. `filtration_mode_force_migrated` (feature-146) suit la même règle : aucune entité HA dédiée, filtrage sur `type` dans `{base}/alerts`.
>
> ⚠️ **Ces deux types ne passent délibérément PAS par `{base}/alerts/sensor_frozen`** (section suivante), alors qu'ils portent eux aussi sur une mesure de température douteuse. La sémantique **établie** de `sensor_frozen` est « une mesure de régulation est morte, **donc le dosage est compromis** » : elle bloque le filtre de mesure pH/ORP et suspend l'injection. Une référence de planning filtration implausible ou une plage nulle réparée **n'ont aucun effet sur le dosage** — y verser ces deux événements sans effet sur le dosage casserait silencieusement des automatisations Home Assistant **existantes** qui réagissent à `sensor_frozen` en supposant un dosage compromis. C'est le même motif que celui déjà retenu pour la température figée elle-même ([ADR-0074](adr/0074-temperature-figee-statu-quo-instrumente.md), voir [Pourquoi la température n'entre pas dans `alerts/sensor_frozen`](#pourquoi-la-température-nentre-pas-dans-alertssensor_frozen)).

> ℹ️ **v2.35.0 (bug-080 lot B-1)** — le type `oom_reserve_absent` est **ajouté** à la liste ci-dessus. Il transite par le topic d'alertes générique `{base}/alerts` et **ne crée AUCUNE entité d'auto-discovery Home Assistant** : l'auto-discovery existante est **strictement inchangée** par cette version. Une automation HA qui veut réagir doit filtrer sur `type == "oom_reserve_absent"` dans la charge utile de `{base}/alerts`. Aucun réglage ni topic MQTT n'est par ailleurs modifié par ce lot.

> ℹ️ **v2.34.0 (feature-091)** — le type `measures_purge_timeout` a été **ajouté** à la liste ci-dessus. Il transitait par le topic d'alertes générique `{base}/alerts` et ne créait **aucune** entité d'auto-discovery Home Assistant : ni capteur d'état de purge, ni `binary_sensor` dédié — l'auto-discovery n'avait alors pas changé. Aucun réglage MQTT n'avait par ailleurs disparu : `stabilization_delay_min` n'a jamais été exposé sur le bus.
>
> ⚠️ **v2.62.0 (feature-139) — ce type est RETIRÉ, plus jamais émis** ([ADR-0092](adr/0092-retrait-analyse-stabilite-porte-mesures.md), supersède l'[ADR-0059](adr/0059-purge-adaptative-fenetre-de-medianes.md)). Le gate `pool-chemistry` a établi que le mécanisme qu'il signalait (purge « adaptative » terminée au plafond sans stabilité des bruts) était un détecteur d'étendue structurellement aveugle à une mesure durablement fausse — il est retiré, pas assoupli. **À ANNONCER EXPLICITEMENT à tout intégrateur Home Assistant** : une automation qui filtrait sur `type == "measures_purge_timeout"` dans `{base}/alerts` devient **inerte** (elle ne se déclenchera plus jamais, sans erreur ni avertissement de son côté — il n'existe aucune entité d'auto-discovery à retirer, puisqu'il n'y en avait aucune). Aucune empreinte de découverte HA à republier (vérifié : ce type ne créait aucune entité). Rien d'autre n'est retiré du bus — aucun topic, aucune entité, aucune commande souscrite ne change.

> ⚠️ **Rupture v2.25.0 — deux types d'alerte renommés.** `ph_raw_floor` devient **`ph_guard`** et `orp_ph_raw_inhibit` devient **`orp_ph_inhibit`** : les seuils ne portent plus exclusivement sur le pH brut, l'ancien nom était donc devenu faux. Deux types sont **ajoutés** (`ph_low_warning`, `ph_guard_scheduled`) et un est **supprimé** (`ph_target_unreachable` — la marge est devenue structurelle, garantie à la compilation, il n'y a plus rien à détecter à l'exécution). Une automation Home Assistant filtrant sur les anciens noms doit être mise à jour.

### Alertes dédiées (feature-021 / feature-022, retain, edge-triggered)

Ces topics sont **retain** : le dernier état persiste sur le broker même après un reboot. Une **payload vide** publiée en retain efface l'alerte (clear) — utile pour HA qui peut alors retirer le badge automatiquement.

| Topic | Payload alerte | Payload clear | Condition de bascule |
|-------|----------------|---------------|----------------------|
| `{base}/alerts/calibration_required` | JSON `{"type":"calibration_required","phCalPoints":<int>,"orpCalPoints":<int>,"timestamp":<ms>}` | (vide) | Bascule **alerte** : `phCalPoints < 2` OU `orpCalPoints < 1`. Bascule **clear** : les deux capteurs OK. |
| `{base}/alerts/sensor_stale` | JSON `{"type":"sensor_stale","phStale":<bool>,"orpStale":<bool>,"timestamp":<ms>}` | (vide) | Bascule **alerte** : `getPh()` ou `getOrp()` retourne NaN (lecture > `kSensorStaleTimeoutMs = 20 s`). Bascule **clear** : les deux lectures redeviennent valides. |
| `{base}/alerts/sensor_frozen` | JSON `{"type":"sensor_frozen","phFrozen":<bool>,"orpFrozen":<bool>,"timestamp":<ms>}` | (vide) | Bascule **alerte** : capteur pH ou ORP **figé** (variance nulle sur 30 lectures — feature-022, v2.10.0). Bascule **clear** : les deux capteurs redeviennent vivants. La température figée (warning-only) n'est **pas** dans ce payload. |

Publication **edge-triggered** : un message n'est émis qu'à la transition (entrée ou sortie de l'état d'alerte), pas à chaque cycle MQTT. Voir [`docs/subsystems/sensors.md`](subsystems/sensors.md) et [`docs/subsystems/mqtt-manager.md`](subsystems/mqtt-manager.md).

---

## Diagnostic

Topic : `{base}/diagnostic` — publié au démarrage, à chaque reconnexion, puis **toutes les 5 min** tant que le lien MQTT tient (`kDiagnosticPublishIntervalMs`, [`src/main.cpp:509`](../src/main.cpp:509)).

```json
{
  "uptime_ms": 123456789,
  "uptime_min": 2057,
  "free_heap": 45230,
  "wifi_ssid": "MonWiFi",
  "wifi_rssi": -65,
  "ip_address": "192.168.1.100",
  "sensors_initialized": true,
  "ph_value": 7.2,
  "orp_value": 720,
  "temperature": 24.5,
  "ph_dosing_active": false,
  "orp_dosing_active": false,
  "ph_daily_ml": 45.2,
  "orp_daily_ml": 120.5,
  "ph_limit_reached": false,
  "orp_limit_reached": false,
  "filtration_running": true,
  "filtration_mode": "auto",
  "ph_target": 7.2,
  "orp_target": 700.0,
  "firmware_version": "1.0.3",
  "hdef": 0,
  "ldef": 0,
  "xdef": 0,
  "ph_starts": 1842,
  "ph_total_ml": 5290.44,
  "ph_anom": 0,
  "ph_anom_ml": 0,
  "orp_starts": 613,
  "orp_total_ml": 1204.8,
  "orp_anom": 0,
  "orp_anom_ml": 0,
  "ezo_comp_t": 25.4,
  "ezo_comp_stage": "mesure"
}
```

> ℹ️ Charge utile **non exhaustive** : d'autres champs de diagnostic interne
> (p. ex. `mqtt_task_stack_hwm`) peuvent être présents. Ne pas supposer un
> ensemble de clés fermé.

### `hdef` / `ldef` / `xdef` — refus d'accès fichier pour famine mémoire (v2.43.0)

Trois compteurs cumulés **depuis le démarrage**, ajoutés par
[`bug-100`](subsystems/acces-fichiers.md) / [ADR-0070](adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md).
Le topic étant **retenu**, ces valeurs **survivent à un redémarrage de
l'intégrateur** — c'est délibéré : sous famine mémoire, le journal de bord de
l'appareil, lui, ne survit pas.

| Clé | Type | Signification |
|---|---|---|
| `hdef` | entier | Sauvegardes ou chargements d'**historique** refusés faute de mémoire. Inclut les **tripwires de boot** (démarrage sans historique persistant). |
| `ldef` | entier | Flushs du **journal** vers le disque refusés. Les lignes sont **conservées en RAM** et le flush est **retenté** ; au-delà de 100 entrées en attente, les plus anciennes sont évincées. |
| `xdef` | entier | Exports `GET /download-logs` servis **tronqués** (journal persistant non ouvrable) : la réponse ne contient alors que la session courante, et le dit. |

**Valeur attendue en régime normal : `0`.** Une valeur non nulle signifie que
l'appareil a **refusé une opération fichier au lieu de planter** — ce n'est pas
une erreur à corriger dans l'intégration, c'est le symptôme d'une pénurie de RAM
interne à instruire côté appareil.

⚠️ **`hdef` ne remplace pas et ne recoupe pas le compteur « partition pleine »**
(`nospc_cnt`, NVS) : famine **mémoire** et saturation **disque** sont deux pannes
différentes.

### `ezo_comp_t` / `ezo_comp_stage` — compensation T° réellement appliquée (feature-068, v2.47.1)

| Champ | Type | Description |
|---|---|---|
| `ezo_comp_t` | float \| `null` | Température **effectivement envoyée** à l'EZO pH dans `RT,<tempC>` (°C, 1 décimale) — donc **après** offset utilisateur écrêté **et** cascade de repli. `null` avant la première lecture EZO du démarrage. |
| `ezo_comp_stage` | string | Étage de la cascade qui a produit cette valeur : `"mesure"` \| `"derniere_valeur_plausible"` \| `"repli_defaut"`. |

Ces deux champs vont **ici et pas dans la trame WebSocket** : ce sont des grandeurs **relues à froid**, plusieurs jours après un épisode, qu'aucun écran ne lit en direct. Le topic `{base}/diagnostic` étant **retenu**, elles survivent à un redémarrage de l'appareil.

**À quoi ça sert, concrètement** : comparés au champ `temperature` du même payload, ils sont le **seul** diagnostic qui distingue les trois causes d'un écart de compensation — une valeur **figée** (constante dans le temps, étage `mesure`), un **repli** (étage ≠ `mesure`) et un **offset** de calibration mal réglé (écart constant avec la mesure). Voir [ADR-0074](adr/0074-temperature-figee-statu-quo-instrumente.md) et [`sensors.md`](subsystems/sensors.md#cascade-de-résolution-de-rt-bug-114).

**Budget de ce payload** ([ADR-0073](adr/0073-chiffre-consigne-porte-sa-methode.md), décompte au **pire cas structurel du type**, jamais sur un payload vivant) :

| Fragment sérialisé | Décompte | Octets |
|---|---|---:|
| `"ezo_comp_t":<float>,` | 1+10+1+1+**12**+1 | 26 |
| `"ezo_comp_stage":"derniere_valeur_plausible",` | 1+14+1+1+**27**+1 | 45 |
| **Total** | | **+71** |

- **12 o** = longueur maximale d'un `float` sérialisé par ArduinoJson 7, établie par **balayage exhaustif** des motifs de bits finis (harnais hôte, méthode `bug-082`) : la notation exponentielle (`1.401298e-45`, ou `-1e+10` avec l'arrondi au dixième appliqué ici) est plus longue que 7 chiffres significatifs. On retient la borne du **type**, pas celle de la plage physique du DS18B20. `null` (4 o) n'est jamais dimensionnant.
- **27 o** = 25 caractères du plus long libellé + 2 guillemets ; les trois libellés sont des **littéraux** de `tempCompStageLabel()`, donc bornés à la compilation.

**Pire cas de trame : 1 057 o** (mesure feature-110, reprise telle quelle — **seul le delta a été recompté**) **+ 71 = 1 128 o**, sous `kMqttPublishBufferBytes` = **1 536** ⇒ **marge résiduelle 408 o** (elle était de 479 o). Aucun bump nécessaire.

> ⚠️ **Le refus de PubSubClient est SILENCIEUX et TOTAL.** Dès que `bufferSize < 5 + 2 + longueur(topic) + longueur(payload)`, le `publish` est refusé **en entier** — ce n'est pas une troncature : `hdef` / `ldef` / `xdef` et tout le reste disparaissent avec. D'où le décompte au pire cas plutôt qu'à la mesure, et la garde de longueur en fin de fonction.

### Relevé du biais de comptage (feature-110, v2.45.0)

Huit clés ajoutées au **même** topic retenu `{base}/diagnostic` — **aucun topic
neuf, aucune entité Home Assistant neuve**. Le topic étant **retenu**, ces valeurs
se relisent **plusieurs jours plus tard** et survivent à un redémarrage de
l'appareil : c'est ce qui fait du changement de bidon — un geste déjà fait — une
mesure auto-suffisante, sans rien à noter au préalable.

| Clé | Type | Signification |
|---|---|---|
| `ph_starts` | entier | **Démarrages de pompe pH crédités** depuis la dernière remise à niveau du bidon (action « Mettre à jour le niveau » ; s'appelait « Nouveau bidon » avant la v2.59.0) — le `N` de `V_compté = V_réel + N·δ`. Incrément **saturant** (jamais de repli à `0`). |
| `ph_total_ml` | float (mL) | Volume compté sur la **même** campagne — le `V`. Même valeur que `ph_total_injected_ml` de [`/get-config`](API.md#get-get-config--write). |
| `ph_anom` | entier | Parmi ces crédits, nombre de ticks de comptabilité **anormalement longs** (> 5 000 ms, `kAcctDeltaAnomalyMs`). **Hygiène du relevé.** |
| `ph_anom_ml` | float (mL) | Volume crédité **par** ces ticks anormaux. Sert à **disqualifier** un relevé pollué, jamais à corriger le volume. |
| `orp_starts` / `orp_total_ml` / `orp_anom` / `orp_anom_ml` | | Idem pour la pompe chlore. |

⚠️ **Les deux couples se lisent ENSEMBLE.** Un `N` sans son `V`, ou l'un des deux
sans son indicateur d'hygiène, **ne permet de conclure sur rien** : voir le mode
d'emploi du relevé sur la [page Produits](features/page-dosages.md#relevé-du-biais-de-comptage--mode-demploi-feature-110-v2450).
**Aucune correction n'est appliquée** par cette version — ces compteurs mesurent.

#### ⚠️ Changement de signification de `ph_total_ml` / `orp_total_ml` (bug-115, v2.61.0)

`ph_total_ml` / `orp_total_ml` (et `ph_total_injected_ml` / `orp_total_injected_ml`
côté [`/get-config`](API.md#get-get-config--write)) reçoivent désormais **aussi**
le volume des **marches de l'assistant de débit** réalisées en mode manuel —
jusqu'ici invisible pour ce suivi, bien que le produit ait réellement quitté le
bidon (correctif bug-115 : `update()` comptabilise désormais **chaque** sortie,
anticipée ou non, via le point de passage unique `settlePumpAccounting()`).

C'est **plus fidèle**, mais deux conséquences pour quiconque lit ce topic :
- **La jauge de bidon descendra plus vite à consommation réelle inchangée.**
- **Aucune campagne de concordance postérieure à v2.61.0 n'est comparable à une
  campagne antérieure** : `N` (démarrages crédités) augmente en même temps que
  `V` diminue (plus de volume fantôme crédité aux coupures), donc le biais `δ`
  change de valeur sans que le matériel ait bougé. Tout relevé futur doit
  **porter la version de firmware**. Voir
  [docs/subsystems/pump-controller.md](subsystems/pump-controller.md#correction-de-la-rémanence-bug-115-v2610--point-de-passage-unique)
  et [ADR-0091](adr/0091-comptabilite-hors-du-corps-decisionnel-dupdate.md).

### `kMqttPublishBufferBytes` porté de 1024 à 1536 o

Les 8 clés pèsent **+182 o au pire cas** (mesuré clé par clé). Sous **1024 o**,
PubSubClient **refusait silencieusement** la publication du diagnostic : la garde
est `bufferSize < MQTT_MAX_HEADER_SIZE + 2 + strnlen(topic) + plength`
(`PubSubClient.cpp:450`, `MQTT_MAX_HEADER_SIZE = 5`) et **retourne `false` sans
rien écrire sur le fil**. Le diagnostic n'aurait pas été *tronqué* : il aurait
**disparu en entier**, `hdef` / `ldef` / `xdef` compris.

Mesures (harnais hôte, ArduinoJson 7) — « buffer requis » = payload + 7 o
d'en-tête (`kMqttPublishFrameOverheadBytes`) + longueur du topic :

| | Payload | Buffer requis |
|---|---:|---:|
| Avant feature-110, nominal | 681 o | 711 o |
| Avant feature-110, **pire cas** | 804 o | 875 o |
| Après feature-110, nominal | 824 o | 854 o |
| Après feature-110, **pire cas** | 986 o | **1 057 o** ← dépassait 1024 de 33 o |

Pire cas = SSID 32 c., `filtration_mode` 16 c., topic de base 64 c. (**aucun des
trois n'est borné** par `/save-config`), `uint32` saturés, flottants à 9 chiffres
significatifs. **1536 o rétablit 479 o de marge (31 %)**. Coût RAM : **+512 o**
dans le buffer PubSubClient, alloués **une fois** dans `begin()`, jamais en boucle.

**Deux WARN throttlés** (1 h, `kMqttDiagnosticWarnThrottleMs`) encadrent désormais
ce site — le retour de `safePublish()` y était **ignoré**, la disparition était
donc totalement silencieuse :

```text
WARNING MQTT: diagnostic trop long pour le buffer de publication (1620 o requis > 1536 o) — la trame sera ABANDONNÉE en entier, pas tronquée
WARNING MQTT: publication du diagnostic refusée (1057 o) — buffer insuffisant ou lien indisponible
```

Les deux causes n'appellent pas la même action : « trop long » se corrige **dans
le firmware** (buffer ou payload), « refusée » est un problème de **lien**. Le
second capte aussi le cas où `setBufferSize()` échoue au `realloc` et laisse la
bibliothèque à ses **256 o** par défaut — cas jusqu'ici **totalement muet**.

---

## Home Assistant Auto-Discovery

Le contrôleur publie automatiquement sa configuration pour Home Assistant au démarrage.

**Préfixe discovery :** `homeassistant/`
**Device ID :** `poolcontroller`

### Republication : conditionnée à l'empreinte, pas à chaque reconnexion (bug-121, v2.53.1)

Le **contrat de découverte ne change pas** : mêmes topics, mêmes charges
utiles, même `retain=true`. Ce qui change est la **fréquence** à laquelle la
rafale complète est rejouée.

Jusqu'en 2.53.0, une reconnexion MQTT republiait l'intégralité des entités —
travail redondant, puisque ces messages `retain=true` sont déjà conservés par
le courtier. Depuis 2.53.1, la rafale n'a lieu que si :

- **aucun enregistrement** de la dernière rafale n'est trouvé en NVS (flash
  neuf, NVS effacée) ;
- l'**empreinte** de la configuration a changé (version de firmware, préfixe
  `homeassistant/`, identifiant d'appareil, topic de base, **serveur** ou
  **port** du courtier) ;
- la **rafale précédente était incomplète** (au moins un échec de publication) ;
- Home Assistant vient de **redémarrer** (voir abonnement ci-dessous).

Dans tous les autres cas — la grande majorité des reconnexions — **rien n'est
republié**. Détail firmware, format de journalisation et coût mesuré d'une
rafale : [`docs/subsystems/mqtt-manager.md`](subsystems/mqtt-manager.md#republication-conditionnée-à-lempreinte-pas-à-la-reconnexion-bug-121-v2531).

### Abonnement à `homeassistant/status` (bug-121, v2.53.1)

Le contrôleur s'abonne désormais à `homeassistant/status` — le **message de
naissance** que Home Assistant publie (`online` au démarrage, `offline` à
l'extinction propre, LWT MQTT). À la réception de `online`, une rafale de
découverte complète est déclenchée inconditionnellement, sans consulter
l'empreinte : c'est le seul chemin de récupération automatique si les topics
retenus ont été perdus côté courtier.

⚠️ Ce chemin ne couvre **que** le redémarrage de Home Assistant. Un courtier
**purgé** (perte des messages retenus) **sans** que Home Assistant redémarre
n'a **aucun rattrapage automatique** dans cette version — voir la section
« Périmètre réduit » de la spec `bug-121`.

### Changement de préfixe : topics orphelins, geste manuel

Changer le préfixe `homeassistant/` (ou le topic de base) fait cesser la
republication sur les **anciens** topics `.../config` — l'empreinte change,
donc la rafale part sur les **nouveaux** topics, mais les anciens restent
**retenus côté courtier** et ne sont pas purgés automatiquement. C'est déjà le
comportement d'avant bug-121 : **aucune régression**, seulement documenté ici
pour la première fois. Pour nettoyer manuellement les topics orphelins :

```bash
mosquitto_sub -h <broker> -t 'homeassistant/#' --remove-retained --retained-only
```

| Type | Nom dans HA | Topic état | Topic commande |
|------|-------------|-----------|----------------|
| Sensor | Piscine Température | `{base}/temperature` | — |
| Sensor | Piscine pH | `{base}/ph` | — |
| Sensor | Piscine ORP | `{base}/orp` | — |
| Sensor | Piscine pH Points Calibrés | `{base}/ph_cal_points` | — |
| Sensor | Piscine ORP Points Calibrés | `{base}/orp_cal_points` | — |
| Binary Sensor | Filtration Active | `{base}/filtration_state` | — |
| Binary Sensor | Dosage pH Actif | `{base}/ph_dosing` | — (⚠️ **session ouverte** depuis la v2.36.0, pas « pompe en marche » — [voir la note](#dosage)) |
| Binary Sensor | Dosage Chlore Actif | `{base}/orp_dosing` | — |
| Binary Sensor | Limite Journalière pH | `{base}/ph_limit` | — |
| Binary Sensor | Limite Journalière Chlore | `{base}/orp_limit` | — |
| Binary Sensor | Stock pH Faible | `{base}/ph_stock_low` | — |
| Binary Sensor | Stock Chlore Faible | `{base}/orp_stock_low` | — |
| Binary Sensor | Capteur pH — problème | `{base}/ph_sensor_problem` | — |
| Binary Sensor | Capteur ORP — problème | `{base}/orp_sensor_problem` | — |
| Sensor | Volume pH Restant | `{base}/ph_remaining_ml` | — |
| Sensor | Volume Chlore Restant | `{base}/orp_remaining_ml` | — |
| Binary Sensor | Contrôleur Status | `{base}/status` | — |
| Select | Mode d'installation | `{base}/install_mode` | `{base}/install_mode/set` |
| Switch | Signal filtration externe | — (optimiste) | `{base}/filtration_external_state/set` |
| Select | Mode Filtration | `{base}/filtration_mode` | `{base}/filtration_mode/set` |
| Text | Filtration début | `{base}/filtration_start` | `{base}/filtration_start/set` |
| Text | Filtration fin | `{base}/filtration_end` | `{base}/filtration_end/set` |
| Select | Mode Régulation pH | `{base}/ph_regulation_mode` | `{base}/ph_regulation_mode/set` |
| Select | Mode Régulation ORP | `{base}/orp_regulation_mode` | `{base}/orp_regulation_mode/set` |
| Switch | Filtration Marche/Arrêt | `{base}/filtration_state` | `{base}/filtration/set` |
| Switch | Éclairage Piscine | `{base}/lighting_state` | `{base}/lighting/set` |
| Switch | Boost | `{base}/boost` | `{base}/boost/set` |
| Switch | Chloration de secours | `{base}/rescue` | `{base}/rescue/set` |
| Sensor | Volume Chloration de secours | `{base}/rescue_ml` | — |
| Select | Mode Éclairage | `{base}/lighting_schedule` | `{base}/lighting_schedule/set` |
| Text | Éclairage début | `{base}/lighting_start` | `{base}/lighting_start/set` |
| Text | Éclairage fin | `{base}/lighting_end` | `{base}/lighting_end/set` |
| Number | Consigne pH | `{base}/ph_target` | `{base}/ph_target/set` — `min 7.2` / `max 7.5` / `step 0.1`, `unit: pH` (**v2.25.0** : bornes republiées depuis `kPhTargetMin`/`kPhTargetMax`/`kPhTargetStep`, `unique_id` `poolcontroller_ph_target` **inchangé**, entité et historique conservés) |
| Number | Consigne ORP | `{base}/orp_target` | `{base}/orp_target/set` |
| Sensor | Dosage pH aujourd'hui | `{base}/ph_daily_ml` | — |
| Sensor | Dosage Chlore aujourd'hui | `{base}/orp_daily_ml` | — |
| Number | Volume quotidien pH | `{base}/ph_daily_target_ml` | `{base}/ph_daily_target_ml/set` |
| Number | Volume quotidien Chlore | `{base}/orp_daily_target_ml` | `{base}/orp_daily_target_ml/set` |
| Button | Redémarrer | — | `{base}/reboot/set` |
| Sensor | Piscine pH Rattrapage | `{base}/ph_integral_pct` | — (v2.26.0) |
| Sensor | Piscine pH Rattrapage Débit | `{base}/ph_integral_term` | — (v2.26.0, `entity_category: diagnostic`) |
| Sensor | Piscine pH Intégrale | `{base}/ph_integral` | — (v2.26.0, `entity_category: diagnostic`) |
| Sensor | Piscine pH Dose de Base | `{base}/ph_ff_ml_per_min` | — (v2.28.0, `entity_category: diagnostic`) |
| Sensor | Piscine pH Dose de Base Statut | `{base}/ph_ff_status` | — (v2.28.0, `entity_category: diagnostic`) |
| Sensor | Piscine pH Dose de Base Apprise | `{base}/ph_ff_learned_ml_per_min` | — (v2.46.0, `entity_category: diagnostic`) |
| Sensor | Piscine pH Écart Courant | `{base}/ph_p_term_ml_per_min` | — (v2.46.0, `entity_category: diagnostic`) |
| Sensor | Piscine pH Demande | `{base}/ph_demand_ml_per_min` | — (v2.46.0, `entity_category: diagnostic`) |
| Sensor | Piscine ORP Écart Courant | `{base}/orp_p_term_ml_per_min` | — (v2.46.0, `entity_category: diagnostic`) |
| Sensor | Piscine ORP Demande | `{base}/orp_demand_ml_per_min` | — (v2.46.0, `entity_category: diagnostic`) |

> **Compatibilité `orp_enabled` :** le champ `orp_enabled` (booléen) est maintenu comme miroir de `orp_regulation_mode` (`true` si mode ≠ `manual`). Les automations Home Assistant qui testent la valeur de ce champ continuent de fonctionner sans modification. Le topic `{base}/orp_regulation_mode` est la source de vérité pour le mode actif.

> **Libellés du select « Mode Filtration » — RÉVISÉS en feature-146 (v2.63.0, [ADR-0093](adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)) :** dans Home Assistant, ce select expose désormais les libellés français **Auto / Programme / Marche / Arrêt** (alignés sur l'UI web), et non plus les valeurs brutes anglaises. La traduction est purement côté HA via templates de la discovery : `value_template` mappe l'état brut publié (`auto`/`manual`/`force`/`off`) vers le libellé affiché (avec `.get(value, value)` → un état inattendu passe sans casser), `command_template` mappe le libellé choisi vers la valeur brute envoyée sur `.../set`. **Le protocole MQTT sur le fil reste strictement `auto`/`manual`/`force`/`off`** (état publié comme commande attendue par `drainCommandQueue`) : aucune automatisation qui publie le jeton brut n'est cassée. ⚠️ **Rupture assumée pour les libellés** : les précédents (bug-ha-filtration-mode-labels, v2.15.0 — **Auto / Programmation / Manuel / Désactivé**) sont remplacés, pas ajoutés ; une automatisation HA qui appelle `select.select_option` avec un ancien libellé cesse de fonctionner. Le libellé « Manuel » pour `force` était précisément le piège relevé par l'incident du 2026-10-09 (« Manuel » désignait un mode qui **arrêtait** la pompe) — corrigé. Les selects `ph`/`orp_regulation_mode` affichaient depuis v2.17.3 les libellés français **Automatique / Programmée / Manuelle** via la même technique (le fil reste `automatic`/`scheduled`/`manual`) — ⚠️ **concernés à leur tour en feature-161 (v2.77.0)** : voir [Libellés français des selects Mode Régulation pH/ORP](#libellés-français-des-selects-mode-régulation-phorp-bug-ha-regulation-mode-labels-v2173) ci-dessous, qui documente le remplacement de « Manuelle » par « Arrêt ».

> **Select « Mode Éclairage » (bug-ha-eclairage-select, v2.17.2) :** la programmation horaire de l'éclairage était exposée en `switch` ON/OFF. Elle est désormais un `select` « Mode Éclairage » avec les libellés **Programmation / Désactivé**, pour cohérence visuelle avec le select « Mode Filtration ». Même technique que ce dernier : `value_template` (`{'ON':'Programmation','OFF':'Désactivé'}.get(value, value)`) et `command_template` (`{'Programmation':'ON','Désactivé':'OFF'}`) traduisent l'affichage HA. **Le protocole MQTT sur le fil reste strictement `ON`/`OFF`** ; le booléen `lightingCfg.scheduleEnabled` et le handler `lighting_schedule` (reçoit toujours ON/OFF) sont **inchangés**. `unique_id` conservé (`poolcontroller_lighting_schedule`). Migration : un payload retain vide est publié une fois sur l'ancien topic `homeassistant/switch/{HA_DEVICE_ID}_lighting_schedule/config` pour retirer le switch orphelin de HA.

## Topic et entité ajoutés en feature-020 (PCB v2)

| Topic | Description | Retain | Auto-discovery HA |
|-------|-------------|--------|-------------------|
| `{base}/temperature_circuit` | T° de la sonde DS18B20 « circuit électronique » (NaN/null si non identifiée) | true | `sensor` "Piscine Température Circuit", `device_class: temperature`, `unit: °C`, `state_class: measurement` |

Le topic `{base}/temperature` (eau piscine) et son entité « Piscine Température » restent **inchangés** (rétrocompat HA).

Le bus OneWire (GPIO 5) supporte 2 sondes DS18B20 sur le PCB v2. Chaque sonde a un rôle (eau/circuit) identifié via Paramètres → Avancé. Voir [ADR-0013](adr/0013-identification-sondes-onewire.md).

## Topics et entités ajoutés en feature-021 (Atlas EZO pH/ORP, PCB v2)

| Topic | Description | Retain | Auto-discovery HA |
|-------|-------------|--------|-------------------|
| `{base}/ph_cal_points` | Nombre de points calibrés EZO pH (-1 = injoignable, 0..3 sinon) | true | `sensor` "Piscine pH Points Calibrés" — `unique_id: poolcontroller_ph_cal_points`, `icon: mdi:numeric` |
| `{base}/orp_cal_points` | Nombre de points calibrés EZO ORP (-1 = injoignable, 0..1 sinon) | true | `sensor` "Piscine ORP Points Calibrés" — `unique_id: poolcontroller_orp_cal_points`, `icon: mdi:numeric` |
| `{base}/alerts/calibration_required` | Alerte calibration EZO incomplète. JSON ou payload vide (clear). | true | aucun (pour automation HA personnalisée) |
| `{base}/alerts/sensor_stale` | Alerte lecture pH/ORP stale (NaN > 20 s). JSON ou payload vide (clear). | true | aucun |

**Précision pH** : le topic `{base}/ph` publie désormais avec **3 décimales** (vs 1 décimale en v1.x). Tout consommateur HA qui parsait `int()` doit basculer sur `float()` ; les sensors HA standards (`device_class: ph`) gèrent cela nativement.

**Topics inchangés** (rétrocompat HA) : `{base}/orp`, `{base}/ph_target`, `{base}/orp_target`, `{base}/ph_dosing`, `{base}/orp_dosing`, `{base}/ph_limit`, `{base}/orp_limit`, `{base}/ph_regulation_mode`, `{base}/orp_regulation_mode`, etc. Les topics et entités HA de calibration ORP héritées (notamment `orp_cal_valid`) restent diffusés pour compatibilité, mais leur source de vérité côté firmware est désormais le module EZO (`orp_cal_points >= 1`).

Voir [ADR-0014](adr/0014-migration-atlas-ezo.md) (décision migration) et [`docs/subsystems/sensors.md`](subsystems/sensors.md) (détails techniques EZO + cache cal_points).

## Topics et entités ajoutés en feature-024 (pente sonde pH)

Diagnostic d'usure de la sonde pH via la commande Atlas `Slope,?`. Toutes les valeurs sont **strictement diagnostiques** — elles n'affectent ni `canDose()` ni le PID. L'évaluation des seuils (sonde excellente / correcte / usée / à remplacer) est faite côté UI, pas en firmware.

| Topic | Description | Retain | Auto-discovery HA |
|-------|-------------|--------|-------------------|
| `{base}/ph_slope_acid` | Pente acide en % (1 décimale, idéal 100 %) | true | `sensor` "Piscine pH Pente Acide" — `unique_id: poolcontroller_ph_slope_acid`, `unit: %`, `icon: mdi:angle-acute`, `state_class: measurement` |
| `{base}/ph_slope_base` | Pente base en % (1 décimale, idéal 100 %) | true | `sensor` "Piscine pH Pente Base" — `unique_id: poolcontroller_ph_slope_base`, `unit: %`, `icon: mdi:angle-obtuse`, `state_class: measurement` |
| `{base}/ph_slope_zero` | Décalage zéro en mV (2 décimales, idéal 0). Non publié tant que NaN — peut rester absent sur firmware EZO ancien qui ne renvoie que 2 floats. | true | `sensor` "Piscine pH Décalage Zéro" — `unique_id: poolcontroller_ph_slope_zero`, `unit: mV`, `icon: mdi:sine-wave`, `state_class: measurement` |

**Publication edge-triggered** : un message n'est émis qu'à la transition de la valeur **arrondie** (1 décimale pour les pentes, 2 pour le zéro). Pas de spam à chaque cycle — la query `Slope,?` n'est elle-même rafraîchie qu'au boot, après calibration EZO et toutes les 24 h.

**Pas de `binary_sensor` "à remplacer"** côté firmware : l'utilisateur peut le créer en automation HA depuis les 3 sensors selon ses propres seuils (par défaut UI : pente min ≥ 95 % et |zéro| ≤ 15 mV → vert ; < 85 % ou |zéro| > 30 mV → rouge).

Voir [`docs/features/page-ph.md`](features/page-ph.md#chip-détat-sonde-feature-024) (chip + modal UI) et [`docs/subsystems/sensors.md`](subsystems/sensors.md#pente-sonde-ph--feature-024) (détails firmware : cache, fail streak, refresh policy).

## Topics et entités ajoutés en feature-025 (lissage mesures pH/ORP)

Lissage logiciel des mesures. Le topic `{base}/ph` / `{base}/orp` publie la valeur **officielle**
(cf. tableau Capteurs ci-dessus) ; les topics ci-dessous exposent en plus le brut et la médiane
pour diagnostic EMI. Tous **retain**.

⚠️ **v2.68.0 (feature-152, refonte de la chaîne de mesure)** : `ph_filtered`/`orp_filtered`,
`ph_filter_ready`/`orp_filter_ready` et `ph_filter_unstable`/`orp_filter_unstable` sont
**RETIRÉS** — le mécanisme qu'ils décrivaient (filtre à état « prêt »/« instable » séparé de la
valeur) a disparu : `{base}/ph`/`{base}/orp` portent directement la valeur officielle, `null`
pendant la montée en régime. Les 4 entrées d'auto-discovery correspondantes (tableau ci-dessous)
sont dépubliées par une **charge vide retained** au démarrage (ADR-0085) pour ne pas laisser
d'entités fantômes chez l'utilisateur qui les avait ajoutées à un dashboard.

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/ph_raw` | `7.46` | Dernière mesure pH **brute** Atlas, **sans offset** (diagnostic EMI, 3 décimales) |
| `{base}/ph_median` | `7.43` | Médiane glissante pH — **corrigée**, **diagnostic seul** depuis v2.68.0 |
| ~~`{base}/ph_filtered`~~ | ~~`7.43`~~ | **RETIRÉ en v2.68.0** (feature-152) — voir note ci-dessus. Description historique (feature-025 à v2.67.x) : mesure pH filtrée (EMA), corrigée, valeur de régulation |
| ~~`{base}/ph_filter_ready`~~ | ~~`ON`/`OFF`~~ | **RETIRÉ en v2.68.0** — le mécanisme « filtre prêt » a disparu, `{base}/ph` vaut directement `null` pendant la montée en régime |
| ~~`{base}/ph_filter_unstable`~~ | ~~`ON`/`OFF`~~ | **RETIRÉ en v2.68.0** — la médiane écarte les aberrations par construction, il n'y a plus de jugement d'instabilité séparé |
| `{base}/ph_rejected_count` | `3` | Nombre de mesures pH rejetées (glissant, 0..255) — **diagnostic seul**, ne décide plus rien |
| `{base}/orp_raw` | `690` | Dernière mesure ORP **brute** Atlas (mV) |
| `{base}/orp_median` | `682` | Médiane glissante ORP (mV) — **diagnostic seul** depuis v2.68.0 |
| ~~`{base}/orp_filtered`~~ | ~~`682`~~ | **RETIRÉ en v2.68.0** — symétrique `ph_filtered` |
| ~~`{base}/orp_filter_ready`~~ | ~~`ON`/`OFF`~~ | **RETIRÉ en v2.68.0** — symétrique `ph_filter_ready` |
| ~~`{base}/orp_filter_unstable`~~ | ~~`ON`/`OFF`~~ | **RETIRÉ en v2.68.0** — symétrique `ph_filter_unstable` |
| `{base}/orp_rejected_count` | `2` | Nombre de mesures ORP rejetées — diagnostic seul |
| `{base}/ph_mixing_delay_active` | `ON`/`OFF` | Pause mélange pH active (15 min après injection) |
| `{base}/orp_mixing_delay_active` | `ON`/`OFF` | Pause mélange ORP active (20 min après injection) |

### Auto-discovery HA (feature-025)

| Entité | Type | Topic d'état | `unique_id` | Détails |
|--------|------|--------------|-------------|---------|
| Piscine pH Brut | `sensor` | `{base}/ph_raw` | `poolcontroller_ph_raw` | `unit: pH`, `icon: mdi:water-outline`, `state_class: measurement` |
| ~~Piscine pH Filtré~~ | ~~`sensor`~~ | ~~`{base}/ph_filtered`~~ | ~~`poolcontroller_ph_filtered`~~ | **RETIRÉ en v2.68.0** (feature-152) — dépubliée (charge vide retained) |
| Piscine ORP Brut | `sensor` | `{base}/orp_raw` | `poolcontroller_orp_raw` | `unit: mV`, `icon: mdi:flash-outline`, `state_class: measurement` |
| ~~Piscine ORP Filtré~~ | ~~`sensor`~~ | ~~`{base}/orp_filtered`~~ | ~~`poolcontroller_orp_filtered`~~ | **RETIRÉ en v2.68.0** — dépubliée |
| ~~Piscine Filtre pH Prêt~~ | ~~`binary_sensor`~~ | ~~`{base}/ph_filter_ready`~~ | ~~`poolcontroller_ph_filter_ready`~~ | **RETIRÉ en v2.68.0** — dépubliée |
| ~~Piscine Filtre ORP Prêt~~ | ~~`binary_sensor`~~ | ~~`{base}/orp_filter_ready`~~ | ~~`poolcontroller_orp_filter_ready`~~ | **RETIRÉ en v2.68.0** — dépubliée |

> Le préfixe réel `unique_id` est `HA_DEVICE_ID` (`poolcontroller`). Les topics `median` et
> `rejected_count` restants sont publiés en retain mais **sans** entité auto-discovery dédiée.
> Voir [`src/mqtt_manager.cpp`](../src/mqtt_manager.cpp) et
> [`docs/subsystems/sensors.md`](subsystems/sensors.md#chaîne-de-mesure-à-5-étages-feature-152).

## Entités ajoutées en feature-009 (modes de régulation commandables, v2.7.0)

Deux entités HA `select` permettent de changer le mode de régulation pH / ORP depuis Home Assistant (automatisation « hivernage », détection d'absence, dashboard HA, etc.) :

| Entité | Discovery topic | `state_topic` | `command_topic` | Détails |
|--------|-----------------|---------------|-----------------|---------|
| Mode Régulation pH | `homeassistant/select/poolcontroller_ph_regulation_mode/config` | `{base}/ph_regulation_mode` | `{base}/ph_regulation_mode/set` | `unique_id: poolcontroller_ph_regulation_mode`, `icon: mdi:ph`, `options: ["Automatique","Programmée","Arrêt"]` + templates (voir note ci-dessous) |
| Mode Régulation ORP | `homeassistant/select/poolcontroller_orp_regulation_mode/config` | `{base}/orp_regulation_mode` | `{base}/orp_regulation_mode/set` | `unique_id: poolcontroller_orp_regulation_mode`, `icon: mdi:flash`, mêmes options + templates |

> **Libellés français des selects Mode Régulation pH/ORP (bug-ha-regulation-mode-labels, v2.17.3) :** dans Home Assistant, ces deux selects exposent les libellés français — **Automatique / Programmée / Manuelle** jusqu'à la v2.76.x, **Automatique / Programmée / Arrêt depuis la v2.77.0 (feature-161)** —, et non plus les valeurs brutes anglaises. Même technique que les selects « Mode Filtration » et « Mode Éclairage » : `value_template` (`{'automatic':'Automatique','scheduled':'Programmée','manual':'Arrêt'}.get(value, value)`) mappe l'état brut publié vers le libellé, `command_template` (`{'Automatique':'automatic','Programmée':'scheduled','Arrêt':'manual'}[value]`) retraduit le libellé choisi vers la valeur brute envoyée sur `.../set`. **Le protocole MQTT sur le fil reste strictement `automatic` / `scheduled` / `manual`** (topics d'état et handlers `PhRegulationMode` / `OrpRegulationMode` inchangés, et la valeur interne `manual` ne change pas) : aucune automatisation Home Assistant existante n'est cassée **au niveau du protocole**.
>
> ⚠️ **Rupture assumée pour les libellés (feature-161, décision utilisateur du 2026-10-13)** : « Manuelle » **disparaît**, remplacé par « Arrêt » — pas ajouté en plus. Une automatisation Home Assistant qui appelle `select.select_option` avec l'ancien libellé **« Manuelle » cesse de fonctionner** et doit être mise à jour côté HA ; celles qui publient directement le jeton brut `manual` sur `.../set` **ne sont pas affectées**. Motif : « Manuelle » laissait croire qu'on dose à la main depuis HA, alors que le mode ne fait qu'arrêter la régulation automatique — voir [docs/features/page-ph.md](features/page-ph.md#libellés-révisés-auto--programme--arrêt-feature-161-v2770).
>
> ⚠️ **Republication de la découverte OBLIGATOIRE** ([ADR-0085](adr/0085-republication-decouverte-ha-sur-empreinte.md)) : la liste d'`options` fait partie de l'empreinte de découverte (qui inclut `FIRMWARE_VERSION`) — un changement de version republie automatiquement au démarrage suivant, sans action manuelle. Sans cette republication, Home Assistant conserverait l'ancienne liste d'options (« Manuelle » visible, mais rejetée en silence par le firmware si sélectionnée — voir le `warning` ajouté en feature-161 pour ce cas, [pump-controller.md](subsystems/pump-controller.md#verrou-étalonnage-sur-le-changement-de-mode-bug-161-gate-c2)).

Comportement :

- **Topics state inchangés** (retain, format existant) — rétrocompat totale des dashboards HA existants. Les topics de commande `.../set` sont **non retain**.
- **Validation stricte** de l'enum (comparaison après normalisation en minuscules) : toute valeur hors `automatic` / `scheduled` / `manual` est **ignorée** avec un log `warning` (« Mode régulation pH/ORP invalide (MQTT): ... »), aucun republish.
- **Même voie que l'UI web** : application sous `configMutex`, miroir booléen `ph_enabled` / `orp_enabled` mis à jour (`true` si mode ≠ `manual`, voir [ADR-0004](adr/0004-mode-regulation-enum-3-valeurs.md)), persistance NVS **uniquement si changement réel**. Republication immédiate de l'état (feedback HA).
- **Changement pendant une injection en cours** : l'injection se termine naturellement, le nouveau mode prend effet au cycle de régulation suivant.
- Pas de `switch` HA séparé pour `ph_enabled` / `orp_enabled` : le `select` est la source de vérité (les miroirs booléens restent publiés pour rétrocompat).

Voir [`docs/subsystems/mqtt-manager.md`](subsystems/mqtt-manager.md#gestion-des-commandes-ha-reçues) (handler) et [`docs/features/page-ph.md`](features/page-ph.md#interaction-mqtt) / [`page-orp.md`](features/page-orp.md#interaction-mqtt).

## Topics et entités ajoutés en feature-022 (problème capteur pH/ORP, v2.10.0)

Observabilité de l'indisponibilité capteur pour Home Assistant : un état binaire **par capteur**, directement consommable par un `binary_sensor` HA sans parser les JSON d'alerte.

### États binaires `{base}/ph_sensor_problem` / `{base}/orp_sensor_problem`

| Topic | Payload | Retain | Condition |
|-------|---------|:------:|-----------|
| `{base}/ph_sensor_problem` | `ON` / `OFF` | oui | `ON` si le capteur pH est **stale** (lecture NaN > 20 s, cf. `alerts/sensor_stale`) **OU figé** (variance nulle, cf. `alerts/sensor_frozen`). `OFF` sinon. |
| `{base}/orp_sensor_problem` | `ON` / `OFF` | oui | Symétrique ORP. |

Publication **dédupliquée** : le message n'est émis qu'au changement d'état (cache mémorisé, première publication forcée après connexion). Retain → HA retrouve l'état après un reboot du contrôleur ou de HA.

### Auto-discovery HA

| Entité | Type | Topic d'état | `unique_id` | Détails |
|--------|------|--------------|-------------|---------|
| Capteur pH — problème | `binary_sensor` | `{base}/ph_sensor_problem` | `poolcontroller_ph_sensor_problem` | `device_class: problem`, `payload_on: ON`, `payload_off: OFF`, `icon: mdi:alert` |
| Capteur ORP — problème | `binary_sensor` | `{base}/orp_sensor_problem` | `poolcontroller_orp_sensor_problem` | `device_class: problem`, mêmes payloads/icône |

### Alerte `{base}/alerts/sensor_frozen`

Calquée sur `alerts/sensor_stale` (retain, edge-triggered, clear = payload vide) — voir le tableau [Alertes dédiées](#alertes-dédiées-feature-021--feature-022-retain-edge-triggered). Publiée à la transition **figé** d'au moins un capteur pH/ORP :

```json
{ "type": "sensor_frozen", "phFrozen": true, "orpFrozen": false, "timestamp": 12345678 }
```

Un capteur figé (30 lectures acceptées dans une bande < ½ LSB, ≈ 2,5 min) rend la **valeur officielle NaN** → **dosage automatique bloqué fail-closed** (depuis v2.68.0, feature-152 : plus de drapeau « filtre non prêt » séparé, le `isnan()` de la valeur suffit). La **température figée** (détection warning-only, aucun impact dosage) n'apparaît **ni** dans cette alerte **ni** dans les états `*_sensor_problem` : depuis la v2.47.1 elle a son **topic dédié** `{base}/temp_sensor_frozen` — motif de cet arbitrage : [Pourquoi la température n'entre pas dans `alerts/sensor_frozen`](#pourquoi-la-température-nentre-pas-dans-alertssensor_frozen). Détails firmware : [`docs/subsystems/sensors.md`](subsystems/sensors.md#détection-capteur-figé--feature-022).

## ~~Topics et entités ajoutés en v2.20.3 (filtre de mesure bloqué)~~ — RETIRÉS en v2.68.0

⚠️ **`{base}/ph_filter_stuck` / `{base}/orp_filter_stuck` et leurs entités de découverte sont
RETIRÉS en v2.68.0** (feature-152) — l'alarme « filtre non prêt durablement » et le verrou
qu'elle tentait de signaler ont tous deux disparu (verdict `pool-chemistry` : ce latch se posait
sur du bruit normal, il n'inhibait pas un danger réel). Dépubliés par une **charge vide
retained** au démarrage (ADR-0085). Section conservée pour archive (description historique
v2.20.3 à v2.67.x) :

<details>
<summary>Description historique (archivée)</summary>

Un filtre pH ou ORP durablement non prêt = plus aucune régulation sur ce paramètre. L'incident
du 2026-08-16 a duré ≥ 3 h sans qu'aucun signal ne parte : ces deux topics comblaient ce trou
d'observabilité. `{base}/ph_filter_stuck` / `{base}/orp_filter_stuck` (retain) passaient à `ON`
si le filtre restait non prêt > 5 min alors que la filtration tournait et que les mesures
étaient représentatives. Entités HA : « Piscine Filtre pH/ORP Bloqué » (`binary_sensor`,
`device_class: problem`). Alertes JSON associées : `ph_filter_not_ready` / `orp_filter_not_ready`
et `ph_filter_rearm_denied` / `orp_filter_rearm_denied` (toutes retirées, voir [Alertes](#alertes)).

</details>

## Topic et entité ajoutés en feature-068 (sonde de température figée, v2.47.1)

### `{base}/temp_sensor_frozen`

**Direction** : ESP32 → broker (**retain = true**)

| Topic | Payload | Condition |
|-------|---------|-----------|
| `{base}/temp_sensor_frozen` | `ON` / `OFF` | `ON` quand la sonde DS18B20 « eau » a rendu `kTempFrozenSamples` = **43 200** lectures **valides** consécutives dans une bande < `kTempFrozenEpsilonC` = **0,03 °C** (≈ **24 h** à 2 s, bug-142 — était 900 / 30 min), **et** que la sonde « eau » rend bien une valeur. `OFF` sinon. |

- Miroir du champ `tempFrozenAlarm` de [`GET /data`](API.md) et du WebSocket, **avec un décalage volontaire de 60 s à la retombée** (voir ci-dessous).
- Publication **dédoublonnée** (`safePublishDedup`, slot 28) : émise au changement d'état seulement, première publication forcée après connexion. Retain ⇒ Home Assistant retrouve l'état après un redémarrage du contrôleur ou de HA.

> ⚠️ **Cet état n'inhibe aucun DOSAGE.** L'erreur de pH induite par une compensation de température figée vaut **≈ 0,001 pH**, soit 2 à 3 ordres de grandeur sous le premier seuil de régulation : ni le dosage, ni la compensation `RT` ne changent. Chiffrage et motif : [ADR-0074](adr/0074-temperature-figee-statu-quo-instrumente.md). En revanche il **gate le planning automatique de filtration** (bug-142) : `filtration.cpp:162` bloque tout recalcul pendant l'épisode, `:405` rejette les échantillons de T°. **Ne pas câbler ce topic comme une condition de sécurité chimique** — c'est un signal de **maintenance** (remplacer le capteur), pas d'urgence.

**Maintien de 60 s à la publication — et NULLE PART ailleurs** (`kMqttTempFrozenHoldMs`, [`src/mqtt_manager.h`](../src/mqtt_manager.h)). La sortie de l'état figé est **immédiate côté firmware, sans hystérésis** (décision du visa : le drapeau est informatif, un battement ne coûte rien chimiquement, et une hystérésis ajouterait un second seuil à régler). Mais une entité HA qui clignote est inutilisable en automatisation : l'état `ON` est donc **maintenu 60 s** après sa retombée, **uniquement sur ce chemin de publication**. Aucune décision de dosage, aucun champ `/data`, aucune trame WS n'en dépend — les trois voient la valeur **instantanée**.

**Garde « aucune sonde eau »** : le topic ne passe **jamais** à `ON` si `getWaterTemperatureRaw()` rend `NaN` — c'est-à-dire si le rôle « eau » n'est identifié sur **aucune** adresse du bus OneWire, ou si sa dernière lecture est invalide. Deux conséquences voulues :

1. « **sonde eau non identifiée** » n'est **jamais** confondu avec « **eau figée** » : les deux états ne sont pas actionnables de la même façon (identifier la sonde vs remplacer le capteur) ;
2. une sonde **débranchée pendant l'épisode** fait retomber le topic à `OFF` au lieu de le laisser « figé » indéfiniment sur un état obsolète.

### Auto-discovery HA (v2.47.1)

| Entité | Type | Topic d'état | `unique_id` | Détails |
|--------|------|--------------|-------------|---------|
| Piscine Sonde Température Figée | `binary_sensor` | `{base}/temp_sensor_frozen` | `poolcontroller_temp_sensor_frozen` | `device_class: problem`, `payload_on: ON`, `payload_off: OFF`, `icon: mdi:thermometer-off` |

Patron **exact** de l'ex-`ph_filter_stuck` (v2.20.3, **retiré en v2.68.0** — voir ci-dessus),
sans `entity_category` ; ce topic-ci (température) n'est **pas** concerné par le retrait,
seul le mécanisme pH/ORP a disparu. Publié à `homeassistant/binary_sensor/poolcontroller_temp_sensor_frozen/config` au démarrage. **Entité nouvelle** : les `unique_id` existants sont intacts, l'appariement des entités déjà en place dans HA ne bouge pas. La longueur du topic de configuration (**54** caractères) est **identique** à celle de `binary_sensor/poolcontroller_orp_sensor_problem/config`, pire cas déjà documenté pour `kDiscoveryFailTopicMax` : ce budget-là est inchangé.

### Pourquoi la température n'entre pas dans `alerts/sensor_frozen`

La spec laissait le choix ouvert. **Arbitrage : topic dédié, et rien dans l'alerte existante.** Trois motifs, dans l'ordre :

1. **`alerts/sensor_frozen` est un contrat externe** dont la sémantique **établie** est « une mesure de régulation est morte, **donc le dosage est compromis** » : un capteur pH/ORP figé rend la valeur officielle NaN et **bloque** l'injection. Y verser la température **reclasserait silencieusement** une alerte de sécurité de dosage — une automatisation Home Assistant **existante** se déclencherait sur un événement **sans aucun effet sur le dosage**. Élargir la portée d'une alerte déjà consommée est un **changement cassant sans bump de version**.
2. **La sévérité diffère de deux à trois ordres de grandeur, chiffrés** : ≈ 0,001 pH contre l'**arrêt complet** d'une boucle de régulation. Une même alerte pour les deux rendrait le niveau d'urgence illisible, donc, à terme, ignoré.
3. **La visibilité demandée est obtenue sans cela** : topic dédié retenu + `binary_sensor` `problem` + champ `/data`/WS + badge UI.

Conséquence assumée : un intégrateur qui veut **les deux** états s'abonne aux **deux** topics. C'est un abonnement de plus, pas une information perdue. Même raison pour les états `{base}/ph|orp_sensor_problem`, qui restent pH/ORP.

## Topics et entités ajoutés en feature-050 (complétude MQTT/HA, v2.14.0)

Issus d'un audit de couverture web/API vs MQTT/HA (périmètre « rentable »). Trois compléments : cumuls journaliers exposés, volumes quotidiens programmés commandables, bouton redémarrage.

### Cumuls journaliers injectés (P1)

| Topic | Description | Retain | Auto-discovery HA |
|-------|-------------|--------|-------------------|
| `{base}/ph_daily_ml` | Cumul journalier injecté pH en mL (`safetyLimits.dailyPhInjectedMl`), publié au cycle 10 s avec dédup, retombe à 0 à minuit | true | `sensor` « Dosage pH aujourd'hui » — `unique_id: poolcontroller_ph_daily_ml`, `unit: mL`, `state_class: measurement` |
| `{base}/orp_daily_ml` | Cumul journalier injecté chlore en mL (`safetyLimits.dailyOrpInjectedMl`), idem | true | `sensor` « Dosage Chlore aujourd'hui » — `unique_id: poolcontroller_orp_daily_ml`, `unit: mL`, `state_class: measurement` |

Complètent les `binary_sensor` « Limite Journalière pH/Chlore » existants (état atteint/non atteint) par la valeur numérique du cumul.

### Volume quotidien programmé commandable (P2)

Les topics d'état `{base}/ph_daily_target_ml` / `{base}/orp_daily_target_ml` (mode Programmée) deviennent commandables via `.../set` + 2 entités HA `number`.

| Entité | Type | `state_topic` | `command_topic` | `unique_id` | Détails |
|--------|------|---------------|-----------------|-------------|---------|
| Volume quotidien pH | `number` | `{base}/ph_daily_target_ml` | `{base}/ph_daily_target_ml/set` | `poolcontroller_ph_daily_target` | min 0 / max 2000 / step 10, `unit: mL` |
| Volume quotidien Chlore | `number` | `{base}/orp_daily_target_ml` | `{base}/orp_daily_target_ml/set` | `poolcontroller_orp_daily_target` | min 0 / max 2000 / step 10, `unit: mL` |

Validation du handler (`drainCommandQueue`, sous `configMutex`) : payload non numérique → ignoré + `warning` ; négatif → clampé à 0 ; **valeur strictement supérieure à la limite journalière VIVE (`maxPhMlPerDay` / `maxChlorineMlPerDay`) lue au moment du drain → refusée** + `warning` + resync HA sur la valeur réelle (condition posée par `pool-chemistry`). Persistance NVS **si changement réel**, republication immédiate, log `info` « ... (MQTT) ». La borne discovery (2000 mL) est indicative ; la limite vive reste le garde-fou effectif.

### Bouton Redémarrer (P3)

| Topic | Description | Retain | Auto-discovery HA |
|-------|-------------|--------|-------------------|
| `{base}/reboot/set` | Tout payload accepté (le bouton HA envoie `PRESS`). Redémarrage **différé propre** : flag consommé en tête de `drainCommandQueue()` au prochain tour de loop, après `kRestartApModeDelayMs`, puis `shutdownForRestart()` (flush `status=offline`, ADR-0011) + `ESP.restart()` | non | `button` « Redémarrer » — `unique_id: poolcontroller_reboot`, `device_class: restart` |

Jamais de `ESP.restart()` direct au moment du drain — même séquence que la route `POST /reboot`.

---

## Topics et entités ajoutés en feature-051 (heures de filtration éditables, v2.16.0)

Expose les heures du créneau de filtration en entités HA `text` **éditables** (composant `text/`), complétant le select « Mode Filtration ».

| Entité | Type | `state_topic` | `command_topic` | `unique_id` | Détails |
|--------|------|---------------|-----------------|-------------|---------|
| Filtration début | `text` | `{base}/filtration_start` | `{base}/filtration_start/set` | `poolcontroller_filtration_start` | `pattern` `^([01][0-9]|2[0-3]):[0-5][0-9]$`, min 5 / max 5, `icon: mdi:clock-start` |
| Filtration fin | `text` | `{base}/filtration_end` | `{base}/filtration_end/set` | `poolcontroller_filtration_end` | idem, `icon: mdi:clock-end` |

Les états `{base}/filtration_start` / `{base}/filtration_end` (retain) sont publiés par `publishFiltrationState()` (valeurs `filtrationCfg.start` / `.end`) — au boot lors de la (re)connexion, à chaque changement, et au cycle périodique de `publishAllStatesInternal()`.

**Sémantique** (identique à l'UI web) : les heures ont un effet réel en mode **Programmation** (`manual`). En mode **Auto**, elles sont recalculées par la température au prochain `filtration.update()` → une saisie HA en mode auto est écrasée et l'état republié montre la valeur recalculée (consultation, auto-corrigé). Le `pattern` de la discovery valide le format côté HA ; le handler revalide `HH:MM` via `timeStringToMinutes` (défense en profondeur). **Aucun chemin de dosage touché** : le dosage teste l'état live `filtration.isRunning()`, pas le planning.

---

## Topics et entités ajoutés en feature-052 (planning éclairage HA, v2.17.0)

Miroir de la feature-051 pour l'éclairage : expose le planning horaire (activation + heures) en entités HA éditables, complétant le switch « Éclairage Piscine » (contrôle manuel du relais, **inchangé**).

| Entité | Type | `state_topic` | `command_topic` | `unique_id` | Détails |
|--------|------|---------------|-----------------|-------------|---------|
| Mode Éclairage | `select` | `{base}/lighting_schedule` | `{base}/lighting_schedule/set` | `poolcontroller_lighting_schedule` | Options **Programmation / Désactivé** ; templates ↔ `ON`/`OFF` sur le fil → booléen `lightingCfg.scheduleEnabled`. `switch` à l'origine (feature-052), migré en `select` en bug-ha-eclairage-select (v2.17.2) |
| Éclairage début | `text` | `{base}/lighting_start` | `{base}/lighting_start/set` | `poolcontroller_lighting_start` | `pattern` `^([01][0-9]|2[0-3]):[0-5][0-9]$`, min 5 / max 5, `icon: mdi:clock-start` |
| Éclairage fin | `text` | `{base}/lighting_end` | `{base}/lighting_end/set` | `poolcontroller_lighting_end` | idem, `icon: mdi:clock-end` |

Les états `{base}/lighting_schedule` / `{base}/lighting_start` / `{base}/lighting_end` (retain) sont publiés par `publishLightingState()` enrichie (valeurs `lightingCfg.scheduleEnabled` / `.startTime` / `.endTime`) — au boot lors de la (re)connexion, à chaque changement, et au cycle périodique de `publishAllStatesInternal()`.

**Choix booléen conservé** (cf. feature-028) : `scheduleEnabled` reste un booléen côté firmware et dans `/get-config` (`lighting_schedule_enabled`) ; l'exposition HA a d'abord été un `switch` (ON/OFF), migrée en `select` « Mode Éclairage » en v2.17.2 (le fil reste ON/OFF, voir la note ci-dessus). Un payload invalide (ni `ON` ni `OFF`, ou `HH:MM` mal formé) est ignoré + `warning` + resync HA sur la valeur réelle. Le broadcast WS config (bug-sync, v2.14.1) propage tout changement HA vers l'UI web sans rechargement. **Aucun chemin de dosage touché.**

---

## Topic et entité ajoutés en feature-053 (Mode Boost, v2.18.0)

Le **Mode Boost** est une surchloration temporaire du jour (filtration prolongée + cible ORP relevée), auto-expirant au prochain minuit local. Détails firmware : [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#mode-boost-feature-053). Décision structurante : [ADR-0025](adr/0025-mode-boost.md).

| Entité | Type | `state_topic` | `command_topic` | `unique_id` | Détails |
|--------|------|---------------|-----------------|-------------|---------|
| Boost | `switch` | `{base}/boost` | `{base}/boost/set` | `poolcontroller_boost` | `payload_on: ON`, `payload_off: OFF`, `icon: mdi:flash` |

L'état `{base}/boost` (retain) est publié à la (re)connexion, à chaque changement (activation / désactivation / expiration) et au cycle périodique de publication. Le switch retombe **automatiquement** sur `OFF` à minuit local sans commande HA (l'expiration côté firmware republie l'état).

**Activation refusée sans heure synchronisée** : si `{base}/boost/set` reçoit `ON` alors que l'horloge n'est pas synchronisée, la commande est **ignorée** (l'expiration à minuit serait incalculable) et l'état réel (`OFF`) est republié → le switch HA se resynchronise. Même politique de sécurité que la route HTTP `POST /boost/start` (`409 time_not_synced`, voir [API.md](API.md#post-booststart--write)).

> **Effet chlore gaté au mode ORP `automatic`** : le relèvement de la cible ORP et de la limite journalière chlore ne s'applique qu'en mode de régulation ORP `automatic`. En mode Manuel / Programmé, l'activation du Boost n'étend **que la filtration** (l'injection manuelle reste bornée à la limite normale). Voir [ADR-0025](adr/0025-mode-boost.md).

---

## Topic et entité ajoutés en feature-140 (Chloration de secours, v2.55.0)

La **Chloration de secours** est une surcouche temporaire non destructive du mode « Programmée » existant : volume de chlore fixe, aveugle à l'ORP, auto-expirant 48 h **glissantes** après l'activation (epoch — pas un ancrage sur minuit local, contrairement au Boost : voir [ADR-0087](adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md)). **v2.56.0 (feature-141) : le volume n'est plus la constante 1500 mL/j** — il est saisi par l'utilisateur à l'activation côté interface web (voir [ci-dessous](#topic-et-entité-ajoutées-en-feature-141-volume-de-secours-saisi-v2560)), borné `[100, min(limite utilisateur, 2000)]` mL/j. **Ne modifie jamais `{base}/orp_regulation_mode`.** Détails firmware : [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#chloration-de-secours-feature-140). Décision structurante : [ADR-0087](adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md).

| Entité | Type | `state_topic` | `command_topic` | `unique_id` | Détails |
|--------|------|---------------|-----------------|-------------|---------|
| Chloration de secours | `switch` | `{base}/rescue` | `{base}/rescue/set` | `poolcontroller_rescue_chlorine` | `payload_on: ON`, `payload_off: OFF`, `icon: mdi:flask-plus-outline` |
| Volume Chloration de secours | `sensor` | `{base}/rescue_ml` | — | `poolcontroller_rescue_ml` | **v2.56.0 (feature-141)**, **lecture seule** (pas de `command_topic` : le volume se SAISIT côté interface web, jamais depuis HA). `unit_of_measurement: mL`, `state_class: measurement`, `icon: mdi:cup-water`. |

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/rescue` | `ON` / `OFF` | État **effectif** de la Chloration de secours (retain). `ON` tant que le mode est actif ; retombe automatiquement à `OFF` à l'expiration des 48 h, à la levée automatique (30 min de mesures ORP de nouveau fiables) ou à l'arrêt manuel. |
| `{base}/rescue_lift_reason` | `0` – `3` | **Complément** (2026-10-05) : raison de la **dernière fin** du mode (`RescueLiftReason`), fenêtre de visibilité glissante de 90 s — `0` = aucune fin récente, `1` = levée automatique, `2` = expiration des 48 h, `3` = arrêt manuel. Retain, republié au cycle périodique (10 s) pour que le topic retombe à `0` de lui-même une fois la fenêtre écoulée. |

L'état `{base}/rescue` (retain) est publié à la (re)connexion, à chaque changement (activation / désactivation / expiration / levée automatique) et au cycle périodique de publication.

**Activation refusée** dans deux cas, sans jamais annoncer une activation qui n'a pas eu lieu (correctif revue, gate 2) :
- **mode Manuel** : si la régulation ORP (NVS) est en `"manual"`, la commande `ON` est **ignorée** (double dosage évité — l'utilisateur dose déjà lui-même) et l'état réel republié. Même refus que la route HTTP `POST /rescue/start` (`409 mode_manual`).
- **heure non synchronisée** : l'échéance 48 h serait indéterminable, même politique de sécurité que le Boost (minuit).

## Topic et entité ajoutées en feature-141 (Volume de secours saisi, v2.56.0)

Le volume quotidien de la Chloration de secours n'est plus la constante `kRescueChlorineDailyMl` (1500 mL/j) : il est **saisi par l'utilisateur à l'activation** côté interface web, borné côté serveur, et persisté en NVS (`rescue_ml`). Détail complet de la chaîne de pré-remplissage et des deux clamps distincts : [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#chloration-de-secours-feature-140).

**`{base}/rescue_ml`** (`sensor`, lecture seule, retain, dédupliqué) : volume **ARMÉ** si le mode est actif, **PROPOSÉ** (chaîne de pré-remplissage) sinon. Jamais `0`. Publié :
- immédiatement après chaque activation (`publishRescueState()`) ;
- au cycle périodique (10 s), dédupliqué — `rescueDailyMlForDisplay()` peut varier hors activation (ex. `orp_daily_target_ml` ou la taille du bassin modifiés pendant que le secours est inactif).

⚠️ **Pourquoi un `sensor` et pas un champ du `switch` existant** : Home Assistant n'a pas de type d'entité « switch avec paramètre numérique » sans changer le protocole du switch lui-même (hors périmètre de cette feature). Le volume **se saisit côté interface web uniquement** — ce capteur rend seulement visible, côté Home Assistant, ce qui a été armé, pour fermer l'angle mort qu'un `ON` envoyé depuis HA arme un volume que l'utilisateur HA ne voit nulle part ailleurs.

**Chemin MQTT `ON` sans volume** (invariant feature-090, préservé à la lettre comme à l'esprit) : la résolution **saute l'étage « taille de bassin »** — `orp_daily_target_ml` (NVS, si > 0) sinon le repli 1500 mL/j. `PoolSize` n'est **jamais** consultée sur ce chemin, contrairement au pré-remplissage du dialogue web (qui, lui, inclut l'étage taille). Aucun héritage de la dernière valeur saisie : chaque `ON` sans volume explicite repart de cette chaîne, pas d'une mémoire de la saisie précédente (arbitrage tranché le 2026-10-05, voir la spec [feature-141](../specs/features/done/feature-141-volume-de-secours-saisi-a-l-activation.md)).

**Désarmement automatique** : si `{base}/orp_regulation_mode/set` reçoit `manual` **pendant** que la Chloration de secours est active, le mode est désarmé automatiquement (`RescueLiftReason::Manual`) et l'état republié — même règle que le changement de mode via la route HTTP `/save-config`.

> **Le pH n'est jamais dosé par ce mode** (condition #11 du gate `pool-chemistry`) : aucun topic pH n'est affecté par la Chloration de secours.

---

## Topics et entités ajoutés en feature-056 (Mode d'installation, v2.19.0)

Le **mode d'installation** décrit le câblage réel de l'installation (3 archétypes) et remplace les anciens `regulation_mode` / `filtration_enabled`. Détails firmware : [`docs/subsystems/filtration.md`](subsystems/filtration.md) et [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md). Décision structurante : [ADR-0026](adr/0026-mode-installation.md).

| Entité | Type | `state_topic` | `command_topic` | `unique_id` | Détails |
|--------|------|---------------|-----------------|-------------|---------|
| Mode d'installation | `select` | `{base}/install_mode` | `{base}/install_mode/set` | `poolcontroller_install_mode` | `icon: mdi:pipe-valve`, options FR **PoolController pilote / Alimenté par filtration / Filtration externe** + templates (voir ci-dessous) |
| Signal filtration externe | `switch` | — (optimiste) | `{base}/filtration_external_state/set` | `poolcontroller_filtration_external` | `payload_on: ON`, `payload_off: OFF`, `optimistic: true` (pas de `state_topic`), `icon: mdi:water-sync` |

**Select « Mode d'installation » — libellés FR côté HA, protocole inchangé** : comme les autres selects (filtration, régulation), la traduction est purement côté HA via templates de la discovery. Le protocole sur le fil reste strictement `managed` / `powered` / `external`.

- `value_template` : `{{ {'managed':'PoolController pilote','powered':'Alimenté par filtration','external':'Filtration externe'}.get(value, value) }}`
- `command_template` : `{{ {'PoolController pilote':'managed','Alimenté par filtration':'powered','Filtration externe':'external'}[value] }}`

**Switch « Signal filtration externe » — optimiste** : il n'a **pas** de `state_topic` (`optimistic: true`) car le contrôleur ne republie pas cet état — c'est un **signal entrant** consommé par la garde « présence d'eau ». Une automation HA (ex. état d'un contacteur, d'une prise connectée) publie `ON`/`OFF` sur `{base}/filtration_external_state/set` ; le contrôleur applique le timeout de fraîcheur (180 s) et le fail-safe OFF. Utile **uniquement** en mode d'installation `external`.

---

## Topics et entités ajoutés en v2.26.0 (terme intégral pH)

La régulation pH passe de **P pur** à **PI** ([ADR-0052](adr/0052-terme-integral-ph-dimensionne-par-autorite.md)) : un proportionnel pur sous charge basique permanente ne peut structurellement pas rejoindre sa consigne. Le terme intégral porte **toute la charge à l'équilibre** (où le proportionnel s'annule) : sans exposition, une intégrale saturée resterait invisible depuis Home Assistant. Détail firmware : [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#terme-intégral-ph--levée-de-ki--0-v2260).

| Topic | Payload | Description | Retain |
|-------|---------|-------------|--------|
| `{base}/ph_integral` | `7450.2` | Intégrale accumulée en **pH·s**, **signée**, 1 décimale. Une valeur négative indique le côté de la consigne, ce n'est pas une anomalie. | true |
| `{base}/ph_integral_term` | `2.759` | Contribution au **débit moyen demandé** (mL/min, 3 décimales) = `ki × intégrale`. Bornée par l'**autorité** `kPhIntegralAuthorityMlPerMin` = **6,0 mL/min**, quelle que soit la vitesse de régulation. | true |
| `{base}/ph_integral_pct` | `46.0` | Remplissage de l'anti-windup, en **% du plafond** (1 décimale, **valeur absolue**). | true |

### Auto-discovery HA (v2.26.0)

| Entité | Type | `state_topic` | `unique_id` | Détails |
|--------|------|---------------|-------------|---------|
| Piscine pH Rattrapage | `sensor` | `{base}/ph_integral_pct` | `poolcontroller_ph_integral_pct` | `unit: %`, `icon: mdi:gauge`, `state_class: measurement` — **entité de tête**, visible sur la carte principale |
| Piscine pH Rattrapage Débit | `sensor` | `{base}/ph_integral_term` | `poolcontroller_ph_integral_term` | `unit: mL/min`, `icon: mdi:speedometer`, `state_class: measurement`, `entity_category: diagnostic` |
| Piscine pH Intégrale | `sensor` | `{base}/ph_integral` | `poolcontroller_ph_integral` | `unit: pH·s`, `icon: mdi:sigma`, `state_class: measurement`, `entity_category: diagnostic`. Pas de `device_class` : aucune classe HA ne correspond à des pH·s. |

- **Le pourcentage est l'entité de tête** : c'est la seule des trois **lisible sans connaître `integralMax`**, lequel **varie** avec la vitesse de correction (43 200 / 16 200 / 7 200 pH·s). Home Assistant n'a donc jamais à connaître ce plafond et **aucune template n'a à le recopier** — le firmware sert le rapport déjà calculé. Les deux autres entités sont du diagnostic (repliées hors de la carte principale).
- **Aucune entité existante n'est touchée** : les trois `unique_id` sont inédits, l'auto-discovery en place est intacte. Le nombre de publications de `publishDiscovery()` passe de 49 à **52**.
- **`NaN` → aucune publication** (`Ki` nul, PID non initialisé) : le retain précédent reste valable, on ne fabrique jamais un `0` qui se lirait « intégrale vide ». Trois **slots de déduplication** ajoutés (18/19/20, `kDedupCacheSlots` 18 → **21**) : l'intégrale étant figée hors dosage, la dédup supprime la quasi-totalité du trafic de ces topics pendant les heures d'arrêt.
- **Aucun équivalent ORP** : `Ki_ORP` reste 0 (asymétrie voulue, hors périmètre de l'ADR-0052).
- Mêmes noms de champs et mêmes arrondis que `GET /data` et que la trame WebSocket : une même grandeur porte le **même nom sur les trois transports**.

L'anomalie « la régulation demande son maximum sans atteindre la consigne » est signalée par l'alerte [`ph_integral_saturated`](#alertes) — c'est elle qui fait autorité, elle seule tenant compte de la **durée** de l'épisode.

---

## Topics et entités ajoutés en v2.28.0 (dose de base apprise — feedforward pH)

La régulation pH ajoute un terme de **feedforward** ([ADR-0055](adr/0055-feedforward-appris-additif-plafonne.md)) : la charge quotidienne connue (médiane 7 j de l'anneau de charge) est couverte d'office pendant les sessions automatiques, le PI ne traitant plus que l'écart résiduel. Détail firmware : [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#dose-de-base-apprise--feedforward-ph-v2280).

Même facture que les topics du terme intégral : **une grandeur par topic scalaire**, retain, dédup, mêmes noms et mêmes arrondis que `GET /data` et la trame WebSocket.

| Topic | Payload | Description | Retain |
|-------|---------|-------------|--------|
| `{base}/ph_ff_ml_per_min` | `1.30` | Débit de base appliqué à la session de dosage pH **automatique** en cours (mL/min, **2 décimales**). Vaut `0` hors session — le feedforward ne vit que sous une session ouverte. `NaN` → aucune publication (garde par symétrie avec l'intégrale, le retain précédent reste valable). | true |
| `{base}/ph_ff_status` | `1` | Code d'état `PhFfStatus`, **contrat figé 0-4** : `0` inactif · `1` actif · `2` retenu par le latch de sécurité · `3` en apprentissage (historique < 3 jours) · `4` quota journalier (≥ 60 % de la limite). **Toujours publié** : c'est lui qui dit *pourquoi* le feedforward ne s'applique pas. Code **brut** (pas de libellé français dans le topic) : le mapping appartient aux clients, comme pour les codes de cause de refus ([ADR-0049](adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md)). | true |

### Auto-discovery HA (v2.28.0)

| Entité | Type | `state_topic` | `unique_id` | Détails |
|--------|------|---------------|-------------|---------|
| Piscine pH Dose de Base | `sensor` | `{base}/ph_ff_ml_per_min` | `poolcontroller_ph_ff_flow` | `unit: mL/min`, `icon: mdi:water-pump`, `state_class: measurement`, `entity_category: diagnostic` |
| Piscine pH Dose de Base Statut | `sensor` | `{base}/ph_ff_status` | `poolcontroller_ph_ff_status` | `icon: mdi:state-machine`, `entity_category: diagnostic`. Pas de `state_class` ni d'`unit_of_measurement` : code discret, pas une mesure. |

- **Aucune entité existante n'est touchée** : les deux `unique_id` sont inédits. Le nombre de publications de `publishDiscovery()` passe de 52 à **54**.
- Deux **slots de déduplication** ajoutés (21/22, `kDedupCacheSlots` 21 → **23**) : le feedforward étant figé hors session, la dédup supprime la quasi-totalité du trafic de ces topics hors dosage.

---

## Topics et entités ajoutés en v2.46.0 (composition du débit d'injection)

La carte « Débit d'injection » (feature-092) rend lisible l'équation que la régulation applique déjà. **Cinq** topics la portent aussi vers Home Assistant — ce qu'aucune entité n'y disait : **l'équation**, et pas seulement ses effets. Détail firmware : [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#composition-du-débit-dinjection--accesseurs-de-lecture-seule-feature-092-v2460).

Même facture que les topics du terme intégral et du feedforward : **une grandeur par topic scalaire**, retain, dédup, mêmes noms et **mêmes arrondis** (2 décimales) que `GET /data` et la trame WebSocket. `NaN` → **aucune publication** : le retain précédent reste valable, on ne fabrique jamais un `0` qui se lirait « pH à la consigne » ou « rien d'appris ». C'est le cas nominal quand la filtration est arrêtée (porte de validité fermée) ou quand l'historique n'atteint pas 3 journées exploitables.

| Topic | Payload | Description | Retain |
|-------|---------|-------------|--------|
| `{base}/ph_ff_learned_ml_per_min` | `1.41` | Dose de base **apprise** (mL/min) = `min(charge médiane 7 j, 60 % du soutenable)`. **Conserve sa valeur entre deux sessions.** | true |
| `{base}/ph_p_term_ml_per_min` | `0.35` | Écart courant `kp·e` (mL/min), **SIGNÉ** — négatif = pH sous la consigne. `0` dans la bande morte, jamais borné. Non publié si `kp = 0`. | true |
| `{base}/ph_demand_ml_per_min` | `4.33` | Demande **totale bornée** (mL/min) = `clamp(PI servi + FF, 0, amplitude de la pompe)`. ⚠️ **Ce n'est pas `min(demande, soutenable)`** — voir l'avertissement ci-dessous. | true |
| `{base}/orp_p_term_ml_per_min` | `1.20` | Écart courant chlore (mL/min), signé. | true |
| `{base}/orp_demand_ml_per_min` | `1.20` | Demande ORP. Régulation **P-pure** : elle **vaut** le terme proportionnel borné. | true |

### Auto-discovery HA (v2.46.0)

**Cinq** entités nouvelles, `unique_id` inédits — **aucune entité existante n'est touchée**. Toutes `sensor`, `entity_category: diagnostic`, `unit_of_measurement: mL/min`, `state_class: measurement`. **Aucune `device_class`** : HA n'en propose aucune pour un débit de dosage.

| Entité | Type | `state_topic` | `unique_id` | Détails |
|--------|------|---------------|-------------|---------|
| Piscine pH Dose de Base Apprise | `sensor` | `{base}/ph_ff_learned_ml_per_min` | `poolcontroller_ph_ff_learned_flow` | `icon: mdi:school` |
| Piscine pH Écart Courant | `sensor` | `{base}/ph_p_term_ml_per_min` | `poolcontroller_ph_p_term_flow` | `icon: mdi:delta` |
| Piscine pH Demande | `sensor` | `{base}/ph_demand_ml_per_min` | `poolcontroller_ph_demand_flow` | `icon: mdi:speedometer` |
| Piscine ORP Écart Courant | `sensor` | `{base}/orp_p_term_ml_per_min` | `poolcontroller_orp_p_term_flow` | `icon: mdi:delta` |
| Piscine ORP Demande | `sensor` | `{base}/orp_demand_ml_per_min` | `poolcontroller_orp_demand_flow` | `icon: mdi:speedometer` |

- Le nombre de publications de `publishDiscovery()` passe de 54 à **59**.
- **Cinq slots de déduplication** ajoutés (23-27, `kDedupCacheSlots` 23 → **28**).

> ⚠️ **« Dose de Base Apprise » coexiste avec « Dose de Base » — deux entités proches, à ne pas confondre.**
>
> | Entité | Topic | Comportement hors session |
> |---|---|---|
> | Piscine pH **Dose de Base** (v2.28.0) | `{base}/ph_ff_ml_per_min` | **retombe à 0** — c'est le débit *appliqué*, et le feedforward ne vit que sous une session ouverte. Depuis la v2.46.0 elle rapporte aussi `0,00` pendant une prolongation de session à débit nul (**régime appliqué** : correction, le champ est documenté « appliqué » depuis l'origine) |
> | Piscine pH **Dose de Base Apprise** (v2.46.0) | `{base}/ph_ff_learned_ml_per_min` | **conserve sa valeur** — c'est ce que l'appareil *sait*, calculé à la lecture depuis l'anneau de charge |
>
> C'est précisément la grandeur qui manquait à Home Assistant : jusqu'ici, la valeur apprise n'était visible **nulle part** (ni `/data`, ni WS, ni MQTT — seulement dans le journal, à l'ouverture de session). Une automatisation qui suit « Dose de Base » voit un créneau ; une automatisation qui suit « Dose de Base Apprise » voit une tendance.

> ⚠️ **`ph_demand_ml_per_min` n'est PAS un débit plafonné au soutenable.** Le firmware borne la demande à l'**amplitude de la pompe**, **jamais** au débit soutenable ; la limite horaire agit **en aval**, par **refus**. Avec une limite de 10 min/h, une demande de 5,20 mL/min est réellement servie **à 5,20 de moyenne jusqu'à épuisement du budget horaire**, puis 0. Un template HA qui calculerait `min(demande, soutenable)` pour afficher « ce que la pompe délivre » **sous-estimerait de 17 %** ce que le bassin reçoit dans les dix minutes suivantes. **Il n'existe aucun topic « appliqué », et il n'en existera pas.**

### Trois champs délibérément HORS MQTT, et pourquoi

| Champ servi sur `/data` + WS | Motif de non-publication |
|---|---|
| `ph_i_term_ml_per_min` | **Doublon fonctionnel** de `{base}/ph_integral_term`, déjà publié depuis la v2.26.0 et déjà une entité HA en production. Les deux ne diffèrent **que dans la bande morte** (le premier est le terme *servi*, le second le terme *brut*) : publier les deux créerait deux entités quasi identiques dans le tableau de bord, pour une nuance qui n'a d'usage que sur la carte web. |
| `orp_sustainable_ml_per_min` | **Miroir** d'un champ pH qui n'est **pas** publié non plus (`ph_sustainable_ml_per_min`, feature-086 — MQTT explicitement hors périmètre de ce cycle-là, budget flash). Publier le miroir sans l'original serait une asymétrie sans justification. |
| `orp_limited_by_hourly_budget` | Même raison : `ph_limited_by_hourly_budget` n'est pas publié. C'est en outre un **verdict d'affichage** dont la valeur d'automatisation est nulle — et un **diagnostic, pas une alarme** (voir ci-dessous). |

> **`orp_limited_by_hourly_budget` est un DIAGNOSTIC, pas une alarme** — à écrire ici parce que la tentation qu'il déclenche est exactement l'inverse de la bonne réponse. Verdict `pool-chemistry` : à CYA **35 mg/L**, une boucle chlore qui sature son quota quotidien est **cohérente avec le coût de la protection UV** (chlore libre visé 2,5-3 mg/L). Ce n'est **pas** une anomalie à corriger en montant la consigne ORP ou le gain. Une automatisation HA qui relèverait la consigne sur ce signal aggraverait la consommation sans améliorer la désinfection.

---

## Exclusions volontaires (politique MQTT/HA)

Cette section documente ce qui **n'est délibérément PAS exposé** en MQTT / Home Assistant. C'est une **politique assumée**, pas un backlog : l'audit de couverture (feature-050) a tranché chaque cas ci-dessous. Une ré-exposition future doit être justifiée par un besoin constaté, pas par principe de complétude.

| Non exposé | Pourquoi |
|-----------|----------|
| **Injections manuelles** (déclenchement direct d'un dosage pH/chlore) | Sécurité chimique : aucun dosage ne doit pouvoir être déclenché par une automatisation sans supervision humaine. Une injection reste une action locale (UI web), sous présence de l'opérateur. |
| **Limites journalières / horaires de dosage** (configurables) | Ce sont des **garde-fous**, pas des préférences d'usage. Les rendre commandables à distance affaiblirait la barrière de sécurité qu'ils constituent (une automatisation HA pourrait relever le plafond avant de doser). Exposition écartée dès la feature-013. |
| **Calibration EZO pH/ORP** | Requiert la présence physique (solutions étalon, sonde plongée). Action non automatisable à distance. |
| **OTA / mise à jour firmware** | Flux supervisé interactivement, avec vérification CRC/signature. Pas de déclenchement fire-and-forget. |
| **Factory reset** | Action destructive irréversible (efface config/NVS). Jamais exposée à une automatisation. |
| **Wi-Fi (scan, connexion, changement)** | Configuration réseau sensible ; couper le Wi-Fi via MQTT couperait le canal MQTT lui-même. Présence physique ou UI locale. |
| **Authentification (token, mot de passe)** | Secret ; jamais transporté ni modifiable via MQTT. |
| **Pompes en mode test** | Actionnement direct des pompes pour maintenance — présence physique requise, potentiellement destructif (dosage à sec). |
| **Transients d'affichage** : état de la purge des mesures (`measures_stab_elapsed_s`, `measures_purge_last_*`, `measures_dose_available_in_s`), `inject_remaining`, **cause de blocage dosage** | États de transition à durée de vie très courte, pertinents seulement pour l'UI temps réel (WebSocket + `/data`). La cause de blocage dosage a été explicitement écartée en feature-022, sur besoin constaté (bruit pour HA, pas de valeur d'automatisation). **v2.34.0 à v2.61.x** : seule l'**anomalie** de purge remontait, sous forme d'alerte `measures_purge_timeout` — aucune entité d'auto-discovery n'était créée pour l'état courant de la purge. **v2.62.0 (feature-139)** : ce mécanisme et son alerte sont **retirés** ; aucun de ces champs, nouveaux compris, ne remonte sur le bus MQTT. |
| **Réarmement d'un filtre de mesure** (`POST /reset-sensor-filter`, v2.20.3) | Action **destructrice pour le diagnostic** : elle efface le détecteur de sonde figée et vide la fenêtre médiane (le latch EMI évoqué dans une version antérieure de cette ligne a disparu en v2.68.0, feature-152 — il n'y a plus de drapeau d'instabilité à réinitialiser). Exposée en MQTT, une automation pourrait la déclencher en boucle et masquer indéfiniment une sonde défaillante. La commande reste locale, sous auth administrateur et cooldown de 10 min ; l'état public de référence est désormais simplement `{base}/ph` à `null`. |
| **Historique** (brut / horaire / quotidien) | Home Assistant dispose de son propre recorder / long-term statistics qui agrège les `state_class: measurement` déjà publiés. Dupliquer l'historique firmware vers MQTT serait redondant. |
