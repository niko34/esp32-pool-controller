# Feature-052 — Uniformisation écran/HA éclairage avec la filtration

- **Type** : feature (UI + MQTT/HA) — précédents : feature-051 (filtration HA), feature-028 (principe : ne pas stringifier un booléen)
- **Statut** : done — tests verts (185/185), revue **Approved**, `pio run` 0 warning, `build_fs.sh` OK
- **Version cible** : 2.17.0
- **Créée** : 2026-07-06
- **Domaines** : data/ (UI), src/mqtt_manager.* (HA). Backend booléen `scheduleEnabled` CONSERVÉ (pas de rename destructeur — décision validée).

## Périmètre

Aligner la section Programmation de l'écran éclairage sur celle de la filtration, et exposer le planning éclairage à HA (miroir de la filtration).

### P1 — UI (data/index.html + app.js + app.css)
- Renommer le label du select `#lighting_schedule_mode` : « Mode de programmation » → « **Mode d'éclairage** ».
- Renommer les 2 options : « Désactivée »/« Activée » → « **Programmation** » (value inchangée `enabled`) / « **Désactivé** » (value inchangée `disabled`). Ordre : Programmation d'abord (cohérence filtration où le mode « actif » est en tête).
- Alignement visuel sur l'écran filtration (même structure de carte, même affichage conditionnel des heures — la logique JS `lighting-schedule-settings` existe déjà, seul le mapping label/option change).
- AUCUN changement des `value` du select (`enabled`/`disabled`) → `collectXxx`/`loadConfig` inchangés côté logique ; seuls les libellés affichés changent.

### P2 — HA/MQTT (src/mqtt_manager.*)
Exposer le planning éclairage en miroir de la filtration (feature-051) :
- **Mode programmation** : entité HA `switch` (ou `select` 2 options) « Programmation éclairage » sur `{b}/lighting_schedule` (état) + `.../lighting_schedule/set` (commande) → pilote `lightingCfg.scheduleEnabled` (booléen : payload ON/OFF pour un switch — le plus simple et honnête pour un booléen).
- **Heures** : 2 entités `text` « Éclairage début / fin » (pattern HH:MM) sur `{b}/lighting_start`, `{b}/lighting_end` + `.../set`, pilotant `lightingCfg.startTime`/`endTime`.
- Handlers drainCommandQueue : validation (ON/OFF pour le switch ; HH:MM via `timeStringToMinutes` pour les heures), écriture sous configMutex, `saveMqttConfig`, `lighting.update()` (applique), republish. Pattern exact de la filtration (feature-051). Le broadcast WS config (bug-sync 2.14.1) propage à l'UI web.
- Publication d'état : ajouter les 3 topics (schedule, start, end) au cycle périodique + une `publishLightingState()` enrichie.

### Hors périmètre
- Rename backend `scheduleEnabled`/`lighting_schedule_enabled` (booléen conservé — cf. décision).
- Le switch « Éclairage Piscine » ON/OFF existant (contrôle manuel) et la luminosité : inchangés.
- Aucun mode « Auto » éclairage (pas de calcul température).

## Critères d'acceptation

- [x] Écran éclairage : label « Mode d'éclairage », options « Programmation »/« Désactivé », heures affichées en mode Programmation ; comportement identique à avant (le booléen sous-jacent ne change pas). *(UI web `data/` ; écran LVGL reste on-target)*
- [x] HA : entité programmation éclairage (ON/OFF) + 2 entités `text` heures ; éditer depuis HA applique et se reflète dans l'UI web. *(validation on-target avec HA)*
- [x] Saisie horaire invalide → ignorée + warning + resync.
- [x] Backend `scheduleEnabled` booléen inchangé ; `/get-config`/`/save-config` `lighting_schedule_enabled` inchangé.
- [x] `pio run` sans warning, 185 tests natifs verts, `./build_fs.sh` OK.

## Notes d'implémentation

- **P1 (UI)** : relabel pur du select `#lighting_schedule_mode` (« Mode d'éclairage », options « Programmation » `enabled` / « Désactivé » `disabled`, Programmation en tête) + heures en grille 2 colonnes « Début »/« Fin » + hint reformulé. **Aucun changement de `value`** ni de la logique `collectXxx`/`loadConfig` ; le select mappe toujours le booléen `lighting_schedule_enabled` de `/get-config` / `/save-config` (backend intact).
- **P2 (HA/MQTT)** : miroir exact de la filtration (feature-051). Nouveaux topics `{b}/lighting_schedule`(+/set), `{b}/lighting_start`(+/set), `{b}/lighting_end`(+/set) ; 3 entités discovery (1 `switch` + 2 `text` pattern `HH:MM`). `InboundCmdType` `LightingSchedule`/`LightingStart`/`LightingEnd`, handlers avec validation avant mutex et resync HA sur invalide, `lighting.update()` hors mutex sur `loopTask`, `publishLightingState()` enrichie + publish périodique. Broadcast WS `config` (bug-sync v2.14.1) reflète les changements HA dans l'UI web.
- **Décision conservée** : `scheduleEnabled` reste un booléen (pas de stringification — cf. feature-028) ; exposé via entité `switch` HA ON/OFF.
- **Verdict** : code-reviewer **Approved**, `pio run` 0 warning, 185/185 tests, `build_fs.sh` OK. `version.h` → 2.17.0.
- **Reste on-target** (cycles futurs / validation matérielle) : écran LVGL éclairage (miroir des libellés) + vérification effective des 3 entités HA sur une instance Home Assistant réelle.
