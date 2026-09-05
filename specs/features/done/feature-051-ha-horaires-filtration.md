# Feature-051 — Heures de filtration modifiables depuis Home Assistant

- **Type** : feature (MQTT/HA) — précédent : feature-050
- **Statut** : done
- **Version cible** : 2.16.0
- **Créée** : 2026-07-06 (suite du point 2 du rapport filtration HA)
- **Domaines** : mqtt_manager.* uniquement — pas de dosage (pas de pool-chemistry ; la filtration conditionne le dosage mais aucune garde n'est affaiblie : le dosage teste l'état live `filtration.isRunning()`, pas le planning)

## Périmètre

Exposer `filtration_start` / `filtration_end` (HH:MM) à HA en entités **`text` éditables**, complétant le select mode filtration.

- **États** : topics retain `{b}/filtration_start`, `{b}/filtration_end` publiés par `publishFiltrationState()` (valeurs `filtrationCfg.start`/`end`).
- **Commandes** : `{b}/filtration_start/set`, `{b}/filtration_end/set` → handlers drainCommandQueue.
- **Discovery** : 2 entités `text` (composant `text/`) avec `pattern` `^([01][0-9]|2[0-3]):[0-5][0-9]$`, min 5 / max 5.
- **Handler** : validation HH:MM via `timeStringToMinutes()` (schedule_logic, -1 = invalide → warning + ignore) ; sous configMutex : écrire la valeur, effacer les overrides `forceOn/forceOff` (comme `/save-config` quand le planning change), `filtration.update()` pour appliquer, `saveMqttConfig()`, `publishFiltrationState()`. Pattern des handlers filtration existants + gardes feature-027 (timeout mutex → resync).
- **Sémantique** (comme l'UI) : les heures n'ont d'effet réel qu'en mode **Programmation** (`manual`). En mode **Auto**, elles sont recalculées par la température → une saisie HA en mode auto sera écrasée au prochain `filtration.update()` et l'état republié montrera la valeur recalculée (consultation, auto-corrigé). À documenter.
- Le broadcast WS config (bug-sync-ws-config, 2.14.1) propage automatiquement le changement à l'UI web (commande non-Reboot).

## Critères d'acceptation

- [ ] 2 entités `text` « Filtration début / fin » dans HA, valeurs = horaires courants (on-target).
- [ ] Saisir 07:30 en mode Programmation depuis HA → planning appliqué, reflété dans l'UI web (on-target).
- [ ] Saisie invalide (ex. `25:00`, `7h30`) → ignorée + warning, état HA resynchronisé sur la valeur réelle.
- [x] Aucun chemin de dosage touché ; contrats existants inchangés.
- [x] `pio run` sans warning, 185 tests natifs verts.

## Statut : done (2026-07-06)

### Notes d'implémentation

- **Fichiers** : `src/mqtt_manager.cpp` (topics filtration_start/end, souscriptions, handler groupé `FiltrationStart`/`FiltrationEnd`, `publishFiltrationState()` enrichi à 4 topics + publish périodique, 2 entités discovery `text`, include `schedule_logic.h`), `src/mqtt_manager.h` (champs `MqttTopics` + `InboundCmdType`), `src/version.h` → 2.16.0.
- **Handler** : validation `HH:MM` via `timeStringToMinutes()` **avant** prise mutex (invalide → warning + resync `publishFiltrationState()`, pas de `Give` à apparier). Sous `configMutex` (timeout borné feature-027 → resync) et **si changement** : écriture cible (`isStart` + `String&`), efface `forceOn`/`forceOff`, `saveMqttConfig()`. Hors mutex : `filtration.update()` + `publishFiltrationState()`. `filtration.update()` hors mutex mais sur `loopTask` (pas de concurrence avec `loop()`).
- **Validation** : `pio run` SUCCESS, 0 warning ; 185/185 tests natifs verts.
- **Revue code-reviewer** : ✅ Approved (aucune issue bloquante ; concurrence, gardes mutex, resync et anti-pollution `doc.clear()` conformes ; aucun chemin de dosage touché).
- **Reste on-target** (nécessite matériel/HA) : voir les 2 entités `text` dans HA avec les valeurs courantes, saisir 07:30 en mode Programmation et vérifier l'application + reflet UI web, saisie invalide ignorée avec resync.
