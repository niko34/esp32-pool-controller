# ADR-0116 — Un seuil flottant dérivé se compare avec une tolérance dérivée de la grille de saisie, jamais du bruit machine

- **Statut** : Accepté
- **Date** : 2026-10-17
- **Décideurs** : `pool-chemistry` (verdict du gate), `embedded-developer`, `test-engineer`
- **Spec(s) liée(s)** : aucune (correctif réactif, fausse alerte constatée en production, hors
  cycle de spec) — fait suite à [ADR-0114](0114-correction-mesure-ph-sans-borne-de-reglage.md) et
  [ADR-0113](0113-filets-ph-bruts-absolus-independants-de-loffset.md) (feature-168), qui ont
  introduit les seuils concernés

## Contexte

Le 2026-10-17 au soir, l'appareil de l'utilisateur a journalisé en `CRITICAL`, et **publié en
MQTT**, l'alerte suivante, à un offset de mesure pH de **+1,20** :

> « Impasse de régulation : … exige une mesure brute de 6,00, sous le refus d'acidifier fixé à
> 5,50. »

**6,00 n'est pas sous 5,50.** L'alerte était fausse, et le message se contredisait lui-même. Une
alerte `CRITICAL` fausse n'est pas un défaut cosmétique : elle a pu déclencher une automation
Home Assistant, et elle use la confiance que l'utilisateur accorde aux alertes réelles qui
suivront.

**Cause racine, mesurée** : `kPhOffsetImpasseThreshold` est un seuil **dérivé** en virgule
flottante simple précision (`kPhFloorCorrected − kPhRawFloorAbsolute` = `7,20f − 0,50f − 5,50f`),
qui ne vaut **pas** exactement 1,2 mais ≈ **1,1999998**. `isPhRegulationImpasseLow()` comparait
l'offset en vigueur à cette constante **au bit près** (`offset > kPhOffsetImpasseThreshold`). Un
offset saisi de +1,20 (lui-même ≈ 1,2000000 une fois passé en `float`) s'est trouvé, par une
coïncidence de représentation, du **mauvais côté** d'un seuil censé valoir la même chose —
l'écart réel entre les deux nombres comparés était d'environ **2,4 × 10⁻⁷ pH**, c'est-à-dire du
bruit de représentation, pas une impasse de régulation.

Un second défaut, de même nature mais sans conséquence observée, affectait le côté haut
(`isPhRegulationImpasseHigh()`) : la comparaison y était **large** (`<=`), ce qui incluait le
seuil exact dans la zone d'impasse — alors que, pour la même raison que ci-dessus, la valeur
exacte du seuil n'a aucun sens métier particulier en flottant. Le message de ce côté était,
par ailleurs, correct — c'est un constat du gate, pas une déduction de symétrie.

⭐ **L'enseignement le plus transférable de cet incident ne porte pas sur le pH, mais sur les
tests.** Avant ce correctif, les tests natifs qui couvraient `isPhRegulationImpasseLow()`
comparaient `kPhOffsetImpasseThreshold` à lui-même ou à des décalages calculés **à partir de la
même constante** (`kPhOffsetImpasseThreshold + 0.01f`) — jamais au **littéral** qu'un
utilisateur saisit réellement (`1.20f`). Un test qui calcule ses deux membres de la même façon
ne peut pas détecter une divergence de représentation entre eux : il ne teste rien sur ce point
précis. Le défaut n'a été trouvé qu'en observant l'appareil réel.

## Décision

**Un seuil de comparaison dérivé par un calcul en virgule flottante ne se compare jamais à une
autre valeur flottante par égalité stricte ou par une inégalité sans marge.** La marge
(epsilon) à appliquer se dérive de la **grille de saisie** qui produit les valeurs comparées à
ce seuil — jamais d'une estimation du bruit machine, et jamais d'une constante arbitraire non
sourcée (ADR-0098).

Application immédiate (`ph_guard_logic.h`, `constants.h`) : `kPhImpasseEpsilon = kPhOffsetStep ×
0,5` (= **0,05**). Tout offset pH passe par `roundPhOffsetToStep()` avant d'atteindre ces
comparaisons : il est donc toujours un multiple de `kPhOffsetStep` (0,10). Une impasse **réelle**
est par construction à au moins `kPhOffsetStep` du seuil théorique — aucune valeur de la grille
ne peut s'en approcher davantage. Fixer la tolérance à la **moitié** du pas :

- absorbe le bruit de représentation observé (≈ 2,4 × 10⁻⁷, soit 200 000× sous la tolérance) ;
- reste strictement 2× **sous** le plus petit écart réel possible (0,10) : aucune valeur de
  grille ne peut à la fois être une impasse réelle et tomber dans la zone de tolérance.

Les deux comparaisons deviennent strictement symétriques et strictement inégales :
`offset > seuil + epsilon` (bas) et `offset < seuilHaut − epsilon` (haut, le `<=` d'origine
devient `<`).

**Corollaire pour les tests** : un test qui vérifie une comparaison de seuil doit inclure au
moins un cas construit à partir du **littéral que l'utilisateur saisirait réellement**
(ex. `1.20f`), pas seulement des décalages calculés depuis la même constante que le code
testé. Un test qui calcule ses deux membres par la même expression que le code qu'il protège
ne peut pas faire la différence entre « le code est correct » et « le code et le test partagent
la même erreur ».

## Alternatives considérées

- **Écrire `kPhOffsetImpasseThreshold` comme un littéral `1.20f`** — **rejetée** : viole
  [ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md) (une constante de la chaîne de
  mesure doit déclarer sa source — ici, une décision produit déjà prise : la plage de consigne et
  les filets bruts absolus) ; un littéral se désynchronise silencieusement de ses deux termes si
  l'un d'eux change un jour, exactement le défaut que la constante calculée visait à éviter.
- **Une tolérance fixe arbitraire (ex. `1e-6f`)** — **rejetée** : ne généralise à aucune autre
  comparaison de seuil du projet, ne communique aucune marge de sécurité par rapport au domaine
  métier (rien ne dit si `1e-6` est beaucoup ou peu par rapport à une impasse réelle), et
  n'aurait pas de source déclarée au sens de l'ADR-0098.
- **Comparer en `double` plutôt qu'en `float`** — **rejetée** : réduirait l'écart de
  représentation sans l'éliminer pour des valeurs non exactement représentables en base 2 ;
  changerait le type de toute la chaîne de mesure pH pour un correctif local, sans supprimer le
  besoin d'une marge de comparaison explicite (un offset saisi et son seuil dérivé resteraient
  deux calculs distincts, même en double précision).
- **Arrondir `kPhOffsetImpasseThreshold` lui-même à la compilation** (ex. `roundf(... * 100) /
  100`) — **rejetée** : masquerait la nature dérivée du seuil (ADR-0098 exige la dérivation
  visible, pas un arrondi qui la cache) et ne protégerait pas symétriquement contre l'arrondi de
  sérialisation JSON côté client (offset `1.2` reçu du client, lui aussi un flottant distinct).

## Conséquences

### Positives
- La fausse alerte constatée en production ne peut plus se reproduire pour ce couple de seuils :
  tout offset de la grille de saisie (multiples de `kPhOffsetStep`) a une réponse déterministe et
  stable, indépendante du bruit de représentation du calcul qui a produit le seuil.
- La règle est vérifiable en revue sur toute future comparaison de seuil dérivé : « cette
  tolérance est-elle dérivée d'une grille de saisie, ou inventée ? »

### Négatives / dette assumée
- La règle ne corrige pas rétroactivement d'autres comparaisons de seuils flottants dérivés qui
  pourraient exister ailleurs dans le projet et n'ont pas été auditées à l'occasion de ce
  correctif (périmètre : uniquement `isPhRegulationImpasseLow/High`, le defaut qui a produit
  l'incident).
- La tolérance de 0,05 pH, bien que dérivée et bornée des deux côtés (bruit vs. plus petit écart
  réel), reste propre au pas de saisie pH (`kPhOffsetStep` = 0,10) : elle ne se transpose pas
  telle quelle à un autre paramètre dont la grille de saisie aurait un pas différent — c'est la
  **méthode** (moitié du pas de saisie) qui est générale, pas le chiffre.

### Ce que ça verrouille
- Toute future comparaison d'un seuil pH **dérivé** par calcul en flottant à une mesure ou un
  offset doit soit démontrer qu'aucune ambiguïté de représentation n'existe, soit porter une
  tolérance dérivée de la grille de saisie concernée — pas une comparaison nue.
- Toute suite de tests qui protège une comparaison de seuil doit inclure un cas construit à
  partir du littéral réellement saisissable par l'utilisateur, pas seulement des décalages
  calculés depuis la même constante que le code testé.

## Références

- Code : [`src/constants.h`](../../src/constants.h) (`kPhImpasseEpsilon`, commentaire de source),
  [`src/ph_guard_logic.h`](../../src/ph_guard_logic.h) (`isPhRegulationImpasseLow/High`)
- Tests : [`test/test_native_ph_guard/test_ph_guard.cpp`](../../test/test_native_ph_guard/test_ph_guard.cpp)
  (cas nommé `test_impasse_basse_offset_production_1_20_n_est_pas_en_impasse`, reproduit
  directement l'incident avec le littéral `1.20f`)
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#zone-dimpasse-de-régulation-v2820),
  [`docs/MQTT.md`](../MQTT.md) (type d'alerte `ph_impasse`), [`docs/API.md`](../API.md#get-get-config--write)
  (`ph_impasse_low`/`ph_impasse_high`)
- ADR lié : [ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md) (sourcer une
  constante), [ADR-0100](0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md) (verdict
  servi, jamais recalculé côté client — application conjointe à ce correctif),
  [ADR-0114](0114-correction-mesure-ph-sans-borne-de-reglage.md) (introduction des seuils
  concernés)
