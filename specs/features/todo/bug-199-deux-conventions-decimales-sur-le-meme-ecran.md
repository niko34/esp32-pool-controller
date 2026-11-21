## Métadonnées
- **ID** : bug-199
- **Slug** : deux-conventions-decimales-sur-le-meme-ecran
- **Type** : bug
- **Statut** : todo
- **Priorité** : basse
- **Version cible** : à arrêter
- **Dernière mise à jour** : 2026-11-18
- **Spec(s) liée(s)** : [feature-195](../done/feature-195-langage-visuel-du-tableau-de-bord.md) (relevé sur capture, sorti de son périmètre à dessein)

## Domaines touchés
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [ ] doc-writer — seulement si un libellé change

## Description
Depuis feature-195, les mesures du tableau de bord s'écrivent à la **virgule** française
(« 7,3 » · « 24,8 »). Mais plusieurs textes du **même écran**, parfois à quelques pixels,
gardent le **point** décimal anglais : « Stock faible (0.7 L) », « Chloration suspendue :
pH 7.3 — … ».

## Contexte
Relevé le **2026-11-18** sur capture pendant feature-195, puis confirmé en revue de code.
⛔ **Volontairement sorti du périmètre de ce lot**, pour deux raisons énoncées par la revue :
ce n'est pas une occurrence isolée mais un **balayage d'écran**, et ce balayage contient un
piège qui mérite d'être écrit avant d'être improvisé.

Occurrences relevées (⚠️ **à recompter**, la liste n'est pas garantie exhaustive) :
- `data/app.js:4764` — `(rem / 1000).toFixed(1)` dans `buildSensorSegments` ⇒ la bande
  « Stock faible (0.7 L) », **dans la carte pH/ORP refondue**, à quelques pixels d'un héros
  à virgule. C'est l'occurrence la plus visible.
- `data/app.js:5819` — « Chloration suspendue : pH 7.3 — … » dans le bandeau de blocage.

## Comportement attendu
Une seule convention décimale dans tout le texte **affiché** : la virgule française.

## ⛔ LE PIÈGE — à lire avant de toucher une ligne
**`fmtFr` ne doit JAMAIS toucher un `.value` d'`<input>` ni un `dataset`.** Ce sont des
**valeurs machine** : une virgule les casse silencieusement.
Deux endroits au moins sont concernés et ⛔ **ne doivent pas être convertis** :
- `data/app.js:5007-5008` — `(… / 1000).toFixed(1)` écrit dans un `input.value` ;
- `buildPhTargetSegments` — écrit dans `btn.dataset.value`.

⇒ **Règle à poser dans le lot** : la conversion ne s'applique qu'à du texte **rendu**
(`textContent`, `innerHTML`), jamais à une valeur relue par du code.

## Comportement actuel
Deux conventions coexistent sur le même écran depuis la v2.96.0.

## Fichiers potentiellement concernés
- `data/app.js` — recenser **toutes** les occurrences de `toFixed(` suivies d'un affichage
- `docs/DESIGN_SYSTEM.md` — si une règle de formatage y est posée

## Livrables — check-list
- [ ] B1 — Inventaire **exhaustif et compté** des nombres affichés avec un point
- [ ] B2 — Conversion des seules occurrences **affichées**
- [ ] D1 — Garde ou harnais : ⛔ aucun `fmtFr` sur un `.value` ou un `dataset`
- [ ] D2 — Captures avant/après du tableau de bord et des bandeaux

## Critères d'acceptation
- [ ] Aucun nombre **affiché** ne porte de point décimal
- [ ] Aucune valeur **machine** (`.value`, `dataset`) n'a été convertie
- [ ] Les champs de saisie acceptent toujours la saisie et l'enregistrement

## Notes pour agent
- ⚠️ `_fmtPh()` est **le** formateur pH partagé, et `fmtFr()` le formateur générique.
  ⛔ Ne pas en créer un troisième : feature-195 a dû corriger exactement ça en revue
  (deux expressions pour la même consigne pH, divergentes à 7,35).

## 📸 Observé sur l'appareil réel — 2026-11-21

Capture du tableau de bord prise pendant la clôture de
[feature-195](../done/feature-195-langage-visuel-du-tableau-de-bord.md) : le défaut est visible
**deux fois sur le même écran, en même temps**, et dans l'état **courant** de la machine — pas
dans un cas limite à provoquer :

- carte **pH** : « Stock faible (0.3 L) » — à six pixels d'un « 7,3 » à virgule
- carte **ORP** : « Stock faible (0.0 L) » — à six pixels d'un « 556 » et d'une « consigne 530 mV »

⇒ Deux conventions décimales **sur la même carte**, répétées **sur deux cartes voisines**. Le
lecteur voit le point et la virgule dans un même coup d'œil. ⚠️ Cela relève la priorité de fait :
ce n'est pas une occurrence rare, c'est l'écran d'accueil au repos.

⛔ **Le piège à ne pas improviser**, déjà écrit par la revue de code de feature-195 : `fmtFr`
⛔ **ne doit jamais toucher un `.value` ni un `dataset`** — ce sont des valeurs machine qu'une
virgule casse (`data/app.js`, `buildPhTargetSegments`).
