# bug-101 — L'import d'historique est plafonné à 16 Ko, et l'échec est muet

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | bug — limite non documentée + échec non diagnosticable |
| Statut | todo |
| Priorité | Modérée — sans conséquence en régime normal, **bloquante le jour d'une restauration** |
| Découvert | 05/09/2026 ~19:20, en restaurant l'historique après le passage au layout v5 (8 Mo) |
| Version | 2.42.0 |

## Domaines touchés

- `src/web_routes_data.cpp` — enregistrement du handler `/history/import` (~l. 1024)
- `data/app.js` — l'écran d'import côté interface
- `docs/API.md`

## Sécurité chimique

**Non.** Aucun chemin de dosage. Gate `pool-chemistry` non requis.

## Le problème, constaté en vraie grandeur

Après le changement de table de partitions du 05/09, l'historique sauvegardé
(**245 points**) devait être réimporté. Seuls **145 points ont pu l'être** : au-delà,
la route répond **HTTP 413**. Les 100 points les plus anciens (23 → 31 août) sont
restés dans le fichier de sauvegarde.

Le plafond est **16 384 octets exactement** : c'est
`AsyncCallbackJsonWebHandler::_maxContentLength`, initialisé en dur par la
bibliothèque et **jamais surchargé** dans ce projet.

Trois défauts distincts, qu'il ne faut pas confondre :

1. **La limite n'est écrite nulle part.** Ni dans `docs/API.md` (corrigé depuis), ni
   dans l'interface. On la découvre le jour où l'on restaure, c'est-à-dire le pire
   jour possible.
2. **L'échec est muet.** Le `413` est émis par la bibliothèque **avant**
   `REQUIRE_AUTH` et ne passe pas par `sendErrorResponse()` : pas de JSON d'erreur
   maison, donc côté client rien n'explique ce qui s'est passé.
3. **La route REMPLACE l'historique, elle n'ajoute pas.** Un envoi par lots
   successifs écraserait donc le lot précédent — l'utilisateur qui « découpe pour
   contourner » perdrait tout sauf le dernier morceau, en silence.

## ⚠️ Le correctif évident est le mauvais

`setMaxContentLength()` au moment de l'enregistrement suffirait techniquement. Mais
`AsyncCallbackJsonWebHandler` fait un **`calloc(total + 1)`** pour accumuler le corps :
relever le plafond, c'est agrandir d'autant une allocation contiguë, **sur un
appareil dont tout le chantier `bug-080` porte précisément sur la famine 8 bits et
la fragmentation**. Un import de 250 points demanderait ~28 Ko contigus — c'est-à-dire
plus que le plus grand bloc disponible en régime établi (13 300 o mesurés le 05/09).

**Ce serait donc offrir un `abort()` à l'utilisateur exactement quand il restaure ses
données.**

## Pistes à arbitrer

1. **Découpage côté interface** (piste privilégiée) : `data/app.js` envoie des lots
   sûrs et l'API gagne un mode **« ajouter »** distinct du mode « remplacer ».
   C'est le seul chemin qui ne touche pas au budget mémoire.
2. **Streaming côté firmware** : traiter le corps au fil de l'eau plutôt que de
   l'accumuler. Plus propre, plus coûteux à écrire, à chiffrer en flash — mais la
   marge est désormais confortable (1,9 Mo depuis le layout v5).
3. **Format compact dédié à la restauration** (CSV, ou JSON à clés courtes) : le
   fichier persisté utilise déjà `pf`/`of` là où l'API expose `phFiltered`/`orpFiltered`.
   Un import au format persisté diviserait le volume.
4. **Au minimum, et quoi qu'il arrive** : un message d'erreur intelligible côté
   client, et la limite affichée **avant** l'envoi.

## Critères d'acceptation

- [ ] Un historique complet (≥ 250 points) se restaure sans perte silencieuse
- [ ] Un dépassement produit un message compréhensible, pas un échec muet
- [ ] Si un mode « ajouter » est introduit, la distinction avec « remplacer » est
      explicite dans l'API **et** dans l'interface
- [ ] Aucune allocation contiguë supplémentaire notable — à **chiffrer**, pas à supposer
- [ ] Test natif du découpage / de la limite + validation par mutation

## Notes

Trouvé en situation réelle, pas en revue : c'est la restauration après le
changement de partitions qui l'a révélé. La sauvegarde complète des 245 points
reste disponible dans `builds/sauvegarde-avant-flash-8mo/history.json` — les
100 points manquants pourront être réinjectés une fois ce bug corrigé.
