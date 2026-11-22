# ADR-0143 — Le Mode Boost ne peut qu'AJOUTER ; `kBoostDailyHardCapMl` plafonne le RELÈVEMENT, jamais la valeur finale

- **Statut** : Accepté
- **Date** : 2026-11-22
- **Décideurs** : Nicolas Philippe, `pool-chemistry` (visa du 2026-11-22, correction 2 du lot bug-208)
- **Spec(s) liée(s)** : bug-208 (la-barre-du-dosage-du-jour-se-remplit-vers-un-plafond-inatteignable) · amende [ADR-0093](0093-le-mode-de-filtration-est-la-seule-autorite.md) et [ADR-0025](0025-mode-boost.md)

## Contexte

Le Mode Boost agit sur **deux** leviers de dosage, tous deux gatés au mode de régulation ORP
`automatic` :

| Levier | Fonction pure | Formule avant ce lot |
|---|---|---|
| Consigne ORP effective | `effectiveOrpTargetPure()` | `max(cible, min(cible + 60 mV, 850 mV))` |
| Limite journalière chlore effective | `effectiveMaxChlorinePure()` | `min(réglé × 1,5 ; 1 000 mL)` |

**Les deux formules ne portaient pas la même garde.** La première contient un `max(cible, …)`
depuis l'origine — c'est la **condition #5** posée par `pool-chemistry` à la feature-053
(« jamais abaissée »). La seconde ne l'a jamais eue.

Conséquence arithmétique : `réglé × 1,5 > 1 000` dès que `réglé > 666,7 mL/j`. **Au-delà de ce
genou, le Boost ABAISSAIT le plafond journalier de chlore au lieu de le relever.**

Sur l'appareil de production — limite réglée à **2 000 mL/j** — armer le Boost ramenait le
plafond réellement opposé à **1 000 mL/j** : le mode censé doser *davantage* **divisait le budget
par deux** et déclenchait un `critical` « LIMITE JOURNALIÈRE CHLORE ATTEINTE » à mi-course, un
jour de forte fréquentation, c'est-à-dire exactement le jour où le Boost est armé.

Le défaut n'était pas visible à la lecture : `min(a × 1,5 ; plafond)` *ressemble* à un
garde-fou, et c'en est un — mais seulement dans un sens. Il a été trouvé en instruisant
l'affichage de la barre « Dosage du jour » (bug-208), parce qu'il fallait répondre à la question
« quel plafond l'appareil oppose-t-il *réellement* aujourd'hui ? ».

## Décision

**Le Mode Boost ne peut qu'AJOUTER. Aucun de ses leviers ne retire jamais ce que
l'utilisateur a réglé.**

```cpp
// src/dosing_logic.cpp — effectiveMaxChlorinePure()
float raised = fminf(maxChlorine * factor, hardCapMl);
return maxChlorine > raised ? maxChlorine : raised;   // ⇐ jamais sous le réglage
```

Soit, en une ligne : `effectif = max(réglé, min(réglé × kBoostDailyFactor, kBoostDailyHardCapMl))`.

**`kBoostDailyHardCapMl = 1000 mL` change de sens, et il faut le dire sans ambiguïté** :

| | Avant | Après |
|---|---|---|
| Ce que plafonne la constante | la **valeur finale** du plafond journalier | le **relèvement** accordé par le Boost |
| Lecture | « le Boost ne laisse jamais dépasser 1 000 mL/j » | « le Boost n'ajoute plus rien au-delà de 1 000 mL/j — et il ne retire jamais » |
| Au-delà du genou (666,7 mL/j réglés) | le Boost **soustrait** | le Boost est **inerte** sur ce levier |

⚠️ **Ce que cette décision ne fait PAS** : elle ne desserre aucune limite réglée par
l'utilisateur. La valeur finale peut désormais dépasser 1 000 mL/j — **mais uniquement jusqu'au
plafond que l'utilisateur a lui-même inscrit en configuration**, jamais au-delà. Les limites par
injection, par heure et par cycle, la garde de présence d'eau, l'anti-rafale et le gate
`automatic` sont **tous inchangés**.

**Contreparties obligatoires, livrées dans le même lot :**

1. **Honnêteté d'effet** — un `warning` au journal à l'activation du Boost quand ce levier est
   **inerte** (réglage ≥ `kBoostDailyHardCapMl`) : *« Limite de chlore du jour déjà à 2 000 mL,
   au-dessus de ce que le Boost accorde (1 000 mL) — le Boost NE change RIEN à cette limite. Il
   relève la consigne de désinfection et prolonge la filtration, si elle est gérée. »* Même
   patron que les deux `warning` existants (régulation non automatique, chloration de secours
   active) : un levier annoncé et sans effet doit le dire.
2. **Le plafond réellement opposé est publié** — clé `orp_effective_max_ml_per_day` sur `/data`
   **et** sur la trame WebSocket, en parité stricte ([ADR-0071](0071-parite-data-trame-capteurs.md)).
   Sans elle, l'écran pourrait contredire le journal : à 500 mL réglés et Boost armé, le firmware
   autorise 750 mL tandis qu'une barre calée sur le réglage saturerait à 100 %.
3. **Deux `static_assert`** verrouillent les bornes dont dépend l'invariant :
   `kBoostDailyFactor >= 1.0f` (un facteur < 1 ferait reposer l'invariant sur le seul `max()`) et
   `kBoostDailyHardCapMl > 0.0f` (un plafond nul annulerait le relèvement).

## Alternatives considérées

- **Option A (rejetée) — Ne rien changer, et documenter le genou.** Le comportement serait resté
  cohérent avec le commentaire d'origine de la constante, et aurait pu être présenté comme un
  garde-fou terminal. Rejetée : il ne protège rien. Le plafond journalier de l'utilisateur est
  déjà une limite de sécurité validée, et le réduire **au moment précis où l'eau en demande
  davantage** est l'inverse d'une protection. Le `critical` à mi-course est un faux positif
  produit par le mode lui-même.
- **Option B (rejetée) — Relever `kBoostDailyHardCapMl` (p. ex. à 3 000 mL).** Déplace le genou
  sans le supprimer : le défaut réapparaît pour tout réglage supérieur à 2 000 mL/j. Et il
  **desserrerait** réellement une borne, ce que la correction retenue ne fait pas.
- **Option C (rejetée) — Supprimer le plafond dur et garder `réglé × 1,5`.** Rendrait le Boost
  multiplicatif sans borne : un réglage à 2 000 autoriserait 3 000 mL/j. Violerait la règle de
  sécurité « limites journalières toujours respectées » et sortirait du périmètre validé par
  `pool-chemistry` à la feature-053.
- **Option D (rejetée) — Gater le Boost chlore à `réglé ≤ 666 mL/j`, et le refuser au-dessus.**
  Transformerait un levier inerte en refus d'activation, alors que les **deux autres** effets du
  Boost (consigne ORP relevée, créneau de filtration élargi) restent parfaitement utiles à
  2 000 mL/j réglés. Un refus coûterait plus qu'il ne protège.
- **Option E (retenue) — `max(réglé, min(réglé × facteur, plafond))`, plus le `warning`
  d'honnêteté et la publication du plafond opposé.** Aligne les deux leviers du Boost sur la
  **même** garde (condition #5), ne desserre rien, et rend l'inertie visible au lieu de la
  laisser deviner.

## Conséquences

### Positives

- **Le Boost fait ce que son nom promet**, sur toute la plage de réglages, y compris celle de
  l'appareil de production. Le `critical` à mi-course disparaît.
- **Les deux leviers du Boost suivent enfin la même règle.** `effectiveOrpTargetPure()` et
  `effectiveMaxChlorinePure()` sont désormais lisibles ensemble ; l'asymétrie était invisible
  tant qu'on ne lisait qu'une des deux fonctions.
- **L'invariant est testé comme une propriété, pas comme un point** : un balayage
  `0 → 3 000 mL` par pas de 50 vérifie `effectif ≥ réglé` **et** `effectif ≤ max(réglé, plafond)`.
  ⚠️ C'est `effectif ≥ réglé` qui tue le mutant — l'ancienne formule était elle aussi monotone
  croissante, donc tester la monotonie seule n'aurait **rien** prouvé.
- **Le plafond opposé devient observable** par l'écran, la domotique et le diagnostic, au lieu
  d'être reconstruit de tête à partir d'un réglage et d'une constante.

### Négatives / dette assumée

- **Le volume de chlore d'une journée boostée peut désormais dépasser 1 000 mL** chez un
  propriétaire qui a réglé davantage. C'est la **restitution** de son réglage, pas un
  desserrage — mais la phrase « le Boost est borné à 1 000 mL/j », vraie depuis la
  feature-053, **devient fausse** et doit être retirée partout où elle a été écrite
  (voir *Ce que ça verrouille*).
- **Le levier devient inerte sans que l'interface le dise.** La contrepartie livrée est un
  `warning` **au journal**, pas un message d'écran : un propriétaire qui n'ouvre pas le journal
  croit toujours que le Boost relève sa limite journalière. Dette écrite, non traitée dans ce
  lot.
- **Le genou de 666,7 mL/j n'est écrit nulle part dans l'interface.** Il se déduit de
  `kBoostDailyHardCapMl / kBoostDailyFactor`, deux constantes non publiées. Un utilisateur ne
  peut pas savoir, depuis l'écran, à partir de quel réglage ce levier cesse d'agir.
- **La chloration de secours garde sa propre borne de 2 000 mL/j** (« même combinée au Boost »),
  indépendante de celle-ci. Les deux mécanismes ne sont pas unifiés, et ce lot ne les unifie pas.

### Ce que ça verrouille

- ⛔ **Aucun mécanisme temporaire ne peut ABAISSER une limite de sécurité réglée par
  l'utilisateur.** Un mode qui veut doser *moins* doit le faire par un réglage persistant,
  explicite et réversible — pas par une surcouche dont le nom annonce le contraire. Toute future
  surcouche « valeurs effectives » doit porter le `max(réglé, …)` dès sa première ligne.
- ⛔ **`kBoostDailyHardCapMl` ne se lit plus comme un plafond absolu.** Toute phrase du dépôt qui
  affirme « le volume reste borné à 1 000 mL quelle que soit la durée » est **fausse** et doit
  être corrigée, pas recopiée. Deux emplacements sont amendés par cet ADR :
  [ADR-0093 § Négatives](0093-le-mode-de-filtration-est-la-seule-autorite.md) (argument
  « Boost + Marche ») et [ADR-0025 § Bornes de sécurité](0025-mode-boost.md), déjà
  *Superseded by ADR-0093*.
- ⛔ **Le visa chimique de la combinaison Boost + Marche repose désormais sur un autre
  argument.** Il ne peut plus s'appuyer sur un plafond en dur de 1 000 mL : il repose sur le
  plafond **réglé par l'utilisateur**, inchangé par le Boost, et sur les limites horaires et par
  injection, elles aussi inchangées. Toute évolution de `kBoostDailyFactor` ou de
  `kBoostDailyHardCapMl` exige un nouveau passage `pool-chemistry`.
- **Trois notions distinctes, jamais interchangeables**, verrouillées par les clés publiées :
  ce qui est **réglé** (`max_chlorine_ml_per_day`, `/get-config`), ce qui est **réellement
  opposé** (`orp_effective_max_ml_per_day`, `/data` + WS) et ce qui est **délivrable** par la
  pompe sur la journée programmée (`orp_daily_max_ml`). Les confondre est la faute même que
  bug-208 dénonce.

## Références

- Code : [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) (`effectiveMaxChlorinePure()`),
  [`src/constants.h`](../../src/constants.h) (`kBoostDailyFactor`, `kBoostDailyHardCapMl` +
  les deux `static_assert`), [`src/config.cpp`](../../src/config.cpp) (`startBoost()` —
  `warning` d'honnêteté d'effet), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) et
  [`src/ws_manager.cpp`](../../src/ws_manager.cpp) (`orp_effective_max_ml_per_day`,
  `orp_daily_max_ml`)
- Tests : [`test/test_native_dosing/test_dosing_decision.cpp`](../../test/test_native_dosing/test_dosing_decision.cpp)
  (suite `test_B208_*`, dont le balayage d'invariant `test_B208_chlorine_invariant_sweep`)
- Spec : `bug-208-la-barre-du-dosage-du-jour-se-remplit-vers-un-plafond-inatteignable.md` (dans `specs/features/` ; ⚠️ le dossier change avec l'état du cycle)
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md),
  [`docs/API.md`](../API.md), [`docs/subsystems/ws-manager.md`](../subsystems/ws-manager.md),
  [`docs/features/page-orp.md`](../features/page-orp.md), [`CHANGELOG.md`](../../CHANGELOG.md)
- ADR liés : [ADR-0025](0025-mode-boost.md) (Mode Boost d'origine — **amendé** sur le sens du
  plafond dur), [ADR-0093](0093-le-mode-de-filtration-est-la-seule-autorite.md) (**amendé** sur
  l'argument « Boost + Marche »), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)
  (logique pure testable — patron des `effective*Pure`),
  [ADR-0071](0071-parite-data-trame-capteurs.md) (parité `/data` ↔ trame capteurs),
  [ADR-0100](0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md) (aucun seuil recalculé côté
  navigateur — raison pour laquelle le plafond opposé est **publié** plutôt que déduit)
