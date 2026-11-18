## Métadonnées
- **ID** : bug-196
- **Slug** : le-bandeau-hors-ligne-passe-sous-le-bouton-menu
- **Type** : bug
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : à arrêter
- **Dernière mise à jour** : 2026-11-18
- **Spec(s) liée(s)** : [feature-195](../doing/feature-195-langage-visuel-du-tableau-de-bord.md) (trouvé pendant son étude d'ergonomie)

## Domaines touchés
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [ ] architect
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
À 390 px — **l'écran de référence du projet** — la ligne d'état de l'en-tête
« ● Déconnecté — Modifications non sauvegardées » déborde et **passe sous le bouton
de menu**. Les derniers caractères sont illisibles.

## Contexte
Trouvé le **2026-11-18** par `ux-ui-designer` pendant l'étude de feature-195, sur une
capture de l'état réel de l'appareil, puis **vérifié sur image recadrée**.

⚠️ Le défaut n'apparaît que dans l'état **le plus défavorable** : les deux mentions sont
présentes en même temps (perte de connexion **et** modifications non sauvegardées). C'est
pourquoi il a survécu — l'écran nominal n'affiche qu'une puce verte et « En ligne ».

## Comportement attendu
À 390 px, la ligne d'état ne recouvre jamais le bouton de menu, dans **toutes** les
combinaisons d'état. Au choix de l'étude : raccourcir le libellé, le tronquer avec des
points de suspension, passer à la ligne, ou déplacer la mention « modifications non
sauvegardées » hors de l'en-tête. ⛔ Ne pas la **supprimer** : elle prévient d'une perte
de saisie.

## Comportement actuel
Les deux mentions s'écrivent à la suite sur une seule ligne, sans largeur maximale ni
troncature, et le bouton de menu est positionné par-dessus.

## Contraintes techniques
- ⚠️ **390 px d'abord** ; vérifier aussi à 320 px.
- Compter les caractères : « Déconnecté — Modifications non sauvegardées » = 43 caractères.
- L'en-tête est **partagé par toutes les vues** : un correctif y touche tous les écrans.
  ⇒ Inventaire des emplois obligatoire (cf. règle du design system).

## Fichiers potentiellement concernés
- `data/index.html` — structure de l'en-tête
- `data/app.css` — mise en page de la ligne d'état
- `data/app.js` — composition du texte (`#net-text`)

## Livrables — check-list
- [ ] B1 — Correctif de mise en page
- [ ] B2 — Vérifié à 320, 390 et 1024 px, dans les quatre combinaisons d'état
- [ ] C1 — `docs/DESIGN_SYSTEM.md` si l'en-tête change de règle
- [ ] D1 — Captures avant/après

## Critères d'acceptation
- [ ] À 390 px, aucun caractère de la ligne d'état n'est recouvert par le bouton de menu
- [ ] Idem à 320 px
- [ ] La mention « modifications non sauvegardées » reste lisible et présente
- [ ] Aucune autre vue n'est dégradée par le correctif

## Plan de test manuel
1. Appareil injoignable **et** une modification non enregistrée en cours : les deux
   mentions coexistent — c'est le cas qui reproduit le défaut.
2. Les trois autres combinaisons (en ligne/hors ligne × avec/sans modification).
3. Rejouer à 320 px et à 1024 px.

## Notes pour agent
- ⚠️ La capture se prend **sans** l'option `-n` de `ui_shot.sh` : c'est l'absence de
  WebSocket qui met l'application hors ligne. Avec `-n`, le défaut ne se voit pas.
