# ADR-0039 — Offset logiciel de mesure pH appliqué en sortie des accesseurs

- **Statut** : Accepté
- **Date** : 2026-08-15
- **Décideurs** : architect, pool-chemistry, embedded-developer
- **Spec(s) liée(s)** : feature-067 (offset de mesure pH)

## Contexte

La mesure pH du bassin présente un décalage constant attribué à l'**implantation de la sonde** dans le circuit hydraulique (position, potentiel électrique de l'eau) — et non à un défaut d'étalonnage électrochimique. Recalibrer le module Atlas EZO pour compenser fausserait la calibration pH 4 / pH 7 elle-même : la sonde deviendrait « juste » à un point et fausse partout ailleurs, et le diagnostic de pente (`Slope,?`) perdrait son sens.

Il faut donc une correction **logicielle, réversible et visible**, strictement séparée de la calibration EZO : `pH corrigé = pH brut EZO + ph_offset`, bornée à ±0,50 pH, persistée en NVS.

Reste à trancher **où** l'appliquer dans la chaîne de mesure. Cette chaîne comporte trois étages :

```text
EZO pH ──► _lastPh (brut) ──► SensorFilter (médiane 13 + EMA + rejet de pics + latch EMI) ──► accesseurs ──► PID / UI / MQTT / historique
```

Le point d'application n'est **pas** un détail d'implémentation : il détermine la latence de prise en compte, la sémantique de chaque valeur publiée (WebSocket, MQTT, Home Assistant), et la nature des mesures sur lesquelles les filets de sécurité chimique peuvent encore raisonner.

Un précédent existe dans le projet, `mqttCfg.tempCalibrationOffset`, qui applique **à l'ingestion** un offset de température. La question posée est de savoir s'il fait jurisprudence.

## Décision

L'offset pH est appliqué **EN SORTIE des accesseurs** de `SensorManager`, jamais à l'entrée du filtre.

1. `SensorFilter` (module pur) continue de recevoir et de traiter le signal **brut**. Il n'est **pas modifié**.
2. Les accesseurs `getPh()`, `getPhMedian()`, `getPhFiltered()` ajoutent l'offset à leur valeur de retour (`applyPhOffset()`, logique pure, `NaN in → NaN out`).
3. **Règle mnémotechnique verrouillée** : *seuls les accesseurs suffixés `*Raw` renvoient du brut capteur*. `getPhRaw()` cesse d'être un alias de `getPh()` ; `getPhFilteredRaw()` est ajouté, réservé aux **filets de sécurité chimique**, avec une **garde d'âge** (`NaN` si le filtre n'est pas `ready()`).
4. Les **filets de sécurité chimique** raisonnent exclusivement sur le brut : plancher de refus d'injection d'acide (6,80 / 6,90 selon l'ampleur de l'offset), inhibition du dosage ORP au-dessus de 7,60, fail-closed sur pH brut inconnu en régulation automatique.
5. **Aucune entité MQTT n'est renommée** : `poolcontroller_ph` et `poolcontroller_ph_filtered` portent la valeur **corrigée**, `poolcontroller_ph_raw` reste le **brut**. Aucune entité `ph_offset` n'est créée.

L'équivalence mathématique qui rend ce choix possible — la médiane et l'EMA sont **invariantes par translation**, donc `filtre(x + c) == filtre(x) + c` — est **démontrée par un test natif** exécuté sur le filtre réel, pas seulement affirmée.

## Alternatives considérées

- **Option A — appliquer à l'ingestion, comme `tempCalibrationOffset`** (rejetée). C'est le patron existant du projet, et il paraît naturel de le suivre. Il ne l'est pas : l'offset de température est appliqué tôt **par nécessité**, parce que la valeur corrigée est **renvoyée au module EZO** dans la commande `RT,<temp>` pour la compensation thermique du pH — c'est la valeur corrigée qui doit partir sur le bus I²C. Le pH corrigé, lui, n'est **réinjecté nulle part** : rien ne force à corriger tôt. Corriger à l'ingestion aurait trois défauts concrets : (1) un changement d'offset serait **dilué sur toute la fenêtre de lissage** (13 échantillons ≈ 65 s) alors que l'utilisateur attend un effet immédiat après avoir enregistré ; (2) les **seuils de rejet de pics** et le **latch EMI** se retrouveraient calés sur un signal translaté, ce qui, à offset proche des bornes, décale la fenêtre de plausibilité ; (3) la trace de diagnostic devrait reconstituer le brut par soustraction. Le patron température n'est donc **pas transposable** — la divergence est assumée et documentée, faute de quoi un lecteur y verra une incohérence.

- **Option B — injecter l'offset dans `SensorFilter`** (rejetée, et interdite). `SensorFilter` est un module **100 % pur**, sans Arduino, sans logger, couvert par les tests natifs ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)). Lui faire lire une variable de configuration globale casserait sa pureté et sa testabilité, pour un gain nul (le résultat est identique par invariance par translation).

- **Option C — corriger uniquement à l'affichage (UI et MQTT), laisser la régulation sur le brut** (rejetée). Le système régulerait alors sur une valeur que l'utilisateur ne voit nulle part, et l'écran afficherait un pH que la régulation ne cherche pas à atteindre. C'est précisément le scénario que la feature veut supprimer.

- **Option D — renommer les entités MQTT pour signaler le changement de sémantique** (rejetée). Renommer `poolcontroller_ph` casserait toutes les automatisations Home Assistant, tous les dashboards et tout l'historique long terme des utilisateurs — pour un décalage de mesure de ±0,50 pH au maximum. La marche dans les historiques HA est **acceptée et documentée** ([CHANGELOG 2.20.0](../../CHANGELOG.md)) ; elle ne survient d'ailleurs que le jour où un offset non nul est saisi.

- **Option E — publier une entité HA `ph_offset`** (rejetée). Elle aurait facilité la lecture des historiques HA, mais aurait ajouté un **point de commande potentiel sur l'entrée de la régulation chimique** en dehors du chemin validé (`POST /save-config`, auth CRITICAL, validation de plage, journalisation). L'offset reste exposé par `/get-config`, par le message WebSocket `config`, et cité dans les messages d'alerte `ph_abnormal`, `ph_raw_floor` et `orp_ph_raw_inhibit`.

- **Option F — écrêter silencieusement une valeur hors plage** (rejetée). C'est le patron de `max_cycles_per_day`. Appliqué ici, il produirait un **mensonge de mesure** : l'utilisateur croirait avoir enregistré ±0,80 alors que la régulation appliquerait ±0,50. La route répond donc **HTTP 400** — première exception de `/save-config` à ce patron, assumée.

## Conséquences

### Positives

- **Prise en compte immédiate** : l'offset est visible dès l'enregistrement, sans purge de la fenêtre de lissage ni redémarrage.
- **Filtre intact** : latch EMI, seuils de rejet de pics, détection de capteur figé et tests natifs de `SensorFilter` sont inchangés — zéro régression sur la partie la plus délicate de la chaîne de mesure.
- **Diagnostic honnête** : à tout instant on peut distinguer ce qui vient de l'EZO (`phRaw`, `{base}/ph_raw`, trace `/debug/sensor_trace`) de ce qui vient de la correction (`phCorrected`).
- **Filets de sécurité indépendants de l'offset** : parce qu'ils lisent le brut, ils restent valides même si l'offset est saisi à l'envers — c'est-à-dire exactement dans le scénario qu'ils doivent couvrir.
- **Contrat MQTT/HA préservé** : aucune entité renommée, aucune automatisation cassée.

### Négatives / dette assumée

- **Deux familles d'accesseurs pH** à ne pas confondre. La règle « seuls les `*Raw` sont bruts » est simple, mais elle doit être **connue** : tout nouveau consommateur du pH doit choisir explicitement sa famille. `getPhRaw()` ayant longtemps été un alias pur de `getPh()`, du code écrit avant la v2.20.0 pouvait les utiliser indifféremment.
- **Une exception à la règle** dans `/debug/sensor_trace`, où `phFiltered` est **brut** (c'est le but de l'outil) — documentée aux deux endroits.
- **Marche dans les historiques Home Assistant** et **discontinuité de l'historique interne** (`/history.json` enregistre le corrigé ; les points antérieurs restent en brut) le jour de l'activation d'un offset. Non réversible.
- **Divergence de style** avec `tempCalibrationOffset` (ingestion) et avec `max_cycles_per_day` (écrêtage silencieux). Deux justifications à porter dans la doc en permanence.

### Ce que ça verrouille

- **Sémantique des entités MQTT** : `{base}/ph` et `{base}/ph_filtered` = **corrigé**, `{base}/ph_raw` = **brut**. Contrat public : tout changement ultérieur exige un nouvel ADR.
- **Règle `*Raw`** : tout futur accesseur de mesure pH doit s'y conformer — brut ⇒ suffixe `Raw`, sinon corrigé.
- **`SensorFilter` reste pur** : aucune configuration applicative ne peut y entrer ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)).
- **Les filets de sécurité chimique lisent le brut**, jamais le corrigé. Un futur filet qui raisonnerait sur la valeur corrigée serait aveugle au scénario d'offset erroné.
- **`getPhFilteredRaw()` conserve sa garde d'âge** : `filtered()` n'expirant jamais de lui-même, la retirer rendrait possible un faux plancher (ou le masquage d'un vrai) sur une EMA fantôme.
- **Écrire l'offset ne réinitialise aucun compteur de dosage** — sinon des enregistrements répétés contourneraient les limites journalières et la pause anti-cycling.
- **La calibration interne du module Atlas EZO n'est jamais touchée** par ce mécanisme, et l'écran de calibration n'affiche que du brut.

## Références

- **Correctif lié (2026-10-05, v2.55.1)** : la priorité des causes de refus pH (butée brute vs garde corrigée) a été rendue **discriminante** côté acide, précisément parce que c'est la valeur **corrigée** qui fait foi à l'écran — conséquence directe du principe posé ici, pas une nouvelle décision. Voir [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#priorité-des-causes-pour-lacide-discriminante-depuis-v2551) et [`bug-135`](../../specs/features/todo/bug-135-butee-brute-ph-se-declenche-sur-une-lecture-isolee.md).
- Code : [`src/ph_offset.h`](../../src/ph_offset.h) (logique pure), [`src/sensors.h`](../../src/sensors.h) / [`src/sensors.cpp`](../../src/sensors.cpp) (accesseurs + copie locale `_phOffset`), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (`isPhRawFloorBlocking`, `isOrpInhibitedByPhRaw`), [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (validation + refus 400), [`src/constants.h`](../../src/constants.h) (`kPhOffset*`, `kPhRaw*`)
- Tests : `test/test_native_ph_offset/` (dont l'invariance par translation du filtre réel)
- Spec : `specs/features/doing/feature-067-offset-mesure-ph.md`
- Doc : [`docs/subsystems/sensors.md`](../subsystems/sensors.md#offset-logiciel-de-mesure-ph--feature-067), [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#filets-de-sécurité-sur-le-ph-brut-feature-067-v2200), [`docs/features/page-ph.md`](../features/page-ph.md#carte--correction-de-la-mesure-ph--feature-067-v2200), [`docs/API.md`](../API.md), [`docs/MQTT.md`](../MQTT.md)
- ADR liés : [ADR-0014](0014-migration-atlas-ezo.md) (calibration dans le module EZO), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable), [ADR-0027](0027-filtre-tendance-centrale.md) (contrat du filtre pH/ORP)
