# Page Filtration — `/filtration`

- **Fichier UI** : [`data/index.html:332`](../../data/index.html:332) (section `#view-filtration`)
- **URL** : `http://poolcontroller.local/#/filtration`

## Rôle

Configurer et piloter la pompe de filtration. **Le mode affiché est la seule autorité**
(feature-146, v2.63.0, [ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)) :
quatre modes, chacun faisant exactement ce que son nom annonce.

| Mode (sélecteur) | Jeton protocole | Comportement |
|---|---|---|
| **Auto** | `auto` | horaires calculés à partir de la température de l'eau (algorithme autour du pivot `kFiltrationPivotHour = 13h`, voir [`constants.h:78`](../../src/constants.h:78)), ajustés quand elle change |
| **Programme** | `manual` | l'utilisateur règle un horaire fixe `start` / `end`, jamais recalculé |
| **Marche** | `force` | la pompe tourne — **sans expiration**, jusqu'au prochain changement de mode |
| **Arrêt** | `off` | la pompe est arrêtée — **sans expiration**, jusqu'au prochain changement de mode |

⚠️ **L'ancien « forçage temporaire » (ON/OFF, minuteur de 4 h) est SUPPRIMÉ.** Il permettait de
déroger à la planification sans modifier le mode affiché ; c'est précisément ce qui causait le
défaut du 2026-10-09 (un changement de mode sans effet tant qu'un forçage MQTT restait actif).
Les boutons **Démarrer** / **Arrêter** de la carte « Contrôle manuel » **changent désormais le
mode** (→ Marche / → Arrêt) au lieu de poser un état séparé.

## Structure

> ⚠️ **Visibilité conditionnée au mode d'installation** (feature-056, v2.19.0). La page n'est pleinement active qu'en mode d'installation **`managed`** (PoolController pilote la filtration). En **`powered`** (alimenté par le circuit de filtration) et **`external`** (filtration tierce signalée), la programmation et le contrôle du relais sont **masqués** : le PoolController ne pilote pas le relais, une note « filtration non pilotée par le PoolController » est affichée. Le mode se règle dans Paramètres → Installation (voir [page-settings.md](page-settings.md#installation-panel-install-feature-056) et [ADR-0026](../adr/0026-mode-installation.md)). L'ancien toggle « Gérer la filtration » (`#filtration_enabled`) est **supprimé** (fusionné dans `install_mode`).

- **Carte Contrôle manuel** (`#filtration-manual-card`) — boutons **Démarrer** / **Arrêter**, qui **changent le mode** (→ Marche / → Arrêt, feature-146) — **jamais grisés** : en mode Arrêt, « Démarrer » est précisément la sortie du mode. Le **badge d'état (Marche / Arrêt)** est placé à droite du titre dans le `card__head` (frère direct du `<h2>`, classes `pill ok` ou `pill bad`) — voir feature-001. La ligne « État actuel » du body, devenue redondante, a été supprimée. Masquée hors mode d'installation `managed`.
- **Carte Configuration** — sélecteur de mode (Auto / Programme / Marche / Arrêt), heure de début, heure de fin, bouton Sauvegarder. Le bandeau d'aide (`#filtration_mode_hint`) décrit désormais les **quatre modes en permanence** (il ne se masque plus selon le mode sélectionné). ⚠️ **feature-164 (v2.80.0)** : un bandeau `#filtration-duration-warning` (`.callout--warn`, non bloquant) apparaît en mode **Programme** si la durée saisie est sous 120 min — voir [Validation serveur des horaires](#validation-serveur-des-horaires-feature-164-v2800). Masquée hors mode d'installation `managed`.

Les autres cartes (graphe, statistiques) peuvent être ajoutées au fil du temps ; se référer à `#view-filtration` pour l'état courant.

> ⚠️ **Hors périmètre de feature-147 (v2.63.0).** Le tableau de bord expose désormais les quatre modes sous forme de **boutons** directement dans la carte Filtration, et le Boost y devient un sous-bloc — voir [docs/features/page-dashboard.md](page-dashboard.md#carte-filtration--quatre-boutons-mode-vs-état-boost-intégré-feature-147-v2630). **Cette page conserve son `<select>` de mode et sa carte « Contrôle manuel » à l'identique** : aucune des deux cartes ci-dessus n'a été modifiée par ce lot.

> ⚠️ **Hors périmètre de feature-161 (lot B, v2.77.0) également.** Ce lot ne touche que la carte
> Filtration du **tableau de bord** : la ligne « Horaires » n'y est plus affichée qu'en Auto/Programme
> (masquée en Marche/Arrêt — aucun créneau n'est suivi dans ces deux modes), et le sous-bloc
> **Boost y devient une option visuellement mineure** (`.card__option`, plus de gabarit identique à la
> carte elle-même). **Cette page n'est pas modifiée** : voir
> [docs/features/page-dashboard.md](page-dashboard.md#sélecteur-de-mode-depuis-les-cartes-ph--orp--filtration-feature-161-v2770)
> pour le détail.

> ⚠️ **feature-164 (v2.80.0)** : la carte **Filtration du tableau de bord** gagne un bloc horaires
> **éditable** en mode Programme (`#dashboard-filtration-schedule-edit`) — voir
> [docs/features/page-dashboard.md](page-dashboard.md#régler-la-consigne-depuis-la-carte-feature-164-v2800).
> **Cette page-ci** (réglage) **n'est pas restructurée**, mais son unique chemin d'écriture
> (`POST /save-config`) gagne la **validation serveur des horaires** décrite ci-dessous — voir
> [Validation serveur des horaires](#validation-serveur-des-horaires-feature-164-v2800).

## Validation serveur des horaires (feature-164, v2.80.0)

⚠️ **Avant ce lot, `filtration_start`/`filtration_end` n'étaient validés NULLE PART sur ce
chemin** : affectés en **String brute**, sans passer par `timeStringToMinutes` ni aucune règle de
cohérence — contrairement à `filtration_mode`, validé depuis feature-146. Deux correctifs,
**communs aux deux surfaces** (cette page **et** la carte du tableau de bord, même chemin
d'écriture) :

- **`début == fin` → refus explicite, HTTP 400 (C11, bloquant)**. Cette plage faisait tourner la
  filtration **zéro heure par jour**, **silencieusement**, jusqu'à ce lot (`isMinutesInRange` avec
  `equalMeansAlways=false` renvoie `false` pour `start==end` — aucun refus ni message n'existait).
  Texte imposé mot pour mot, cite l'heure fautive et un exemple :
  > « Heure de début et heure de fin identiques ({HH:MM}) : la filtration ne démarrerait jamais.
  > Saisissez deux heures différentes — par exemple 00:00 et 23:59 pour filtrer en continu. »
  ⛔ **`fin < début` reste ACCEPTÉ** : cas d'usage réel, une plage qui **enjambe minuit**
  (ex. filtration de nuit) — voir [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp).
- **Durée programmée sous 120 min → avertissement non bloquant (C12)**, cite la durée saisie **et**
  le seuil (ADR-0095), même texte à l'écran (`#filtration-duration-warning`, cette page **et** la
  carte) et au journal (`warning`) :
  > « Filtration programmée : {X} min par jour. Sous 120 min, la circulation est trop courte pour
  > que la régulation pH/ORP atteigne sa consigne — l'eau restera durablement à côté de la valeur
  > visée. »

Détail complet (ordre d'évaluation, corps JSON) : [API.md](../API.md#post-save-config--horaires-de-filtration-feature-164-v2800).

## Données consommées (WebSocket `/ws`)

- `install_mode` (`managed` / `powered` / `external`) — pilote la visibilité de la page (feature-056). `filtration_enabled` **retiré**.
- `filtration_running` (bool — relais fermé, pertinent en `managed`)
- `filtration_mode` (`auto` / `manual` / `force` / `off`)
- `filtration_start`, `filtration_end` (strings `HH:MM`)

⚠️ **`filtration_force_on` / `filtration_force_off` sont SUPPRIMÉS** (feature-146, v2.63.0) avec
l'ordre direct qu'ils portaient — plus aucun champ WS de forçage temporaire.

## Actions

| Action | Endpoint | Auth |
|--------|----------|------|
| Démarrer manuellement (= passer en mode Marche) | `POST /save-config` `{filtration_mode: "force"}` | CRITICAL |
| Arrêter manuellement (= passer en mode Arrêt) | `POST /save-config` `{filtration_mode: "off"}` | CRITICAL |
| Sauvegarder mode/horaires | `POST /save-config` (champs `filtration_mode`, `filtration_start`, `filtration_end` — `filtration_enabled` retiré, feature-056 ; `filtration_mode` validé serveur par `parseFiltrationMode()`, un jeton inconnu est refusé, feature-146) | CRITICAL |

⚠️ **Il n'existe plus de route `/filtration/on` / `/filtration/off` dédiée** (feature-146) : les
boutons Démarrer/Arrêter passent par `/save-config`, comme tout changement de mode.

⚠️ **Correction de doc** : il n'existe pas de fonction `saveFiltration()`. Le bouton « Sauvegarder » de la carte Configuration (`#filtration_save_btn`) est géré par `bindFiltrationManualSave()`, qui lit `collectFiltrationConfig()` (champs `filtration_mode`/`filtration_start`/`filtration_end` uniquement, indépendamment des autres saves) — c'est l'autosave générique (`bindAutosave()`) qui **exclut explicitement** les champs numériques et horaires, mode et horaires de filtration compris (voir le commentaire du code : « Programmation (mode, start, end) — pas d'auto-save, bouton Sauvegarder uniquement »). Les boutons Démarrer/Arrêter du tableau de bord **et** de cette page partagent désormais le même chemin, `setFiltrationRunMode()` (`data/app.js`).

## Règles firmware

**Le mode affiché est la seule autorité** (feature-146, v2.63.0, [ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)) — l'ancien « ordre direct » (forçage ON/OFF, minuteur de 4 h) qui pouvait rendre un changement de mode sans effet est supprimé.

- Mode **Auto** (bug-126, [ADR-0079](../adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)) : l'horaire affiché **ne bouge plus en cours de journée**. Il est calculé à partir de la **moyenne** de température du cycle précédent (échantillons post-purge et en plage **de base**), figé dans un planning en attente **à l'arrêt** du cycle, et appliqué **à minuit seulement** — jamais pendant que la pompe tourne. `computeAutoSchedule()` ([`filtration.cpp`](../../src/filtration.cpp)) ne reste appelée que pour la mise en service (bascule vers le mode Auto), pas en cours de marche.
- Mode **Programme** (jeton `manual`) : l'horaire est respecté tel qu'entré par l'utilisateur, jamais recalculé.
- Mode **Marche** (jeton `force`) : le relais est fermé en permanence, sans dépendre d'un créneau — jusqu'au prochain changement de mode, sans expiration.
- Mode **Arrêt** (jeton `off`) : le relais est forcé à LOW en permanence, jusqu'au prochain changement de mode, sans expiration. Le Mode Boost s'y **efface** (pas de surchloration possible sans circulation).
- **Relais piloté uniquement en mode d'installation `managed`** (feature-056) : en `powered` / `external`, `filtration.update()` retourne de façon anticipée, aucune commande relais, timer de stabilisation au démarrage filtration non armé.
- Interaction régulation : voir [page pH](page-ph.md), [page ORP](page-orp.md) et [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md). En mode `managed`, le démarrage de la filtration arme le timer de stabilisation (voir `armStabilizationTimer()` dans [`pump_controller.h:112`](../../src/pump_controller.h:112)).

## Interaction MQTT / Home Assistant

| Entité HA | Topic état | Topic commande |
|-----------|------------|----------------|
| `switch.*_filtration_switch` | `{base}/filtration_state` (`ON`/`OFF`) | `{base}/filtration/set` — pilote désormais le **mode** (ON → Marche, OFF → Arrêt), plus un ordre direct séparé (feature-146) |
| `select.*_filtration_mode` | `{base}/filtration_mode` (`auto`/`manual`/`force`/`off`) | `{base}/filtration_mode/set` |
| `binary_sensor.*_filtration` | `{base}/filtration_state` | — |

⚠️ **Libellés du select HA changés en feature-146** (v2.63.0) : `Auto` / `Programme` / `Marche` /
`Arrêt` (étaient `Auto` / `Programmation` / `Manuel` / `Désactivé`). Le **protocole MQTT sur le
fil reste inchangé** (`auto`/`manual`/`force`/`off`). Rupture assumée pour une automatisation HA
qui appellerait `select.select_option` avec un ancien libellé français ; celles qui publient le
jeton brut survivent. Voir [`docs/MQTT.md`](../MQTT.md) pour l'auto-discovery complet.

## Cas limites

- **WebSocket déconnecté** : les valeurs affichent `--:--` ; les boutons Démarrer/Arrêter restent cliquables et produisent une erreur si l'API HTTP est aussi injoignable.
- **OTA en cours** : le contrôleur n'accepte plus de changements d'état, `PumpController.setOtaInProgress(true)` stoppe également les pompes doseuses ([`pump_controller.cpp`](../../src/pump_controller.cpp)).
- **Horaire de fin avant le début** : la logique `isMinutesInRange()` gère les plages qui franchissent minuit ([`filtration.cpp`](../../src/filtration.cpp)).
- **Horaire affiché en mode Auto** : ne change qu'à minuit, jamais en cours de journée, y compris pendant un cycle de filtration (bug-126, [ADR-0079](../adr/0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)). Le planning en attente n'est **pas** exposé sur cette page (spec de suivi à venir).
- **Marche et Arrêt n'expirent plus et survivent à un redémarrage** (feature-146) — à la différence de l'ancien forçage temporaire (4 h, perdu au reboot). Un mode laissé par inadvertance (Marche en hivernage, Arrêt après une maintenance) le reste jusqu'au prochain changement explicite.

## Fichiers

- [`data/index.html:332`](../../data/index.html:332)
- [`data/app.js`](../../data/app.js) — `saveFiltration()`, `setFiltrationRunMode()` (feature-146 ; depuis feature-147/v2.63.0 partagée par les **quatre boutons de mode** du tableau de bord — Auto/Programme/Marche/Arrêt, qui remplacent les anciens Démarrer/Arrêter — et par les boutons Démarrer/Arrêter de **cette page**, inchangés), `updateFiltrationControls()` (jamais grisés, hint permanent), `updateFiltrationBadges()` (split détail = `pill ok/bad/mid` dans `card__head` ; dashboard = `state-badge--*` inchangé, et depuis feature-147 pilote aussi les quatre boutons de mode et le texte « Mode X » du tableau de bord), `getFiltrationState()` qui expose `pillClass` (mapping `warn → mid`)
- [`src/filtration.h`](../../src/filtration.h), [`src/filtration.cpp`](../../src/filtration.cpp) — logique firmware
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — endpoint `/filtration/external-state`
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — persistance config, validation du mode (`/save-config`)
- [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) — `publishFiltrationState()`

## Documentation liée

- [docs/subsystems/filtration.md](../subsystems/filtration.md) — règles de comportement et interaction filtration ↔ régulation.
- [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) — gardes (`canDose()` requiert la présence d'eau `resolveWaterPresence()`).
- [ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md) — le mode de filtration est la seule autorité (feature-146).
- [ADR-0026](../adr/0026-mode-installation.md) — mode d'installation (3 archétypes de câblage) + résolution de la présence d'eau.
