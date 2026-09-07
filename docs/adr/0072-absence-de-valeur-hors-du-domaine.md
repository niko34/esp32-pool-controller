# ADR-0072 — Le domaine d'une valeur ne porte jamais l'absence de valeur

- **Statut** : Accepté
- **Date** : 2026-09-07
- **Décideurs** : architect, embedded-developer, web-ui-developer, test-engineer, code-reviewer, doc-writer
- **Spec(s) liée(s)** : [bug-098](../../specs/features/done/bug-098-drapeau-dosage-orp-perdu-a-l-ecriture.md), [bug-106](../../specs/features/done/bug-106-journee-archivee-a-zero-sur-compteur-en-recul.md)

## Contexte

**La règle que cet ADR énonce était déjà écrite deux fois dans le dépôt — dans deux fichiers, sous forme de deux remarques locales.**

[`src/history.h:23`](../../src/history.h:23), à propos des mesures lissées :

> `NaN = absence` (filtre non amorcé, porte de mesure fermée, agrégat HOURLY/DAILY) — **JAMAIS 0**, et jamais de repli sur le brut.

[`src/time_to_target_logic.h:78`](../../src/time_to_target_logic.h:78), à propos de l'état d'une journée archivée :

> **CHAMP DE BITS** (et non énumération) : une journée peut cumuler plusieurs circonstances, et les bits restent disponibles sans casser les pastilles déjà archivées.

Les deux bugs de ce lot sont **exactement ce qui arrive quand une règle existe sans être énoncée** : on la respecte là où on y pense, on l'oublie ailleurs. `history.h` interdisait le `0` pour la mesure lissée dans le champ voisin de celui qui, trois lignes plus bas, écrasait la voie de dosage ; `time_to_target_logic.h` défendait le champ de bits pour `state` et `flags` dans la structure même dont le champ `minutes` archivait `0` pour dire « pas de mesure ».

### Ce que les deux défauts faisaient — et ce n'est pas « perdre de l'information »

**`bug-098` — le domaine d'une grandeur discrète.** `saveToFile()` fusionnait `phDosing` et `orpDosing` en une seule clé booléenne :

```cpp
obj["d"] = point.phDosing || point.orpDosing;   // écriture : la voie disparaît
dp.phDosing = point["d"];                       // relecture : elle réapparaît en pH
```

À la relecture, `d == true` sur une injection **ORP seule** devenait `phDosing == true` : le firmware **inventait un dosage pH**. L'historique sous-déclarait la consommation de chlore et surdéclarait celle d'acide — puis `consolidateData()` figeait ce faux positif dans les agrégats **DAILY**, où il vit **75 à 90 jours**. Et il le figeait dans le jeu de données même de la [campagne de mesure du biais de dosage](../../specs/features/done/feature-110-compteur-d-impulsions-de-dosage.md) en cours.

**`bug-106` — le domaine d'une durée.** Quand le compteur de filtration recule (effacement de la NVS `pool-daily`, restauration d'usine, restauration de configuration en cours de tenue), la soustraction `holdStartFiltMin − t0FiltMin` n'a plus de sens et le ternaire de garde rendait **0**. Or `0` minute est **déjà une valeur légitime** de l'indicateur : « la cible était atteinte dès le départ ». L'interface rendait donc « 0h00 » dans la classe **`.ttt__day--good`**, simplement parce que `0 <= TTT_LONG_MIN` — **la mesure nulle s'affichait comme la meilleure journée possible**, et tirait vers le bas toute moyenne de comparaison de versions.

### Le point commun, et pourquoi il justifie un ADR

**Ces défauts ne perdent pas une donnée : ils en fabriquent une fausse et FAVORABLE.** C'est ce qui les rend insidieux. Un trou se voit — une courbe qui s'interrompt, une case vide, un `null` — et quelqu'un finit par demander pourquoi. Un chiffre flatteur, non : il se lit comme une bonne nouvelle, il conforte, et rien dans le système ne le contredit. Les deux bugs ont vécu jusqu'à ce qu'un développeur les trouve **en cherchant autre chose**.

## Décision

**Le domaine d'une valeur ne porte jamais l'absence de valeur.**

Deux applications, selon la nature de la grandeur :

1. **Grandeur continue (flottant)** → l'absence se dit **`NAN`**. Jamais `0`, jamais un repli sur une autre grandeur, jamais une valeur « neutre ».
2. **Grandeur discrète** (voie, cause, état) → l'absence, l'indétermination et les circonstances se disent par **un bit dans un champ de bits adjacent extensible**, jamais par une valeur de la grandeur elle-même.

Corollaires opératoires :

- Un **champ de bits** plutôt qu'une énumération dès qu'un cas peut se **cumuler** avec un autre. Les valeurs de bits sont un **contrat** (persistées et servies sur le fil) : elles ne se renumérotent pas, les bits libres restent réservés, et un client **teste des bits, jamais l'égalité**.
- Un **troisième état explicite** quand une relecture ne peut pas savoir : « dosage, voie **indéterminée** » est une information ; « dosage pH » déduit d'un booléen est une **invention**.
- Une **sentinelle dans le domaine** (`0xFFFF` minutes, `-1` mV, `0` pour « pas de mesure ») est **interdite** — c'est précisément la faute que ces deux bugs commettaient.

## Alternatives considérées

- **Sentinelle réservée dans le domaine de la valeur** — `minutes = 0xFFFF` pour dire « durée non mesurable » (piste 1 de bug-106) — **rejetée**. Elle recommet la faute dans le champ voisin : le domaine d'un `uint16_t` de minutes redevient porteur d'une information qui n'est pas une durée, chaque lecteur doit connaître la valeur magique, et un consommateur qui l'ignore affiche « 65535 minutes » — un mensonge **défavorable** cette fois, mais un mensonge. Le champ `flags` existait déjà, à trois octets de là, précisément pour ça.
- **Ne pas archiver la journée du tout** (piste 3 de bug-106) — **rejetée**. `tttHistoryCloseOrphans()` rouvrirait la case le lendemain et en ferait une « non atteinte à 0 minute » : le mensonge reviendrait par un autre chemin, et cette fois sans aucune trace de sa cause. Mieux vaut une journée **signalée** qu'une journée absente.
- **Deux clés booléennes séparées** dans `/history.json` (`"dp"` / `"do"`) — **rejetée**. Elle résout bug-098 sans énoncer la règle, coûte **plus** d'octets qu'un masque entier, et ne laisse aucune place pour un troisième état ni pour une voie future : une clé de plus par voie ajoutée.
- **Énumération à N valeurs** (`0 = aucun`, `1 = pH`, `2 = ORP`, `3 = les deux`) — **rejetée** malgré une taille identique. C'est un champ de bits qui refuse de le dire : l'extension exige d'énumérer les combinaisons (une voie de plus double la table), et un consommateur qui teste `== 1` casse dès qu'un bit inconnu apparaît. Le champ de bits impose la bonne discipline de lecture.
- **Clé de version de schéma** dans `/history.json` — **rejetée** au profit du **discriminant par type JSON** (voir Conséquences). Elle aurait imposé d'écrire, de relire et de faire vivre un numéro, alors que les deux écritures possibles sont déjà **exactement** séparables par leur type. Une clé de version est un contrat de plus à ne pas se tromper.
- **Laisser la règle à l'état de convention orale**, comme depuis le début — **rejetée**, et c'est le cœur de cet ADR : la règle était déjà écrite **deux fois**, et cela n'a empêché **ni l'un ni l'autre** des deux bugs. Une règle qui vit en commentaire local protège le fichier qui la porte, pas le système.

## Conséquences

### Positives

- **Un idiome nommé**, applicable à toute grandeur du projet, et deux exemples canoniques à citer.
- **Le masque `uint8_t doseMask`** ([`src/history.h`](../../src/history.h)) remplace deux booléens dont un seul survivait :

  | Bit | Constante | Sens |
  |---|---|---|
  | `0x01` | `kHistDosePh` | pompe pH active à l'instant du point |
  | `0x02` | `kHistDoseOrp` | pompe ORP (chlore) active |
  | `0x04` | `kHistDoseUnknownChannel` | **dosage, voie indéterminée** — point écrit par un firmware ≤ 2.46 |

  Les bits 3-7 restent libres : une voie de plus ne coûtera **pas un octet** de `DataPoint` (`static_assert(sizeof(DataPoint) == 28)` inchangé).
- **Le troisième état referme la boucle du mensonge.** Un point ancien se relit « voie indéterminée », jamais « pH » — l'invention est structurellement impossible.
- **`kTttFlagCounterReset = 0x20`** ([`src/time_to_target_logic.h`](../../src/time_to_target_logic.h)) dit « cette valeur n'est pas une mesure » sans toucher ni à `state` ni à `minutes` : le format servi à l'UI reste brut, et l'affichage réemploie le vocabulaire des journées sans entrée (pointillé estompé, aucune durée) — **ni « bonne journée », ni « échec »**.
- **Le format persisté RÉTRÉCIT.** `,"d":false` (10 o) et `,"d":true` (9 o) contre `,"d":0` … `,"d":7` (6 o) : **−1 113 o** sur la charge de référence de 339 points. Mesuré par balayage des longueurs de clé dans [`test/test_native_dose_mask/test_dose_mask.cpp`](../../test/test_native_dose_mask/test_dose_mask.cpp) (section 6, `3 147 → 2 034 o`), pas estimé.

### Négatives / dette assumée

- **La relecture doit masquer sur `0x07`, et non `0x03`** — et c'est le piège qui a été **effectivement rencontré pendant le cycle**. Avec `& (kHistDosePh | kHistDoseOrp)`, le premier cycle sauvegarde/relecture qui suit l'OTA — **cinq minutes** après la mise à jour — écrivait `"d":4` puis relisait `0` : le mensonge favorable revenait **par la porte de derrière, réintroduit par le correctif lui-même**. Le masque de relecture inclut donc `kHistDoseUnknownChannel`, aux **trois** sites de lecture (`history.cpp::loadFromFile`, `web_routes_data.cpp` pour `POST /history/import`, et le codec de test qui les modélise).
- **Discriminant par type JSON, sans clé de version** : `is<bool>()` ⇒ ancien format (voie indéterminée), `is<int>()` ⇒ masque, clé absente ⇒ `0`. **Limite assumée** : un `"d"` **flottant** (`3.0`) ou entier **64 bits hors plage `int`** ne satisfait ni l'un ni l'autre et retombe sur `0`, donc sur le mensonge favorable. Ni le firmware ni l'export ne produisent ces formes — il faut un fichier édité à la main ou un export étranger. Non traité pour ne pas élargir le discriminant à des types qui rendraient la séparation des deux formats moins nette.
- **Le drapeau `kTttFlagCounterReset` doit être posé à TROIS sites** ([`src/time_to_target_logic.cpp`](../../src/time_to_target_logic.cpp)) : le chemin vif (`tttUpdateElapsedMinutes`, l. 128), le recul entre `t0` et le début de tenue (l. 307) et l'archivage à la clôture (l. 326). L'oubli d'un seul rouvre le défaut sur un chemin.
- **Trou résiduel connu de bug-106** : le bit vit dans `TttLive` (RAM RTC) et n'atteint la NVS qu'à l'archivage. Un recul **puis** une remontée du compteur au-dessus de `t0`, **avec** une coupure secteur entre les deux, perd donc le bit — la journée se rouvre depuis l'anneau sans lui. Atténué par `kTttFlagPowerCycle`, qui signale la journée pour le **mauvais motif**. Non corrigé : le remède (une écriture NVS à chaque recul) coûterait de l'usure de flash pour un scénario à **trois conditions simultanées**.

### Ce que ça verrouille

- **Les valeurs de bits `kHistDose*` et `kTttFlag*` sont un contrat** : persistées dans `/history.json` et la NVS `pool-kpi`, servies par `GET /get-history` et `GET /get-kpi`. Elles ne se renumérotent jamais ; une voie ou une circonstance nouvelle **s'ajoute sur un bit libre**, un bit retiré du firmware reste **réservé**.
- **La clé `dosing` de `GET /get-history` est un entier**, définitivement. La transition a été conçue pour ne pas casser un client qui la lisait en booléen JavaScript (`0` falsy, `1`/`2`/`3`/`4` truthy) — mais le type ne reviendra pas au booléen.
- **La colonne CSV d'export s'appelle `dosing_mask`, pas `dosing`.** Réutiliser le nom aurait rendu un `1` **indécidable** — pH, ou voie inconnue ? — puisque les anciens exports écrivaient déjà `1` pour dire « il y a eu dosage ». La colonne `dosing` est **conservée** en booléen d'héritage à côté.
- **La forme `tttUpdateElapsedMinutes(TttLive&, uint32_t)` est un précédent assumé** — voir ci-dessous.

## Le précédent de forme : un accesseur qui marque son propre état

`tttUpdateElapsedMinutes(TttLive& st, uint32_t filtTotalMin)` prend son entrée par **référence non const** et la **mute** : un recul du compteur y pose `kTttFlagCounterReset`. La revue a examiné la forme et l'a **retenue**, pour trois raisons :

1. **Ce module n'est pas un jeu de fonctions pures, c'est une machine à états** dont tout le contrat est de muter `TttLive&` (`tttLiveReset`, `tttTick`, …). L'accesseur en fait partie ; l'isoler comme fonction pure serait une exception dans le module, pas la règle.
2. **[ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) porte sur l'absence de dépendance matérielle**, pas sur l'immutabilité. La fonction reste testable en natif, sans FreeRTOS ni LittleFS — c'est la propriété que l'ADR exige, et elle est intacte.
3. **Rendre le drapeau à l'appelant aurait déplacé sa pose à trois sites, dont un dans une autre unité de compilation** ([`src/time_to_target.cpp`](../../src/time_to_target.cpp), reprise après coupure). C'est-à-dire exactement l'oubli qui a produit bug-106 : le chemin **vif**, celui qui alimente l'instantané servi à l'UI, était précisément celui que la spec ne nommait pas. La fonction est le **point de passage unique** des trois chemins ; le drapeau va là.

### ⚠️ Condition de copie — à reprendre telle quelle

Cette forme **sera imitée**. Elle n'est correcte que parce que l'effet de bord est un **OR idempotent sur un champ de bits**. La fonction est appelée **jusqu'à une fois par seconde et par voie**, et **plusieurs fois par tick** sur le chemin de clôture.

Un **compteur** (`++`), un **horodatage**, un drapeau incrémental — toute écriture **non idempotente** — casserait la forme **EN SILENCE** : aucun test ne rougirait, seulement une valeur qui dérive avec la fréquence d'appel. Si un tel besoin apparaît, il faut **sortir l'effet de bord de l'accesseur**, pas l'y ajouter.

Ajouter un **« peek »** (lecture sans marquage, par exemple pour un aperçu d'interface) est possible, mais il **duplique le calcul** sans la pose du bit. Le partager en passant un booléen « ne pas marquer » ramènerait exactement le défaut de bug-106 : un chemin de lecture qui oublie de déclarer que sa valeur n'est pas une mesure.

La condition est inscrite dans l'en-tête de la déclaration ([`src/time_to_target_logic.h`](../../src/time_to_target_logic.h)) ; cet ADR en est la référence stable.

## Références

- Code : [`src/history.h`](../../src/history.h) (`kHistDose*`, `DataPoint::doseMask`), [`src/history.cpp`](../../src/history.cpp) (`saveToFile`, `loadFromFile`, `consolidateData`, `dedupAggregatedPoints`), [`src/history_export_chunk.h`](../../src/history_export_chunk.h) (`formatHistoryPointJson`), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) (`POST /history/import`), [`src/time_to_target_logic.h`](../../src/time_to_target_logic.h) / [`.cpp`](../../src/time_to_target_logic.cpp) (`kTttFlagCounterReset`), [`data/app.js`](../../data/app.js) (`dosingMaskFromApi`, `dosingMarksPlugin`, `tttCounterReset`)
- Tests : [`test/test_native_dose_mask/`](../../test/test_native_dose_mask/test_dose_mask.cpp), [`test/test_native_time_to_target/`](../../test/test_native_time_to_target/test_time_to_target.cpp), [`test/test_native_history_export/`](../../test/test_native_history_export/test_history_export.cpp)
- Specs : [`bug-098`](../../specs/features/done/bug-098-drapeau-dosage-orp-perdu-a-l-ecriture.md), [`bug-106`](../../specs/features/done/bug-106-journee-archivee-a-zero-sur-compteur-en-recul.md)
- Doc : [`docs/subsystems/history.md`](../subsystems/history.md), [`docs/subsystems/time-to-target.md`](../subsystems/time-to-target.md), [`docs/API.md`](../API.md)
- ADR liés : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure — portée précisée ici), [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) (clé absente ≠ `null` ≠ `0`, même famille de raisonnement), [ADR-0063](0063-definition-normative-du-temps-d-atteinte-de-cible.md) (définition normative du temps d'atteinte), [ADR-0064](0064-la-courbe-suit-le-nombre-affiche.md) (`NaN` de la mesure lissée), [ADR-0068](0068-partitions-layout-v5-flash-8-mo.md) (partition `history` 256 Ko, qui a levé l'obstacle de budget)
