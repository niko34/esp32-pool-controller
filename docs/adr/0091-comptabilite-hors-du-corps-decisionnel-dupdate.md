# ADR-0091 — La comptabilité des volumes est hors du corps décisionnel d'`update()`

- **Statut** : Accepté
- **Date** : 2026-10-08
- **Décideurs** : `architect`, `pool-chemistry` (gates P1/P2), agent principal (arbitrage)
- **Spec(s) liée(s)** : [bug-115](../../specs/features/done/bug-115-etat-de-comptabilite-remanent-aux-sorties-anticipees.md)

## Contexte

`PumpControllerClass::update()` a quatre sorties anticipées (OTA en cours, mode
maintenance, capteurs non initialisés, eau absente / stabilisation post-calibration /
assistant de débit — ce dernier triplet partageant un seul `return`). Chacune force
`applyPumpDuty(*, 0)` puis **revenait avant** l'unique appel à `accountPumpVolume()`,
situé en queue de la fonction.

`_acctPumpOn[]` et `lastSafetyTimestamp` n'étaient donc écrits que dans ce chemin
nominal. Pendant toute sortie anticipée, ils restaient **rémanents** : à la reprise,
`deltaMs` valait la durée **entière** de la coupure et `pulseDeltaVolumeMl` créditait
tout ce delta au débit plateau — jusqu'à 950 mL sur une coupure nocturne longue, soit
20 à 48 % du plafond journalier vivant (2 000 mL/j des deux côtés) — tandis que le
redémarrage physique n'était jamais compté (`prevOn` restait vrai).

Deux campagnes de concordance jerricane (08/09 : +5,11 % sur 86 démarrages ; 08/10 :
+6,25 % sur 356 démarrages) ont mesuré un biais de comptage sans que ce défaut ne les
ait polluées (compteurs d'hygiène `*_anom` à zéro) — mais toute correction de ce
défaut **change nécessairement** la valeur future de ce biais, d'où la nécessité de
trancher où, structurellement, la comptabilité doit vivre.

## Décision

**La comptabilité des volumes injectés est retirée du corps décisionnel d'`update()`
et posée en point de passage unique, appelé à chaque sortie — anticipée ou non.**

`update()` devient un wrapper de quatre lignes :

```cpp
void PumpControllerClass::update() {
  unsigned long now = millis();
  _acctSettledThisTick = false;
  updateDosingCore(now);
  if (!_acctSettledThisTick) settlePumpAccounting(now);
}
```

L'ancien corps d'`update()` est renommé `updateDosingCore(now)`, **non réindenté**,
ses quatre `return` intacts. Chaque sortie anticipée appelle désormais elle-même
`settlePumpAccounting()` — seul appelant d'`accountPumpVolume()` — juste après avoir
forcé `applyPumpDuty(*, 0)`. Le wrapper d'`update()` est le filet structurel qui
couvrirait aussi une cinquième sortie anticipée future que personne n'aurait pensé à
instrumenter : **toute sortie, présente ou future, est comptabilisée par
construction**, qu'elle ait ou non pensé à appeler `settlePumpAccounting()`.

La décision du crédit et la transition d'état (`_acctPumpOn[]` / `lastSafetyTimestamp`)
sont rendues par le module pur [`src/acct_window_logic.h`](../../src/acct_window_logic.h)
(`evaluateAcctWindow()`, [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) :
`nextOn = onNow` **toujours**, ce qui rend la rémanence structurellement impossible
quel que soit l'appelant.

## Alternatives considérées

- **Option A (retenue) — point de passage unique** : voir ci-dessus. Corrige le
  défaut pour toute sortie présente *et future*, au prix d'un appel supplémentaire
  à poser à chaque nouvelle sortie anticipée (compensé par le wrapper filet).
- **Option B (rejetée) — purge branche par branche** : réinitialiser `_acctPumpOn[]`
  à `false` et `lastSafetyTimestamp` à `0` dans chaque branche de sortie anticipée,
  telle que proposée initialement par la spec. **Rejetée par le gate `pool-chemistry`
  (P1)** : une purge aveugle **perd le volume réellement injecté** pendant la
  fenêtre, sur deux sites où la pompe peut continuer de tourner en mode manuel
  (`flowAssistSuspend` ne coupe pas `manualMode`) ou être coupée en plein pouls
  (OTA, maintenance). Corriger un surcompte en introduisant un sous-compte aurait
  été **pire** : le sous-compte va dans la direction dangereuse (plus de produit
  réellement injecté sous le même plafond journalier). Et chaque branche ajoutée
  dans le futur aurait exigé qu'on se souvienne d'y ajouter la purge — aucun filet
  structurel.
- **Option C (rejetée) — ne rien faire, ne garder que les compteurs d'hygiène de
  feature-110** : ces compteurs **signalent** le défaut (tick de reprise anormalement
  long), ils ne le **corrigent** pas. Insuffisant dès lors que le défaut représente
  20 à 48 % du plafond journalier en queue de distribution.

## Conséquences

### Positives
- Le défaut de rémanence est fermé pour les quatre sorties anticipées actuelles
  **et** pour toute sortie future, sans dépendre de la mémoire de qui l'écrit.
- Bénéfice collatéral : le dernier tick ON avant une coupure, auparavant perdu,
  est désormais crédité exactement une fois.
- `*_anom` / `*_anom_ml` (feature-110) changent de rôle sans changer de seuil :
  d'un détecteur d'un défaut connu, ils deviennent un canari d'un chemin inconnu.

### Négatives / dette assumée
- **Discontinuité métrologique** : à firmware corrigé, `N` (démarrages crédités)
  augmente et `V` (volume compté) diminue — `δ = (V_compté − V_réel) / N` change de
  valeur **sans que le matériel ait bougé**. Aucun relevé de concordance postérieur
  à v2.61.0 n'est comparable aux deux campagnes de 2026 (+5,11 % sur 86 démarrages,
  +6,25 % sur 356). Tout relevé futur doit porter la version de firmware.
- `ph_total_injected_ml` / `orp_total_injected_ml` (MQTT, UI, `/get-config`) changent
  de signification : ils incluent désormais le volume des marches de l'assistant de
  débit en mode manuel, jusqu'ici invisible. La jauge de bidon descendra plus vite à
  consommation réelle inchangée.
- Aucun test natif ne peut démontrer que les quatre sorties anticipées atteignent
  toutes `settlePumpAccounting()` : `pump_controller.cpp` n'est pas compilable en
  `env:native` (FreeRTOS, Arduino, I2C). La garantie est structurelle (le wrapper),
  pas testée unitairement — compensée par une garde CI (`check_acct_settle_single_point.sh`)
  qui vérifie une condition nécessaire : un seul appelant d'`accountPumpVolume()`, et
  les trois lignes du wrapper présentes dans `update()`.

### Ce que ça verrouille
- **Toute nouvelle sortie anticipée d'`update()`/`updateDosingCore()` doit soit
  appeler `settlePumpAccounting()` elle-même, soit laisser le wrapper d'`update()`
  le faire — jamais purger `_acctPumpOn[]` / `lastSafetyTimestamp` directement.**
  Un reset direct contournerait le point de passage unique et réintroduirait un
  sous-compte.
- `accountPumpVolume()` ne doit avoir **qu'un seul appelant** (`settlePumpAccounting()`),
  et le wrapper d'`update()` ne doit jamais être réintégré dans `updateDosingCore()`
  sans conserver `_acctSettledThisTick` — ce verrou est outillé par
  `tools/check_acct_settle_single_point.sh` (CI, job « Comptabilité pompe — point de
  passage unique », `hygiene-specs.yml`).
- Toute campagne de concordance jerricane future doit être estampillée de la
  version de firmware : la discontinuité métrologique de ce correctif n'est pas la
  dernière qui se produira.

## Références

- Code : [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (`update()`,
  `updateDosingCore()`, `settlePumpAccounting()`, `accountPumpVolume()`),
  [`src/pump_controller.h`](../../src/pump_controller.h),
  [`src/acct_window_logic.h`](../../src/acct_window_logic.h)
- Tests : [`test/test_native_acct_window/`](../../test/test_native_acct_window/)
- Garde CI : [`tools/check_acct_settle_single_point.sh`](../../tools/check_acct_settle_single_point.sh)
- Spec : [bug-115](../../specs/features/done/bug-115-etat-de-comptabilite-remanent-aux-sorties-anticipees.md)
- Doc : [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md#correction-de-la-rémanence-bug-115-v2610--point-de-passage-unique),
  [docs/MQTT.md](../MQTT.md#️-changement-de-signification-de-ph_total_ml--orp_total_ml-bug-115-v2610),
  [docs/features/page-dosages.md](../features/page-dosages.md)
- ADR voisin : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)
  (logique métier pure, Humble Object), [ADR-0070](0070-fopen-interne-helper-unique-et-classes-de-contention.md)
  (même motif de point d'admission unique, appliqué à `fopen`)
