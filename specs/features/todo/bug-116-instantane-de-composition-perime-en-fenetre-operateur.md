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
- [ ] ~~Aucune ligne du chemin de dosage modifiée~~ — **critère CADUC, voir note
      2026-10-08 ci-dessous**

## ⚠️ Mise à jour du 2026-10-08 — critère « aucune ligne modifiée » devenu caduc

`bug-115` (v2.61.0, même chaîne de sorties anticipées) a changé la **structure**
d'`update()` : l'ancien corps est renommé `updateDosingCore(now)` et un nouveau
`update()` l'enveloppe (point de passage unique de la comptabilité pompe, voir
[ADR-0091](../../docs/adr/0091-comptabilite-hors-du-corps-decisionnel-dupdate.md)).
Le critère d'acceptation initial de **cette** spec — « aucune ligne du chemin de
dosage modifiée » — est donc **matériellement impossible à tenir** : toute
implémentation de `bug-116` touchera forcément des lignes de ce chemin, ne
serait-ce que pour invalider l'instantané dans `updateDosingCore()` ou dans une des
sorties anticipées qu'elle renomme.

**Reformulation à faire au moment de la reprise de cette spec** (pas avant — voir
ci-dessous) : remplacer ce critère par un critère de **non-régression du
comportement de dosage** (volumes injectés, cadence de décision, budget horaire
inchangés), et non plus d'absence de modification textuelle.

Arbitrage (agent principal, 2026-10-08) : les deux défauts restent **séparés**
(avis `pool-chemistry` — `bug-115` touche l'**écriture** des compteurs de sécurité,
`bug-116` est de la **lecture** seule ; les fusionner rendrait illisible la fenêtre
de surveillance de `bug-115`). **`bug-116` est repris après la fenêtre de
surveillance de `bug-115`** (clôture au plus tôt le 2026-10-11, voir le statut de
`bug-115` dans `specs/features/done/`).

## Notes pour agent
Un commentaire au-dessus de `composeDose()` (`src/pump_controller.cpp`) documente
cette fenêtre **et** le défaut voisin `bug-115` — **désormais marqué corrigé** dans
ce commentaire (v2.61.0). **Les deux traces doivent rester au même endroit** :
quiconque touche à `update()`/`updateDosingCore()` doit trouver les deux
avertissements côte à côte, sinon il rejouera l'un ou l'autre.
