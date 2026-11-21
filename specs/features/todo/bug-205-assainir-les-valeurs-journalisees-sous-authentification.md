## Métadonnées
- **ID** : bug-205
- **Slug** : assainir-les-valeurs-journalisees-sous-authentification
- **Type** : bug
- **Statut** : todo
- **Priorité** : basse
- **Version cible** : à fixer
- **Dernière mise à jour** : 2026-11-21
- **Spec(s) liée(s)** : [bug-179](../done/bug-179-journal-rendu-en-innerhtml-sous-csp-permissive.md)

## Domaines touchés
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [x] OTA

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Assainir les valeurs journalisées qui exigent des identifiants et ont échappé à la règle 6 de `check_unauth_log_guard.sh` (variable intermédiaire).

## Contexte
Relevé par l'audit P2 de bug-179. Effet : fausse ligne d'apparence authentique ; ⛔ jamais d'exécution (le rendu est en DOM). Sites : `src/web_routes_ota.cpp:113`, `:133`, `:2137`, `:2155` · `src/web_routes_config.cpp:1366`, `:1368`, `:1393`, `:1492` · `src/main.cpp:1052` · `src/mqtt_manager.cpp:346`, `:489`, `:669`, `:2477` · `src/atlas_ezo.cpp:168`, `:231`. ⚠️ Numéros de ligne relevés le 2026-11-21 : à revérifier avant de traiter.

## Livrables — check-list
- [ ] A1 — Passer chacun des 15 sites par `logSanitizeExternalText`
- [ ] D1 — Étendre la règle 6 (suivi de la variable intermédiaire) ou la règle 6b à ces fichiers
- [ ] D2 — Test natif de non-régression

## ADR attendu ?
- **Probable** : non
