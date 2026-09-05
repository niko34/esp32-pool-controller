# Page pH — `/ph`

- **Fichier UI** : [`data/index.html`](../../data/index.html) (section `#view-ph`)
- **URL** : `http://poolcontroller.local/#/ph`

## Rôle

Point d'entrée unique pour **tout** ce qui concerne le pH : mesure, mode de régulation, injection manuelle, historique, calibration.

## Structure

En mode nominal, cinq zones :

1. **Bloc Statistiques** (bandeau compact, sans titre) — valeur pH **filtrée** courante en grand (1 décimale, cohérent avec le tableau de bord) + **ligne brute discrète** « brut · médiane · maj » + **rangée de chips** : chip d'état filtre cliquable (cf. [Chip d'état filtre](#chip-détat-filtre-feature-025)) et **chip unique sonde + calibration** cliquable (cf. [Chip d'état sonde](#chip-détat-sonde-feature-024)) + dosage du jour (barre de progression). Depuis feature-034, le chip de calibration distinct est **supprimé** : son information est fusionnée dans le chip sonde.
2. **Carte Régulation pH** — sélecteur de mode : `Automatique`, `Programmée`, `Manuelle`. Sous-blocs conditionnels :
   - **Automatique** : pH cible — **contrôle segmenté 4 positions** `#ph_target_group` depuis la v2.25.0 (7,20 / 7,30 / 7,40 / 7,50), avec hint de plage `#ph_target_hint`. Voir [Consigne pH : contrôle segmenté](#consigne-ph--contrôle-segmenté-v2250) et [Règles firmware appliquées](#règles-firmware-appliquées).
   - **Programmée** : volume quotidien en mL (borné par `max_ph_ml_per_day`) + bouton Sauvegarder. Depuis la v2.8.0 (feature-011) : ligne « **Débit calculé : X,X mL/min** » (`#ph_scheduled_flow_line` / `#ph_scheduled_flow_value`, alimentée par le champ WS `ph_scheduled_flow_ml_per_min` ; affiche « — » avec `title` « Hors plage de filtration ou données indisponibles » quand la valeur est `null`) + hint statique « Le volume non injecté avant minuit est perdu (pas de report au lendemain). ». Volontairement **pas** de « sur Y h restantes » côté client : l'horloge du navigateur peut différer de celle de l'ESP32, seul le débit (source firmware) est affiché.
   - **Manuelle** : bloc Injection manuelle (volume + bouton ▶ Injecter / ⏹ Stopper).
   - **Pied de carte, mode Automatique uniquement** : bloc « **Rattrapage accumulé** » (`#ph-integral-section`, v2.26.0) — voir [Rattrapage accumulé par la régulation pH](#rattrapage-accumulé-par-la-régulation-ph-v2260).
3. **Carte Correction de la mesure pH** (`#ph-card-offset`, feature-067, v2.20.0) — offset logiciel appliqué à la mesure. Voir [Carte « Correction de la mesure pH »](#carte--correction-de-la-mesure-ph--feature-067-v2200).
4. **Carte Historique** — graphique uPlot (feature-043, ex-Chart.js), plages `Tout` / `30j` / `7j` / `24h`. Depuis la **v2.28.1** (feature-083) : bande fixe **7,2-7,6** (plage idéale de l'eau) + **ligne de consigne pointillée** à `ph_target` — voir [Repères du graphique d'historique](#repères-du-graphique-dhistorique-feature-083-v2281).
5. **Carte Calibration** — **remplace** Régulation + Historique pendant une session de calibration. Écran **guidé par stepper** (feature-034) — voir [Workflow calibration](#workflow-calibration).

Le bloc Statistiques reste visible **en permanence**, y compris pendant la calibration.

> **Calibration accessible dans tous les modes (feature-034)** : le bouton « Calibrer la sonde » (`#ph_cal_trigger_btn`) est désormais affiché et fonctionnel quel que soit le mode de régulation — **automatique, programmée et manuelle**. Depuis l'itération 3, ce bouton est placé **sous la rangée de chips** du bloc Statistiques (et non plus en bas de la carte de régulation dans `#ph-calibration-info`, supprimé). La session de calibration ne modifie pas `ph_regulation_mode` : à la fermeture (`#ph_cal_close_btn`), on **revient au mode précédemment sélectionné** (aucune bascule forcée en automatique).

## Badge d'état mesure (`#ph-page-badge`, feature-061, v2.13.0)

Sous le bouton « Calibrer la sonde » du bloc Statistiques, un badge (`role="status"`, masqué par défaut) affiche l'**état de validité de la mesure** quand l'eau ne circule pas ou vient de reprendre. Il est alimenté par les **seuls segments d'état mesure** (`measureStateSegments('ph', …)`, app.js — pas les segments dosage/stock, déjà portés ailleurs sur la page) :

| Condition | Libellé exact | Variant |
|---|---|---|
| `water_present === false` (arrêt franc) | `Filtration arrêtée` | `info` (bleu — état normal, attendre) |
| `water_present === false` + `filtration_state_stale` | `Signal filtration périmé` | `warning` (ambré — anomalie du signal externe) |
| `water_present === true` + `measures_valid === false` | **v2.34.0** : `Stabilisation depuis N min` — ou `Re-stabilisation depuis N min` si `measures_purge_count ≥ 1` (`depuis < 1 min` sous 60 s ; `Stabilisation en cours` si le champ manque). Avant : `Stabilisation : ~N min` | `info` (bleu) |

> **v2.34.0 (feature-091)** : la purge se terminant sur la **stabilité des mesures**, il n'y a plus de fin datée à l'avance — le badge affiche le temps **écoulé** (`measures_stab_elapsed_s`) et non plus un restant. Le **plafond atteint sans stabilité** n'apparaît **pas** ici (il ouvre la porte) : il est signalé par le bandeau ci-dessous.

Pendant ces états, la **valeur pH « actuelle » affiche `--`** (le badge explique pourquoi) et les points ne sont **plus historisés** (trous dans le graphique). Les lectures live de la **calibration** et de la ligne brute `#ph-filter-sub` ne sont **pas** gatées (diagnostic / sonde hors de l'eau). Détails et rétrocompat : [page-dashboard.md](page-dashboard.md#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130) ; côté firmware : [measure-validity.md](../subsystems/measure-validity.md).

## Affichage mesure filtrée / brute (feature-025)

La valeur affichée **en grand** dans le bloc Statistiques est la mesure **filtrée** (`phFiltered`, médiane + EMA). C'est aussi la valeur utilisée par la régulation. La fonction `_renderFilterSub('ph', json)` ([`data/app.js`](../../data/app.js)) rend une **ligne discrète** `#ph-filter-sub` :

```
brut 7.24 · médiane 7.24 · maj à l'instant
```

- **brut** = `phRaw` (dernière mesure Atlas non filtrée, **sans offset** — diagnostic EMI) ;
- **médiane** = `phMedian` (médiane glissante, **corrigée** depuis la v2.20.0) ;
- **maj** = âge de la dernière mesure brute valide (`à l'instant` / `il y a N s/min`).

> **feature-067 (v2.20.0)** : la valeur affichée en grand (`ph`), la médiane et la valeur filtrée intègrent l'**offset logiciel** `ph_offset` ; seul `phRaw` reste brut. Voir [Carte « Correction de la mesure pH »](#carte--correction-de-la-mesure-ph--feature-067-v2200).

`--` est affiché si la valeur est `null` (filtre non amorcé / EZO injoignable).

## Repères du graphique d'historique (feature-083, v2.28.1)

Le graphique de détail (carte Historique) porte deux repères **distincts** :

- **Bande fixe 7,2 – 7,6** (lignes rouges + zones ombrées hors plage) : la **plage idéale de l'eau** selon la littérature (poolplanet — optimum 7,4 ; sous 7,2 : irritation, corrosion ; au-dessus de 7,6 : calcaire, chlore moins efficace). C'est la **seule** borne de graphique codée en dur côté client (`PH_IDEAL_MIN` / `PH_IDEAL_MAX`, commentées avec leur référence dans [`data/app.js`](../../data/app.js)) — **Option B** de la spec 083 ; l'option « bornes servies par le firmware » est écartée pour ce cycle, reportable dans une spec future. L'ancienne bande **7,0-7,4** ne correspondait à aucune référence.
- **Ligne de consigne pointillée** (bleue) à `ph_target` : la valeur **configurée**, dynamique.

> ⚠️ **Trois plages différentes, et elles le restent** (arbitrage du 30/08/2026, consigné dans la spec — ne pas les unifier) :
>
> | | Valeur | Sens |
> |---|---|---|
> | Bande du graphique | **7,2 – 7,6** | plage idéale de l'eau (littérature) |
> | Plage de consigne réglable | **7,20 – 7,50** | ce que l'appareil autorise à viser |
> | Seuils de garde | **6,70 / 7,60** | où le dosage est refusé |
>
> La consigne est **délibérément plus resserrée** que la plage idéale : elle laisse une marge avant l'inhibition du chlore à 7,60. Caler la bande sur la plage de consigne (première version de la demande, écartée) ferait apparaître un pH 7,10 « hors plage » alors qu'il est acceptable.

**Mise à jour sans rechargement** : la ligne de consigne est posée/mise à jour par `loadConfig()` (`phChart?.setTarget(cfg.ph_target)`) — point de passage **unique** couvrant l'UI, l'API et MQTT : chaque sauvegarde recharge `/get-config`, et les trames WS `config` passent aussi par `loadConfig`. La valeur est **normalisée à 2 décimales** avant comparaison (`/get-config` sert un arrondi, le WS le float brut — sans normalisation, chaque trame provoquerait un re-render cosmétique).

**Repli** : consigne jamais servie (firmware antérieur, clé absente) → **aucune ligne tracée**, jamais de valeur inventée (piège maison : clé absente ≠ 0). Une valeur **absente ou non finie** dans une trame est ignorée : une trame partielle ne fait **jamais disparaître** une ligne existante. Les bornes d'axe Y (`calculateAxisLimits`) incluent la consigne en plus des données : la ligne reste toujours visible dans le cadre.

## Rattrapage accumulé par la régulation pH (v2.26.0)

Bloc `#ph-integral-section`, en **pied de la carte Régulation pH**, **visible en mode Automatique uniquement**. Il expose le **terme intégral** introduit par la [levée de `Ki = 0`](../subsystems/pump-controller.md#terme-intégral-ph--levée-de-ki--0-v2260) ([ADR-0052](../adr/0052-terme-integral-ph-dimensionne-par-autorite.md)).

**Contenu**

```
La régulation met de côté une correction lente pour compenser une dérive
permanente du pH (dégazage du CO₂, apport d'eau neuve). Cette réserve se
vide d'elle-même quand le pH rejoint sa consigne.

Rattrapage accumulé : 46 %   (7 450 pH·s · 2,8 mL/min demandés)
```

| Élément | Id | Source | Format |
|---|---|---|---|
| Valeur de tête | `#ph_integral_pct_value` | `ph_integral_pct` | entier + « % », `—` si `null` |
| Détail discret | `#ph_integral_detail` | `ph_integral` et `ph_integral_term` | `(N pH·s · X,X mL/min demandés)`, **valeurs absolues** |
| Encart d'avertissement | `#ph_integral_warning` | dérivé de `ph_integral_pct` | `callout--warn`, `role="status"`, masqué hors saturation |

**Règles d'affichage** (point de décision unique : `updatePhIntegralDisplay()`, appelée depuis `updateDetailSections()` et depuis le changement de mode) :

| Condition | Comportement |
|---|---|
| `ph_integral_pct` **absent** (firmware < 2.26.0) | **Bloc entier masqué.** Afficher « 0 % » mentirait sur une régulation dont on ne sait rien. |
| Mode de régulation ≠ `automatic` | **Bloc masqué** : le rattrapage n'évolue que là ; ailleurs on n'afficherait qu'un reliquat figé, sans lecture possible. |
| Valeur `null` | « — » — le firmware dit explicitement « non calculable » (terme intégral inactif). **Jamais un zéro.** |
| Nombre fini, `0` compris | Valeur affirmée : `0 %` = **réserve vide**. |
| `ph_integral_pct ≥ 95` | Encart d'avertissement affiché : *« Rattrapage au maximum : la régulation demande tout ce qu'elle peut sans que le pH rejoigne sa consigne. Vérifiez le niveau du bidon, l'amorçage de la pompe doseuse et le TAC de l'eau (une alcalinité élevée bloque le pH). »* |

**Pourquoi le pourcentage en premier** : c'est la seule des trois grandeurs lisible **sans connaître le plafond de l'anti-windup**, lequel **varie** avec la vitesse de correction (43 200 / 16 200 / 7 200 pH·s). Ce plafond n'est donc **jamais recopié côté client** — le firmware sert le rapport déjà calculé. Recalculer un pourcentage dans le navigateur rouvrirait le motif du miroir de constante firmware, éliminé quatre fois dans ce projet.

**Le signe n'est pas affiché** : le détail montre les valeurs **absolues**, le sens de correction étant déjà porté plus haut dans la carte (pH- / pH+).

> ⚠️ **Le seuil `PH_INTEGRAL_SATURATION_PCT = 95` de `app.js` est un seuil de MISE EN FORME**, aligné sur `kPhIntegralSaturationPct`. **L'autorité reste l'alerte firmware** `ph_integral_saturated`, seule à tenir compte de la **durée** (`warning` à 2 h, `critical` à 6 h, rappel 12 h) — l'encart web peut apparaître sur un transitoire de quelques minutes sans qu'aucune alerte ne parte, et c'est voulu.

**Volontairement absent d'ailleurs** : du **tableau de bord** (c'est une grandeur d'automatique, pas une mesure d'eau) et de la page **Réglages** (c'est un **état temps réel**, pas un paramètre à sauvegarder).

### Ligne « Dose de base apprise » (feature-085, v2.28.0)

Ligne `#ph_ff_line`, ajoutée **dans le bloc Rattrapage accumulé** (mêmes règles de visibilité : mode Automatique uniquement). Elle expose le [feedforward pH](../subsystems/pump-controller.md#dose-de-base-apprise--feedforward-ph-v2280) ([ADR-0055](../adr/0055-feedforward-appris-additif-plafonne.md)) : la charge quotidienne connue (médiane 7 j) couverte d'office pendant les sessions automatiques.

| Élément | Id | Source | Format |
|---|---|---|---|
| Valeur | `#ph_ff_value` | `ph_ff_ml_per_min` | `X,XX mL/min` (fr-FR, 2 décimales) — **uniquement** au statut `actif` ; `—` sinon |
| État | `#ph_ff_state` | `ph_ff_status` | libellé discret, voir table |

**Libellés des 5 états** (`ph_ff_status`, contrat firmware figé) :

| Code | Affichage |
|---|---|
| `0` (inactif) | tiret seul, pas de libellé |
| `1` (actif) | valeur + « active » |
| `2` (latch de sécurité) | « suspendue (pH sous consigne) » |
| `3` (historique < 3 j) | « en apprentissage (historique < 3 j) » |
| `4` (quota) | « suspendue (60 % du quota atteint) » |
| code inconnu (firmware plus récent) | tiret seul — jamais un libellé voisin |

**Règles** :

- `ph_ff_status` **absent** (`undefined`, firmware < 2.28.0) → **ligne masquée** (`hidden`). Afficher « inactif » mentirait sur un firmware qui n'a pas de feedforward.
- La valeur n'est affichée **que** pour le statut `actif` : dans tous les autres états le débit vaut 0 par construction, et un « 0,00 mL/min » se lirait comme une mesure.
- Parité `/data` ↔ WS : les deux champs sont servis à l'identique par les deux transports (voir [API.md](../API.md#champs-du-feedforward-ph-feature-085-v2280)).

## Sens de « en dosage » (feature-094, v2.36.0)

Aucun élément d'interface n'est ajouté, déplacé ni renommé par ce lot — mais **le champ `ph_dosing` que la page consomme ne veut plus dire la même chose** ([ADR-0062](../adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md)) :

| | Avant (≤ 2.35.0) | Depuis 2.36.0 |
|---|---|---|
| `ph_dosing === true` | un débit est commandé (≈ la pompe tourne) | une **session de dosage est ouverte** — **la pompe peut être à l'arrêt** |

Le verrou de session pH suit désormais la **porte** de session, décidée sur une moyenne glissante de 10 min de la mesure filtrée, et non plus le débit instantané ([Porte de session pH](../subsystems/pump-controller.md#porte-de-session-ph-robuste-au-bruit-feature-094-v2360)). Pendant une **prolongation** (durée minimale de 30 min murales, débit plafonné à la dose de base puis nul), `ph_dosing` **reste `true`** — jusqu'à 3 h au pire cas.

Conséquences pour cette page :

- **Verrou de calibration** (`_updateInjectButtons`, condition `(ph_inject_remaining_s > 0) || (ph_dosing === true)`, cf. [État de calibration](#état-de-calibration--bouton--calibrer-la-sonde--feature-034)) : le bouton « Calibrer la sonde » reste **désactivé plus longtemps**, y compris pompe à l'arrêt. C'est **cohérent avec l'intention d'origine** du verrou — il a toujours couvert **toute la session** d'impulsions, repos compris — et il va dans le sens **restrictif**, donc aucun changement n'est apporté.
- **Statut de dosage des cartes** : la distinction « **commandé** vs **effectif** » posée en v2.21.1 ([ADR-0045](../adr/0045-etat-impulsion-expose-en-calendrier.md)) reste la règle et devient indispensable — « la pompe est alimentée maintenant » se lit **uniquement** sur `ph_pulse_phase_ms < ph_pulse_ms`, **jamais** sur `ph_dosing`. Voir [page-dashboard.md](page-dashboard.md#statut-dosage-des-cartes-phorp--commandé-vs-effectif-v2211).

## Capacités de dosage et bandeaux de limite d'actionneur (feature-086, v2.29.0)

Trois éléments exposent la [limite d'actionneur pH](../subsystems/pump-controller.md#limite-dactionneur-ph--exposition-et-alerte-de-sous-dimensionnement-feature-086-v2290) ([ADR-0056](../adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)) : un bloc de capacités, un bandeau informatif, une alerte de diagnostic. Point de décision **unique** : `updatePhActuatorLimitDisplay()` (appelée depuis `updateDetailSections()`, sous `try/catch` — un rendu qui lance ne masque jamais les rubriques suivantes en silence). **Sentinelle de présence** : `ph_limited_by_hourly_budget` non booléen (`undefined`, firmware < 2.29.0) → les **trois** éléments restent masqués, rien n'est inventé.

### Bloc capacités (`#ph_capacity_block`)

Sous la barre de dose journalière, présentation volontairement **discrète** (`muted small`) : ce sont des **capacités dérivées des réglages** (limite horaire × débit calibré, × minutes de filtration programmées), pas des mesures.

| Élément | Id | Source | Format |
|---|---|---|---|
| Débit soutenable | `#ph_sustainable_value` | `ph_sustainable_ml_per_min` | `X,XX mL/min` (fr-FR, 2 décimales) ; `null` → **« Indisponible — pompe non caractérisée »** |
| Maximum journalier | `#ph_daily_max_value` | `ph_daily_max_ml` | `N mL/j` (entier, fr-FR) ; `null` → « Indisponible » |

### Bandeau informatif « Dosage au maximum autorisé » (`#ph_budget_banner`)

`callout--info`, `role="status"` — ton **neutre** validé par `pool-chemistry` : c'est un **état normal** de la régulation, pas une anomalie (pas de rouge, pas d'icône d'alerte). Affiché quand `ph_limited_by_hourly_budget` est `true`, texte composé par `updatePhActuatorLimitDisplay()` :

> **Dosage au maximum autorisé**
> La limite horaire d'injection ({X} min/h) plafonne l'apport de correcteur à {Y} mL/min, soit au plus {Z} mL sur la journée de filtration programmée. Le pH redescend au rythme le plus rapide que ce réglage autorise.

`{X}` = `ph_limit_minutes` (config), `{Y}` = `ph_sustainable_ml_per_min`, `{Z}` = `ph_daily_max_ml`. Si `{Z}` est `null`, la première phrase est tronquée après « {Y} mL/min. ». Si `{Y}` est `null`, le bandeau est **masqué** (garde défensive : sans débit soutenable, la phrase serait creuse — cas théorique, le firmware ne tient pas d'épisode C5 sans pompe caractérisée).

### Alerte « Capacité de dosage insuffisante » (`#ph_undersized_banner`)

`callout--warn`, `role="status"` — diagnostic d'installation, visuellement distinct du bandeau informatif, plus grave, mais **pas une alarme critique**. Affichée quand `ph_undersized` est `true`. Texte **statique** (seul `hidden` est piloté), libellé exact validé par `pool-chemistry` :

> **Capacité de dosage insuffisante**
> Depuis 3 jours, la pompe pH délivre chaque jour tout ce que la limite horaire autorise, sans ramener le pH à la consigne. Le réglage actuel ne couvre pas le besoin du bassin : allonger la durée de filtration ou, après vérification de l'installation, relever la limite horaire.

**Règles de rédaction tenues** :
- **L'alerte REMPLACE le bandeau informatif** — jamais d'empilement : quand `ph_undersized` est vrai, le bandeau est masqué (l'alerte contient l'information).
- **Aucune mention de bidon** (leçon v2.26.0 : le client annonçait « vérifiez le bidon » sur une saturation qui désignait la limite horaire).
- Le conseil cite **la filtration en premier** ; le relèvement de la limite horaire n'est proposé qu'**« après vérification de l'installation »** (le relèvement augmente proportionnellement le volume injecté pendant le retard de mélange — point 3 de la spec, différé).
- « 3 jours » est la **constante firmware** du diagnostic (`kPhActuatorLimitConsecutiveDays`), pas un champ servi.

## Bandeau « Stabilisation des mesures non atteinte » (`#measures_purge_timeout_banner`, feature-091, v2.34.0)

`callout--warn`, `role="status" aria-live="polite"`, placé **au-dessus** de la carte Régulation pH ([`data/index.html`](../../data/index.html)). Texte **statique** — seul l'attribut `hidden` est piloté :

> **Stabilisation des mesures non atteinte**
> Après 90 min de circulation, le pH ou l'ORP brut varie encore. Vérifier les sondes et la chambre de mesure (bulles, encrassement). L'injection a repris sous la surveillance des gardes habituelles.

- **Condition d'affichage** : `measures_purge_last_end === 2` (plafond `kPurgeCapMs` atteint sans stabilité). Rendu par `updateMeasuresPurgeDisplay(latestSensorData)` ([`data/app.js`](../../data/app.js)), la **même** fonction que la ligne « Dernière purge » des [Réglages](page-settings.md) — un seul point de décision.
- **Un doublon strictement identique existe sur la page ORP** (`#measures_purge_timeout_banner_orp`) : l'anomalie concerne les **deux** voies de mesure, elle doit être visible depuis la page consultée. Voir [page-orp.md](page-orp.md).
- **Warning, pas critique** : le plafond **ouvre** la porte de validité — l'injection a repris, sous les gardes de sécurité habituelles. Le bandeau reste visible tant que le motif vaut `2`, **y compris** si une re-purge a rouvert le badge capteur entre-temps.
- **Rétrocompat** : `measures_purge_last_end` absent ou non numérique (firmware < 2.34.0) ⇒ bandeau masqué, rien d'inventé.
- Côté firmware, la même transition émet un `WARNING` `[STAB]` et l'alerte MQTT [`measures_purge_timeout`](../MQTT.md#alertes) — c'est le **seul** canal MQTT de cette information (aucune entité d'auto-discovery créée). Détail : [measure-validity.md](../subsystems/measure-validity.md), [ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md).

## Carte « Correction de la mesure pH » (feature-067, v2.20.0)

Carte `#ph-card-offset`, placée **entre la carte Régulation et la carte Historique**. Elle est **toujours visible**, quel que soit le mode de régulation : elle corrige la **mesure**, pas la régulation. Elle est **distincte de la calibration EZO**, qui reste dans sa propre carte.

### Consigne pH : contrôle segmenté (v2.25.0)

Le champ `<input type="number">` est remplacé par un **contrôle segmenté** `#ph_target_group` construit dynamiquement par le JS : une position par valeur autorisée (4 positions à 7,20 / 7,30 / 7,40 / 7,50). Un `<input type="hidden" id="ph_target">` porte la valeur courante pour le reste du code.

- **La saisie hors plage devient impossible depuis l'interface** : ni champ libre, ni curseur `input[type=range]`. Un clic sur une position **enregistre immédiatement** (autosave, pas de bouton Sauvegarder).
- **Aucune borne n'est codée en dur côté client.** Les positions sont calculées depuis `ph_target_min` / `ph_target_max` / `ph_target_step`, servis par `GET /get-config`. Un repli de secours existe pour un firmware antérieur qui ne servirait pas ces champs, mais il n'a **jamais autorité** : un lot de bornes absent ou incohérent (borne inversée, pas nul) est ignoré **en bloc** et les bornes déjà connues sont conservées — indispensable, car la trame `config` du WebSocket ne transporte pas ces bornes.
- Le nombre de positions est **borné à 20** (garde-fou : un pas aberrant servi par un firmware corrompu ne doit pas fabriquer des milliers de boutons).
- `#ph_target_hint` n'est plus un avertissement de validation (il n'y a plus rien à valider côté client) : il rappelle la plage servie par le firmware et le **référentiel** des seuils — « Plage autorisée : 7,20 à 7,50, par pas de 0,10. Les seuils de sécurité du dosage portent sur le pH affiché (offset compris). » Le **rouge est réservé aux échecs de sauvegarde** (`showPhTargetSaveError`).

> ⚠️ **Pourquoi pas un `input[type=range]`** : sur mobile, un curseur n'affiche pas sa valeur, n'a pas de cran tactile et se déclenche au premier contact — y compris pendant un scroll. Voir [ADR-0048](../adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md).

### Offset pH : incrémenteur (v2.25.0)

| Élément | Détail |
|---|---|
| `#ph-offset-stepper` | **Incrémenteur** `−` / valeur / `+` (`#ph_offset_dec`, `#ph_offset_inc`). Remplace le champ numérique libre. Pas de **0,10** (était 0,01), plage `[−0,50 ; +0,50]` → 11 positions. Les boutons sont **désactivés aux bornes** ; **pas d'auto-répétition** sur appui long — chaque cran est un acte délibéré, sur le réglage le plus sensible de la page (un cran = 0,10 pH de décalage sur toute la régulation). Un `<input type="hidden" id="ph_offset">` porte la valeur. |
| `#ph_offset_help` | Texte d'aide **permanent** : « Correction logicielle de la mesure pH. Ne remplace pas la calibration du module EZO : à n'utiliser que pour compenser un écart lié à l'implantation de la sonde, après comparaison avec une mesure de référence (trousse DPD ou photomètre). pH corrigé = pH brut + offset. » |
| `#ph_offset_save_btn` | Bouton « Sauvegarder », suivi de `trackDirtyState` (actif seulement si la valeur diffère de la config enregistrée). |
| `#ph_offset_error` | Message d'erreur (`role="alert"`, rouge) : **refus serveur affiché tel quel**. |

Bornes et pas viennent de `ph_offset_min` / `ph_offset_max` / `ph_offset_step` (`GET /get-config`), même règle que la consigne : rien n'est codé en dur.

### Deux paliers d'avertissement (`#ph_offset_warning`)

Callout ambré (`role="status"`, `aria-live="polite"`) calculé par `renderPhOffsetWarning()` sur la **valeur en cours de saisie** (pas sur la valeur enregistrée) : l'utilisateur est prévenu **avant** d'enregistrer. La consigne utilisée est le contrôle segmenté s'il est initialisé, sinon `window._config.ph_target`, sinon la borne basse de la plage servie.

| \|offset\| | Affichage |
|---|---|
| `< 0,15` (`PH_OFFSET_WARN1`) | callout masqué |
| `0,15` à `< 0,30` | « Attention : cet offset décale la régulation. Avec une consigne de **7,20** et cet offset, le système régulera l'eau à un pH réel de **7,00**. Un offset erroné entraîne un surdosage d'acide et une eau agressive. Vérifiez votre mesure de référence avant d'enregistrer. » |
| `≥ 0,30` (`PH_OFFSET_WARN2`) | « Offset élevé (**+0,30** pH). Si cet offset est erroné, l'eau réelle s'établira autour de **6,90** au lieu de **7,20**. Au-delà de 0,30, la sonde doit être recalibrée plutôt que compensée. **Les seuils de sécurité du dosage portent sur le pH affiché (offset compris)** : un offset faux les déplace d'autant sur l'eau réelle. » |

Formatage français : virgule décimale, 2 décimales, signe explicite sur l'offset.

> **v2.25.0 — plus AUCUN seuil de sécurité n'est annoncé ni recalculé côté client.** L'ancien message citait un plancher brut calculé par une réplique JS de `phRawFloorFor(offset)` : ce miroir de constante firmware est **supprimé**, avec la fonction qu'il répliquait. Les seuils sont désormais fixes et portent sur le pH affiché — il n'y a plus rien à recalculer, donc plus rien à maintenir en double. Le palier `0,30` reste une règle **purement ergonomique** (à partir de quand insister sur la recalibration) : il ne correspond à aucun seuil firmware.

### Enregistrement

`POST /save-config` avec le seul champ `{"ph_offset": <valeur>}` (via `sendConfigDetailed`), puis `loadConfig()`. L'incrémenteur ne produisant que des multiples du pas dans la plage, il n'y a plus de pré-contrôle de saisie à faire : la validation qui fait foi est côté serveur, qui **refuse en HTTP 400** hors bornes (jamais d'écrêtage) et **arrondit au pas** dans la plage (voir [API.md](../API.md#bornes-de-la-consigne-ph-et-de-loffset-v2250)). En cas de refus, le message serveur est affiché tel quel et le bouton passe en « Erreur » 2 s — l'UI ne laisse jamais croire qu'une valeur refusée a été enregistrée.

### Diagnostic dans le modal filtre pH

Le `<dialog id="ph-filter-modal">` (« État filtre pH », ouvert par le chip d'état filtre) expose la chaîne complète :

| Ligne | Élément | Contenu |
|---|---|---|
| Valeur brute | `#ph-fm-raw` | `phRaw` — brut Atlas EZO |
| Offset | `#ph-fm-offset` | offset appliqué, signé (`+0.40`) |
| Valeur corrigée | `#ph-fm-corrected` | `phRaw + offset` (`--` si le brut est indisponible) |
| Médiane (corrigée) | `#ph-fm-median` | `phMedian` — déjà corrigée côté firmware |
| Valeur filtrée (corrigée) | `#ph-fm-filtered` | `phFiltered` — déjà corrigée côté firmware |

Suivent les lignes existantes (pics rejetés, âge, filtre prêt, raison de blocage dosage).

### L'écran de calibration reste sur le BRUT

⚠️ **Invariant** : `updateCalibrationReadouts()` alimente `#cal-ph-mid-readout` / `#cal-ph-low-readout` **exclusivement** depuis `json.phRaw`. Le **repli sur `json.ph`** qui existait auparavant a été **supprimé** : ce champ porte désormais la valeur corrigée, et afficher un pH corrigé face à une solution étalon reviendrait à calibrer la sonde sur une valeur déjà décalée. Conséquence assumée : si `phRaw` est indisponible (EZO muet, lecture stale), le readout affiche `--` au lieu de replier sur une autre valeur. Le lissage léger dédié calibration (médiane des ~5 dernières brutes) et l'indicateur de stabilité Δ60 s travaillent également sur le brut, comme avant.

## Chip d'état filtre (feature-025)

À côté de la valeur filtrée, un **chip cliquable** `#ph-filter-chip` reflète l'état du filtre. Classification **côté UI** par `_classifyFilterState(json, 'ph', st)` ([`data/app.js`](../../data/app.js)), évaluée dans cet ordre :

| Condition (ordre de priorité) | Label chip | Classe / couleur |
|---|---|---|
| **`phFilterAlarm === true`** (v2.20.3) | **« Régulation à l'arrêt »** | `bad` (rouge) |
| **`phDoseBlockedCode` = PANNE de mesure** (v2.25.1 — codes 16/20 « sonde figée », 17/21 « plus aucune mesure acceptée ») | « Sonde figée » / « Sonde muette » | `bad` (rouge) |
| `phFilterUnstable === true` | « Capteur instable » | `bad` (rouge) |
| `phRaw` invalide (`null`) **et** dernière brute valide < 1 min (v2.25.1) | « Lecture EZO manquante » | `warn` (ambré) |
| `phRaw` invalide (`null`) et aucune brute valide depuis ≥ 1 min | « EZO indisponible » | `unknown` (gris) |
| **`phDoseBlockedCode` = état TRANSITOIRE** (v2.25.1 — codes 15/19 warmup, 18/22 mesures non représentatives) | « Stabilisation… » / « Mesures non représentatives » | `warn` (ambré) |
| `phFilterReady === false` | « Stabilisation… » | `warn` (ambré) |
| `phRejectedCount` a augmenté récemment (`< FILTER_REJECT_WINDOW_MS`) | « Pics rejetés » | `warn2` (ambré) |
| sinon | « Mesure stable » | `good` (vert) |

> Le compteur de rejets étant **cumulatif** côté firmware, l'UI ne signale « Pics rejetés » que si `phRejectedCount` a augmenté dans la fenêtre récente (`st.lastRejectAt`), pas tant que le total reste non nul.

> **v2.25.1 — deux changements de rang et un nouveau libellé.**
> 1. Le **code de refus** est consulté **avant** le test « brut invalide ». Ce test condamnait l'EZO sans savoir pourquoi : au démarrage, pendant que la porte de validité était fermée et que la fenêtre de lissage se remplissait, le chip annonçait « EZO indisponible » — une panne matérielle affichée sur une sonde saine, pendant environ six minutes à chaque redémarrage.
> 2. Une lecture brute manquante **depuis moins d'une minute** (`FILTER_RAW_BLIP_MS`) devient « **Lecture EZO manquante** » (avertissement) et non plus « EZO indisponible » (panne) : un blip de bus I²C n'est pas une panne établie.
>
> Les **pannes** (sonde figée, sonde muette) et les **états transitoires** (préchauffage, mesures non représentatives) sont désormais dans deux registres visuels **disjoints** — le rouge est réservé aux pannes. La catégorie vient du **firmware** (`phDoseBlockedCode`), plus d'une analyse de la phrase française : voir [ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md) et la [table des codes](../API.md#code-de-cause-de-refus-de-dosage-v2251).

### Modal détail filtre

Clic sur le chip → `<dialog id="ph-filter-modal">` (`État filtre pH`). `_renderFilterModalValues('ph', json)` remplit : valeur brute, médiane, filtrée, **Pics rejetés** (`phRejectedCount`), âge dernière mesure, **Filtre prêt** (Oui/Non), et une ligne conditionnelle **raison de blocage dosage** (affiche aussi « Mélange en cours » si `phMixingDelayActive`).

**v2.25.1 — la raison de blocage est traduite depuis le CODE** (`_doseBlockInfo()` + table `DOSE_BLOCK_CODES`), plus par recherche de fragments de texte (`_doseBlockReasonFr()`, supprimée). Trois chemins **distincts** :

| État de `phDoseBlockedCode` | Affichage |
|---|---|
| `0` ou `null` | ligne **masquée** (dosage autorisé) |
| code répertorié | libellé de la table, avec ton **rouge uniquement si PANNE** (`probe-modal__value--bad`) ; un préchauffage ou une porte de validité fermée restent en ton neutre |
| `250` | texte serveur cité tel quel (message de garde à valeurs runtime) |
| clé **absente** (firmware plus ancien) | repli sur `phDoseBlockedReason`, affiché tel quel — **seul** cas de repli sur le texte |
| code **inconnu** (firmware plus récent que la page) | « **Cause inconnue (code N)** » — jamais le libellé voisin : une page périmée doit avouer son ignorance plutôt que nommer une cause plausible et fausse |

> **v2.20.3** — l'état `phFilterAlarm` prime sur **tout** le reste, y compris sur « EZO indisponible ». Justification : il signifie qu'il n'y a plus **aucune** régulation depuis plus de 5 min alors que la filtration tourne — c'est le seul état que l'utilisateur doit voir immédiatement, avant tout détail de diagnostic. Le firmware ne l'arme jamais pendant une purge, un warmup ou une filtration à l'arrêt.

### Bouton « Réarmer le filtre pH » (v2.20.3)

`<button id="ph-filter-reset" class="btn btn--danger">` au pied du modal « État filtre pH ». C'est la **sortie de secours utilisateur** quand le filtre reste verrouillé : avant la v2.20.3, la seule porte de sortie était `POST /debug/sensor_filter_reset`, absente de l'interface — le bassin est resté sans régulation ≥ 3 h faute de recours accessible.

**Confirmation explicite obligatoire** (exigence `pool-chemistry`) via `confirmDialog({ danger: true })`. Le texte annonce les trois conséquences :

1. la fenêtre de mesure est vidée et repart de zéro ⇒ **dosage automatique bloqué ~1 min** (le temps du réamorçage) ;
2. ⚠️ la **détection de sonde figée** et l'**alarme de bruit électrique** en cours **seront EFFACÉES** — si la sonde est réellement défectueuse, le défaut sera de nouveau masqué et devra être re-détecté ;
3. un seul réarmement est autorisé **par capteur toutes les 10 minutes**.

Appel : `POST /reset-sensor-filter?sensor=ph` via `authFetch`. Le bouton est désactivé et libellé « Réarmement… » pendant la requête, puis restauré dans un `finally` (l'UI ne reste jamais figée, même contrôleur injoignable).

| Réponse | Retour utilisateur |
|---|---|
| `200` | toast succès « Filtre pH en cours de réarmement (~1 min avant reprise du dosage). » + fermeture du modal |
| `429` | toast erreur « Réarmement refusé : réessayez dans N min. » (`retry_after_s` arrondi à la minute supérieure ; message générique si le corps est vide) |
| `401` / `403` | toast erreur « Réarmement refusé : droits administrateur requis. » |
| autre | toast erreur « Échec du réarmement du filtre pH. » |
| exception réseau | toast erreur « Contrôleur injoignable — réarmement non envoyé. » |

La page ORP expose le même bouton pour son propre capteur ([page-orp.md](page-orp.md#bouton--réarmer-le-filtre-orp--v2203)) : réparer le pH ne touche jamais l'ORP.

### Bannière « Bascule automatique en mode manuel » (v2.20.3)

`<div id="ph-auto-manual-banner" class="regul-alert regul-alert--inline" role="alert" hidden>` dans la carte Régulation pH, rendu par `renderPhAutoManualBanner(cfg)` à partir de `GET /get-config` (`ph_auto_manual_epoch` / `ph_auto_manual_reason`).

- Affichée dès que l'un des deux champs est renseigné ; masquée sinon.
- Texte (**v2.25.0**) : `Le <date locale FR>, l'appareil a basculé la régulation pH en mode manuel par sécurité. Cause : <raison>. Tant que cette alerte n'est pas acquittée, la régulation ne peut pas être réactivée : traitez d'abord la cause, puis utilisez le bouton ci-dessous. L'acquittement n'enclenche aucun dosage — il vous rend seulement le choix du mode.`
- `epoch = 0` (horloge non synchronisée au moment de la bascule) ⇒ « date inconnue (horloge non synchronisée au moment de la bascule) », la cause restant affichée.
- La trace est **persistée en NVS** côté firmware : elle survit au redémarrage et à la rotation du ring buffer du journal. Voir [pump-controller.md](../subsystems/pump-controller.md#persistance-de-la-bascule-automatique-en-manuel-v2203).

#### Bouton « Acquitter l'alerte » (v2.25.0)

`<button id="ph-auto-manual-ack" class="btn btn--primary">Acquitter l'alerte</button>`, placé **dans** la bannière (`bindPhAutoManualAck()`).

- Envoie **uniquement** `{"ph_auto_manual_ack": true}` sur `POST /save-config` — **jamais** de mode : effacer la trace ne doit pas réarmer la régulation. L'appareil **reste en manuel**, l'utilisateur choisit ensuite.
- Pendant l'appel : bouton désactivé, libellé « Acquittement… ». Il est **toujours** réhabilité en sortie (succès comme hors ligne) — jamais figé.
- Succès ⇒ toast « Alerte acquittée — vous pouvez choisir un mode de régulation. » puis `loadConfig()` (la bannière disparaît, le segmented se resynchronise).
- Échec ⇒ toast d'erreur, la bannière reste.

#### Parcours en cas de refus 409 sur le sélecteur de mode

Tant que la trace existe, le firmware **refuse (409)** toute sortie du mode `manual` (voir [API.md](../API.md#post-save-config--critical)). Le clic sur le segmented `#ph_regulation_mode` est donc asynchrone et gère ce refus :

1. Mémorisation du mode précédent, affichage optimiste du mode cliqué.
2. `POST /save-config {"ph_regulation_mode": …}` via `sendConfigDetailed()` (qui remonte désormais `status` et le champ `message` du corps d'erreur, en plus de `error`).
3. Sur `409` : **revert** de `window._config` **et** du segmented sur le mode réel, toast portant le message français du serveur, puis **défilement automatique vers la bannière** (`scrollIntoView`) — l'utilisateur est renvoyé vers la cause et le bouton d'acquittement.
4. Sur toute autre erreur : revert + toast générique.
5. Dans tous les cas de refus, `loadConfig()` resynchronise l'UI sur l'état **réel** du contrôleur.

> Le toast de refus **ne propose pas** d'acquitter en un clic : l'acquittement doit rester un acte **lu et réfléchi** (bouton de la bannière, qui affiche la cause), pas un réflexe pour débloquer un clic refusé.

> ⚠️ L'acquittement n'est possible **que depuis cette interface web**. La commande MQTT/Home Assistant `ph_regulation_mode/set` est refusée sous trace et n'efface jamais rien (recommandation du gate `pool-chemistry` — voir [MQTT.md](../MQTT.md#refus-du-changement-de-mode-ph-sous-trace-de-sécurité-v2250)).

> **v2.25.0 — `collectConfig()` n'envoie plus `ph_regulation_mode` ni `ph_enabled`.** Le mode a son **propre** chemin de sauvegarde (clic sur le segmented) ; le réémettre dans le payload global avec un repli **en dur** sur `"automatic"` réarmait la régulation depuis un onglet périmé (incident du 2026-08-25 : bascule de sécurité annulée et trace effacée sur une simple consultation de l'app). Même patron que feature-074 pour `pumpN_max_duty_pct`. `ph_enabled` n'est en outre plus accepté en écriture par le firmware.

## Chip d'état sonde (feature-024)

Sous la valeur pH dans le bloc Statistiques, un **chip cliquable** affiche en permanence l'état diagnostique de la sonde pH (pente Atlas EZO + décalage zéro). L'évaluation est faite **côté UI** — le firmware expose les valeurs brutes (cf. [docs/subsystems/sensors.md](../subsystems/sensors.md#pente-sonde-ph--feature-024)).

### Placement

Élément `<button id="ph-probe-chip" class="chip chip--probe ...">` à l'intérieur du bloc Statistiques pH (`#ph-card-stats`), sous l'affichage pH (1 décimale). Le chip est focusable (rôle bouton, `Enter`/`Espace` ouvrent le modal).

### Classification (UI)

Source : champs WS `phSlopeAcid`, `phSlopeBase`, `phSlopeZero`, `phSlopeAgeMs`, `phCalPoints`. Les seuils sont **dans `data/app.js`** (constantes `PH_PROBE_*` + fonction `classifyPhProbe()`) — modifier sans reflasher le firmware.

> **v2.21.2 (parité de transport)** : les 4 champs `phSlope*` sont désormais **aussi dans `GET /data`** (mêmes règles null/arrondi que le WS) — un client sans WebSocket voit la pente via le poll `/data`, voir [API.md](../API.md#get-data--write).

> **feature-034 — chip unique sonde + calibration** : ce chip est désormais l'**unique** indicateur « sonde + calibration » du bloc Statistiques pH (le chip de calibration séparé `#ph-cal-chip` a été supprimé). Pour `phCalPoints < 2`, il affiche l'**état de calibration** ; pour `≥ 2`, il affiche le **diagnostic de pente** (santé sonde).

| `phCalPoints` | min(acide, base) | \|zéro\| (mV) | Variante CSS | Libellé | Couleur |
|---|---|---|---|---|---|
| `null` ou `< 0` | — | — | `chip--probe-unknown` | « Sonde pH : lecture en cours… » | gris |
| `0` | — | — | `chip--probe-bad` | « Calibration requise » | rouge |
| `1` | — | — | `chip--probe-warn` | « Calibration 1/2 » | ambré |
| `≥ 2` | NaN / `null` **et** `phSlopeAgeMs` `null` (jamais lu) | — | `chip--probe-unknown` | « Pente en cours de lecture… » | gris |
| `≥ 2` | NaN / `null` **et** `phSlopeAgeMs` non-`null` (pente invalidée) | — | `chip--probe-warn` | « Pente non disponible » | ambré |
| `≥ 2` | `≥ 95 %` | `≤ 15` | `chip--probe-good` | « Sonde excellente · 98 % » | vert (`var(--success)`) |
| `≥ 2` | `90–95 %` | `≤ 30` | `chip--probe-warn` | « Sonde correcte · 92 % » | ambré clair (`var(--warn)`) |
| `≥ 2` | `85–90 %` | `≤ 30` | `chip--probe-warn2` | « Sonde usée · 87 % » | ambré (`var(--warn)`) |
| `≥ 2` | `< 85 %` OU `> 30` | — | `chip--probe-bad` | « Sonde à remplacer · 81 % » | rouge (`var(--danger)`, point pulsant) |

> **v2.21.2 (bug-slope-boot-perdue) — 3 états au lieu du fourre-tout « EZO indisponible » / « Pente non disponible »** :
>
> 1. **« Sonde pH : lecture en cours… »** (`calPoints` `null`/`< 0`, classe neutre) — transitoire de boot : le cache calibration vaut `-1` les toutes premières secondes. Aucun champ de `/data` ou du WS ne distingue ce transitoire d'un vrai mutisme EZO (les deux donnent `calPoints -1` et `ph null`) → choix architect : **un seul libellé**, adouci (l'ancien « EZO indisponible » était trompeur). Un vrai mutisme finit de toute façon en log `EZO pH` au journal + valeurs pH figées à « -- ».
> 2. **« Pente en cours de lecture… »** (`phSlopeAgeMs` `null`, classe neutre, **pas** warning) — la pente n'a **jamais** été lue depuis le boot. État transitoire attendu : le firmware **retente automatiquement** la `Slope,?` de boot ([sensors.md](../subsystems/sensors.md#retentative-bornée-de-la-slope-de-boot--v2212)) → se résout en **< 2 min**.
> 3. **« Pente non disponible »** (`phSlope*` `null` **mais** `phSlopeAgeMs` non-`null`, classe `warn`) — la pente a déjà été lue **puis invalidée** après échecs répétés (`kEzoBusFailMaxConsecutive`) : **erreur réelle**, seul état d'échec des trois.
>
> Les états sonde excellente / correcte / usée / à remplacer et calibration requise / 1-2 sont inchangés.

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
- **EZO pH débranché en runtime** : après 2 échecs consécutifs (`kEzoBusFailMaxConsecutive`), les champs `phSlope*` passent à `null` mais `phSlopeAgeMs` reste connu (`_phSlopeQueriedMs` non remis à zéro) → chip ambré « Pente non disponible » (erreur réelle, v2.21.2) — pas de crash, pas de toast.
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
- **Garde injection prioritaire** : le bouton « Calibrer la sonde » est forcé **désactivé** pendant une injection pH en cours, indépendamment de l'état de calibration (cf. [Cas limites](#cas-limites)). Depuis la **v2.21.1**, la désactivation proactive (`_updateInjectButtons`, à chaque push WS) inclut aussi la **séance de dosage automatique** (`ph_dosing === true`), comme l'ORP le faisait déjà — harmonisation dans le sens **restrictif**. C'est un **verrou fonctionnel voulu** (pas de calibration pendant une injection), distinct de la conflation d'affichage corrigée par l'itération 3 : le verrou couvre **toute la session** d'impulsions, y compris les repos entre deux pouls.

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

> **Readout de calibration = valeur brute.** `updateCalibrationReadouts()` alimente `#cal-ph-mid-readout` et `#cal-ph-low-readout` avec la mesure **brute** (`json.phRaw` **uniquement** — le repli sur `json.ph` a été **supprimé** en v2.20.0, ce champ portant désormais la valeur corrigée par l'offset ; cf. [L'écran de calibration reste sur le BRUT](#lécran-de-calibration-reste-sur-le-brut)), **pas** la valeur lissée affichée ailleurs dans l'UI. Raison : en changeant de solution étalon (ex. pH 7 → pH 4, saut ≈ 3.0), le lissé — médiane sur fenêtre **longue** (13 échantillons ≈ 65 s) + EMA (feature-058) — met plusieurs dizaines de secondes à converger vers la nouvelle valeur ; le saut initial ressemble d'ailleurs à un **spike dur** (`|raw − médiane| > kPhFilterSpikeReject` = 1.0) et est écarté tant que la médiane n'a pas suivi. Le brut suit immédiatement le potentiel réel de l'électrode, indispensable pour juger la stabilité avant de calibrer. Le reste de l'UI (dashboard, valeur principale pH, MQTT) continue d'afficher le lissé.

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

**Mesure** : `ph` (= valeur **filtrée corrigée**, 3 décimales) ; `phMedian`, `phFiltered` (**corrigés**) ; `phRaw` (**brut EZO**, feature-025 / feature-067)
**État filtre** : `phFilterReady`, `phFilterUnstable`, `phRejectedCount`, `phMixingDelayActive`, `phDoseBlockedReason` (feature-025), `phFilterAlarm` (v2.20.3), **`phDoseBlockedCode`** (v2.25.1 — code machine de la cause de refus, source de la catégorie) — voir [Chip d'état filtre](#chip-détat-filtre-feature-025)
**Calibration** : `phCalPoints` (`-1..3`) — voir [Chip d'état de calibration](#chip-détat-de-calibration-feature-034)
**Pente sonde** : `phSlopeAcid`, `phSlopeBase`, `phSlopeZero`, `phSlopeAgeMs` (feature-024) — voir [Chip d'état sonde](#chip-détat-sonde-feature-024)
**Dosage** : `ph_dosing`, `ph_used_ms`, `ph_daily_ml`, `ph_limit_reached` — ⚠️ **`ph_dosing` change de SENS en v2.36.0**, voir [Sens de « en dosage »](#sens-de--en-dosage--feature-094-v2360)
**Mode** : `ph_regulation_mode` (`automatic` / `scheduled` / `manual`), `ph_daily_target_ml`, `ph_enabled` (miroir, voir [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md))
**Répartition scheduled (feature-011, v2.8.0)** : `ph_scheduled_flow_ml_per_min` (float 1 décimale, mL/min — débit moyen planifié restant ; `null` hors mode `scheduled`, hors plage de filtration ou heure ESP32 invalide → l'UI affiche « — »)
**Terme intégral (v2.26.0)** : `ph_integral_pct` (float, % — remplissage de la réserve), `ph_integral` (float, pH·s), `ph_integral_term` (float, mL/min). Servis **à l'identique** par `GET /data`. Clé **absente** = firmware antérieur → bloc masqué ; `null` = « non calculable » → « — » ; `0` = **réserve vide, affirmé**. Voir [Rattrapage accumulé](#rattrapage-accumulé-par-la-régulation-ph-v2260)
**Dose de base apprise (feature-085, v2.28.0)** : `ph_ff_ml_per_min` (float 2 décimales, mL/min), `ph_ff_status` (int 0-4, contrat figé). Servis **à l'identique** par `GET /data`. `ph_ff_status` absent = firmware antérieur → ligne masquée. Voir [Ligne « Dose de base apprise »](#ligne--dose-de-base-apprise--feature-085-v2280)
**Limite d'actionneur (feature-086, v2.29.0)** : `ph_sustainable_ml_per_min` (float 2 décimales \| `null`), `ph_daily_max_ml` (int mL/j \| `null`), `ph_limited_by_hourly_budget` (bool), `ph_undersized` (bool). Servis **à l'identique** par `GET /data` (même passage des deux côtés — aucun clignotement possible). `ph_limited_by_hourly_budget` non booléen = firmware antérieur → capacités **et** bandeaux masqués. Voir [Capacités et bandeaux](#capacités-de-dosage-et-bandeaux-de-limite-dactionneur-feature-086-v2290)
**Config** : `ph_target`, `ph_correction_type` (`ph_minus` / `ph_plus`), `max_ph_ml_per_day`, `ph_offset` (feature-067, message `config` — pas `sensor_data`)
**Injection manuelle** : `ph_inject_remaining_s`
**Stabilisation** : `stabilization_remaining_s`
**Validité mesure (feature-061, v2.13.0 ; purge adaptative feature-091, v2.34.0)** : `water_present`, `measures_valid`, `measures_stab_elapsed_s`, `measures_purge_count`, `measures_purge_last_duration_s`, `measures_purge_last_end`, `filtration_state_stale` — badge `#ph-page-badge` + valeur `--` quand la mesure n'est pas représentative (voir [Badge d'état mesure](#badge-détat-mesure-ph-page-badge-feature-061-v2130)), bandeau `#measures_purge_timeout_banner` sur `measures_purge_last_end === 2`. Servis **à l'identique** par `GET /data` (parité obligatoire). ⚠️ `measures_stab_remaining_s` **n'existe plus** depuis la v2.34.0.

> Champs **supprimés** depuis feature-021 : `ph_raw`, `ph_voltage_mv`, `ph_cal_valid` (remplacé par `phCalPoints`), `ph_calibration_date`, `ph_calibration_temp` (calibration mémorisée dans le module EZO, pas en NVS ESP32).

## Actions

| Action | Endpoint | Payload | Auth |
|--------|----------|---------|------|
| Sauvegarder config (mode, cible, volume) | `POST /save-config` | JSON config complète | CRITICAL |
| Changer le mode de régulation pH | `POST /save-config` | `{"ph_regulation_mode":"automatic"}` (payload **dédié**, envoi au clic) | CRITICAL |
| **Acquitter la bascule de sécurité (v2.25.0)** | `POST /save-config` | `{"ph_auto_manual_ack":true}` | CRITICAL |
| Enregistrer l'offset pH (feature-067) | `POST /save-config` | `{"ph_offset":0.15}` | CRITICAL |
| Injection manuelle start | `POST /ph/inject/start?volume=N` | querystring | WRITE |
| Injection manuelle stop | `POST /ph/inject/stop` | — | WRITE |
| Calibration EZO (mid ou low) | `POST /calibrate_ph` | `{"step":"mid"}` ou `{"step":"low"}` | WRITE |
| Effacer calibration EZO | `POST /calibrate_clear` | `{"sensor":"ph"}` | WRITE |
| Refresh pente sonde (feature-024) | `POST /debug/ph_slope_refresh` | — | aucune |
| **Réarmer le filtre pH (v2.20.3)** | `POST /reset-sensor-filter?sensor=ph` | querystring | **CRITICAL** (1×/10 min → `429`) |

> Routes legacy supprimées (404) : `/calibrate_ph_neutral`, `/calibrate_ph_acid`, `/clear_ph_calibration`. Voir [ADR-0014](../adr/0014-migration-atlas-ezo.md).

Validation côté firmware :
- `ph_regulation_mode` rejeté si hors de l'enum → valeur ignorée. **v2.25.0** : sortie du mode `manual` refusée en **HTTP 409** tant que la bascule de sécurité n'est pas acquittée.
- `ph_auto_manual_ack` : **booléen strict** (une chaîne `"true"` / `"false"` est rejetée). Envoyé seul, il efface la trace **sans** réarmer.
- `ph_enabled` : **plus accepté en écriture** (dérivé pur du mode) ; une clé envoyée est ignorée.
- `ph_daily_target_ml` > `max_ph_ml_per_day` → HTTP 400.
- `ph_offset` hors de `[−0,50 ; +0,50]` → **HTTP 400** (refus, pas d'écrêtage) ; champ absent ou non numérique → inchangé.

## Règles firmware appliquées

Voir [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) pour le contrat complet, notamment la section [Garde-fous `canDose`](../subsystems/pump-controller.md#garde-fous-candose). Résumé pH :

- **Mode `automatic`** : PID vers `ph_target`, actif uniquement pendant filtration (mode `pilote`), bloqué pendant stabilisation. **Inhibé tant que `phCalPoints < 2`** (calibration EZO incomplète).
- **Mode `scheduled`** : injecte jusqu'à `ph_daily_target_ml` pendant les plages de filtration, **intentionnellement aveugle à la valeur mesurée du pH** — un capteur déréglé ou en cours de remplacement n'arrête pas l'injection. Voir [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md).
  - ✅ **Répartition 24 h (v2.8.0, feature-011,** [ADR-0021](../adr/0021-repartition-scheduled.md)**)** : le volume quotidien n'est plus injecté d'un bloc mais **réparti par fenêtres de 15 min** sur la plage de filtration restante, **bornée à minuit**. Le volume de fenêtre (`restant / fenêtres restantes`, recalculé à chaque fenêtre — donc auto-corrigé après changement de cible, injection manuelle ou reboot) est borné par le **budget horaire partagé** (`ph_limit_minutes`, [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)) et **reporté** à la fenêtre suivante si l'injection correspondante durerait moins de 30 s (anti short-cycling). **Pas de rattrapage J+1** : le reliquat non injecté à minuit est perdu (log `info`). Heure ESP32 invalide → dosage suspendu. **Nuance pompe rapide** : avec un débit élevé (ex. 60 mL/min) et une petite cible, les premières fenêtres sont reportées (volume < équivalent 30 s) — les injections démarrent quand l'horizon se resserre ; c'est le fonctionnement voulu du report.
- **Mode `manual`** : aucune régulation automatique, seule l'injection manuelle pilote la pompe.
- **Gardes de sécurité pH — seuils FIXES sur le pH CORRIGÉ (feature-067, v2.20.0 ; refondues v2.25.0, [ADR-0048](../adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md))** : le plancher asservi à l'offset est **supprimé**. Les seuils sont désormais **fixes** et portent sur le pH **affiché** (offset compris) ; seules deux butées de dernier recours portent sur le pH **brut**. Régulation automatique **et** programmée.

  | Produit | Refus sur le pH CORRIGÉ | Butée sur le pH BRUT |
  |---|---|---|
  | `ph_minus` (acide) | sous **6,70** | sous **6,20** |
  | `ph_plus` (base) | au-dessus de **7,60** | au-dessus de **8,10** |

  **Hystérésis de relâche 0,05 pH** sur toutes les gardes : plus de rafale de refus sur une mesure qui oscille. `critical` + alerte MQTT **`ph_guard`** (ex-`ph_raw_floor`) à la transition. **3 refus en 24 h en mode `automatic`** → bascule de `ph_regulation_mode` en `manual` (persistée) ; le mode **programmé** refuse tout autant mais **n'escalade pas** (alerte `ph_guard_scheduled` réémise au plus une fois par heure). Le message d'escalade est **conditionné au diagnostic EZO** et dit explicitement « la sonde n'est pas en cause » quand les pentes et le zéro sont nominaux. **Le pH+ est gardé pour la première fois** : aucun seuil ne l'encadrait avant la v2.25.0.
- **Avertissement d'eau acide NON BLOQUANT à 7,00 (v2.25.0)** : `warning` + alerte MQTT `ph_low_warning` quand le pH corrigé passe sous 7,00. Ne bloque rien, n'incrémente aucun compteur — c'est le premier avertisseur, conservé malgré l'abaissement du seuil de blocage à 6,70.
- **Les alertes de garde partent hors de toute conjonction d'éligibilité (v2.25.0)** : quota journalier épuisé et **mode `manual`** compris — c'est-à-dire dans l'état exact où l'escalade laisse l'appareil. Avant, la détection était enfermée dans un court-circuit `&&` et devenait muette précisément là.
- **Consigne pH bornée à [7,20 ; 7,50], pas 0,10 (v2.25.0)** : ⚠️ la borne haute passe de 7,60 à **7,50** — à 7,60 pile, la consigne atteindrait le seuil d'inhibition ORP et le chlore serait suspendu **en permanence**. Une installation à 7,60 est **clampée au premier démarrage**, avec avertissement journalisé. La saisie hors plage est devenue **impossible depuis l'interface** (contrôle segmenté, voir ci-dessous) ; la validation serveur (`400`) est conservée en défense en profondeur pour l'API et MQTT.
- **`checkPhTargetReachable()` et l'alerte `ph_target_unreachable` sont SUPPRIMÉES (v2.25.0)** : la marge entre la consigne et le seuil est devenue **structurelle**, garantie par `static_assert` à la compilation — il n'y a plus rien à détecter à l'exécution.
- **Limites** : `ph_limit_minutes` (défaut 5 min/h glissante) et `max_ph_ml_per_day` (défaut 300 mL/j).
- **Anti-rafale court terme** : ≤ 6 cycles/min ET ≤ 20 cycles/15 min (correctif Pass 3.5).
- **Stabilisation post-cal pH** : 5 min (`kStabilizationDurationPhMs`) après chaque calibration EZO réussie. Le dosage est refusé pendant cette fenêtre (cond #3 pool-chemistry).
- **Pause mélange pH** : 15 min (`kPhMixingDelayMs`) **après chaque injection** — le dosage suivant est différé le temps que le correcteur se mélange et que la sonde reflète l'effet (anti-surdosage). Depuis **v2.19.1**, affichée sur le widget du tableau de bord (« Pause mélange : … »). Chronologie complète : [pump-controller.md § Cycle de régulation](../subsystems/pump-controller.md#cycle-de-régulation--chronologie-stabilisation--pause-mélange).
- **Cumul journalier** : persisté en NVS, reset à minuit local — voir [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md).
- ✅ **Injection manuelle gardée (v2.6.0, feature-006)** : `POST /ph/inject/start` passe par les gardes de `evaluateManualInject()` — dans l'ordre : watchdog, filtration (sauf mode `continu`), stabilisation post-cal **de la pompe pH**, double démarrage, **limite journalière prédictive** (frontière `==` acceptée ; depuis la **v2.9.2**, une demande supérieure au reliquat est **écrêtée automatiquement au reliquat** au lieu d'être refusée — le refus `daily_limit` ne subsiste que si le reliquat est nul ou < 1 s de pompe, avec `remaining_ml` arrondi vers le bas), **limite horaire partagée avec l'auto** (`ph_limit_minutes`, [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)), cycles/jour (20) et anti-rafale (6/min, 20/15 min — ring partagé avec l'auto). Refus = **409 JSON** `{"error","message","seconds_remaining"?,"remaining_ml"?}` — voir [Comportement UI injection manuelle](#comportement-ui-injection-manuelle-v260) et [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md#gardes-des-injections-manuelles-feature-006). Le mode de régulation et la mesure capteur ne sont volontairement **pas** vérifiés (le manuel est disponible dans tous les modes et aveugle à la mesure). Arrêt cyclique automatique conservé si la filtration s'arrête pendant l'injection. `POST /ph/inject/stop` n'est **jamais** gardé.
- **Bornage durée** : `duration` plafonné à 600 s (10 min, `kManualInjectMaxDurationS`) au lieu de 3600 s avant v2.1.2. Les gardes évaluent le volume **post-plafonnement** (ce qui sera réellement injecté).
- **Durée estimée sous le champ volume (« ≈ X min »)** : `calcInjectDurationS()` ([`data/app.js`](../../data/app.js)) est un miroir *best-effort* de `planManualInject()` — amplitude == duty calibré → débit mesuré de la pompe ; sinon débit **majorant à 255**. ⚠️ **v2.25.2** ([ADR-0050](../adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md)) : ce débit majorant n'est plus une **constante recopiée dans le navigateur** (`FLOW_255_ML_PER_MIN = {1: 61, 2: 67}`, devenue fausse de ~21 %) mais la valeur **servie** par `flow_assist.flow255` de [`GET /data`](../API.md#get-data--write). Le **repli 61 / 67** est conservé **délibérément** pour les firmwares antérieurs, qui ne publient pas ce champ et dosent bien à ces débits-là — l'aligner sur 74 mentirait sur un appareil ancien. Le firmware reste l'autorité : l'estimation client n'est qu'un indicatif.
  > **Dette connue** : `MAX_INJECT_DURATION_S = 600` reste un **miroir en dur** de `kManualInjectMaxDurationS` dans `data/app.js` (4ᵉ occurrence du motif). Candidat à être servi par `/get-config`.

### Comportement UI injection manuelle (v2.6.0)

**Blocage proactif** : à chaque push WS, `_updateInjectButtons()` → `getInjectBlockReason()` ([`data/app.js`](../../data/app.js)) désactive le bouton « ▶ Injecter » (`#ph_inject_btn`) et affiche la raison dans le hint `#ph_inject_block_hint` (+ `title`) quand un blocage est **déjà connu côté client** :

| Blocage connu (ordre d'évaluation) | Hint affiché |
|---|---|
| `ph_limit_reached === true` | « Limite journalière atteinte — injection impossible jusqu'à minuit » |
| `stabilization_remaining_s > 0` | « Stabilisation post-calibration en cours — réessayer dans N s/min » |
| Filtration arrêtée en mode `pilote` | « Filtration arrêtée — injection impossible (sécurité chimique) » |

Le firmware reste l'**autorité** : miroir best-effort seulement (WS déconnecté → bouton cliquable, le 409 tranche). Le bouton « ⏹ Arrêter » n'est **jamais** désactivé (coupe-circuit opérateur).

#### Avertissement « sécurité pH brut active » avant injection manuelle (v2.25.0)

Encart `#ph_inject_raw_guard_warning` (titre `#ph_inject_raw_guard_title`, texte `#ph_inject_raw_guard_text`), rendu par `renderInjectRawGuardWarning()` **au-dessus** du bouton Injecter.

**Le chemin d'injection manuelle ignore délibérément les butées de sécurité sur le pH brut** — c'est l'échappatoire de l'opérateur, décision produit assumée : le firmware obéit. L'interface n'a donc **rien à bloquer** ; elle doit dire, **avant le clic**, que la sécurité est active, pourquoi, et que l'injection passera outre.

- **Condition d'affichage** : `phRawGuardBlocked === true` **strictement**. `false` **et** information absente (firmware antérieur à la parité `/data` ↔ WS) → **aucun encart**. Un encart affiché « par précaution » quand l'info manque serait ignoré et détruirait la valeur de signal de l'encart réel.
- **Aucun seuil n'est reconstruit côté client** (ni 6,20 ni 8,10) : l'UI consomme le booléen et la **chaîne de cause** rapportée par l'appareil, avec pour seule normalisation la virgule décimale française.
- Le texte explique que la butée porte sur le **pH brut** — la mesure du capteur, sans l'offset : c'est le seul repère fiable si l'offset est mal réglé, car le pH affiché peut alors sembler normal. Il invite à contrôler au réactif et à vérifier l'offset.
- L'encart est rendu **avant et indépendamment** de l'état du bouton : il reste visible pendant une injection en cours (la sécurité mord toujours) et n'entre jamais en concurrence avec `#ph_inject_block_hint`, qui porte les blocages **réels** (limite journalière, filtration).
- **Confirmation obligatoire** : un clic sur Injecter alors que la butée mord ouvre une `confirmDialog` annonçant explicitement le contournement. Elle n'apparaît **jamais** quand l'information est absente.

> ⚠️ Cet indicateur **gèle** sur sa dernière valeur quand la boucle de dosage sort par anticipation (filtration arrêtée, OTA, mode maintenance) : la surveillance ne tourne pas en eau stagnante. Après un arrêt de filtration, l'encart peut refléter un état antérieur. Voir [pump-controller.md](../subsystems/pump-controller.md#surveillance-inconditionnelle-séparée-de-la-décision).


**Toasts sur refus 409** (mapping `INJECT_REFUSAL_MESSAGES` par code `error`) :

| Situation | Réaction UI |
|-----------|-------------|
| 409 JSON (8 codes : `watchdog_inactive`, `filtration_off`, `stabilization_in_progress`, `already_injecting`, `daily_limit`, `hourly_limit`, `max_cycles`, `burst_limit`) | Bouton restauré + toast rouge français par code ; `daily_limit` ajoute « Reste disponible aujourd'hui : N mL » (`remaining_ml`), `stabilization_in_progress` ajoute « Réessayer dans N s/min » (`seconds_remaining`) |
| 409 non JSON (firmware ancien / proxy) | Fallback : toast filtration (seule cause 409 de l'ancien format) |
| Filtration **s'arrête en cours** d'injection | Toast rouge capté via WS log critical (`[Injection] pH INTERROMPUE`) : « Injection pH interrompue : la filtration s'est arrêtée. Relancez l'injection après reprise de la filtration. » |
| Erreur HTTP autre (4xx/5xx) | Lecture du body texte affiché si court (< 200 chars), sinon message générique. |

**Comportement attendu utilisateur** : démarrer la filtration **avant** d'injecter, ou relancer manuellement après reprise. Pas de reprise automatique — l'injection en cours est perdue.

## Cas limites

- **Filtration arrêtée / purge post-reprise (feature-061)** : valeur pH « actuelle » à `--`, badge `#ph-page-badge` bleu « Filtration arrêtée » ou « Stabilisation depuis N min », trous dans le graphique d'historique. Ligne brute et calibration non gatées.
- **EZO pH injoignable** : chip rouge « EZO pH injoignable » sur la carte Régulation. Régulation auto inhibée (cond #5).
- **Lecture pH stale > 20 s** : valeur du bloc Statistiques en `is-stale` (opacité 0.5). Régulation auto inhibée (cond #1) + alerte MQTT `pool/alerts/sensor_stale`.
- **Calibration en cours et utilisateur clique 2× sur le même bouton** : queue à 4 slots accepte sans erreur les 4 premières mises en file. Au-delà : 503 + toast « Calibration en cours, réessayer dans 1 seconde ».
- **WebSocket déconnecté** : éléments dépendants des données capteur taggés `.is-stale` (transition 300 ms). Reset automatique à la reconnexion.
- **Reboot inattendu** : à la première réception WS après un reboot dont `reset_reason` ∈ {`WATCHDOG`, `BROWNOUT`, `PANIC`, `EXTERNAL`, `UNKNOWN`}, un toast d'alerte est affiché une seule fois par session.
- **Limite journalière atteinte** : message « Limite atteinte — dosage suspendu jusqu'à minuit », barre en rouge.
- **Pendant la calibration** : le bouton « Calibrer le point » de l'étape concernée affiche un spinner. Les autres actions UI restent disponibles.
- **Injection pH en cours (feature-034)** : le clic sur « Calibrer » (`#ph_cal_trigger_btn`) est **bloqué** si une injection pH est active — condition `(ph_inject_remaining_s > 0) || (ph_dosing === true)`, toast « Calibration impossible pendant une injection pH ». Ce garde existait déjà côté ORP ; il est désormais ajouté côté pH (symétrie de sécurité). Depuis la **v2.21.1**, la **désactivation proactive** du bouton (`_updateInjectButtons`) applique la même condition (elle n'incluait pas `ph_dosing` avant — le clic était bloqué mais le bouton restait cliquable pendant une séance auto).

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
| `sensor.*_ph_integral_pct` | `{base}/ph_integral_pct` (% — v2.26.0) | — |
| `sensor.*_ph_integral_term` | `{base}/ph_integral_term` (mL/min — v2.26.0, diagnostic) | — |
| `sensor.*_ph_integral` | `{base}/ph_integral` (pH·s — v2.26.0, diagnostic) | — |
| (alerte) | `{base}/alerts/calibration_required` | — |
| (alerte) | `{base}/alerts` type `ph_integral_saturated` (v2.26.0) | — |

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
- [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md) — offset logiciel pH appliqué en sortie des accesseurs (feature-067, v2.20.0).
