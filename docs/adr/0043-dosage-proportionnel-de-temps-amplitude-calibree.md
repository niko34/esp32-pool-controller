# ADR-0043 — Dosage en proportionnel de temps à amplitude calibrée (fail-closed, sans interpolation)

- **Statut** : Accepté — **seuil C amendé par [ADR-0046](0046-accumulateur-demande-sous-pouls-minimal.md)** (2026-08-23, v2.22.0) : la demande sous le pouls minimal n'est plus refusée mais **accumulée** jusqu'à valoir une impulsion minimale réelle. **Défauts de calibration amendés par [ADR-0047](0047-calibration-de-type-par-defaut-et-gel-amplitude.md)** (2026-08-24, v2.23.0, contexte flotte) : le « pesée obligatoire, pas de valeur provisoire » ne vaut plus pour les DÉFAUTS — un appareil vierge reçoit la **calibration de type** (127 / 28,0 ml/min) ; le fail-closed reste pour les clés NVS présentes à 0. **Pouls minimal amendé par [ADR-0051](0051-pouls-minimal-double-rendement-avant-comptage.md)** (2026-08-28, v2.25.3) : le pouls minimal de la condition B3 passe de **5 s à 10 s** (rendement des démarrages ; la dose demandée est inchangée). Le reste de la décision (proportionnel de temps, amplitude calibrée, kick, égalité exacte, aucune interpolation) est inchangé.
- **Date** : 2026-08-23
- **Décideurs** : gate `pool-chemistry` (verdicts 2026-08-22 et 2026-08-23), utilisateur (amendement amplitude), architect
- **Spec(s) liée(s)** : [bug-dosage-fantome-duty-non-calibre](../../specs/features/todo/bug-dosage-fantome-duty-non-calibre.md) (itération 2, v2.21.0)

## Contexte

Le régulateur commandait les pompes doseuses par un **duty PWM analogique** interpolé
linéairement entre `MIN_ACTIVE_DUTY` et 255 (`flowToDuty`/`dutyToFlow`), et la
comptabilité créditait le **débit du modèle**, jamais un débit mesuré. Le diagnostic
on-target du 2026-08-22 (dosage fantôme) puis la **mesure au banc du 2026-08-23** ont
prouvé que ce modèle est faux **aux deux extrémités** :

| Duty | Modèle linéaire | Mesuré (pesée) | Écart |
|---|---|---|---|
| 82 | 6,4 ml/min | **12,0 ml/min** (rotation entretenue) | sous-estimation **×1,9** |
| 255 | 90 ml/min | **61,0 ml/min** (P1) / **67,0** (P2) | surestimation **×1,5** |

Pire : à duty 82 la pompe tourne en rotation **entretenue** mais **ne démarre pas à
froid** (couple de décollage) — le contrôleur comptait donc de l'acide qui ne sortait
pas du bidon (479 mL fictifs en 75 min). Une péristaltique n'a pas un débit
proportionnel au rapport cyclique en bas de plage : sous un seuil elle cale, autour du
seuil son débit est erratique.

Trois options étaient en arbitrage : **(A)** recalibrer le modèle linéaire sur des
points mesurés, **(B)** proportionnel de **temps** (pompe à duty franc pendant une
fraction de la période), **(C)** plancher de refus sous un débit non réalisable.

## Décision

Le débit moyen est réalisé par **proportionnel de temps** (option **B + C**, module pur
[`src/pulse_dosing_logic.{h,cpp}`](../../src/pulse_dosing_logic.h), patron ADR-0017) :

- **Période fixe 60 s** (`kPulsePeriodMs`), **pouls minimal 5 s** (`kPulseMinMs`,
  condition B3 du gate). `pulseMs = période × débit_demandé / débit_amplitude`.
- La pompe ne tourne **qu'à une amplitude unique** : `pumpN_max_duty_pct × 255/100`
  (sémantique PWM du réglage conservée), **bornée [40..100] %** serveur + UI
  (`kPumpAmpMinPct = 40` ⇒ 102 counts ≥ `MIN_ACTIVE_DUTY = 98` = Dmin mesuré 82
  + 16 counts de marge P2 — couple de décollage à froid observé au terrain).
- **Kick de démarrage** : 400 ms à duty 255 en tête de chaque pouls
  (`kPulseKickMs`), puis retombée à l'amplitude — vaincre le couple de décollage.
- **Calibration fail-closed PAR pompe** (`pumpN_calibrated_duty` +
  `pumpN_pulse_flow_ml_per_min`, pesée utilisateur ; écart de +10 % constaté entre
  deux pompes identiques — le tube commande) : `calibrated_duty == 0` (jamais pesée)
  ou **amplitude ≠ duty calibré** ⇒ dosage automatique/programmé **REFUSÉ**
  (`PulseRefusal::FlowUncharacterized`, critical throttlé, badge UI). **AUCUNE
  interpolation nulle part** : les deux seuls débits connus sont la pesée à
  l'amplitude et la pesée à 255 (`kPumpNFlow255MlPerMin`).
- **Seuil C (deadband de refus)** : une demande exigeant un pouls < 5 s
  (< débit_amplitude × 5/60) est refusée (info throttlé) — on attend que l'erreur
  grossisse plutôt que d'injecter par pouls au débit non reproductible.
- **Comptabilité exacte par géométrie de pouls** : `V = 0,4 s × débit_255 +
  reste × débit_amplitude` (`pulseDeltaVolumeMl`, par timestamps). Manuel `?duty=N` :
  crédit au **majorant** débit_255 sauf duty == calibré (surestimer est fail-safe, P4).
- **Ancienne clé NVS partagée `pump_max_flow` supprimée SANS conversion** : c'est le
  modèle faux qu'on retire, aucune valeur héritée n'est digne de confiance.

## Alternatives considérées

- **(A) Recalibrer le modèle linéaire** (rejetée, NO GO explicite du gate 2026-08-22) —
  conserve un modèle faux entre les points, ne supprime pas la zone erratique au ras du
  seuil, et si `Dmin` mesuré avait dépassé le duty max configuré, `dutyToFlow` aurait
  rendu 0 → **régulation silencieusement morte**.
- **(A)+(C) après mesure** (rejetée au re-arbitrage du 2026-08-23) — la mesure a
  invalidé l'hypothèse `Dmin > 127` (Dmin = 82), mais (A)+(C) donne une **régulation
  morte en pratique** : avec `kp = 8`, les demandes typiques (~6,4 ml/min pour une
  erreur de 0,8 pH) sont **sous le plancher physique mesuré** (12 ml/min à 82,
  16,5 ml/min avec marge) — le plancher (C) refuserait en permanence.
- **(B) à plein régime 255 fixe** (écartée par l'amendement utilisateur) — pouls très
  courts et bruyants ; l'utilisateur a demandé une **amplitude réglable** (plancher
  40 %) pour garder la main sur le régime de la pompe. Le kick 400 ms compense le
  couple de décollage que l'amplitude réduite ne garantit pas seule.
- **Interpolation entre les deux pesées (amplitude et 255)** (rejetée) — ré-introduirait
  exactement le mensonge qu'on retire : entre deux points mesurés, le débit d'une
  péristaltique n'est pas linéaire. Fail-closed strict à la place.
- **Conversion de l'ancienne clé `pump_max_flow`** (rejetée) — migrer une valeur issue
  du modèle faux donnerait une calibration d'apparence valide sans pesée réelle.

## Conséquences

### Positives
- Le volume crédité vaut **débit mesuré × temps** : traçable, vérifiable au bidon.
- Le régime « pompe marginale » (rotation intermittente sous le seuil) est
  structurellement impossible : la pompe est soit OFF, soit à l'amplitude calibrée.
- L'écart tube entre pompes (+10 %) est absorbé par la calibration par pompe.

### Négatives / dette assumée
- **Le dosage automatique est BLOQUÉ tant que la pesée n'est pas saisie** (fail-closed
  voulu) — coût opérationnel à chaque changement de tube ou d'amplitude (re-pesée).
- Sous-comptage connu ~0,1 ml/pouls à la transition kick→plateau (premier tick après
  400 ms) — borné, documenté, sans enjeu de sécurité (le registre de sécurité est
  par ailleurs majorant sur le chemin manuel).
- Le pouls minimal de 5 s crée une **zone morte de demande** (< débit_amplitude/12) :
  la correction des petites erreurs est différée.

### Ce que ça verrouille
- Plus **jamais** d'interpolation duty↔débit — toute réintroduction exige un nouvel ADR
  et un GO `pool-chemistry`.
- `pumpN_max_duty_pct` ne descend plus sous 40 % (plancher de rotation).
- Le contrat `/save-config` / `/get-config` : 4 clés de calibration par pompe,
  `pump_max_flow_ml_per_min` définitivement retiré (entrant ignoré).
- Toute évolution des constantes `kPulse*` / `MIN_ACTIVE_DUTY` / `kPumpNFlow255MlPerMin`
  exige une nouvelle mesure au banc + gate `pool-chemistry`.

## Références

- Code : [`src/pulse_dosing_logic.h`](../../src/pulse_dosing_logic.h),
  [`src/pulse_dosing_logic.cpp`](../../src/pulse_dosing_logic.cpp),
  [`src/pump_controller.cpp`](../../src/pump_controller.cpp),
  [`src/constants.h`](../../src/constants.h) (`kPulse*`, `kPumpNFlow255MlPerMin`,
  `kPumpAmpMinPct`), [`src/config.h`](../../src/config.h) (`MIN_ACTIVE_DUTY = 98`)
- Spec : [bug-dosage-fantome-duty-non-calibre](../../specs/features/todo/bug-dosage-fantome-duty-non-calibre.md)
  (mesure au banc du 2026-08-23, verdicts gate des 2026-08-22 / 2026-08-23)
- ADR liés : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (module
  pur natif), [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md) (l'offset
  pH est **hors périmètre** de cette décision — condition C0 du gate),
  [ADR-0044](0044-budget-horaire-et-temps-min-en-temps-pompe-on.md) (comptage du budget
  horaire en temps pompe-ON, corollaire séparé)
- Doc : [pump-controller.md](../subsystems/pump-controller.md#dosage-en-proportionnel-de-temps-itération-2-v2210)
