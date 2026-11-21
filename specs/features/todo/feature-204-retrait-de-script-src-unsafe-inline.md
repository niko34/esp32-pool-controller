## Métadonnées
- **ID** : feature-204
- **Slug** : retrait-de-script-src-unsafe-inline
- **Type** : feature
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : à fixer
- **Dernière mise à jour** : 2026-11-21
- **Spec(s) liée(s)** : [bug-179](../done/bug-179-journal-rendu-en-innerhtml-sous-csp-permissive.md)

## Domaines touchés
- [x] Web — routes HTTP / WebSocket
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
Retirer `'unsafe-inline'` de `script-src` dans la CSP, en externalisant les scripts en ligne.

## Contexte
Laissé volontairement en place par bug-179 (v2.97.0). Inventaire mesuré : 4 blocs `<script>` en ligne (`index.html:15` 552 o · `login.html:337` 9 550 o · `wifi.html:415` 7 633 o · `wizard.html:988` 31 032 o) et **3** `onclick=` restants dans `wizard.html` (5 avant, 2 partis avec la notification supprimée). Marge FS ~302 800 o sur 524 288.
⚠️ Risque principal : une externalisation fautive de `login.html` rend l'appareil **inatteignable** (aucun port série dans le conteneur) ; prévoir un essai sur appareil dédié.

## Livrables — check-list
- [ ] A1 — Externaliser les 4 scripts en ligne
- [ ] A2 — Remplacer les 3 `onclick=` de `wizard.html` par des écouteurs
- [ ] A3 — Retirer `'unsafe-inline'` de `script-src` (`src/web_server.cpp:104`) et mettre à jour `docs/subsystems/web-server.md`
- [ ] D1 — Garde CI : aucun script ni gestionnaire en ligne dans `data/`

## ADR attendu ?
- **Probable** : non (suite de [ADR-0136](../../../docs/adr/0136-assainir-le-texte-externe-a-la-source-jamais-dans-le-journal.md))
