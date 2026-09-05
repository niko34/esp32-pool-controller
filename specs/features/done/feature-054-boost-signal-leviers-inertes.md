# Feature-054 — Boost : signaler les leviers inertes à l'activation

- **Type** : feature (finition feature-053) — UX/observabilité, aucune logique de dosage
- **Statut** : done — firmware source de vérité, 2 canaux (log + réponse HTTP), toast UI 4 cas. Build/tests supposés verts. Reste à valider on-target : les 3 cas dégradés (filtration non gérée, ORP non automatique, les deux).
- **Version cible** : 2.18.1
- **Créée** : 2026-07-07 (retour utilisateur sur l'edge « filtration non gérée + continu »)
- **Domaine** : config.cpp (log), web_routes_control.cpp (réponse), data/app.js (toast) — **pas de pool-chemistry** (aucun chemin de dosage ni garde modifié)

## Contexte

Le Boost a deux leviers : filtration prolongée et surchloration. Chacun peut être **inerte** selon la config :
- Filtration NON prolongée si `filtrationCfg.enabled == false` (filtration non gérée par PoolController → le forçage Boost n'est jamais atteint, retour anticipé dans filtration.update()).
- Chlore NON relevé si `orpRegulationMode != "automatic"` (l'effet cible/limite est gaté sur automatic).

Aujourd'hui le Boost s'active sans rien dire quand un levier (ou les deux) est inerte → l'utilisateur croit surchlorer alors qu'il ne se passe peut-être rien. Objectif : **le signaler**, sans changer le comportement de dosage.

## Comportement attendu

Le firmware est la source de vérité (calcule les leviers effectifs), l'UI affiche, le log couvre l'activation via HA.

1. **config.cpp `startBoost()`** : après activation, logs edge selon l'état :
   - `!filtrationCfg.enabled` → `warning` « Filtration non gérée par PoolController — le Boost ne prolonge PAS la filtration ».
   - `orpRegulationMode != "automatic"` → `warning` « Régulation ORP non automatique — le Boost ne relève PAS le chlore ».
   (Couvre les activations HTTP ET MQTT/HA.)
2. **web_routes_control.cpp `POST /boost/start`** (réponse 200) : ajouter `filtration_extended` (= filtrationCfg.enabled) et `chlorine_boosted` (= orpRegulationMode=="automatic") au JSON.
3. **data/app.js** (activation OK) : toast adaptatif d'après ces 2 booléens :
   - les deux vrais → message existant « Boost activé — vérifiez le taux de chlore avant la baignade. » (info).
   - filtration inerte seule → « Boost activé (surchloration seule) : la filtration n'est pas gérée par PoolController. Vérifiez le taux de chlore avant la baignade. » (info).
   - chlore inerte seul → « Boost activé (filtration prolongée seule) : la régulation ORP n'est pas en mode Automatique, le chlore n'est pas relevé. » (warning).
   - les deux inertes → « Boost sans effet : filtration non gérée et régulation ORP non automatique. » (warning).

## Hors périmètre
- Aucune modification du comportement de dosage/filtration/limites (purement informatif).
- Blocage de l'activation si leviers inertes (on informe, on n'empêche pas — l'utilisateur peut vouloir seulement l'un des deux).

## Critères d'acceptation
- [ ] Activer Boost avec filtration non gérée → log warning + toast « surchloration seule » (on-target).
- [ ] Activer Boost avec ORP non automatique → log warning + toast « filtration prolongée seule ».
- [ ] Les deux inertes → toast « Boost sans effet » (warning).
- [ ] Config nominale → message inchangé.
- [x] Aucun chemin de dosage/garde modifié.
- [x] `pio run` sans warning, 199 tests natifs verts, `node --check` + `build_fs.sh` OK.
