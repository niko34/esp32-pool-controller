# Subsystem — `lighting`

- **Fichiers** : [`src/lighting.h`](../../src/lighting.h), [`src/lighting.cpp`](../../src/lighting.cpp)
- **Singleton** : `extern LightingManager lighting;`
- **GPIO relais** : `kLightingRelayPin = 27` (PCB v2, [`constants.h`](../../src/constants.h)) — actif haut. Voir [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md).

## Rôle

Pilote le relais de l'éclairage de la piscine en **tout-ou-rien** (LOW / HIGH). Trois modes
exclusifs : **Allumé**, **Éteint**, **Programme** (horaire).

## ⚠️ feature-162 (v2.79.0) — `lighting_mode` devient la SEULE autorité

Jusqu'à la v2.78.x, l'état reposait sur un **triplet** (`lightingCfg.enabled`,
`lightingCfg.scheduleEnabled`, et un `state.manualOverride` **caché**, jamais persisté ni publié)
qui pouvait diverger de l'affichage : la carte annonçait « Programmation activée » sur le seul
`scheduleEnabled`, alors que `manualOverride` **neutralisait** la programmation sans que rien ne le
dise. `decideLightingOn()` évaluait en outre l'override **avant** la programmation — l'ordre du
code et l'ordre affiché ne racontaient pas la même histoire (exactement le défaut qu'[ADR-0093](../adr/0093-le-mode-de-filtration-est-la-seule-autorite.md)
avait déjà corrigé côté filtration).

**Décision (feature-162, [ADR-0109](../adr/0109-atterrissage-de-migration-et-alias-ha-portee-a-une-energie.md))** :
un champ unique `lightingCfg.mode` (`String`, jetons `"on"` / `"off"` / `"schedule"`) est
désormais la **seule** autorité. Allumer à la main **est un mode** (`On`), pas une suspension
invisible de la programmation — application directe d'[ADR-0107](../adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)
(« un état visible et persistant plutôt qu'une suspension invisible »). Il n'existe plus d'état
caché à publier : la carte ne peut plus mentir sur ce qu'elle affiche.

## API publique

```cpp
void begin();
void update();              // appelé dans loop()
bool isOn() const;
bool getRelayState() const;
void setMode(LightingMode mode);  // remplace setManualOn()/setManualOff()/clearManualOverride()
void ensureTimesValid();
void publishState();
```

`setMode()` écrit `lightingCfg.mode` (via `lightingModeToken()`) et republie immédiatement l'état
MQTT — elle **ne persiste pas** en NVS elle-même : l'appelant appelle `saveMqttConfig()` (un seul
chemin d'écriture, cf. [`src/schedule_logic.h`](../../src/schedule_logic.h)).

## État interne

Plus de `struct LightingRuntime` (`manualOverride` / `manualSetAtMs`, retirée) : le seul champ
mutable de `LightingManager` est `relayState` (état physique du relais). Tout le reste vit dans
`lightingCfg.mode`, persisté.

## Config (`LightingConfig` — [`config.h`](../../src/config.h))

- `featureEnabled` — active / masque la fonctionnalité.
- `mode` (`String`, **feature-162**) — `"on"` / `"off"` / `"schedule"`, seule autorité. Remplace
  `enabled` + `scheduleEnabled` + l'ancien `manualOverride` caché.
- `startTime` / `endTime` (HH:MM) — plage d'allumage, effective **uniquement** en mode `schedule`.
- `brightness` (0-255, PWM) — **présent mais non exposé dans l'UI à date** (relais tout-ou-rien).

**Contrats historiques conservés en LECTURE DÉRIVÉE** — aucun client (MQTT, HA, UI ancienne) ne
casse : `lighting_enabled` (`/get-config`, trame WS `config`) vaut `mode == "on"` ;
`lighting_schedule_enabled` vaut `mode == "schedule"`. Les deux sont calculés **à l'émission**, il
n'existe plus de booléen séparé à tenir synchronisé avec le mode.

### Migration NVS (`light_enabled`/`light_sched_en` → `light_mode`)

Au premier chargement sur ce firmware, si la clé NVS `light_mode` est **absente**, une migration
**one-shot pure** (`migrateLightingMode(lightEnabled, lightScheduleEnabled)`,
[`src/schedule_logic.h`](../../src/schedule_logic.h), testée en natif, zéro `Preferences`/`String`/
`Arduino.h`) dérive le mode des deux anciens booléens — **`light_sched_en` prime** : une
installation où les deux étaient vrais migre vers **Programme**, jamais vers Allumé. Le résultat
est aussitôt réécrit en NVS (`light_mode`), les deux anciennes clés restant également écrites en
**dérivé** (coût nul) pour un retour arrière OTA sûr vers un firmware antérieur.

⚠️ **Nuance à ne pas taire** : l'ancien `manualOverride` n'a **jamais été persisté** (il vivait en
RAM, feature-040) — il est donc **déjà perdu** à chaque redémarrage, y compris sur le firmware
d'avant ce lot. Le critère « une installation dont l'override était actif se retrouve en
Allumé/Éteint, jamais en Programme » (critère d'acceptation de feature-162) ne peut donc être
garanti **qu'à partir du premier démarrage sur ce nouveau firmware** — pas pendant la migration
elle-même, qui ne voit par construction que les deux booléens déjà persistés. C'est **conforme**,
pas une régression : le seul cas où la migration « perd » une information (programmation active ET
forçage manuel actif en RAM au moment du reboot) retombe sur Programme, **exactement** ce que le
firmware actuel produit déjà à ce même redémarrage. Voir [ADR-0109](../adr/0109-atterrissage-de-migration-et-alias-ha-portee-a-une-energie.md)
pour l'arbitrage complet.

## Endpoints HTTP

| Action | Endpoint | Auth |
|--------|----------|------|
| Choisir le mode Allumé | `POST /lighting/on` (≡ `POST /save-config {"lighting_mode":"on"}`) | WRITE |
| Choisir le mode Éteint | `POST /lighting/off` (≡ `{"lighting_mode":"off"}`) | WRITE |
| Choisir un mode (dont Programme) / sauvegarder les horaires | `POST /save-config` (`lighting_mode`, `lighting_start_time`, `lighting_end_time`) | CRITICAL |

⚠️ **v2.79.0 — le sens de `/lighting/on`\|`/off` change, les routes restent** : avant ce lot, elles
posaient un forçage temporaire non persisté ; elles **choisissent désormais un mode**, persisté en
NVS et publié en MQTT. Voir [API.md](../API.md#post-lightingon-lightingoff--write).

## MQTT / Home Assistant

| Entité | Topic état | Topic commande |
|---|---|---|
| `select.*_lighting_mode` (**feature-162**, 3 options Allumé/Éteint/Programme) | `{base}/lighting_mode` | `{base}/lighting_mode/set` |
| `switch.*_lighting` (état réel du relais) | `{base}/lighting_state` | `{base}/lighting/set` (ON/OFF, persisté désormais) |
| `{base}/lighting_schedule` (lecture dérivée, **sans commande** depuis feature-162) | `{base}/lighting_schedule` | — |
| `{base}/lighting_start` / `{base}/lighting_end` | idem | `.../set` |

⚠️ **Rupture assumée (feature-162)** : l'ancien select 2 options « Mode Éclairage »
(Programmation/Désactivé, `bug-ha-eclairage-select`) est **supprimé**, pas aliasé — voir
[docs/MQTT.md](../MQTT.md) et [ADR-0109](../adr/0109-atterrissage-de-migration-et-alias-ha-portee-a-une-energie.md).

## Logique d'horaire pure (`schedule_logic`)

[feature-040](../../specs/features/done/feature-040-eclairage-horaire-testable.md) a appliqué le
pattern **Humble Object** ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md))
à la décision d'horaire de l'éclairage, en réutilisant le module pur
[`src/schedule_logic.h`](../../src/schedule_logic.h) / [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp)
créé pour la filtration (feature-038). **feature-162** y ajoute l'énum `LightingMode` (`On`, `Off`,
`Programme`, `Inconnu`) et ses trois fonctions de correspondance pure :

| Fonction | Rôle |
|----------|------|
| `LightingMode parseLightingMode(const char* token)` | Jeton (minuscules) → sens. **Site unique** de correspondance forward. `nullptr` ou jeton non reconnu → `Inconnu`. |
| `const char* lightingModeToken(LightingMode mode)` | Réciproque (sens → jeton). `Inconnu` → `"off"` par repli fail-closed (ce mode n'est normalement jamais choisi, seulement produit par une décision). |
| `LightingMode migrateLightingMode(bool lightEnabled, bool lightScheduleEnabled)` | Migration one-shot NVS (voir ci-dessus). |
| `bool decideLightingOn(LightingMode mode, bool haveTime, int nowMin, int startMin, int endMin, bool currentlyOn)` | Décision marche/arrêt. **Switch exhaustif, sans `default`** : un mode non traité ne compile pas. |

`decideLightingOn()` fait **exactement** ce que son nom de mode annonce :

- `On` → `true` ; `Off` → `false` ;
- `Programme` → `isMinutesInRange(nowMin, startMin, endMin, true)` si `haveTime`, **sinon conserve
  `currentlyOn`** (pas de faux toggle pendant un OTA / perte RTC) ;
- `Inconnu` → conserve `currentlyOn` (fail-safe de **décision** ; `ensureTimesValid()` coerce
  normalement tout jeton inconnu vers `Off` en tête d'`update()` — ce cas protège une course entre
  lecture et coercition, même motif que `decideFiltrationRun`).

### Divergence intentionnelle `start == end`

Le paramètre `equalMeansAlways` de `isMinutesInRange` distingue les deux domaines sur la plage dégénérée `start == end` :

| Domaine | `equalMeansAlways` | `start == end` signifie |
|---------|--------------------|--------------------------|
| **Filtration** | `false` (défaut) | plage invalide → **jamais** (relais OFF) |
| **Éclairage** | `true` | **allumé toute la journée** |

Cette divergence est **voulue** et ne doit pas être unifiée. **Inchangée** par feature-162.

### Testabilité native

`schedule_logic.cpp` reste couvert à **100 % des lignes**. **94 `RUN_TEST` au total** dans
`test/test_native_schedule/test_schedule_logic.cpp` (chiffre mesuré au 2026-10-14, à **recompter**
plutôt qu'à recopier — cf. CLAUDE.md ADR-0073), dont **15 dédiés** à `LightingMode` ajoutés par
feature-162 : matrice complète des 4 modes × `decideLightingOn`, gel sur heure indisponible
réservé au seul mode Programme, bornes égales = toujours allumé, `Inconnu` conserve l'état,
`parseLightingMode`/`lightingModeToken` (jetons valides, casse, troncature, aller-retour), les
quatre combinaisons de `migrateLightingMode` et la garantie « jamais Programme quand
`light_sched_en` était faux ». Voir [BUILD.md](../BUILD.md) pour `pio test -e native`.

## Parité `/data` ↔ WebSocket, par expression

`lighting_mode`, `lighting_enabled` et `lighting_schedule_enabled` sont vérifiés **par expression**
(pas seulement par présence de clé) entre `src/web_routes_config.cpp` (`/get-config`) et
`src/ws_manager.cpp` (trame WS `config`) par `test_h_lighting_expression_partagee_identique_des_deux_cotes`
(`test/test_native_ws_config_parity/test_ws_config_parity.cpp`) — même mécanisme d'extraction de
texte source que `test_native_ws_data_parity` (ADR-0099), pour fermer l'angle mort d'une même clé
présente des deux côtés mais **calculée différemment** (4ᵉ récidive du projet sur ce motif, dont
une forme invisible au grep).

## Cas limites

- **`featureEnabled = false`** : le relais est forcé à LOW, UI masquée.
- **Mode Programme, `start == end`** : **allumé toute la journée** (`isMinutesInRange(...,
  equalMeansAlways=true)`) — divergence voulue avec la filtration.
- **Plage traversant minuit** (`start > end`, ex. 22:00 → 06:00) : gérée.
- **Heure indisponible en mode Programme** (`haveTime=false`) : conserve l'état courant.
- **Jeton de mode inconnu en NVS** (config corrompue, client legacy) : `ensureTimesValid()` coerce
  fail-closed vers `Off` en tête d'`update()`, avec un `warning` journalisé.

## Fichiers liés

- [`src/schedule_logic.h`](../../src/schedule_logic.h), [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) — logique d'horaire pure partagée filtration/éclairage, `LightingMode` et ses correspondances
- [`src/lighting.h`](../../src/lighting.h), [`src/lighting.cpp`](../../src/lighting.cpp)
- [`src/config.h`](../../src/config.h) — struct `LightingConfig`, `parseLightingModeFrom()`
- [`src/constants.h`](../../src/constants.h) — `kLightingRelayPin = 27` (PCB v2)
- [ADR-0012](../adr/0012-mapping-gpio-pcb-v2.md) — mapping GPIO PCB v2
- [ADR-0107](../adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md) — état visible et persistant plutôt que suspension invisible
- [ADR-0108](../adr/0108-basculer-le-mode-est-une-commande-injecter-est-un-geste.md) — basculer un mode est une commande d'accueil
- [ADR-0109](../adr/0109-atterrissage-de-migration-et-alias-ha-portee-a-une-energie.md) — atterrissage de migration (Programme) et suppression (pas alias) de l'entité HA
- [page-lighting.md](../features/page-lighting.md) — UI correspondante
