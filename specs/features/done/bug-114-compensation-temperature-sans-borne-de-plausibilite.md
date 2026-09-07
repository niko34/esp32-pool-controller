# bug-114 — La compensation de température des sondes n'a pas de borne de plausibilité

## Métadonnées
| | |
|---|---|
| Type | bug (intégrité de mesure → sécurité chimique) |
| Statut | **terminé (2026-09-07)** — tests verts, revue OK, livré en v2.47.1 |
| Priorité | **Haute** — fausse le pH sans toucher la calibration, donc invisible aux gardes existantes |
| Version cible | **2.47.1** |
| Relevé par | `pool-chemistry`, 2026-09-07, question de complétude de l'audit des chemins d'intégrité de mesure |
| Specs liées | `bug-save-config…` (l'audit qui l'a fait remonter), `feature-070` (routes de débogage), `feature-068` (sonde de température figée) |

## ⚠️ VERDICT `pool-chemistry` DU 2026-09-07 — LA CIBLE DE CETTE SPEC SE DÉPLACE

**Ce n'est pas la sonde, c'est l'OFFSET DE CALIBRATION.**

Les lectures DS18B20 sont **déjà bornées** à `(−55 ; 125)` °C, avec rejet de `85.0`
(power-on reset du capteur) et de `DEVICE_DISCONNECTED_C` — `src/sensors.cpp:324-326`.
Même aux bornes de cette fenêtre, l'erreur de pH induite plafonne à **0,05-0,07 pH**.

**Le trou réel** : `mqttCfg.tempCalibrationOffset` est écrit **sans aucune borne**
(`src/web_routes_config.cpp:1027`), et `getWaterTemperature()` = brut + offset alimente
la commande `RT` envoyée à l'EZO. Donc :
- un offset de **+1000** produit `RT,1025.0` ⇒ **+0,15 pH** d'erreur ;
- un offset très **négatif** fait tendre `T_comp` vers 0 K ⇒ erreur **NON BORNÉE**.

C'est une **entrée de configuration**, pas une mesure — donc une valeur qu'un opérateur
(ou une écriture non authentifiée, cf. le correctif de `/save-config` en v2.25.0) peut
poser durablement en NVS.

### Chiffrage de référence (méthode : pente de Nernst)
`pH_lu − pH_vrai = (pH_vrai − 7) × (T_vrai − T_comp)/T_comp` (T en kelvins), soit
**≈ 0,7 millième de pH par °C d'erreur**. Sens : `T_comp` trop haute ⇒ lecture tirée
vers 7 ⇒ pH lu trop basique ⇒ **sur-dosage** d'acide (brut sous 7).

### Correctif retenu
1. **Clamper l'offset à ±5 °C.** Motif : un DS18B20 dérive de **dixièmes** de degré, pas
   de degrés. Clamper **à l'écriture** (route de configuration) **et** **à l'émission**
   (défense en profondeur : une NVS déjà polluée par une version antérieure).
2. **Fenêtre acceptée pour `RT` : [0 ; 40] °C.** Motifs : 0 °C est le plancher de l'eau
   liquide d'un bassin extérieur (au gel la filtration est arrêtée et les mesures n'ont
   aucun sens) ; 40 °C laisse ~6 °C de marge au-dessus de toute eau de bassin plausible
   en canicule. ⚠️ **Ne PAS resserrer sur 15-30** : cela rejetterait des températures
   **vraies** (hors saison, canicule) et dégraderait la compensation au lieu de la
   protéger.
3. **Repli en CASCADE, jamais de suspension** : valeur plausible courante → **dernière
   valeur plausible** si son âge est inférieur à la fenêtre stale existante →
   `kEzoFallbackTempC` = 25 °C. Journaliser `warning` à chaque substitution, et
   `critical` si le repli à 25 °C **dure**. Coût maximal de ce repli : **± 0,01 pH**.

### ⛔ « Suspendre la compensation » N'EXISTE PAS — option interdite
Ne pas envoyer `RT` (`src/atlas_ezo.cpp:149-155`) fait retomber sur `R` : le module
**conserve sa dernière température interne**, silencieusement périmée. C'est pire que
n'importe quel repli, puisque la valeur devient invisible et indatable.

### À vérifier avant de figer la borne haute — TRANCHÉ, ET NON RÉSOLU
Recherche faite dans le dépôt le **2026-09-07** : **aucune trace** de la plage admise
par la commande `RT` (`docs/subsystems/sensors.md` ne documente que les **délais** de
transaction). La borne de **40 °C** est donc gravée comme **borne de PRUDENCE**, et
**signalée comme non confirmée par la datasheet partout où elle est citée** (constants.h,
tests, `docs/subsystems/sensors.md`, `docs/features/page-temperature.md`, ADR-0074).
Motif de cette insistance : une borne présentée comme constructeur alors que personne
n'a lu la datasheet est le mécanisme qui a produit une série de valeurs fausses dans ce
dépôt en une journée (ADR-0073). Si la datasheet est un jour consultée, reprendre
`constants.h` **et** `test_native_temp_compensation`, en repassant par `pool-chemistry`.

### Tests exigés (natifs, validés par mutation)
Clamp de l'offset (±5), fenêtre [0 ; 40], **cascade** de repli, et non-régression :
température figée ⇒ `RT` inchangé, planning inchangé, dosage inchangé.

### Lien avec `feature-068`
Cette spec et `feature-068` se traitent **ensemble**, mais le partage est net : là-bas on
**instrumente** un état figé qui n'a aucune conséquence chimique mesurable (≈ 0,001 pH) ;
ici on **borne** une entrée dont la conséquence est réelle et potentiellement non bornée.

## Domaines touchés
- [x] Capteurs (`src/atlas_ezo.cpp:150`, commande `RT,<tempC>`)
- [x] Régulation — **indirectement, et c'est tout le sujet**
- [ ] Web — [ ] MQTT — [ ] Frontend — [ ] OTA

## Agents requis
- [x] architect · [x] **`pool-chemistry` obligatoire** · [x] embedded-developer
- [x] test-engineer · [x] code-reviewer · [x] doc-writer

## Description
La commande `RT,<tempC>` envoyée aux sondes EZO applique la **compensation de
température** de la mesure de pH. Aucune borne de plausibilité n'est visible à ce
niveau : **une température fausse fausse le pH**, sans toucher à la calibration.

## Pourquoi c'est sérieux, et différent des autres chemins
L'audit du 07/09 a établi que les filets de sécurité chimique du projet — plancher
acide 6,70 corrigé / 6,20 brut, inhibition ORP à 7,60 — sont calculés **sur la mesure**.
Ils sont dérivés de `kPhOffsetMax` et couvrent donc l'**offset**, pas les autres
falsifications de la mesure. Une compensation de température erronée est **exactement**
ce cas : la mesure sort fausse, et toutes les gardes valident la falsification.

Les seules bornes réelles restent `maxPhMlPerDay` (défaut 300 mL/j) et la limite
horaire : elles plafonnent le **débit**, mais le quota se réarme à minuit — la dérive
cumulative n'a **pas** de plafond.

## Lien avec `feature-068`
`feature-068` traite d'une sonde de température **figée** (relevée pendant l'incident
du 16/08) dont la valeur continue d'alimenter la compensation. Les deux specs se
touchent : `feature-068` décide quoi faire d'une température qu'on **sait** fausse,
celle-ci borne ce qu'on **accepte** d'envoyer. **À traiter dans cet ordre ou ensemble.**

## Comportement attendu
- Borner la température envoyée à un intervalle physiquement plausible pour un bassin
  (l'intervalle exact est à trancher par `pool-chemistry`, **pas** par le développeur).
- Refuser et **journaliser** une valeur hors bornes plutôt que de la transmettre.
- Décider explicitement du repli : dernière valeur plausible, valeur par défaut, ou
  suspension de la compensation — chacun a une conséquence différente sur le dosage,
  et c'est un arbitrage chimique.

## Sécurité chimique
- Dosage impliqué : **oui, indirectement** — la mesure pilote le dosage d'acide.
- **Risque identifié** : une borne mal choisie pourrait rejeter une température
  légitime (bassin très froid en hiver, très chaud en canicule) et dégrader la
  compensation au lieu de la protéger. `pool-chemistry` doit fixer l'intervalle **et**
  le repli.

## Critères d'acceptation
- [x] Intervalle de plausibilité fixé par `pool-chemistry`, avec son motif écrit — offset `[−5 ; +5]` °C (`kTempCalOffsetMin/Max`), fenêtre `RT` `[0 ; 40]` °C (`kEzoCompTempMinC/MaxC`), motifs dans `src/constants.h` et ADR-0074
- [x] Une valeur hors bornes est refusée **et** journalisée, jamais transmise — écrêtage aux **quatre** points (écriture, chargement NVS, émission mesure, émission `RT`) + `warning` nommant valeur demandée / valeur retenue / IP cliente ; `warning` à chaque substitution de compensation, `critical` si le repli **dure** > 10 min
- [x] Le repli est explicite et validé chimiquement — cascade `mesure → dernière plausible (âge ≤ 20 s) → 25 °C`, coût maximal **±0,01 pH** ; « suspendre la compensation » **interdit** et documenté
- [x] Test natif de la borne, **validé par mutation** — `test/test_native_temp_compensation/`, **13 mutants : 12 tués + 1 équivalent PROUVÉ** (garde `NaN` de la fenêtre : IEEE-754 rend `NaN >= borne` faux ; vrai uniquement sans `-ffast-math`, **vérifié absent de `platformio.ini`**)
- [x] Aucune régression sur la compensation en régime normal — sentinelles structurelles : `pump_controller.cpp` ne référence aucun nouveau symbole, `DoseInputs` n'a aucun champ de température, `computeAutoWindow` reste bornée `[60 ; 1440]` min même sur −127 / −55 / 0 / 85 / 125 / 1025

## Notes de clôture (2026-09-07, v2.47.1)

**Livré** : `src/temp_compensation_logic.h` (**nouveau**, logique pure ADR-0017 : `clampTempCalOffset`, `isTempCalOffsetInRange`, `isCompensationTempPlausible`, `resolveCompensationTemp`, `tempCompStageLabel`), `src/constants.h` (bornes + motifs + seuils de journalisation), `src/sensors.{h,cpp}` (`_resolveEzoCompensationTemp`, clamps 3/4 et 4/4, accesseurs de diagnostic), `src/config.cpp` (clamp 2/4), `src/web_routes_config.cpp` (clamp 1/4), `src/mqtt_manager.{h,cpp}` (`ezo_comp_t` / `ezo_comp_stage`), `test/test_native_temp_compensation/` (**nouveau**).

**Ce que ce cycle a appris, et qui vaut plus que le correctif** :

1. **Le compte des points de clamp a été annoncé « deux », puis « trois », avant d'être grepé et fixé à QUATRE** — dans les commentaires mêmes qui devaient empêcher une simplification future. L'erreur venait du **brief de l'agent principal** et s'est propagée de là (ADR-0073, corollaire : « un chiffre relayé sans vérification devient un fait établi »).
2. **La sentinelle de test qui prouve le câblage était satisfaite par un COMMENTAIRE** : sa première version comptait `clampTempCalOffset` au niveau du **fichier**, si bien que la mention `// bug-114 : clampTempCalOffset()` de `src/config.cpp:5` suffisait — **supprimer l'appel réel laissait la suite verte**. Corrigée : code **dépouillé de ses commentaires**, chaque point visé **dans sa fonction** par **signature**. Démonstration faite pour les **quatre** suppressions : ancienne sentinelle **verte**, nouvelle **rouge** à chaque fois.
3. **Le motif du clamp au chargement NVS a un nom** : sans lui, `/get-config` exposerait une valeur que le firmware n'applique **pas**, et la règle du dépôt « toujours lire `/get-config` avant tout diagnostic chiffré » deviendrait un **piège** — un diagnostic du 06/09 avait déjà déraillé sur ce motif.

**Documentation** : ADR-0074 (décision conjointe avec `feature-068`), `docs/subsystems/sensors.md`, `docs/API.md`, `docs/MQTT.md`, `docs/features/page-temperature.md`, `docs/features/page-dashboard.md`, `docs/subsystems/ws-manager.md`, `CHANGELOG.md` `[2.47.1]`, `docs/user/src/guide-utilisation.html`.

**Reste ouvert** : la plage réellement admise par `RT` sur les modules Atlas EZO (borne de 40 °C = prudence, **non confirmée**).

## Notes pour agent
⚠️ Ne pas confondre avec l'**offset pH**, qui n'est **pas** une suspicion de sonde : il
corrige un décalage de **mesure** (courants de fuite), et la sonde est prouvée saine
(zéro à −1,03 mV). Ce principe a été rappelé plusieurs fois par l'utilisateur.
