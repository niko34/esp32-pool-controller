# ADR-0046 — Accumulateur de demande sous le pouls minimal (amende le seuil C de l'ADR-0043)

- **Statut** : Accepté
- **Date** : 2026-08-23
- **Décideurs** : gate `pool-chemistry` (GO amont sous 5 conditions + GO en contre-visite,
  conditions vérifiées ligne par ligne, 2026-08-23), utilisateur (refus de l'allongement
  de période), architect
- **Spec(s) liée(s)** : [feature-073-accumulateur-dosage-sous-plancher](../../specs/features/done/feature-073-accumulateur-dosage-sous-plancher.md) (v2.22.0)

## Contexte

Le **seuil C** de l'[ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md)
refusait toute demande exigeant un pouls < 5 s (< débit_amplitude × 5/60, soit
~2,1 mL/min à l'amplitude 50 %) : un deadband de refus assumé comme dette
(« la correction des petites erreurs est différée »). Le terrain a montré le 23/08/2026
que cette dette n'est pas un simple retard mais une **zone morte permanente** : avec
`kp = 8`, la régulation pH s'arrête quand `kp × erreur` passe sous le plancher, soit une
**stagnation à ~cible + 0,26** observée en production pendant le rattrapage — la consigne
n'est **jamais** atteinte. L'utilisateur veut atteindre la cible.

## Décision

La demande sous le pouls minimal n'est plus refusée : elle est **cumulée** dans un
accumulateur de demande **borné, par produit** (pH / ORP), et restituée par des
**impulsions minimales entières** (`planPulseWithCarry`,
[`src/pulse_dosing_logic.{h,cpp}`](../../src/pulse_dosing_logic.h), pur ADR-0017) :

- **Accumulation** : à chaque période où la demande est vivante mais sous le plancher,
  `demande × période / 60000` (mL) s'ajoute au cumul ; la **séance reste OUVERTE**
  (pompe OFF entre les tirs — l'UI affiche déjà « Dosage en attente : Ns », zéro
  modification web).
- **Tir** : dès que le cumul **atteint** (égalité incluse) le volume **RÉEL kick-inclus**
  d'une impulsion minimale — `V_pouls = pulseDeltaVolumeMl(0, kick, 0, poulsMin,
  flow255, ampFlow)`, **décision gate n°1** : le nominal `ampFlow × 5 s` sous-estime le
  volume délivré de **+10-12 %** (kick 400 ms au débit 255) et aurait produit une
  sur-livraison systématique — **UNE** impulsion de 5 s exactement part par le chemin
  **standard** (`tickPulse`, `usedMs`, volume pesé — **aucun passe-droit sur les
  gardes**, réévaluées à chaque tour en amont), le **résidu est conservé** (F3), jamais
  deux pouls par période, invariant de sortie `cumul < V_pouls`.
- **F1 — demande nulle** : cumul remis à **zéro** (une dette sans demande vivante n'a
  pas de justification chimique).
- **F2 — retour ≥ plancher** : cumul **abandonné**, délégation bit-identique à
  `planPulse` (fusionner reviendrait à compter deux fois la même erreur ; testé
  identique sur 255 demandes).
- **Décision gate n°2 — séance unique** : l'accumulation vit **dans** une séance
  ordinaire, jamais une séance par pouls.
- **Décision gate n°3 — aucune dette héritée** : cumul remis à zéro à chaque
  **ouverture** de séance.
- **Resets exhaustifs** : ouverture ET fermeture de séance (la fermeture est
  inconditionnelle à chaque tick sans débit → couvre tous les refus de gate, `canDose`,
  limites, suspensions), reboot (**non persistant**), entrées invalides ou cumul
  d'entrée suspect (écrêtage fail-closed à zéro — jamais de rafale de rattrapage).
- **Log** : `INFO Dosage par accumulation : impulsion minimale de 5 s (X mL cumulés,
  résidu Y mL)` — **un par séance** (premier tir), remplace l'`INFO` « injection
  différée (seuil C) » supprimé.

Ordre de grandeur : une demande de 0,7 mL/min produit une impulsion de 5 s toutes les
3-4 périodes (V_pouls ≈ 2,32 mL), volume restitué exact à ±5 %.

## Alternatives considérées

- **Conserver le refus seuil C (ADR-0043)** (rejetée) — c'était l'option retenue au
  premier cycle pour limiter le périmètre ; le terrain a prouvé qu'elle laisse la
  régulation **structurellement incapable d'atteindre la consigne** (arrêt à
  ~cible + plancher/kp).
- **Allonger la période à 120 s** (refusée explicitement par l'utilisateur pour ce
  cycle) — abaisserait le plancher de demande (pouls min sur période double) et
  ménagerait les démarrages, mais change la dynamique de tout le train d'impulsions ;
  hors périmètre.
- **Augmenter kp** (rejetée) — déplace le point de stagnation (~plancher/kp) sans
  supprimer la zone morte, et durcit la réponse sur les grandes erreurs (risque
  d'overshoot) ; c'est un contournement, pas une correction.
- **V_pouls nominal (`ampFlow × 5 s`, sans kick)** (rejetée, décision gate n°1) —
  seuil et décrément sous-évalués de +10-12 % → sur-livraison systématique du même
  ordre en régime d'accumulation.
- **Une séance par pouls d'accumulateur** (rejetée, décision gate n°2) — à 0,7 mL/min
  cela ferait **~480 séances/jour** : auto-blocage immédiat par `maxCyclesPerDay` (24)
  et pause mélange armée entre chaque pouls — régulation morte par ses propres gardes.
- **Fusionner le cumul au retour ≥ plancher** (rejetée, F2) — le régime proportionnel
  sert déjà l'erreur courante ; y ajouter la dette accumulée compterait **deux fois la
  même erreur**.
- **Persister le cumul (NVS / reboot)** (rejetée) — un reboot invalide le contexte de
  la dette (mesure, séance, gardes) ; au pire on perd < V_pouls (≈ 2,3 mL), sans enjeu.

## Conséquences

### Positives
- La zone morte disparaît : le pH atteint réellement la consigne au lieu de stagner à
  ~cible + 0,26.
- Le pouls minimal de 5 s (débit reproductible, condition B3) et **toutes** les limites
  (journalières, horaires, cycles, présence d'eau, calibration) restent respectés — le
  pouls d'accumulateur est un pouls **normal** (comptabilité `pulseDeltaVolumeMl`
  inchangée, calendrier ADR-0045 déjà correct).
- Comportement ≥ plancher **bit-identique** (délégation stricte à `planPulse`).

### Négatives / dette assumée
- La restitution des très faibles demandes est **quantifiée** (paquets de V_pouls) et
  différée de quelques périodes — voulu, c'est le prix du débit reproductible.
- Le « X mL cumulés » du log peut être légèrement faux si l'écrêtage défensif
  (`cumul ≥ V_pouls` en entrée) a joué — cosmétique, le volume **crédité** reste exact.
- Toute fermeture de séance perd le cumul en cours (< V_pouls) — sous-dosage borné,
  fail-safe.

### Ce que ça verrouille
- Le tir d'accumulateur passe **toujours** par le chemin standard (`tickPulse`,
  `usedMs`, volume pesé) : tout raccourci ou passe-droit sur les gardes exige un nouvel
  ADR + gate `pool-chemistry`.
- `V_pouls` de référence = **kick-inclus** (`pulseDeltaVolumeMl`), pour le seuil ET le
  décrément — jamais le nominal.
- L'accumulateur reste **non persistant** et **borné < V_pouls** en sortie ; persistance
  ou fusion F2 = nouvel ADR + gate.
- Le seuil C de l'ADR-0043 est **amendé** (le reste de l'ADR-0043 est inchangé).

## Références

- Code : [`src/pulse_dosing_logic.h`](../../src/pulse_dosing_logic.h)
  (`PulseCarryPlan`, `planPulseWithCarry`),
  [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (`_pulseCarryMl`,
  `_carryPulseLogged`, resets), [`test/test_native_dosing/test_dosing_decision.cpp`](../../test/test_native_dosing/test_dosing_decision.cpp)
  (9 tests `test_F073_*`, 649 natifs au total)
- Spec : [feature-073-accumulateur-dosage-sous-plancher](../../specs/features/done/feature-073-accumulateur-dosage-sous-plancher.md)
- ADR liés : [ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md)
  (seuil C amendé — le reste : proportionnel de temps, kick, fail-closed, inchangé),
  [ADR-0044](0044-budget-horaire-et-temps-min-en-temps-pompe-on.md) (usedMs pompe-ON,
  s'applique aux tirs d'accumulateur), [ADR-0045](0045-etat-impulsion-expose-en-calendrier.md)
  (le calendrier décrit déjà les périodes sans pouls), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)
- Doc : [pump-controller.md](../subsystems/pump-controller.md#accumulateur-sous-le-pouls-minimal-feature-073-v2220)
