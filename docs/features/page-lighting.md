# Page Éclairage — `/lighting`

- **Fichier UI** : [`data/index.html`](../../data/index.html) (section `#view-lighting`)
- **URL** : `http://poolcontroller.local/#/lighting`

## Rôle

Contrôler l'éclairage de la piscine (relais GPIO26). Trois modes exclusifs : **Allumé**, **Éteint**,
**Programme** (horaire) — choisis depuis **cette page** ou directement depuis la carte du
[tableau de bord](page-dashboard.md#carte-éclairage--trois-modes-feature-162-v2790).

## ⚠️ feature-162 (v2.79.0) — les deux cartes fusionnent, le motif rejoint pH/ORP/Filtration

Jusqu'à la v2.78.x, la page portait **deux cartes séparées** : « Contrôle manuel » (Allumer /
Éteindre, un forçage manuel **non persisté** qui neutralisait la programmation sans le dire) et
« Programmation » (un `<select>` à 2 options, Programmation/Désactivé). Ces deux cartes
**fusionnent en une seule**, « Régulation éclairage » — même mouvement que la fusion des cartes pH
(Mode réglage, feature-156). Allumer à la main **cesse d'être une action séparée et devient un
mode** (`On`), au même titre que `Off` et `Programme` — voir
[docs/subsystems/lighting.md](../subsystems/lighting.md) et [ADR-0107](../adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)/[ADR-0108](../adr/0108-basculer-le-mode-est-une-commande-injecter-est-un-geste.md).

**Défaut corrigé au passage** : la carte affichait « Programmation activée » sur le seul
`lighting_schedule_enabled`, alors qu'un forçage manuel pouvait neutraliser la programmation
**sans jamais être publié**. Avec un mode unique et visible, il n'existe plus d'état caché
derrière l'affichage.

## Structure

- **Toggle « Gérer l'éclairage »** — masque / démasque la carte si la fonctionnalité n'est pas désirée (équivalent `lighting_feature_enabled` dans [`config.h`](../../src/config.h)).
- **Carte « Régulation éclairage »** (`#lighting-card-regulation`, fusion des deux cartes
  précédentes) :
  - **Sélecteur `.mode-select`** (`#lighting_regulation_mode`, [DESIGN_SYSTEM.md §5.2](../DESIGN_SYSTEM.md#52-sélecteur-de-mode--mode-select)) — **Allumé / Éteint / Programme**, même composant
    que les cartes pH/ORP/Filtration, pour le même repère visuel aux deux endroits (page de réglage
    **et** carte d'accueil).
  - ⛔ **Aucun libellé ne répète le mode** — le segment actif **est** la seule source (différence
    assumée avec pH/ORP : là-bas, la redondance protège un dosage chimique ; ici, aucun enjeu
    chimique, l'argument ne s'applique pas).
  - **Horaires modifiables directement dans la carte, en mode Programme uniquement**
    (`#lighting-schedule-edit`, composition [DESIGN_SYSTEM.md §5.20](../DESIGN_SYSTEM.md#520-modifier-une-valeur-dans-une-carte--une-composition-pas-une-classe-nouvelle)) : deux champs `<input type="time">` **côte à côte**
    (`.schedule-fields.grid--2`, ne s'empilent pas sous 640 px) + bouton **Sauvegarder** sur sa
    **propre ligne** (jamais à côté d'un champ — règle née du défaut de la page Produits,
    2026-10-09). ⛔ **Absents en Allumé/Éteint** : ils n'ont aucun effet dans ces deux modes, les
    afficher serait trompeur. ⚠️ **Pas d'autosave** : les champs horaires sont explicitement
    exclus de `bindAutosave()` (projet-wide) — bouton Sauvegarder requis.
  - Échec d'enregistrement des horaires ⇒ valeurs réelles restaurées + `.field.is-error` sur le
    champ fautif, message relié par `aria-describedby`.

## Carte Éclairage — tableau de bord (feature-162, v2.79.0)

Voir [docs/features/page-dashboard.md](page-dashboard.md#carte-éclairage--trois-modes-feature-162-v2790)
pour le détail complet (sélecteur, horaires en carte, badge conditionnel, ce qui a disparu) — non
dupliqué ici : la carte et cette page partagent le **même** mécanisme (`applyLightingMode()`,
`renderLightingModeCard()`, `data/app.js`), aucune logique propre à l'une ou l'autre surface.

## Données consommées (WebSocket `/ws`, trame `config`)

- `lighting_feature_enabled` (bool)
- `lighting_mode` (string — **feature-162**, seule autorité : `"on"` / `"off"` / `"schedule"`)
- `lighting_enabled` (bool — dérivé, `mode == "on"`, conservé pour compatibilité)
- `lighting_schedule_enabled` (bool — dérivé, `mode == "schedule"`, conservé pour compatibilité)
- `lighting_start_time`, `lighting_end_time` (strings `HH:MM`)

Trame `sensor_data` : `lighting_enabled` y porte un sens **différent** — l'état **réel** du relais
(`lighting.isOn()`), pas le réglage. C'est cette valeur, et uniquement en mode Programme, qui
alimente le badge d'en-tête (voir [page-dashboard.md](page-dashboard.md#carte-éclairage--trois-modes-feature-162-v2790)).

## Actions

| Action | Endpoint | Auth |
|--------|----------|------|
| Choisir le mode Allumé | `POST /lighting/on` (≡ `{"lighting_mode":"on"}`) | WRITE |
| Choisir le mode Éteint | `POST /lighting/off` (≡ `{"lighting_mode":"off"}`) | WRITE |
| Choisir un mode (dont Programme) | `POST /save-config` `{lighting_mode}` | CRITICAL |
| Sauvegarder les horaires (mode Programme) | `POST /save-config` `{lighting_start_time, lighting_end_time}` | CRITICAL |

⚠️ **v2.79.0** : `/lighting/on`\|`/off` **persistent désormais en NVS et republient en MQTT**, ce
qu'elles ne faisaient pas avant (ancien forçage manuel non persisté). Voir [API.md](../API.md#post-lightingon-lightingoff--write).

## Interaction MQTT / Home Assistant

| Entité HA | Topic état | Topic commande |
|-----------|------------|----------------|
| `select.*_lighting_mode` (**feature-162**, Allumé/Éteint/Programme) | `{base}/lighting_mode` | `{base}/lighting_mode/set` |
| `switch.*_lighting` (Éclairage Piscine — état réel du relais) | `{base}/lighting_state` | `{base}/lighting/set` |
| `text.*_lighting_start` (Éclairage début) | `{base}/lighting_start` | `{base}/lighting_start/set` |
| `text.*_lighting_end` (Éclairage fin) | `{base}/lighting_end` | `{base}/lighting_end/set` |

⚠️ **Rupture assumée (feature-162, confirmée par l'utilisateur)** : l'ancien select 2 options
« Mode Éclairage » (`select.*_lighting_schedule`, Programmation/Désactivé) est **supprimé**, pas
aliasé. Une automatisation Home Assistant qui lui envoyait encore un libellé doit être **mise à
jour** vers le nouveau select 3 options. `{base}/lighting_schedule` reste publié en **lecture
dérivée** (sans commande). Voir [docs/MQTT.md](../MQTT.md) et [ADR-0109](../adr/0109-atterrissage-de-migration-et-alias-ha-portee-a-une-energie.md).

## Règles firmware

- Le relais est piloté en tout-ou-rien (LOW / HIGH). Un champ `brightness` (PWM 0-255) existe dans
  la struct [`LightingConfig`](../../src/config.h) pour un futur support de dimming, non exposé
  dans l'UI à date.
- `lightingCfg.mode` est la **seule autorité** (feature-162) : `decideLightingOn()` fait
  **exactement** ce que le mode nomme (`On` → toujours allumé, `Off` → toujours éteint, `Programme`
  → horaire). Vérifiée à chaque `loop()` par [`lighting.cpp`](../../src/lighting.cpp), plages
  franchissant minuit gérées. Détail complet : [docs/subsystems/lighting.md](../subsystems/lighting.md).
- **Migration d'une installation existante** : une config où la programmation était active migre
  vers **Programme** ; sinon, vers Allumé/Éteint selon l'ancien relais manuel. ⚠️ Le forçage manuel
  n'ayant **jamais été persisté**, ce critère ne vaut qu'**à partir du premier démarrage** sur ce
  firmware — voir [docs/subsystems/lighting.md](../subsystems/lighting.md#migration-nvs-light_enabledlight_sched_en--light_mode) et [ADR-0109](../adr/0109-atterrissage-de-migration-et-alias-ha-portee-a-une-energie.md).

## Cas limites

- **Pas d'éclairage câblé** : désactiver `lighting_feature_enabled` masque toute la carte sur le dashboard et la page `/lighting`.
- **Mode Programme, heures identiques** (`start == end`) : la logique `isMinutesInRange()` traite ce cas comme **allumé toute la journée** — divergence volontaire avec la Filtration (voir [docs/subsystems/lighting.md](../subsystems/lighting.md#divergence-intentionnelle-start--end)).
- **Jeton de mode inconnu en NVS** : repli fail-closed vers Éteint (`ensureTimesValid()`), `warning` journalisé.
- **Changement de mode depuis MQTT ou un autre onglet** : reflété sans rechargement (`renderLightingModeCard()`, appelée sur chaque push WS `config`/`sensor_data`).
- **Échec d'enregistrement du mode** : le sélecteur revient à l'état réel (`resolveSelectorMode`, partagé avec pH/ORP — pas de second mécanisme).

## Fichiers

- [`data/index.html`](../../data/index.html)
- [`data/app.js`](../../data/app.js) — `renderLightingModeCard()`, `applyLightingMode()`, `bindLightingScheduleSave()` (réutilisée pour les deux surfaces, carte et page)
- [`src/lighting.h`](../../src/lighting.h), [`src/lighting.cpp`](../../src/lighting.cpp)
- [`src/schedule_logic.h`](../../src/schedule_logic.h), [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) — `LightingMode`, `decideLightingOn`, `migrateLightingMode`
- [`src/config.h`](../../src/config.h) struct `LightingConfig` (`mode`, seule autorité)
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — endpoints `/lighting/*`
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — `/save-config` (`lighting_mode`)

## Specs historiques

- **feature-052** (v2.17.0) : uniformisation de la carte Programmation avec la filtration — **remplacée** par feature-162.
- **feature-162** (v2.79.0) : fusion des deux cartes, mode unique Allumé/Éteint/Programme, horaires en carte, défaut d'affichage corrigé. Voir [ADR-0107](../adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md), [ADR-0108](../adr/0108-basculer-le-mode-est-une-commande-injecter-est-un-geste.md), [ADR-0109](../adr/0109-atterrissage-de-migration-et-alias-ha-portee-a-une-energie.md).
