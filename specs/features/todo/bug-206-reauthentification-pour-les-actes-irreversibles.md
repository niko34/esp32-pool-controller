## Métadonnées
- **ID** : bug-206
- **Slug** : reauthentification-pour-les-actes-irreversibles
- **Type** : bug
- **Statut** : todo
- **Priorité** : haute
- **Version cible** : à fixer
- **Dernière mise à jour** : 2026-11-21
- **Spec(s) liée(s)** : [bug-179](../done/bug-179-journal-rendu-en-innerhtml-sous-csp-permissive.md)

## Domaines touchés
- [x] Web — routes HTTP / WebSocket
- [x] Authentification
- [x] OTA
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [ ] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Exiger le mot de passe courant pour les actes irréversibles, au lieu du seul jeton.

## Contexte
Relevé par l'audit P1 de bug-179. `/save-config` écrit `auth_password` et `auth_enabled` **sans demander le mot de passe courant** (`src/web_routes_config.cpp:1659-1665`, le commentaire sur place le signale déjà) et `POST /ota-signature-switch/arm` n'exige que le jeton. ⇒ Tout vol de jeton devient une **perte définitive** de l'appareil. ⚠️ Revérifier les lignes avant de traiter.

## Livrables — check-list
- [ ] A1 — Mot de passe courant exigé pour écrire `auth_password` / `auth_enabled` via `/save-config`
- [ ] A2 — Idem pour `POST /ota-signature-switch/arm`
- [ ] B1 — Invite de saisie dans l'interface (390 px d'abord)
- [ ] C1 — `docs/API.md`, `docs/subsystems/auth.md`

## ADR attendu ?
- **Probable** : oui probable (contrainte sur le contrat d'authentification, critères 1 et 4)
