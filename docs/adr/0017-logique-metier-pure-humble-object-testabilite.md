# ADR-0017 — Logique métier pure (Humble Object) séparée de la couche hardware pour testabilité native

- **Statut** : Accepté
- **Date** : 2026-06-27
- **Décideurs** : architect, pool-chemistry
- **Spec(s) liée(s)** : [feature-036](../../specs/features/done/feature-036-dosage-testable-decision-pure.md)

## Contexte

La décision de dosage (`canDose()` dans [`pump_controller.cpp`](../../src/pump_controller.cpp)) est la zone la plus sensible du firmware côté sécurité chimique : elle décide si une pompe doseuse peut injecter du pH-, du pH+ ou du chlore. Or elle n'avait **aucun test automatisé**.

`canDose()` lit directement une dizaine d'objets globaux (`sensors`, `filtration`, `mqttCfg`, `safetyLimits`, `millis()`, `esp_task_wdt_status()`, le ring buffer anti-rafale). Cette dépendance aux singletons concrets et à FreeRTOS/I²C rend tout test natif impossible sans shimmer toute la chaîne matérielle.

Le risque s'est matérialisé : le bug « pause mélange armée au démarrage » (v2.2.5) coupait l'injection auto après ~1 cycle de boucle (`minInjectionTimeMs` jamais respecté) et n'a été détecté qu'en test terrain. Un test unitaire de la décision l'aurait attrapé immédiatement.

Un précédent existe : `SensorFilter` (feature-025) est déjà un module **pur** sans Arduino, couvert par 17 tests natifs. Il faut décider **maintenant** si on généralise ce pattern à la décision de dosage — et plus largement à la logique de régulation — pour pouvoir l'étendre sans rediscussion lors des prochains refactors.

## Décision

La **logique métier sensible est extraite en modules purs** (pattern *Humble Object*), séparés de la couche hardware :

- Le module pur ([`src/dosing_logic.h`](../../src/dosing_logic.h) / [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp)) **n'inclut ni `<Arduino.h>`, ni FreeRTOS, ni I²C, ni `<vector>`/`<FS.h>`, ni `String`**. Il n'utilise que les en-têtes C (`<stdint.h>`, `<math.h>`), donc compile en natif.
- Les **entrées** sont une **struct POD** (`DoseInputs`) ; la **sortie** est une struct (`DoseDecision`) portant un **énum** de cause (`DoseRefusal`), pas une chaîne.
- La **coquille hardware** (`canDose()` dans `pump_controller.cpp`) reste « humble » : elle **collecte** les valeurs depuis les globals, appelle `evaluateDose()`, puis **mappe l'énum → String française** en y réinjectant les valeurs runtime. Les effets de bord (log edge-triggered, exposition WebSocket de la cause) restent dans la coquille.
- Le **temps est injecté en paramètre** (`runTimeMs`, `usedMs`) pour rendre les seuils temporels (ex. `minInjectionTimeMs`) testables sans attendre le temps réel.
- L'extraction est un **characterization refactor** : aucun verdict, cause, seuil ni ordre d'évaluation ne change (équivalence stricte validée par `pool-chemistry`).
- Chaque module pur a sa **suite de tests native** dans un dossier `test/test_*` dédié (`pio test -e native` compile un binaire par dossier).

## Alternatives considérées

- **Mocker les singletons `sensors` / `filtration` / `mqttCfg`** (rejetée) — exigerait d'introduire des interfaces virtuelles ou des doubles de test pour des objets concrets fortement couplés au matériel I²C/FreeRTOS. Coût élevé, surface de mock large et fragile, risque de tester le mock plutôt que la logique. Le pattern Humble Object atteint le même but (logique testée) en déplaçant la logique plutôt qu'en simulant le matériel.
- **Cause de refus en `const char*` statique dans le module pur** (rejetée) — aurait gardé le texte FR dans la logique pure. Rejetée car (1) elle mélange présentation (chaîne utilisateur) et décision, (2) elle empêche de réinjecter les valeurs runtime (`cal=X requis=Y`, `mode=X`) qui n'existent qu'au moment de la collecte côté coquille, (3) elle imposerait `<string.h>`/formatage au module pur. L'**énum + formatage dans la coquille** sépare proprement décision et libellé.
- **Statu quo (aucun test, logique dans `pump_controller`)** (rejetée) — laisse la zone la plus critique du firmware sans filet, comme l'a montré le bug pause-mélange.

## Conséquences

### Positives

- La décision de dosage est **couverte par des tests natifs rapides** (sans matériel), exécutables en CI/dev : 21 tests Unity dont un **verrou de non-régression** du bug pause-mélange v2.2.5.
- **Séparation nette** décision (pure, déterministe, auditable) / collecte+présentation (coquille). La table de vérité des gardes est lisible d'un coup d'œil dans `dosing_logic.cpp`.
- **Convention réutilisable** : les futurs refactors de régulation (calcul d'erreur, hystérésis, anti-windup) suivent le même découpage pur/coquille.

### Négatives / dette assumée

- **Frontière à maintenir manuellement** : la coquille doit garder la collecte des `DoseInputs` synchronisée avec les globals, et le mapping énum → String synchronisé avec `DoseRefusal`. Un oubli de champ côté collecte ne serait pas attrapé par les tests purs (ils testent la décision, pas la collecte).
- **Léger éclatement** : une décision est désormais répartie sur deux fichiers (module pur + coquille).

### Ce que ça verrouille

- Toute évolution future de la **décision de dosage passe par `dosing_logic`** et doit conserver l'équivalence stricte (validation `pool-chemistry`).
- La **frontière String/POD** est posée : les libellés français restent côté coquille, jamais dans la logique pure.
- Les modules de logique pure restent **sans dépendance Arduino/FreeRTOS** (en-têtes C uniquement) pour rester compilables en natif.
- `pio test -e native` reste la porte d'entrée des tests : un nouveau module pur ajoute un dossier `test/test_*` + son entrée dans `build_src_filter`.

## Références

- Code : [`src/dosing_logic.h`](../../src/dosing_logic.h), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (coquille `canDose`), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) (précédent pur, feature-025)
- Tests : `test/test_native_dosing/`, `test/test_native_sensor_filter/`, [`platformio.ini`](../../platformio.ini) (env `native`)
- Spec : [`specs/features/done/feature-036-dosage-testable-decision-pure.md`](../../specs/features/done/feature-036-dosage-testable-decision-pure.md)
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md), [`docs/BUILD.md`](../BUILD.md)
- Voir aussi : [ADR-0016](0016-regulation-p-temporisee-vs-pid.md) (régulation P temporisée, feature-025)
