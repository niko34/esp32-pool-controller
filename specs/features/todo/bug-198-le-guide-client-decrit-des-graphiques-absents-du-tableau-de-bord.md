## Métadonnées
- **ID** : bug-198
- **Slug** : le-guide-client-decrit-des-graphiques-absents-du-tableau-de-bord
- **Type** : bug
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : à arrêter
- **Dernière mise à jour** : 2026-11-18
- **Spec(s) liée(s)** : [feature-195](../done/feature-195-langage-visuel-du-tableau-de-bord.md) (signalé par son `doc-writer`, hors de son périmètre)

## Domaines touchés
- [ ] Frontend (`data/`) — **aucune modification de code attendue**

## Agents requis
- [x] spec-reader
- [ ] architect
- [ ] web-ui-developer
- [ ] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Le **guide d'utilisation livré aux clients** décrit sur une cinquantaine de lignes des
**mini-graphiques du tableau de bord qui n'existent plus** : deux courbes pH et chlore, un
interrupteur « Brut », des repères de dosage sous les courbes, une infobulle au survol.

## Contexte
Signalé le **2026-11-18** par le `doc-writer` de feature-195, qui a eu la rigueur de dire
qu'il ne l'avait pas traité au lieu de le passer sous silence. **Vérifié ensuite dans le
code** : `data/index.html` ne contient **aucun** élément `<canvas>`, et aucun sélecteur de
mini-graphique. Le compte est de **zéro**.

⚠️ **La dérive est ANTÉRIEURE à feature-195** : `git log -S` date la disparition des
commits `cf5fe33` et `4dd8071`. ⛔ Ne pas l'imputer à la refonte du tableau de bord, qui
n'a rien retiré de tel — elle n'a fait que rendre l'écart visible.

Passages concernés dans `docs/user/src/guide-utilisation.html` (relevés, à revérifier avant
de couper) : l. 371-383 « Deux courbes sur les graphiques pH et chlore » · l. 384-391
l'interrupteur « Brut » · l. 392-409 « Les petits repères en bas des graphiques » ·
l. 410-416 l'infobulle au survol · l. 417-422 la note sur la courbe lissée.

## Comportement attendu
Le guide ne décrit que ce que l'appareil fait. Deux cas à distinguer **avant de supprimer** :
1. **Ces graphiques existent ailleurs** (pages pH, ORP, Historique, carte « Signaux
   capteurs ») ⇒ **déplacer** le texte dans le bon chapitre, ne pas le jeter.
2. **Ils n'existent nulle part** ⇒ supprimer, et vérifier qu'aucune capture du guide ne
   les montre.

⛔ **Vérifier d'abord, couper ensuite.** Une suppression hâtive retirerait au client la
documentation d'une fonction qui existe encore, ailleurs.

## Comportement actuel
Le guide décrit ces graphiques comme faisant partie du tableau de bord.

## Fichiers potentiellement concernés
- `docs/user/src/guide-utilisation.html`
- `docs/features/page-dashboard.md` — ses sections historiques sur les mini-graphiques sont
  signalées périmées par feature-195, mais **pas réécrites**
- `screenshots/` — si une capture montre ces graphiques

## Livrables — check-list
- [ ] C1 — Statuer, passage par passage : déplacer ou supprimer
- [ ] C2 — `guide-utilisation.html` corrigé
- [ ] C3 — `docs/features/page-dashboard.md` : réécrire les sections historiques
- [ ] C4 — Liste des captures à refaire, s'il y en a

## Critères d'acceptation
- [ ] Aucun passage du guide ne décrit un élément absent de l'interface
- [ ] Aucune fonction encore vivante n'a perdu sa documentation au passage
- [ ] `./tools/check_doc_links.sh` passe

## Notes pour agent
- ⚠️ Ce guide est **livré aux clients**. Une documentation qui décrit une fonction absente
  fait douter de tout le reste.
- ⚠️ **Le PDF se régénère avec `./tools/build_user_pdfs.sh`**, jamais à la main, et ⛔ les
  PDF ne se commitent pas.
