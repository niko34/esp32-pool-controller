# bug-116 — Instantané de composition périmé pendant OTA, maintenance et assistant de débit

## Métadonnées
| | |
|---|---|
| Type | bug (observabilité) |
| Statut | todo |
| Priorité | Basse — fenêtre bornée et déclenchée par l'opérateur ; aucune conséquence sur le dosage |
| Version cible | à définir |
| Relevé par | `code-reviewer`, 2026-09-07, revue delta de `feature-092` |
| Specs liées | `feature-092` (la carte concernée), `bug-115` (**même chaîne de sorties anticipées**) |

## Description
La carte « Débit d'injection » (`feature-092`, v2.46.0) est alimentée par un instantané
publié à chaque tour de la branche automatique de `update()`. Mais **trois sorties
anticipées reviennent avant cette branche** — OTA en cours (`~:3006`), mode maintenance
(`~:3021`), assistant de mesure du débit (`~:3134`) — **sans invalider `measuresValid`**,
la filtration pouvant tourner.

Pendant ces fenêtres, `/data`, la trame WebSocket et MQTT servent donc l'**instantané du
dernier tour de la branche automatique** comme s'il était courant.

## Contexte
C'est la **forme résiduelle** du défaut bloquant corrigé pendant le cycle `feature-092` :
l'instantané n'était alors publié que sous `shouldDose`, ce qui rendait la carte fausse
la majeure partie du temps (sessions de 7-8 min suivies de 15 min de pause mélange).
Le correctif — publication à chaque tour de la branche automatique — ferme le cas
courant. Il reste ces trois fenêtres.

`code-reviewer` les juge « bornées et déclenchées par l'opérateur, sans commune mesure
avec le défaut initial » — d'où la priorité basse, et le choix **assumé** de ne pas les
corriger dans `feature-092`.

## Comportement attendu
Pendant ces trois fenêtres, la carte doit afficher une **absence** (« — ») et non une
valeur périmée. Deux pistes, à arbitrer : invalider l'instantané en entrée de ces
branches, ou étendre la porte de mesure à ces trois états.

## Contraintes techniques
- ⚠️ Le correctif touche `update()`, fonction de dosage : `pool-chemistry` requis, même
  si le changement est purement d'observabilité.
- Ne pas invalider `measuresValid` lui-même : d'autres consommateurs en dépendent, et
  la filtration tourne réellement pendant ces fenêtres.

## Critères d'acceptation
- [ ] Pendant un OTA, en maintenance et pendant l'assistant de débit, les champs de
      composition sont `null` (donc « — » à l'écran), pas des valeurs figées
- [ ] Aucune régression sur le cas courant fermé par `feature-092`
- [ ] Aucune ligne du chemin de dosage modifiée

## Notes pour agent
Un commentaire au-dessus de `composeDose()` (`src/pump_controller.cpp`) documente déjà
cette fenêtre **et** le défaut voisin `bug-115`. **Les deux traces doivent rester au
même endroit** : quiconque touche à `update()` doit trouver les deux avertissements
côte à côte, sinon il rejouera l'un ou l'autre.
