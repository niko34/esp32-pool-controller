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
| `{base}/ph` | `7.234` | Valeur pH **filtrée et CORRIGÉE** (médiane + EMA + offset logiciel `ph_offset` depuis 2.20.0) — repli sur le pH courant **corrigé** si le filtre n'est pas amorcé, jamais sur le brut (**3 décimales** depuis 2.0.0 — voir [ADR-0014](adr/0014-migration-atlas-ezo.md)). Topics brut/médiane séparés ci-dessous (feature-025). |
| `{base}/orp` | `720` | Valeur ORP **filtrée** (mV) depuis 2.2.0 — fallback brut si filtre non amorcé |
| `{base}/ph_cal_points` | `2` | Points de calibration EZO pH (entier `-1..3`, `-1` = EZO injoignable). Retain. Voir [feature-021](../specs/features/done/feature-021-migration-atlas-ezo.md). |
| `{base}/orp_cal_points` | `1` | Points de calibration EZO ORP (entier `-1..1`, `-1` = EZO injoignable). Retain. |
| `{base}/ph_slope_acid` | `99.7` | Pente acide sonde pH EZO en % (1 décimale). Retain. Edge-triggered ([feature-024](../specs/features/done/feature-024-pente-sonde-ph.md)). |
| `{base}/ph_slope_base` | `100.3` | Pente base sonde pH EZO en % (1 décimale). Retain. Edge-triggered. |
| `{base}/ph_slope_zero` | `-0.89` | Décalage zéro sonde pH EZO en mV (2 décimales). Retain. Non publié si firmware EZO ancien. |

> **Sémantique brut / corrigé du pH (feature-067, v2.20.0)** — l'offset logiciel `ph_offset` (`pH corrigé = pH brut EZO + ph_offset`) n'a provoqué **aucun renommage d'entité, aucun nouveau topic, aucune suppression**. Seule la **valeur** de deux topics existants change :
>
> | Topic | `unique_id` HA | Contenu | Offset appliqué ? |
> |-------|----------------|---------|-------------------|
> | `{base}/ph` | `poolcontroller_ph` | pH filtré **corrigé** — valeur « vraie » du bassin | ✅ oui |
> | `{base}/ph_filtered` | `poolcontroller_ph_filtered` | pH lissé **corrigé** — valeur consommée par la régulation | ✅ oui |
> | `{base}/ph_median` | *(pas d'entité HA)* | médiane **corrigée** | ✅ oui |
> | `{base}/ph_raw` | `poolcontroller_ph_raw` | pH **brut Atlas EZO**, jamais corrigé | ❌ non |
>
> ⚠️ **Conséquence côté Home Assistant** : `sensor.*_ph` et `sensor.*_ph_filtered` gardent leur `entity_id` et leur historique, mais **changent de valeur** le jour où un offset non nul est enregistré → une **marche** apparaît dans les graphiques HA. C'est le comportement voulu ([ADR-0039](adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)) : renommer les entités aurait cassé toutes les automatisations et tous les dashboards existants pour un simple décalage de mesure. Pour comparer brut et corrigé, superposer `sensor.*_ph_raw` et `sensor.*_ph`.
>
> **Aucune entité `ph_offset` n'est publiée** : l'offset est un paramètre de configuration, exposé par `GET /get-config` et le message WebSocket `config`, pas une mesure. En créer une entité HA aurait ajouté un point de commande sur l'**entrée de la régulation chimique** hors du chemin validé. La valeur de l'offset apparaît dans les messages d'alerte `ph_abnormal`, `ph_guard` et `orp_ph_inhibit` (voir [Alertes](#alertes)).
>
> Le test de vivacité du capteur (`phStale` de l'alerte `{base}/alerts/sensor_stale`) consomme le **brut** : un offset ne peut ni masquer ni provoquer une détection de sonde muette.

### Filtration

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/install_mode` | `managed` / `powered` / `external` | **Mode d'installation** (feature-056, v2.19.0). Retain. Décrit le câblage : `managed` = PoolController pilote la filtration ; `powered` = alimenté par le circuit de filtration ; `external` = filtration tierce signalée. Valeurs brutes sur le fil ; le select HA les affiche traduites (voir feature-056 ci-dessous). Voir [ADR-0026](adr/0026-mode-installation.md). |
| `{base}/filtration_state` | `ON` / `OFF` | État du relais de filtration |
| `{base}/filtration_mode` | `auto` / `manual` / `force` / `off` | Mode de filtration courant (valeurs brutes sur le fil ; le select HA les affiche traduites — voir ci-dessous). Sémantique : `auto` = créneau calculé selon la température (heures en lecture seule) ; `manual` = **Programmation** (créneau à heures fixées par l'utilisateur) ; `force` = **Manuel** (contrôle ON/OFF sans planning) ; `off` = **Désactivé**. |
| `{base}/filtration_start` | `HH:MM` (ex. `08:00`) | Heure de début du créneau de filtration (feature-051, v2.16.0). Retain. En mode `auto`, recalculée selon la température (lecture seule) ; en mode `manual` (**Programmation**), heure fixée par l'utilisateur. |
| `{base}/filtration_end` | `HH:MM` (ex. `20:00`) | Heure de fin du créneau de filtration (feature-051, v2.16.0). Retain. Même sémantique. |

### Éclairage

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/lighting_state` | `ON` / `OFF` | État du relais d'éclairage (contrôle manuel) |

### Boost (feature-053, v2.18.0)

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/boost` | `ON` / `OFF` | État du **Mode Boost** (surchloration temporaire du jour). Retain. `ON` tant que le boost est actif ; retombe automatiquement à `OFF` au prochain minuit local (expiration). Voir [ADR-0025](adr/0025-mode-boost.md). |
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
| `{base}/filtration/set` | `ON` / `OFF` | Force marche/arrêt filtration |
| `{base}/install_mode/set` | `managed` / `powered` / `external` | Change le **mode d'installation** (feature-056, v2.19.0). Valeurs brutes attendues par le handler ; depuis HA le `command_template` retraduit le libellé français choisi. Valeur hors enum → ignorée + resync HA sur l'état réel. Appliqué sous `configMutex`, persisté NVS, republié (état + broadcast WS config). |
| `{base}/filtration_external_state/set` | `ON` / `OFF` | **Signal de filtration externe** (feature-056, v2.19.0). Non retain. Alimente la garde « présence d'eau » du dosage en mode `external` (équivalent de `POST /filtration/external-state`). `ON`/`OFF` insensible à la casse. **Aucune persistance** : au boot, état inconnu → dosage suspendu (fail-safe OFF) ; au-delà de 180 s sans nouveau signal → périmé → dosage suspendu. Voir [ADR-0026](adr/0026-mode-installation.md). |
| `{base}/filtration_mode/set` | `auto` / `manual` / `force` / `off` | Change le mode de filtration (valeurs brutes attendues par le handler ; depuis HA le `command_template` retraduit le libellé français choisi vers ces valeurs) |
| `{base}/filtration_start/set` | `HH:MM` (ex. `07:30`) | Change l'heure de début du créneau (feature-051, v2.16.0). Validée `HH:MM` via `timeStringToMinutes` (format invalide → ignoré + `warning` + resync HA sur la valeur réelle). Efface les overrides `forceOn`/`forceOff`, applique via `filtration.update()`, persisté NVS, republié. **Effet réel en mode Programmation** ; en mode **Auto** l'heure est recalculée par la température au prochain `update()` (saisie écrasée, consultation) |
| `{base}/filtration_end/set` | `HH:MM` (ex. `20:30`) | Change l'heure de fin du créneau (symétrique, feature-051, v2.16.0) |
| `{base}/lighting/set` | `ON` / `OFF` | Allume/éteint l'éclairage (contrôle manuel du relais) |
| `{base}/boost/set` | `ON` / `OFF` | Active / désactive le **Mode Boost** (feature-053, v2.18.0). `ON` → active la surchloration temporaire du jour (filtration forcée + cible/limite chlore effectives relevées, cf. [ADR-0025](adr/0025-mode-boost.md)) ; `OFF` → désactive. **L'activation est refusée si l'heure n'est pas synchronisée** (l'expiration à minuit ne peut être calculée) → aucun changement + resync HA sur l'état réel. Payload invalide ignoré. Expire automatiquement au prochain minuit local. |
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
3. suivie d'un `publishTargetState()` qui **resynchronise Home Assistant** sur le mode **réel** — le select HA revient donc de lui-même sur « Manuelle », il ne reste pas figé sur une valeur fantôme.

La trace n'est **jamais** effacée par MQTT (comportement changé : en v2.20.3, une commande HA acquittait la trace). **Motif** : un select Home Assistant est un clic **distant** qui n'affiche ni la cause de la bascule ni le refus ; pire, une automation ou une simple **restauration d'état HA au redémarrage** rejouerait `automatic` et désarmerait la sécurité **sans témoin**. L'acquittement passe donc obligatoirement par le bouton de la bannière de la [page pH](features/page-ph.md) (`ph_auto_manual_ack`), seul endroit où la cause est affichée. Voir [ADR-0048](adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md).

Entrer ou rester en `manual` reste toujours possible depuis HA.

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
| `ph_guard_scheduled` | **Dosage pH PROGRAMMÉ refusé par une garde (v2.25.0)** : une garde pH bloque alors que le mode de régulation est `scheduled`. Ce mode **ne compte pas ses refus et n'escalade jamais** — une consigne volumétrique choisie par l'utilisateur ne doit pas fermer la régulation. Sans cette alerte, un refus permanent y serait un no-op **silencieux** après la première transition. Réémise à **débit limité** (`kPhGuardScheduledAlertIntervalMs` = **1 h**) tant que la garde tient ; log `WARNING`, non-retained. Message : `Dosage pH programmé refusé — pH sous le plancher de sécurité (6.65 < 6.70). Le volume quotidien ne sera pas injecté tant que la garde tient ; la régulation n'est PAS basculée en manuel dans ce mode. Contrôler le pH au réactif, l'offset et le produit déclaré.` |
| `orp_ph_inhibit` | **Inhibition du dosage chlore sur pH élevé (feature-067, v2.20.0 ; ex-`orp_ph_raw_inhibit`)** : le dosage ORP est suspendu tant que la condition dure. **v2.25.0 — le seuil porte sur le pH CORRIGÉ** (`kPhOrpInhibit` = **7,60**, relâche ≈ 7,55) et non plus sur le brut : la fraction d'acide hypochloreux (HClO, forme active du chlore) dépend du pH **réel** de l'eau. Au-delà, l'ORP mesuré chute à concentration de chlore constante et la régulation ORP compenserait en **surdosant**. Une **butée haute sur le pH BRUT** (**8,10**) s'y ajoute en OU logique, contre un offset négatif saisi à tort qui masquerait une eau réellement haute. Alerte **edge-triggered**, non-retained, log `CRITICAL`. Messages : `pH 7.68 > 7.60 (offset=-0.20) — chlore peu actif à ce pH : dosage ORP suspendu (faire baisser le pH)` ; `pH BRUT 8.14 au-dessus de la butée 8.10 (offset=-0.20) — dosage ORP suspendu quelle que soit la valeur corrigée : offset négatif probablement erroné, contrôler le pH au réactif`. Le correctif est de **faire baisser le pH**, pas de relever la consigne ORP. **Anti-rafale (v2.25.0)** : une seule alerte **par gravité et par épisode** (butée brute > seuil corrigé > mesure absente), jamais de réémission en dégradation — sans quoi un bassin oscillant sur un seuil produisait deux messages contradictoires en alternance. Quand la mesure est retombée dans la **bande de relâche**, le message le dit (« maintenu dans la bande d'hystérésis ») plutôt que d'affirmer un dépassement ; et l'offset n'est mis en cause que s'il est réellement **négatif**. ⚠️ Le cas « pH **indisponible** » (sonde muette / filtre non amorcé) suspend **aussi** le dosage chlore **automatique** (fail-closed) mais est journalisé en `INFO` **sans alerte MQTT** — situation transitoire normale au démarrage ; le motif « mesures non représentatives » (purge de la chambre) en est distingué. Le dosage **programmé** reste aveugle à la mesure, par conception. |
| `ph_filter_not_ready` / `orp_filter_not_ready` | **Filtre de mesure bloqué (v2.20.3)** : le filtre est resté **non prêt plus de 5 min** (`kSensorFilterNotReadyAlarmMs`) alors que la filtration tournait **et** que les mesures étaient déclarées représentatives → il n'y a plus **aucune** régulation sur ce paramètre (et plus de chloration non plus si c'est le pH qui est bloqué, par fail-closed). Publiée à la **pose** avec un log `CRITICAL`, **répétée toutes les 30 min** (`kSensorFilterNotReadyRepeatMs`) tant que la panne dure ; la **levée** est journalisée en `INFO`, sans alerte. Message : `Filtre pH NON PRÊT depuis 7 min alors que la filtration tourne et que les mesures sont représentatives — régulation pH À L'ARRÊT (cause probable : fenêtre de mesure non remplie, rejets=255). Vérifier la sonde ; réinitialisation du filtre possible depuis l'interface.` La purge de la chambre de mesure et la circulation arrêtée ne l'arment **jamais**. L'état permanent pour HA est porté par le binary_sensor `{base}/ph_filter_stuck` (retain). Voir [`docs/subsystems/sensors.md`](subsystems/sensors.md#alarme--filtre-non-prêt-durablement--v2203). |
| `ph_filter_rearm_denied` / `orp_filter_rearm_denied` | **Ré-amorçage de filtre REFUSÉ (v2.20.3)** : la sortie de secours du filtre (12 rejets cohérents) a été **bloquée par une garde de sécurité** — cooldown de 30 min non écoulé, ou valeur hors bande admissible (pH brut [6,50 ; 8,20] / ORP [300 ; 950]). Le filtre **reste** non prêt, donc la régulation reste à l'arrêt : c'est le signal d'une sonde probablement défaillante, pas d'un échelon légitime. Alerte **edge-triggered** (une fois par épisode, le motif de refus étant collant), log `CRITICAL`. Message : `Filtre pH : ré-amorçage REFUSÉ (valeur hors bande admissible) sur brut 4.793 — le filtre reste NON PRÊT, régulation pH à l'arrêt : vérifier la sonde`. Voir [`docs/subsystems/sensors.md`](subsystems/sensors.md#sortie-de-secours-sur-rejets-cohérents-v2203). |
| `ph_dose_sensor_fault` / `orp_dose_sensor_fault` | **PANNE DE MESURE bloquant le dosage (v2.25.1)** : la cause de refus courante du dosage est une **panne** de la chaîne de mesure — **sonde figée** (`DoseRefusal::FilterFrozen` / `ReadingNaNFrozen`) ou **plus aucune mesure acceptée** (`FilterStale` / `ReadingNaNStale`) — et cet état dure **sans interruption** depuis `kDoseSensorFaultAlertDelayMs` = **60 s** (12 cycles de lecture EZO). La régulation de ce paramètre est à l'arrêt et, avant la v2.25.1, rien ne le signalait hors de l'interface web. Alerte **une seule fois par épisode**, log `warning` (émis par `publishAlert()`, donc présent au journal même broker déconnecté), non-retained. Message : `[Dosage pH] Refus : sonde figée (valeur constante) — filtre non prêt — régulation pH à l'arrêt depuis 65 s : vérifier la sonde`. **Pourquoi 60 s et pas immédiatement** : à la réouverture de la porte d'alimentation du filtre (reprise de circulation, fin de purge) le filtre est ré-amorcé et son état interne est légitimement « périmé » pendant au plus un cycle de lecture — sans temporisation, chaque démarrage de filtration aurait produit une alerte « sonde muette », un cri au loup quotidien. L'**affichage**, lui, montre la cause **sans délai**. **L'épisode n'est PAS redaté quand la panne change de nature** (`Frozen` ↔ `Stale`) : un EZO agonisant alternant les deux par phases courtes n'aurait jamais déclenché l'alerte. Les états **normaux** (fenêtre de lissage en cours, mesures non représentatives) n'émettent **jamais** sur ce type. Complémentaire de `ph_filter_not_ready` (seuil 5 min, conditionné à la filtration en marche et aux mesures représentatives) : cette alerte-ci qualifie la **nature** de la panne. Voir [ADR-0049](adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md) et [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md#alerte-de-panne-de-mesure-v2251). |
| `ph_integral_saturated` | **Régulation pH saturée sans atteindre la consigne (v2.26.0, [ADR-0052](adr/0052-terme-integral-ph-dimensionne-par-autorite.md))** : le correcteur demande **son maximum** et l'erreur reste **au-dessus du seuil d'arrêt**. Le prédicat porte sur le rapport **demande PID / débit moyen soutenable** (`ampFlow × budget_horaire / 60`), **pas** sur le remplissage de `integralMax` — le gel d'anti-windup immobilise l'intégrale bien en dessous du plafond (~34 % avec 5 min/h), un seuil sur le plafond n'aurait **jamais** été atteint. Hystérésis **95 % / 85 %**. La tenue se compte en **temps de branche automatique** (un passage en `manual`, une garde bloquante ou un hiver sans filtration **suspendent** l'épisode au lieu de le faire mûrir). **Deux paliers, un seul type d'alerte** — la gravité est portée par le message et le niveau de journal : `warning` après **2 h**, `critical` après **6 h** (= une constante d'intégration entière à autorité pleine). Le `critical` est **RÉPÉTÉ toutes les 12 h** tant que l'anomalie dure : un latch « une alerte par épisode » laissait un silence **permanent** sur un état qui, lui, ne cessait pas. Causes physiques typiques : **bidon vide, tube désamorcé, fuite du circuit de dosage, sonde figée, limite horaire trop basse**. Message `critical` (tronqué à 128 o sur le bus, intégral au journal) : `Régulation pH saturée depuis plus de 6 h (2.10 mL/min demandés pour 2.15 mL/min soutenables, terme intégral 2.05 mL/min) — la régulation demande son maximum SANS atteindre la consigne : vérifier le niveau et l'amorçage du bidon, l'étanchéité du circuit de dosage, la sonde pH et la limite horaire d'injection`. La **sortie d'hystérésis** est journalisée en `info`, sans alerte. Aucun équivalent ORP (`Ki_ORP` reste 0). |
| `measures_purge_timeout` | **Purge des mesures ouverte au PLAFOND (feature-091, v2.34.0, [ADR-0059](adr/0059-purge-adaptative-fenetre-de-medianes.md))** : à la reprise de la circulation, les bruts pH/ORP n'ont **jamais atteint la stabilité** avant `kPurgeCapMs` = **90 min** — la porte de validité s'ouvre quand même (fail-safe de disponibilité : la régulation reprend sous les gardes habituelles) et l'anomalie est signalée. Émise **une fois par purge concernée** (à la fin de purge, `PurgeEnd::kCap`), non-retained, log `WARNING` préfixé `[STAB]`. Message : `Purge des mesures ouverte au PLAFOND (90 min 0 s) sans stabilité (pH 5.970→7.310 ; ORP 512.0→604.0 mV) — sonde bruyante ou transitoire long ; régulation reprise, à surveiller` — les étendues sont les **bruts min→max** observés pendant la purge (`n/a` si une voie n'a jamais été lue). ⚠️ Le message dépasse `kMaxPayloadLen` : il sort **raccourci** sur le bus (`... (voir journal)`), texte intégral dans le journal système. Causes typiques : sonde encrassée ou bulleuse, chambre de mesure mal purgée, injection en amont pendant la purge. Après le plafond, la surveillance de re-fermeture est **désarmée** jusqu'à la prochaine reprise d'eau. Aucune sortie **normale** (stabilité, dispense, fin de re-purge) n'émet sur ce type — elles restent en `INFO`. Côté web, l'information est portée par `measures_purge_last_end = 2` ([`/data`](API.md#get-data--write) + WS) et les bandeaux « Stabilisation des mesures non atteinte » des pages pH et ORP. Voir [`docs/subsystems/measure-validity.md`](subsystems/measure-validity.md). |
| `oom_reserve_absent` | **Parachute OOM sans réserve (bug-080 lot B-1, v2.35.0, [ADR-0061](adr/0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md))** : **aucun** palier de réserve — ni nominal (12 288 o) ni de repli (1 024 o) — n'a pu être armé depuis `kOomNoReserveCriticalDelayMs` = **5 min**, soit **30 tentatives** de réarmement consécutives (`kOomRearmCheckIntervalMs` = 10 s). Il n'y a alors **plus aucun filet anti-`bad_alloc`** : c'est exactement le régime des **heures** de vol à nu des 13 et 15/08/2026, qu'aucun signal ne dénonçait. Alerte **one-shot par épisode** (le compteur repart dès qu'une réserve est de nouveau armée), non-retained, log **`CRITICAL`** : `Parachute OOM SANS RÉSERVE depuis 5 min (aucun palier armable) — plus aucun filet anti-bad_alloc, maxBloc8=<n> o, libre8=<n> o`. Message publié : `Parachute OOM sans réserve depuis plusieurs minutes — plus aucun filet anti-bad_alloc ; voir le journal pour les valeurs de tas`. ⚠️ **Point d'architecture** : `oom_guard` **ne publie PAS lui-même** — il lève un **drapeau**, drainé par `mqtt_manager` **depuis sa propre tâche** (`oomGuardConsumeNoReserveAlert()`). Motifs : `publishAlert()` **alloue** (String + JsonDocument) au moment précis où le tas vient de refuser 1 024 o trente fois de suite, il ré-écrirait le **même** message une **seconde** fois dans un journal de 8 Ko dont la rétention est de 25-35 min (fenêtre forensique divisée par deux), et `oom_guard` est une **feuille** armée avant tout le reste au `setup()` : la faire dépendre du sous-système applicatif le plus haut inverserait les couches. **Conséquence assumée : si MQTT est désactivé ou déconnecté, l'alerte est PERDUE** — le `critical()` du journal reste la **source de vérité**. L'état instantané est lisible à tout moment via `oom_reserve_bytes` / `oom_reserve_internal` de [`/get-system-info`](API.md#get-get-system-info--write). Voir [`docs/subsystems/oom-guard.md`](subsystems/oom-guard.md#politique-dallocation-par-défaut--bascule-psram-v2350). |
| `pump_unusable` | **Pompe doseuse hors service (feature-064, v2.16.0 — exigence pool-chemistry P1, porte de bascule [ADR-0034](adr/0034-migration-core3-idf5.md))** : une pompe a échoué son attache PWM au boot (`ledcAttachChannel`, **core Arduino 3 uniquement** — donc **réellement armée en production depuis la v2.20.4**, le core 3 étant l'environnement par défaut, [ADR-0042](adr/0042-core3-environnement-par-defaut.md) ; dormante en repli core 2) et a été mise en sécurité fail-closed (`pumpUsable`, feature-063) : **aucune injection possible jusqu'au reboot**. Une alerte **par pompe** inutilisable, **re-publiée à CHAQUE (re)connexion MQTT** (arbitrage Q1, décision pool-chemistry P1) et non pas one-shot au boot : l'alerte n'étant **pas retained** et l'état étant **permanent jusqu'au reboot**, un broker absent au boot ne la perd plus — la 1ʳᵉ connexion couvre le boot, les reconnexions couvrent le reste. Limitation résiduelle assumée : un HA qui redémarre **sans** reconnexion de l'ESP32 ne revoit pas l'alerte. Message : `Pompe 1 (pH) HORS SERVICE : échec init PWM (GPIO=25, canal LEDC=0) — aucune injection possible jusqu'au reboot` (idem `Pompe 2 (Chlore/ORP)`, GPIO=33, canal 1). Voir [`docs/subsystems/pump-controller.md`](subsystems/pump-controller.md) et [`docs/subsystems/mqtt-manager.md`](subsystems/mqtt-manager.md). |

> ℹ️ **v2.35.0 (bug-080 lot B-1)** — le type `oom_reserve_absent` est **ajouté** à la liste ci-dessus. Il transite par le topic d'alertes générique `{base}/alerts` et **ne crée AUCUNE entité d'auto-discovery Home Assistant** : l'auto-discovery existante est **strictement inchangée** par cette version. Une automation HA qui veut réagir doit filtrer sur `type == "oom_reserve_absent"` dans la charge utile de `{base}/alerts`. Aucun réglage ni topic MQTT n'est par ailleurs modifié par ce lot.

> ℹ️ **v2.34.0 (feature-091)** — le type `measures_purge_timeout` est **ajouté** à la liste ci-dessus. Il transite par le topic d'alertes générique `{base}/alerts` et **ne crée AUCUNE entité d'auto-discovery Home Assistant** : ni capteur d'état de purge, ni `binary_sensor` dédié — l'auto-discovery existante est **strictement inchangée** par cette version. Une automation HA qui veut réagir doit filtrer sur `type == "measures_purge_timeout"` dans la charge utile de `{base}/alerts` (patron des autres types du tableau). Aucun réglage MQTT n'a par ailleurs disparu : `stabilization_delay_min` n'a jamais été exposé sur le bus.

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

Topic : `{base}/diagnostic` — publié au démarrage et à chaque reconnexion.

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
  "firmware_version": "1.0.3"
}
```

---

## Home Assistant Auto-Discovery

Le contrôleur publie automatiquement sa configuration pour Home Assistant au démarrage.

**Préfixe discovery :** `homeassistant/`
**Device ID :** `poolcontroller`

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

> **Compatibilité `orp_enabled` :** le champ `orp_enabled` (booléen) est maintenu comme miroir de `orp_regulation_mode` (`true` si mode ≠ `manual`). Les automations Home Assistant qui testent la valeur de ce champ continuent de fonctionner sans modification. Le topic `{base}/orp_regulation_mode` est la source de vérité pour le mode actif.

> **Libellés du select « Mode Filtration » (bug-ha-filtration-mode-labels, v2.15.0) :** dans Home Assistant, ce select expose les libellés français **Auto / Programmation / Manuel / Désactivé** (alignés sur l'UI web), et non plus les valeurs brutes anglaises. La traduction est purement côté HA via templates de la discovery : `value_template` mappe l'état brut publié (`auto`/`manual`/`force`/`off`) vers le libellé affiché (avec `.get(value, value)` → un état inattendu passe sans casser), `command_template` mappe le libellé choisi vers la valeur brute envoyée sur `.../set`. **Le protocole MQTT sur le fil reste strictement `auto`/`manual`/`force`/`off`** (état publié comme commande attendue par `drainCommandQueue`) : aucune automatisation existante n'est cassée. Levée d'ambiguïté : `manual` = « Programmation » (créneau à heures fixées), `force` = « Manuel » (contrôle ON/OFF sans planning). Les selects `ph`/`orp_regulation_mode` affichent depuis v2.17.3 les libellés français **Automatique / Programmée / Manuelle** via la même technique (le fil reste `automatic`/`scheduled`/`manual`, voir bug-ha-regulation-mode-labels ci-dessous).

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

Lissage logiciel des mesures (médiane 7 + EMA). Le topic `{base}/ph` / `{base}/orp` publie désormais la valeur **filtrée** (cf. tableau Capteurs ci-dessus) ; les topics ci-dessous exposent en plus le brut, la médiane et l'état du filtre pour diagnostic EMI. Tous **retain**.

| Topic | Payload | Description |
|-------|---------|-------------|
| `{base}/ph_raw` | `7.46` | Dernière mesure pH **brute** Atlas, **sans offset** (diagnostic EMI, 3 décimales) |
| `{base}/ph_median` | `7.43` | Médiane glissante pH — **corrigée** depuis 2.20.0 |
| `{base}/ph_filtered` | `7.43` | Mesure pH **filtrée** (EMA) — **corrigée** depuis 2.20.0, valeur de régulation |
| `{base}/ph_filter_ready` | `ON`/`OFF` | Filtre pH prêt (warmup terminé + mesure récente) |
| `{base}/ph_filter_unstable` | `ON`/`OFF` | Capteur pH instable (trop de rejets consécutifs) |
| `{base}/ph_rejected_count` | `3` | Nombre de mesures pH rejetées (glissant, 0..255) |
| `{base}/orp_raw` | `690` | Dernière mesure ORP **brute** Atlas (mV) |
| `{base}/orp_median` | `682` | Médiane glissante ORP (mV) |
| `{base}/orp_filtered` | `682` | Mesure ORP **filtrée** (mV) — valeur de régulation |
| `{base}/orp_filter_ready` | `ON`/`OFF` | Filtre ORP prêt |
| `{base}/orp_filter_unstable` | `ON`/`OFF` | Capteur ORP instable |
| `{base}/orp_rejected_count` | `2` | Nombre de mesures ORP rejetées |
| `{base}/ph_mixing_delay_active` | `ON`/`OFF` | Pause mélange pH active (15 min après injection) |
| `{base}/orp_mixing_delay_active` | `ON`/`OFF` | Pause mélange ORP active (20 min après injection) |

### Auto-discovery HA (feature-025)

| Entité | Type | Topic d'état | `unique_id` | Détails |
|--------|------|--------------|-------------|---------|
| Piscine pH Brut | `sensor` | `{base}/ph_raw` | `poolcontroller_ph_raw` | `unit: pH`, `icon: mdi:water-outline`, `state_class: measurement` |
| Piscine pH Filtré | `sensor` | `{base}/ph_filtered` | `poolcontroller_ph_filtered` | `unit: pH`, `icon: mdi:water-check`, `state_class: measurement` |
| Piscine ORP Brut | `sensor` | `{base}/orp_raw` | `poolcontroller_orp_raw` | `unit: mV`, `icon: mdi:flash-outline`, `state_class: measurement` |
| Piscine ORP Filtré | `sensor` | `{base}/orp_filtered` | `poolcontroller_orp_filtered` | `unit: mV`, `icon: mdi:flash-alert`, `state_class: measurement` |
| Piscine Filtre pH Prêt | `binary_sensor` | `{base}/ph_filter_ready` | `poolcontroller_ph_filter_ready` | `payload_on: ON`, `payload_off: OFF`, `icon: mdi:filter-check` |
| Piscine Filtre ORP Prêt | `binary_sensor` | `{base}/orp_filter_ready` | `poolcontroller_orp_filter_ready` | `payload_on: ON`, `payload_off: OFF`, `icon: mdi:filter-check` |

> Le préfixe réel `unique_id` est `HA_DEVICE_ID` (`poolcontroller`). Les topics `median`, `filter_unstable`, `rejected_count` et `mixing_delay_active` sont publiés en retain mais **sans** entité auto-discovery dédiée (l'utilisateur peut créer des sensors HA manuels s'il le souhaite). Voir [`src/mqtt_manager.cpp`](../src/mqtt_manager.cpp) et [`docs/subsystems/sensors.md`](subsystems/sensors.md#filtrage-des-mesures-phorp--feature-025).

## Entités ajoutées en feature-009 (modes de régulation commandables, v2.7.0)

Deux entités HA `select` permettent de changer le mode de régulation pH / ORP depuis Home Assistant (automatisation « hivernage », détection d'absence, dashboard HA, etc.) :

| Entité | Discovery topic | `state_topic` | `command_topic` | Détails |
|--------|-----------------|---------------|-----------------|---------|
| Mode Régulation pH | `homeassistant/select/poolcontroller_ph_regulation_mode/config` | `{base}/ph_regulation_mode` | `{base}/ph_regulation_mode/set` | `unique_id: poolcontroller_ph_regulation_mode`, `icon: mdi:ph`, `options: ["Automatique","Programmée","Manuelle"]` + templates (voir note ci-dessous) |
| Mode Régulation ORP | `homeassistant/select/poolcontroller_orp_regulation_mode/config` | `{base}/orp_regulation_mode` | `{base}/orp_regulation_mode/set` | `unique_id: poolcontroller_orp_regulation_mode`, `icon: mdi:flash`, mêmes options + templates |

> **Libellés français des selects Mode Régulation pH/ORP (bug-ha-regulation-mode-labels, v2.17.3) :** dans Home Assistant, ces deux selects exposent les libellés français **Automatique / Programmée / Manuelle** (libellés exacts de l'UI web), et non plus les valeurs brutes anglaises. Même technique que les selects « Mode Filtration » et « Mode Éclairage » : `value_template` (`{'automatic':'Automatique','scheduled':'Programmée','manual':'Manuelle'}.get(value, value)`) mappe l'état brut publié vers le libellé, `command_template` (`{'Automatique':'automatic','Programmée':'scheduled','Manuelle':'manual'}[value]`) retraduit le libellé choisi vers la valeur brute envoyée sur `.../set`. **Le protocole MQTT sur le fil reste strictement `automatic` / `scheduled` / `manual`** (topics d'état et handlers `PhRegulationMode` / `OrpRegulationMode` inchangés) : aucune automatisation Home Assistant existante n'est cassée.

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

Un capteur figé (30 lectures acceptées dans une bande < ½ LSB, ≈ 2,5 min) rend le filtre non prêt → **dosage automatique bloqué fail-closed** (garde `FilterNotReady` existante). La **température figée** (détection warning-only, aucun impact dosage) n'apparaît **ni** dans cette alerte **ni** dans les états `*_sensor_problem`. Détails firmware : [`docs/subsystems/sensors.md`](subsystems/sensors.md#détection-capteur-figé--feature-022).

## Topics et entités ajoutés en v2.20.3 (filtre de mesure bloqué)

Un filtre pH ou ORP durablement non prêt = plus **aucune** régulation sur ce paramètre. L'incident du 2026-08-16 a duré ≥ 3 h sans qu'aucun signal ne parte : ces deux topics comblent ce trou d'observabilité.

### `{base}/ph_filter_stuck` / `{base}/orp_filter_stuck`

**Direction** : ESP32 → broker (**retain = true**)

| Topic | Payload | Condition |
|-------|---------|-----------|
| `{base}/ph_filter_stuck` | `ON` / `OFF` | `ON` si le filtre pH est resté **non prêt > 5 min** (`kSensorFilterNotReadyAlarmMs`) **alors que** la filtration tournait **et** que les mesures étaient déclarées représentatives. `OFF` sinon. |
| `{base}/orp_filter_stuck` | `ON` / `OFF` | Symétrique ORP. |

- Miroir exact des champs `phFilterAlarm` / `orpFilterAlarm` de `GET /data` et du WebSocket (`sensors.isPhFilterNotReadyAlarm()`).
- ⚠️ **Ne pas confondre** avec `{base}/ph_filter_ready` (feature-025), qui bascule à `OFF` à **chaque** warmup ou purge, donc plusieurs fois par jour en fonctionnement normal. `ph_filter_stuck` ne s'arme que sur une **anomalie durable** : purge de la chambre de mesure, warmup et filtration à l'arrêt ne le déclenchent **jamais**. C'est ce topic, et pas `filter_ready`, qu'il faut câbler dans une automation d'alerte.
- Publication **dédoublonnée** (`safePublishDedup`, slots 16/17) : émis au changement d'état seulement, première publication forcée après connexion. Retain → HA retrouve l'état après un reboot du contrôleur ou de HA.
- Reprise : `POST /reset-sensor-filter?sensor=ph|orp` (interface web) — volontairement **non exposé** en MQTT, cf. [Exclusions volontaires](#exclusions-volontaires-politique-mqttha).

### Auto-discovery HA (v2.20.3)

| Entité | Type | Topic d'état | `unique_id` | Détails |
|--------|------|--------------|-------------|---------|
| Piscine Filtre pH Bloqué | `binary_sensor` | `{base}/ph_filter_stuck` | `poolcontroller_ph_filter_stuck` | `device_class: problem`, `payload_on: ON`, `payload_off: OFF`, `icon: mdi:filter-remove` |
| Piscine Filtre ORP Bloqué | `binary_sensor` | `{base}/orp_filter_stuck` | `poolcontroller_orp_filter_stuck` | `device_class: problem`, mêmes payloads/icône |

Publié à `homeassistant/binary_sensor/poolcontroller_ph_filter_stuck/config` (resp. `_orp_…`) au démarrage. **Entités nouvelles** : l'auto-discovery des entités existantes est inchangée. Alertes JSON associées sur `{base}/alerts` : `ph_filter_not_ready` / `orp_filter_not_ready` et `ph_filter_rearm_denied` / `orp_filter_rearm_denied` (cf. [Alertes](#alertes)).

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
| **Transients d'affichage** : état de la purge des mesures (`measures_stab_elapsed_s`, `measures_purge_last_*`), `inject_remaining`, **cause de blocage dosage** | États de transition à durée de vie très courte, pertinents seulement pour l'UI temps réel (WebSocket + `/data`). La cause de blocage dosage a été explicitement écartée en feature-022, sur besoin constaté (bruit pour HA, pas de valeur d'automatisation). **v2.34.0** : seule l'**anomalie** de purge remonte, sous forme d'alerte `measures_purge_timeout` — aucune entité d'auto-discovery n'est créée pour l'état courant de la purge. |
| **Réarmement d'un filtre de mesure** (`POST /reset-sensor-filter`, v2.20.3) | Action **destructrice pour le diagnostic** : elle efface la détection de sonde figée et le latch EMI. Exposée en MQTT, une automation pourrait la déclencher en boucle et masquer indéfiniment une sonde défaillante. L'**état** est publié (`{base}/ph_filter_stuck`), la **commande** reste locale, sous auth administrateur et cooldown de 10 min. |
| **Historique** (brut / horaire / quotidien) | Home Assistant dispose de son propre recorder / long-term statistics qui agrège les `state_class: measurement` déjà publiés. Dupliquer l'historique firmware vers MQTT serait redondant. |
