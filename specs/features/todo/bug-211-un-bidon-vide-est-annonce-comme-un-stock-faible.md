## Métadonnées
- **ID** : bug-211
- **Slug** : un-bidon-vide-est-annonce-comme-un-stock-faible
- **Type** : bug (libellé — ⛔ aucune logique de dosage modifiée)
- **Statut** : todo
- **Priorité** : à définir
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-11-22
- **Spec(s) liée(s)** : [bug-199](../doing/bug-199-deux-conventions-decimales-sur-le-meme-ecran.md)
  (⭐ **c'est son étude d'ergonomie qui a sorti ce sujet de son périmètre, à dessein**)

## Domaines touchés
- [x] Frontend (`data/`) — un libellé, à **deux** endroits
- [ ] Régulation · [ ] Capteurs · [ ] MQTT · [ ] Web

## Agents requis
- [x] ux-ui-designer · [x] web-ui-developer · [x] test-engineer · [x] code-reviewer
- [x] **doc-writer** — ⚠️ un libellé **visible** change

## Description
Quand le bidon est **vide**, l'écran annonce **« Stock faible (0,0 L) »**.
⇒ ⭐ *« Faible » dit « pensez à en racheter ». « Vide » dit « la régulation ne peut plus doser ».*
⛔ Ce n'est pas la même information, et c'est la seconde qui compte.

## Observé — 2026-11-22, sur l'appareil réel
Capture du tableau de bord à 390 px, carte **ORP** : **« Stock faible (0.0 L) »**, dans l'état
**courant** de la machine — ⛔ pas un cas limite à provoquer.

## 🔴 Les DEUX emplois — ⛔ indissociables
| Fichier | Ce qui s'affiche | Où |
|---|---|---|
| `data/app.js:5143` | `Stock faible (${…} L)` | bandeau de la carte pH/ORP du **tableau de bord** |
| `data/app.js:5336` | `status.textContent = isLow ? 'Stock faible' : 'OK'` | **badge** des cartes de dosage |

⛔ **Traiter l'un sans l'autre est interdit** : on obtiendrait « Bidon vide » d'un côté et
« Stock faible » de l'autre, pour le même état. ⭐ C'est **exactement** la seconde forme que le
projet combat — *un design system appliqué à moitié rend l'interface MOINS cohérente qu'avant*.

## Comportement attendu (proposition de l'ergonomie, ⛔ à valider)
- reste **nul** (ou arrondi à 0,0 L, c'est-à-dire sous ~50 mL) ⇒ **« Bidon vide »**
- sinon ⇒ **« Stock faible (0,3 L) »** *(inchangé)*
⚠️ Le seuil « sous 50 mL » est une **proposition**, ⛔ pas une mesure. À arrêter.

## ⚠️ Ce qu'il faut vérifier avant de rédiger le texte
🔴 **Que se passe-t-il réellement quand le bidon est vide ?** ⛔ Ne pas l'écrire sans l'avoir lu
dans le code. L'appareil **compte** un volume délivré, ⛔ il ne **mesure** pas le niveau du bidon.
⇒ « Vide » signifie ici *« le compteur dit qu'il ne reste rien »*, ⛔ pas *« l'appareil a constaté
que le bidon est vide »*. ⚠️ **Le texte ne doit pas affirmer plus que ça.**
ⓘ Et le projet a déjà mesuré que ce compteur **dérive** : biais relevé sur la pompe ORP, deux
campagnes, ⛔ ni le modèle en % ni le modèle par démarrage n'est constant.

## Contraintes
- ⛔ **Aucune logique de dosage touchée** : seuls des libellés changent.
- ⛔ Le badge reste un `state-badge--warn` : ⚠️ ne pas inventer un troisième état visuel.
- ⛔ Pas de point décimal (acquis de bug-199). ⚠️ « Bidon vide » **n'affiche aucun nombre** : c'est
  aussi ce qui le rend plus clair que « 0,0 L ».
- 390 px d'abord — ⭐ « Bidon vide » (10 car.) est **plus court** que « Stock faible (0,3 L) ».

## Livrables — check-list
- [ ] A1 — lire le code : d'où vient le « reste », et ce que « vide » veut **honnêtement** dire
- [ ] B1 — les **deux** emplois changés **ensemble**
- [ ] D1 — vecteurs : reste nul · reste faible · reste normal · reste **absent**.
      **Mutation** : ne changer qu'un seul des deux emplois ⇒ un vecteur **meurt**
- [ ] D2 — capture 390 px, avant / après
- [ ] C1 — `docs/features/page-dashboard.md`, `page-dosages.md`, `CHANGELOG`
