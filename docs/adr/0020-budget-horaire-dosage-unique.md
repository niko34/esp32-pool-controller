# ADR-0020 — Budget horaire de dosage unique, partagé entre régulation auto et injections manuelles

- **Statut** : Accepté
- **Date** : 2026-07-04
- **Décideurs** : architect + pool-chemistry (GO pré-implémentation et post-implémentation, feature-006)
- **Spec(s) liée(s)** : feature-006 (injections manuelles gardées)

## Contexte

La limite horaire d'injection (`ph_limit_minutes` / `orp_limit_minutes`, fenêtre glissante 1 h) est suivie par le compteur `usedMs` de `DosingState` (`refreshDosingState()`, [`src/pump_controller.cpp`](../../src/pump_controller.cpp)). Avant feature-006, seule la régulation **automatique** alimentait ce compteur : une injection **manuelle** n'était ni vérifiée contre la limite horaire, ni **comptée** dedans. Conséquence chimique : un opérateur pouvait consommer jusqu'à **2× le budget par heure** (le plein quota auto + des injections manuelles illimitées), en toute invisibilité pour la garde #9 de `canDose()`.

En ajoutant la garde horaire aux injections manuelles (feature-006), il fallait trancher : compteur **partagé** avec l'auto, ou compteur manuel **séparé** (question laissée ouverte à l'architect par la spec).

## Décision

Le budget horaire est **unique** : `usedMs` est **partagé** entre régulation automatique et activations manuelles. `refreshDosingState(state, now, manualActive)` accumule le temps de marche si `state.active || manualActive` — un **OR unique sur un seul point d'accumulation**, donc pas de double comptage possible.

Le prédicat `manualActive` est **celui du safety tracking** (`manualMode[i] && pumpDuty[i] > 0`) : injections manuelles web (`/ph|orp/inject/start`), **pompes test** (`/pumpN/on`, `/pumpN/duty/*`) et **commandes UART** (`pump_test`) consomment toutes le budget horaire. La garde manuelle (`evaluateManualInject`, garde #6) est **prédictive** : refus si `usedMs + durée demandée > limite`, frontière `==` acceptée (strict `>`), limite 0 = illimité (conventions identiques à l'auto).

## Alternatives considérées

- **Compteur manuel séparé** (rejetée) — deux quotas indépendants reproduisent exactement le défaut initial : le total injectable par heure devient `limite_auto + limite_manuelle`. La limite horaire est une borne **chimique** sur le produit versé dans le bassin, pas une borne par « source de commande » ; la scinder n'a pas de sens physique.
- **Compter le manuel sans le vérifier** (rejetée) — l'auto se retrouverait bloquée par du budget consommé manuellement sans que l'opérateur soit jamais refusé : incohérent et surprenant (le manuel resterait un contournement de fait).
- **Compteur partagé, prédicat safety-tracking** (retenue) — un seul quota reflétant le temps de pompe réel quelle que soit la source ; le prédicat réutilisé (`manualMode && duty > 0`) est déjà celui qui alimente les cumuls journaliers, garantissant la cohérence entre les deux comptabilités et couvrant automatiquement pompes test et UART.

## Conséquences

### Positives
- Plus aucun chemin de commande ne peut dépasser le budget horaire configuré : `limite/h` redevient une borne absolue sur le produit injecté.
- L'anti-rafale et les cycles/jour suivent la même philosophie (ring et compteur partagés) : modèle mental unique « un budget chimique, plusieurs sources ».
- Testable en natif : la garde prédictive et ses frontières sont verrouillées par les tests `test_F006_*`.

### Négatives / dette assumée
- Une injection manuelle consomme du budget que la régulation auto ne pourra plus utiliser dans l'heure (et réciproquement) — assumé : c'est le but.
- Les pompes test (`/pumpN/on` sans durée bornée) peuvent épuiser le budget horaire d'une pompe pendant un diagnostic — acceptable (bench de test, opérateur présent).
- La commande UART `pump_test` consomme le budget mais n'est **pas gardée** en amont (écran non déployé) — à guarder si l'écran est mis en service (condition pool-chemistry #4).

### Ce que ça verrouille
- Toute **future source d'activation de pompe doseuse** (nouvelle route, automatisation, écran UART) doit consommer le budget `usedMs` partagé — pas de compteur parallèle.
- Le prédicat d'accumulation manuel reste aligné sur celui du safety tracking journalier : si l'un évolue, l'autre doit suivre.
- Les conventions de frontière (`==` accepté, strict `>`) et « limite 0/≤ 0 = illimité » sont figées pool-chemistry ; les changer exige un nouvel ADR.

## Références

- Code : [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (`refreshDosingState`, consommation du pending en tête d'`update()`), [`src/dosing_logic.h`](../../src/dosing_logic.h) / [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) (`evaluateManualInject`, garde #6), [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) (collecte `getPhUsedMs()`/`getOrpUsedMs()`)
- Spec : `specs/features/doing/feature-006-injection-manuelle-gardee.md`
- Doc : [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md#gardes-des-injections-manuelles-feature-006), [docs/API.md](../API.md#codes-de-refus-409-v260)
- ADR liés : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (pattern logique pure)
