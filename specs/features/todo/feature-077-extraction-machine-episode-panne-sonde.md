# feature-077 — Extraction de la machine d'épisode de panne sonde en module pur

## Métadonnées

| Champ | Valeur |
|---|---|
| Statut | todo |
| Origine | Rapport `test-engineer` du cycle v2.25.1, 27/08/2026 |
| Priorité | Moyenne — dette de testabilité sur un chemin d'alerte de sécurité |
| Version cible | à définir |

## Domaines touchés

- `src/pump_controller.{h,cpp}` — extraction de `_sensorFaultActive` / `_sensorFaultSinceMs` / `_sensorFaultAlerted`
- Nouveau module pur `src/sensor_fault_episode.h` (ADR-0017)
- `test/test_native_sensor_fault_episode/` — nouvelle suite
- `docs/subsystems/pump-controller.md`

## Sécurité chimique

**Oui — validation `pool-chemistry` requise.** Il s'agit du chemin d'alerte qui
signale une sonde figée ou muette, c'est-à-dire l'absence de mesure fiable pour
la régulation. Un refactor qui casserait cette alerte rendrait la panne
silencieuse — la classe de défaut que la v2.25.1 corrige.

## Problème

La v2.25.1 a introduit une machine d'épisode de panne : une alerte `warning`
+ MQTT part une fois par épisode, après 60 s de panne continue
(`kDoseSensorFaultAlertDelayMs`), sans être redatée quand la panne change de
nature.

Cette logique vit dans `PumpControllerClass`, mêlée à `millis()`,
`systemLogger` et `mqttManager`. Elle est donc **non testable en natif**. Le
test-engineer a couvert ce qu'il pouvait — la prémisse (une panne continue
change bien de code 20↔21), la frontière panne / non-panne des 10 codes, la
valeur du délai — et a refusé de dupliquer la machine dans un test, ce qui
aurait été un trompe-l'œil.

**Restent non couverts :**

- la redatation sur la seule transition non-panne → panne ;
- l'unicité de l'alerte par épisode ;
- la clôture de l'épisode au retour du dosage ;
- **la robustesse au repliement de `millis()`**.

## Pourquoi le dernier point justifie cette spec à lui seul

Ce projet a connu **trois** défauts de repliement de compteur :

1. La retentative de lecture de pente au démarrage (v2.21.2) — un underflow
   produisait une boucle infinie. Corrigé en encodant la garde *dans* une
   fonction pure, précisément pour la rendre testable.
2. La comptabilité des impulsions de dosage — `tickPulse` a dû être rendue
   wrap-safe.
3. La grâce de ré-amorçage du filtre (v2.25.1, trouvé en test) — un drapeau
   jamais désarmé faisait se retourner une comparaison wrap-safe au bout de
   24,8 jours, masquant une panne de sonde pour 24,8 jours de plus.

Les trois fois, le défaut était invisible à la lecture et évident au test. La
machine d'épisode manipule `millis()` sur des fenêtres de 60 s avec un uptime
qui atteint 49,7 jours, et **elle n'est pas testée contre le repliement**. Le
motif est constant : une comparaison juste au premier tour qui se retourne au
second.

## Piste (proposée par le test-engineer)

```cpp
// src/sensor_fault_episode.h — module PUR (ADR-0017)
struct SensorFaultEpisode {
  bool     active  = false;
  uint32_t sinceMs = 0;
  bool     alerted = false;
};

// Renvoie true quand il faut alerter MAINTENANT (une seule fois par épisode).
bool noteSensorFault(SensorFaultEpisode& ep, bool isFault,
                     uint32_t nowMs, uint32_t delayMs);
```

Le temps devient un paramètre, `PumpControllerClass` ne garde que l'appel et
l'émission. Estimation du test-engineer : ~15 tests couvrent alors la
chronologie complète, repliement compris.

## Item connexe — `_gateFloorUntilMs` (audit v2.25.1)

Trouvé en auditant les comparaisons de temps de `src/sensor_filter.cpp` après le
troisième défaut de repliement. L'expiration de `_gateFloorUntilMs` (deadline
signée, `sensor_filter.cpp:220`) est bien évaluée dans le non-const
`setContext()`, mais **uniquement dans la branche `!closed`**.

Pour mordre, il faudrait `waterPresent` vrai ET `measuresValid` faux pendant
24,8 jours consécutifs, alors que cette fenêtre de stabilisation dure quelques
minutes. L'effet serait de plus fail-closed. **Ce n'est donc pas le même défaut
exact et il n'a pas été corrigé** — mais il relève de la même famille, et il
gate l'alimentation du filtre : à traiter avec un visa `pool-chemistry`, pas au
détour d'un refactor.

Deux autres comparaisons du module ont été auditées et sont saines : le cooldown
de ré-amorçage (intervalle fermé non signé, pire cas un refus, fail-closed) et
`ageMs()` (différence non signée unique, forme canonique).

## Item connexe PRIORITAIRE — `_mixingEndMs` (revue de clôture v2.25.1)

**Quatrième occurrence du motif, la plus conséquente, préexistante.**

`src/pump_controller.cpp:329-345` : `isMixing()` teste `(int32_t)(end - now) > 0`
et `_mixingEndMs` n'est **jamais remis à 0** après expiration. 24,8 jours après
le dernier dosage, la comparaison se retourne : l'appareil se croit en phase de
mélange et **refuse de doser pendant 24,8 jours**.

Fail-closed, donc sans danger chimique immédiat — mais un bassin privé de
régulation pH et ORP pendant près d'un mois, sans cause visible, sur un appareil
destiné à des tiers. Le déclencheur est banal : une longue période sans dosage,
c'est-à-dire une eau stable, ou un appareil laissé en mode manuel.

Ce défaut est **hors périmètre de la v2.25.1** (préexistant, non touché par le
cycle) et n'a délibérément pas été corrigé en fin de cycle : c'est un chemin de
dosage, il exige un visa `pool-chemistry` et non un correctif glissé après
l'approbation de la revue.

Correctif attendu : même principe que la grâce de ré-amorçage — un intervalle
fermé depuis un instant d'armement plutôt qu'une comparaison vers une échéance,
et une remise à zéro effective à l'expiration.

## Règle générale à retenir

Les quatre occurrences partagent une forme : **une comparaison vers une échéance
future** (`until`, `end`), juste au premier tour et fausse au second. La forme
sûre est **un intervalle fermé depuis un instant d'armement** :
`(now - from) < duree`, intrinsèquement bornée.

Tout nouveau code manipulant `millis()` devrait adopter cette forme, et tout
état temporel devrait être remis à zéro dans un chemin non-const plutôt
qu'évalué paresseusement dans un accesseur qui ne peut pas le nettoyer.

## Critères de clôture

- Chronologie complète testée en natif : ouverture, changement de nature sans
  redatation, alerte unique à l'échéance, clôture, réarmement du délai à
  l'épisode suivant.
- **Repliement de `millis()` couvert explicitement**, y compris un épisode qui
  enjambe le repli.
- Validation par mutation : une mutation redatant sur tout changement de code
  doit tuer au moins un test (c'était le bloquant de la revue v2.25.1).
- Aucun changement de comportement observable : mêmes alertes, mêmes délais.
- Visa `pool-chemistry`.
