# ADR-0118 — Une sortie anticipée publie elle-même l'observable qu'elle rend inatteignable

- **Statut** : Accepté
- **Date** : 2026-10-18
- **Décideurs** : `pool-chemistry` (gate H1-H5, GO sous conditions), utilisateur (constat terrain)
- **Spec(s) liée(s)** : [`bug-172`](../../specs/features/done/bug-172-la-cause-de-refus-affichee-est-figee-quand-le-dosage-est-suspendu.md) · [feature-163](../../specs/features/done/feature-163-refus-de-dosage-pendant-un-etalonnage.md) (revue, point bloquant B1 — mécanisme déjà relevé, corrigé pour moitié)

## Contexte

Le 2026-10-18, l'utilisateur relève sur son appareil : *« il y a marqué pause mélange pour le pH
alors que la filtration est arrêtée »*. `GET /data` confirme la contradiction dans la réponse
elle-même : `water_present: false` **et** `orpDoseBlockedReason: "pause mélange en cours"` avec un
décompte de 207 s.

Cause racine, vérifiée dans le code : le bloc de suspension globale d'`updateDosingCore()`
(`src/pump_controller.cpp`) se déclenche sur quatre conditions — eau absente, session d'étalonnage
ouverte, stabilisation post-calibration, assistant de mesure du débit — coupe les pompes, et
`return`-e **avant** `canDose()`. `evaluateDose()` n'est donc jamais évalué, `setGuardRefusalCause()`
n'est jamais appelé, et `_lastRefusalCause` conserve la valeur du **dernier** appel de `canDose()`
— celle d'un état qui n'a plus cours. La cause affichée n'était pas calculée : elle était héritée.

**Ce n'est pas une découverte isolée.** La revue de feature-163, la veille, avait relevé
**exactement ce mécanisme** (son point bloquant B1 : *« le `return` précède `canDose()` : le
`critical` imposé et le libellé de cause sont inatteignables »*) — mais seule la moitié
« journal » a été corrigée (une transition journalisée ajoutée avant le `return`). La moitié
« cause affichée », qui alimente `ph/orpDoseBlockedReason` sur `/data` et le WebSocket, est restée
ouverte, et c'est elle que l'utilisateur a vue dès le lendemain. **Un défaut à deux faces corrigé
sur une seule face revient par l'autre.**

Le gate `pool-chemistry` a trouvé, en instruisant ce correctif, un défaut supplémentaire du même
ordre : la cascade existante (construite pour le seul journal, uniquement quand une injection
était en cours) ordonnait eau → stabilisation → assistant → étalonnage, contredisant le rang 3 de
l'étalonnage dans `canDose()`. Selon le chemin emprunté, l'appareil pouvait nommer **deux causes
différentes pour le même état**.

## Décision

**Tout bloc qui court-circuite — par un `return` anticipé ou équivalent — l'appel à une fonction
de décision dont une partie du résultat est un observable exposé (API, WebSocket, journal) doit
calculer et publier lui-même cet observable pour la condition qui déclenche le court-circuit.
Il ne doit jamais laisser en place la valeur du dernier appel réellement exécuté de la fonction
contournée.**

Concrètement (bug-172) :

1. `evaluateSuspendCause()` ([`src/dosing_logic.h`](../../src/dosing_logic.h) /
   [`.cpp`](../../src/dosing_logic.cpp)) est une cascade **pure**, nouvelle, appelée depuis le bloc
   de suspension globale d'`updateDosingCore()` lui-même — au point exact du court-circuit, pas
   après coup ni à la lecture.
2. Son ordre est **calqué sur les rangs de la fonction qu'elle contourne** (`evaluateDose()`) :
   eau (rang 2) devant étalonnage (rang 3) devant stabilisation (rang 6) — le défaut relevé par
   le gate ci-dessus (ordre contradictoire) est corrigé dans le même geste. `flowAssistSuspend`,
   qui n'a aucun rang dans `evaluateDose()` (ce n'est pas une garde de `canDose()`), est évalué en
   dernier : il ne peut jamais passer devant une garde rangée.
3. Elle est appelée **pour les deux pompes**, **à chaque tick**, **même sans injection en cours**
   — c'est précisément le cas qui échouait : l'utilisateur a vu une cause héritée alors qu'aucune
   session de dosage n'était ouverte.
4. Les quatre textes exposés viennent d'une **source unique** partagée avec `canDose()`
   (`kCauseFiltrationOff`, etc.) : aucune reformulation locale, pour ne jamais produire deux voix
   pour un même fait.
5. Le chemin d'écriture est le chemin **propriétaire** de `canDose()` (`logRefusalOnce()`,
   `_refusalCauseFromGuard = false`), jamais `setGuardRefusalCause()` — la sentinelle `250` reste
   réservée aux gardes chimiques pH/ORP (`250` n'est pas un code de `canDose()`).
6. Edge-trigger strict sur le **code** avant toute construction de chaîne (`postSuspendCause()`) :
   aucune allocation hors transition réelle (ADR-0082), ce bloc tournant à chaque tick.

## Alternatives considérées

- **Appeler `evaluateDose()` même pendant la suspension, en ignorant son verdict `allowed` pour
  n'en garder que la cause** (rejetée) — `evaluateDose()` filtre aussi sur des conditions sans
  rapport avec la suspension globale (mode de régulation, quotas, anti-rafale) ; le mode
  `scheduled` ne l'appelle d'ailleurs jamais du tout. Le réemployer aurait pu faire remonter une
  cause de **rang inférieur** (un quota atteint, par exemple) devant la cause de suspension
  réelle, qui est de rang supérieur par construction — l'inverse de ce que corrige ce lot.
- **Recalculer la cause à la demande, au moment de la sérialisation `/data`/WebSocket** (rejetée)
  — romprait la discipline « edge-trigger, aucune allocation sur le chemin périodique » (ADR-0082) :
  les quatre textes dépassent la SSO de `WString` (14 octets), et les reconstruire à chaque lecture
  (potentiellement plusieurs fois par seconde, un client HTTP et une trame WS) fractionnerait le
  tas 8 bits au lieu d'une fois par transition réelle.
- **Une cascade pure, posée explicitement au point du court-circuit, avec edge-trigger sur le
  code** (retenue) — même discipline que le reste du module `dosing_logic` (ADR-0017), coût nul en
  régime stable, et la source du texte reste unique avec `canDose()`.

## Conséquences

### Positives
- La cause affichée ne peut plus rester figée sur un état qui n'a plus cours : elle est
  recalculée à chaque tick de la condition qui la motive, y compris sans injection en cours.
- Le défaut d'ordre trouvé par le gate (stabilisation avant étalonnage) est corrigé dans le même
  geste, puisque l'ordre est désormais dérivé explicitement des rangs de `canDose()` plutôt que
  reconstruit à la main.

### Négatives / dette assumée
- **Deux cascades à tenir manuellement synchronisées** : l'ordre des rangs d'`evaluateDose()` et
  l'ordre d'`evaluateSuspendCause()` doivent rester cohérents, sans qu'aucun mécanisme du langage
  ne l'impose. Un futur réordonnancement de `canDose()` qui oublierait de revoir
  `evaluateSuspendCause()` reproduirait exactement le défaut que ce lot corrige. Ce risque est
  nommé, pas supprimé — voir le commentaire de verrouillage en tête d'`evaluateSuspendCause()`
  (`dosing_logic.h`).
- `evaluateSuspendCause()` ne couvre que les quatre conditions de la suspension globale
  d'`updateDosingCore()` ; elle ne généralise pas automatiquement à un futur bloc de
  court-circuit qui serait ajouté ailleurs dans le firmware — chaque nouveau court-circuit doit
  être traité explicitement selon la règle énoncée ici, elle ne s'applique pas d'elle-même.

### Ce que ça verrouille
1. **Tout futur `return` anticipé qui empêche l'appel d'une fonction de décision exposée doit
   poser lui-même, au point du `return`, la valeur de l'observable qu'il rend inatteignable** —
   jamais laisser la valeur du dernier appel réel en place.
2. **Une cascade de secours qui mime l'ordre d'une fonction de décision existante doit être
   explicitement dérivée de ses rangs documentés**, et revue conjointement à tout changement de
   ces rangs — pas reconstruite indépendamment "à l'œil", ce qui a produit le défaut d'ordre
   relevé ci-dessus.
3. **Un défaut relevé en revue sur un mécanisme à deux faces (ici : journal et cause affichée)
   n'est clos que lorsque les deux faces sont vérifiées** — une correction partielle doit être
   signalée explicitement comme telle dans la spec qui la livre, pas simplement cochée.

## Références

- Code : [`src/dosing_logic.h`](../../src/dosing_logic.h) (`evaluateSuspendCause`, commentaire de
  verrouillage de l'ordre), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp),
  [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (`postSuspendCause()`,
  bloc de suspension globale d'`updateDosingCore()`).
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#cascade-de-la-suspension-globale--evaluatesuspendcause-bug-172-v2830),
  [`docs/API.md`](../API.md#code-de-cause-de-refus-de-dosage-v2251).
- Spec : [`bug-172`](../../specs/features/done/bug-172-la-cause-de-refus-affichee-est-figee-quand-le-dosage-est-suspendu.md).
- ADR liés : [ADR-0049](0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md) (contrat
  de sérialisation du code machine, prolongé ici par le code `25`) ; [ADR-0095](0095-une-cause-de-garde-cite-un-seuil-pas-une-mesure.md)
  (la cause citée doit être celle qui mord réellement — même esprit, axe différent : ici c'est la
  *fraîcheur* du calcul qui est en cause, pas son référentiel) ; [ADR-0107](0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)
  et [ADR-0115](0115-une-garde-sur-etat-partage-vit-au-point-dusage-final.md) (décisions voisines
  sur la même zone de code, axes distincts : visibilité du mécanisme de suspension et point
  d'évaluation d'une garde, pas fraîcheur d'un observable dérivé).
