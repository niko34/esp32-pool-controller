# ADR-0129 — Toute marche en mode manuel porte une échéance fournie par l'appelant, vérifiée hors de toute conjonction d'éligibilité

- **Statut** : Accepté
- **Date** : 2026-11-17
- **Décideurs** : `architect`, `pool-chemistry` (gate), utilisateur
- **Spec(s) liée(s)** : [bug-171](../../specs/features/done/bug-171-une-pompe-en-mode-manuel-echappe-au-blocage-global.md) · [feature-163](../../specs/features/done/feature-163-refus-de-dosage-pendant-un-etalonnage.md) · [ADR-0115](0115-une-garde-sur-etat-partage-vit-au-point-dusage-final.md)

## Contexte

`setManualPump()` posait `manualMode[i] = true` sans borne de durée, et les trois blocs de suspension de `updateDosingCore()` épargnent `manualMode` (l'assistant de mesure du débit en dépend). Résultat : une pompe lancée par `POST /pumpN/on` n'était arrêtée que par un `setTimeout` du navigateur — qui ne survit pas à la veille d'un téléphone, à un onglet fermé ou à une coupure Wi-Fi — et ne l'était ni par une perte de présence d'eau, ni par l'ouverture d'un étalonnage (trou dans la garantie de feature-163). L'interface et la documentation promettaient pourtant un arrêt « côté firmware » qui n'existait pas.

## Décision

**Toute marche en mode manuel porte une échéance fournie par l'appelant** : `setManualPump(i, duty, budgetMs)` arme `_manualDeadlineMs[i]` pour tout `duty > 0`. `tickManualPumpGuard()` la vérifie **à chaque tour de `PumpController.update()`, hors de toute conjonction d'éligibilité** (placé avant les sorties « capteurs non initialisés » et « filtration coupée »), et coupe aussi sur perte de présence d'eau et session d'étalonnage. `duty == 0` reste inconditionnel. Le test de pompe passe `kManualPumpTestMaxOnMs` (10 s, sans grâce) ; les marches qui possèdent leur propre arrêt passent durée + `kManualRunGraceMs` (2 s). Le refus `409 already_injecting` porte sur le **conflit d'usage** (autre marche manuelle en cours), jamais sur l'état chimique du bassin : **aucun plafond journalier** sur `/pumpN/on` (décision produit : un test matériel doit toujours fonctionner).

## Alternatives considérées

- **A — Retirer l'exemption `!manualMode[i]` des blocs de suspension** (rejetée) : casse l'assistant de mesure du débit, qui en dépend.
- **C — Interrompre le test seulement à l'entrée en étalonnage** (rejetée) : laisse ouvert le cas « filtration arrêtée » et celui du téléphone en veille.
- **Plafond unique, identique pour tous les appelants** (rejetée) : couperait les injections manuelles de 60/120 s, ou laisserait le test à 120 s ; la borne doit venir de l'appelant, qui connaît sa durée.
- **Refuser `/pumpN/on` sur plafond journalier** (rejetée par l'utilisateur) : un outil de diagnostic matériel ne dépend pas de l'état chimique.
- **Exclusivité justifiée par un risque de Cl₂** : justification **retirée** après calcul (canalisation pleine d'eau, dissolution ~10× la quantité émise, diffusion ≈ 1 mm en 10 min) ; l'exclusivité est conservée pour trois autres motifs (coût borné, une seule échéance, alignement sur `assist_busy`).

## Conséquences

### Positives
- La promesse « arrêt automatique après 10 secondes » devient vraie, indépendamment du navigateur.
- La garantie « aucun dosage pendant un étalonnage » de feature-163 couvre aussi ce qui tournait déjà.
- L'exemption `manualMode` des blocs de suspension est conservée (assistant de débit intact).

### Négatives / dette assumée
- Doublon d'alerte connu et accepté quand la filtration s'arrête pendant une injection manuelle (réordonner `loop()` est hors périmètre).
- Les marches à crédit plancher dépendent d'une grâce de 2 s : une durée d'appelant sous-estimée la tronquerait.

### Ce que ça verrouille
- **Tout futur appelant de `setManualPump(duty > 0)` doit fournir sa durée** (ou accepter les 10 s par défaut) : aucune marche manuelle sans échéance. La contrainte est invisible dans le code appelant.
- Ajouter un plafond journalier à `/pumpN/on` contredirait une décision produit explicite.
- Ne pas réintroduire la justification « anti-Cl₂ » de l'exclusivité.

## Références

- Code : `src/pump_controller.cpp` (`setManualPump`, `tickManualPumpGuard`), `src/manual_pump_guard_logic.h`, `src/web_routes_control.cpp` (`manualRunBusyReject`), `src/constants.h` (`kManualPumpTestMaxOnMs`, `kManualRunGraceMs`)
- Spec : `specs/features/done/bug-171-une-pompe-en-mode-manuel-echappe-au-blocage-global.md`
- Doc : [pump-controller.md](../subsystems/pump-controller.md#marche-manuelle-bornée-bug-171-v2910)
