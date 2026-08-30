# ADR-0056 — Sous-dimensionnement d'actionneur : 3 jours × 80 %, anneau évalué au soutenable courant

- **Statut** : Accepté
- **Date** : 2026-08-30
- **Décideurs** : architect + pool-chemistry (visa GO sous conditions du 30/08/2026)
- **Spec(s) liée(s)** : feature-086 (points 1-2, v2.29.0)

## Contexte

Le visa `pool-chemistry` du 30/08/2026 établit que le bassin de référence est
limité par l'**actionneur**, pas par le correcteur : avec 5 min/h de limite
horaire d'injection et ~26 mL/min de débit d'amplitude, le maximum délivrable
vaut ~911 mL/j pour un besoin de ~1 800 mL/j. Trois itérations sur le correcteur
(cadence de l'intégrale, persistance, feedforward) ne pouvaient pas combler ce
déficit. Il faut **détecter et signaler** cette situation quand elle se répète —
sans persister de nouvel état, sans toucher au schéma de l'anneau `ph_load7`
(v2.27.0), et sans qu'une alerte légitime devienne un cri au loup qui persiste
après le remède.

Trois questions à trancher : **combien** de journées consécutives, **à quel
seuil** de remplissage compter une journée « saturée », et **comment l'alerte se
réarme** quand l'utilisateur agit (relève la limite, change de produit).

## Décision

Le sous-dimensionnement est **suspecté** quand **3 journées calendaires
consécutives exploitables** (`kPhActuatorLimitConsecutiveDays`) ont chacune
délivré **≥ 80 %** (`kPhActuatorLimitDayFillRatio`) du maximum théorique de
**leurs** minutes de circulation archivées, **pendant que** l'épisode C5
(« demande saturée sans atteindre la consigne ») est encore ouvert et que la
mesure pH est valide.

La série est **recalculée à chaque relecture de l'anneau contre le débit
soutenable COURANT** — jamais contre celui du jour archivé. Le réarmement est
donc **implicite** : relever la limite fait retomber les ratios des journées
passées (l'alerte se lève d'elle-même), la baisser rend les journées passées
implausibles au filet de vraisemblance (écartées, ADR-0054).

### Chiffrage de la marge (80 %, pas 90-95 %)

La marge est **contre-intuitive** : on attendrait 90-95 % pour « colle au
maximum ». Or en saturation permanente **réelle**, le délivré observé plafonne à
**84-88 %** du maximum théorique — les pauses de mélange, le délai de purge et
les plafonds de session amputent mécaniquement le reste. Chiffrage de l'incident
du 23/08/2026 : **765-802 mL comptés pour ~911 mL max**. Un seuil à 90-95 % ne se
déclencherait **jamais**. À l'inverse, un régime convergé (pH tenu) délivre
< 50 % du maximum : 0,80 n'attrape aucun faux positif de ce côté. Deux
`static_assert` verrouillent ]0,5 ; 0,9[ et [2 ; 7] avec ces justifications.

N = 3 est aligné sur le jalon d'observation terrain de la spec et sur
`kPhIntegralRestoreMinHistoryDays` : une journée saturée isolée (canicule, apport
après baignade, remise en eau) n'est pas un régime.

## Alternatives considérées

- **Stocker le soutenable du jour dans l'anneau `ph_load7`** (rejetée) — chaque
  journée serait jugée contre le réglage de SON époque, réarmement « exact ».
  Mais cela modifie le schéma et l'empreinte CRC de l'anneau
  (`kPhLoadHistorySchema`), ce qui invaliderait les 7 journées existantes au
  déploiement et rouvrirait la surface de persistance — **interdit** par le
  cadrage du cycle (aucun champ NVS nouveau, aucun schéma modifié).
- **Compteur persisté de jours en anomalie** (rejetée) — un champ NVS dédié qui
  s'incrémente à la clôture. Champ **interdit** (même cadrage), et redondant :
  l'état vivant de l'anomalie C5 (`anomalyHeldMs`, tenu par le module intégral)
  **suffit** comme second terme de la conjonction — il porte déjà l'hystérésis
  et la remise à zéro exacte.
- **Évaluation au soutenable courant, sans persistance nouvelle** (retenue) —
  zéro octet NVS ajouté, réarmement automatique sur action corrective, un seul
  trade-off assumé (ci-dessous).

## Conséquences

### Positives
- Réarmement **sans code dédié** : les cas limites de la spec (changement de
  produit, saison basse, relèvement de limite) se résolvent par construction.
- Aucun nouvel état persisté : schéma d'anneau, CRC et surface NVS inchangés.
- La marge 80 % est **déclenchable en pratique** (prouvé par l'incident du
  23/08), pas seulement en théorie.

### Négatives / dette assumée
- **Relever la limite « blanchit » jusqu'à 7 jours d'historique** : les journées
  archivées sous l'ancien réglage ne comptent plus, la preuve repart de zéro.
  Assumé : c'est précisément l'**action corrective attendue** — une alerte qui
  survivrait au remède serait un défaut pire.
- Le réarmement implicite est **invisible dans le code** (aucune ligne ne
  « réarme ») : sans cet ADR, un mainteneur pourrait ajouter un réarmement
  explicite redondant, ou « corriger » l'évaluation au soutenable courant en
  croyant réparer un bug.

### Ce que ça verrouille
- La série se calcule **toujours** contre le soutenable courant — ne pas figer
  le soutenable dans l'anneau, ne pas ajouter de compteur persisté.
- Les deux verdicts booléens publiés (`ph_limited_by_hourly_budget`,
  `ph_undersized`) portent **strictement la même garde mesure-valide** : ne pas
  en faire des variantes.
- `kPhActuatorLimitDayFillRatio` reste sous 0,9 : tout relèvement « pour être
  plus strict » rendrait l'alerte inerte (délivré réel 84-88 %).
- Diagnostic **lecture seule** : aucune décision de dosage ne doit jamais
  consommer ces accesseurs.

## Références

- Code : `src/ph_actuator_limit_logic.h` (logique pure), `src/pump_controller.cpp`
  (accesseurs + `tickPhUndersizedAlert()`), `src/config.cpp`
  (`readPhActuatorLimitStreak`), `src/constants.h` (`kPhActuatorLimit*`)
- Spec : `specs/features/doing/feature-086-limite-actionneur-dosage-ph.md`
- Doc : `docs/subsystems/pump-controller.md` (section « Limite d'actionneur pH »),
  [ADR-0054](0054-seuil-de-vraisemblance-fonction-de-la-configuration.md) (le
  filet de vraisemblance suit la configuration — même principe, ici étendu au
  critère de saturation)
