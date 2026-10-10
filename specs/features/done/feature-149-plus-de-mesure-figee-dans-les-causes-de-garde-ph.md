## Métadonnées
- **ID** : feature-149
- **Slug** : plus-de-mesure-figee-dans-les-causes-de-garde-ph
- **Type** : bug
- **Statut** : livré — tests verts, revue OK (non commité, voir Notes d'implémentation)
- **Priorité** : normale
- **Version cible** : 2.65.0
- **Dernière mise à jour** : 2026-10-10
- **Spec(s) liée(s)** : feature-148 (dédoublonnage bandeau/badge — même écran, autre défaut) ·
  bug-135 (origine du libellé verrouillé de la ligne 1025)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`) — un seul site, `data/app.js:4012` (message L4)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry** — OBLIGATOIRE. Le texte de la ligne 1025 est verrouillé MOT À MOT par
      ce gate (bug-135, 2026-10-05) ; le modifier exige un nouveau passage. Les textes MQTT
      (`publishAlert`) et `critical` associés sont également de son ressort.
- [ ] ux-ui-designer — non : aucune mise en page touchée, aucune surface nouvelle.
- [x] embedded-developer
- [x] web-ui-developer — UN site : `data/app.js:4012`, passage à une décimale (GO du gate, 2026-10-10).
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Les causes de refus posées par les gardes pH inscrivent la mesure du moment dans leur texte.
Ce texte est latché et ne se régénère plus : il finit par contredire la carte, qui est vive.
Appliquer aux quatre sites pH le remède **déjà retenu et appliqué sur la voie ORP** — citer le
SEUIL, qui est une constante, et laisser la mesure vive à la carte.

## Contexte

Constat utilisateur du 2026-10-10, capture à l'appui : le bandeau annonçait
« pH **5.79** sous le plancher de sécurité (6.70) » pendant que la carte affichait **6.5**,
avec 39 minutes d'écart. Relevé en direct sur l'appareil le même jour : `/data` donnait
`ph = 6.58` et `phDoseBlockedReason` citant « pH **6.54** ».

### ⚠️ RELEVÉ DE PRODUCTION DU 2026-10-10 (v2.64.0) — l'argument décisif

Second relevé, pris après déploiement, bien plus parlant que le premier :

| | |
|---|---|
| pH vivant (carte) | **6.67** |
| texte du bandeau | « pH **6.70** sous le plancher de sécurité (**6.70**) — dosage acide suspendu. » |
| bloqué depuis | 306 s |

**Le même nombre apparaît deux fois, et la phrase se contredit** : elle affirme qu'un pH de
6,70 est « sous » 6,70.

**Ce n'est pas un hasard, c'est structurel.** `setGuardRefusalCause()` étant un edge-trigger,
il est appelé **au franchissement du seuil** — instant où la mesure vaut, par construction, le
seuil lui-même (à la résolution d'affichage près). La valeur inscrite n'apporte donc **aucune
information au moment où elle est écrite**, puis devient **fausse** à mesure que le pH évolue.
Elle est inutile tout de suite et trompeuse ensuite.

⇒ Cet unique relevé suffit à clore le débat ouvert ci-dessous entre le verdict du 2026-10-05
(texte imposé AVEC la mesure) et celui du 2026-10-08 (la mesure est un défaut) : citer la
mesure au point de déclenchement n'a **jamais** eu de valeur diagnostique. Le gate garde la
décision, mais il la prend sur ce fait, pas sur une préférence de style.

**Mécanique du défaut.** `setGuardRefusalCause()` (`src/pump_controller.cpp:759`) prend la
propriété de la chaîne ; elle n'est effacée qu'à la relâche de la garde. Les appels sont des
**edge-triggers** — par exemple `if (low && !_phLowBlockActive)` (ligne 1009). Le texte, valeurs
comprises, est donc figé à l'instant du déclenchement pour toute la durée du blocage.

### ⚠️ Le remède est DÉJÀ ÉTABLI — cette spec ne fait que le propager
Le même défaut a été constaté en production le **2026-10-08** sur la voie ORP, et corrigé. Le
commentaire de `pump_controller.cpp:1452-1461` porte la décision :

> « La cause de refus est LATCHÉE (voir setGuardRefusalCause) : elle n'est réécrite qu'en cas
> d'AGGRAVATION, délibérément — réécrire à chaque mesure faisait battre deux libellés
> contradictoires, un critical et une alerte MQTT par oscillation, et chassait le journal de la
> partition. **Conséquence : toute MESURE inscrite ici se fige et finit par contredire la carte,
> qui est vive.** Signalé en production le 2026-10-08 (message à 8.28 pendant que la carte
> affichait 8.1). **On y met donc le SEUIL, qui est une constante et ne périme jamais** — et qui
> est de toute façon le chiffre utile : il dit où redescendre. La mesure vive est déjà affichée
> sur la carte pH, à côté. »

Ce commentaire écarte aussi, avec son motif, la solution naïve : **rafraîchir le texte à chaque
tick est explicitement REJETÉ** (libellés contradictoires, rafale de `critical`, alertes MQTT
par oscillation, journal chassé de la partition). S'y ajoute la discipline mémoire du projet
(CLAUDE.md § Mémoire, ADR-0082) : reconstruire une `String` sur un chemin périodique est
précisément ce qu'on évite. **Ne pas rouvrir cette option sans argument neuf.**

### État réel : corrigé d'un côté, pas de l'autre
Recensement exhaustif des appels à `setGuardRefusalCause()` :

| Ligne | Voie | Contenu | État |
|---|---|---|---|
| 1279 | ORP | « pH indisponible / stabilisation » — aucune valeur | pas de mesure figée |
| 1429 | ORP | seuil `kPhCeilingRawHard` seul, **à 2 décimales** | pas de mesure figée ; décimales NON examinées |
| 1462 | ORP | seuil `kPhOrpInhibit` seul | ✅ **corrigé le 2026-10-08** |
| 1139 | pH | « régulation basculée en manuel » — aucune valeur | pas de mesure figée ; **point final manquant**, corrigé en cours de lot |
| **916** | pH | `String(phRaw, 2)` | ❌ **se périme** |
| **938** | pH | `String(ph, 2)` | ❌ **se périme** |
| **994** | pH | `String(phRaw, 2)` **et** `String(ph, 2)` | ❌ **se périme, deux fois** |
| **1025** | pH | `String(ph, 2)` | ❌ **se périme — cas signalé** |

La voie ORP ne porte aucune mesure figée ; la voie pH n'a jamais reçu le correctif.

### ⚠️ Tension à trancher par le gate, explicitement
Le texte de la ligne 1025 a été **imposé mot à mot le 2026-10-05** (bug-135), mesure comprise :
commentaire ligne 1016, « Texte imposé MOT À MOT par le gate pool-chemistry — ne pas reformuler
sans nouveau passage ». **Trois jours plus tard**, le même gate établissait sur la voie ORP
qu'inscrire une mesure est un défaut. Les deux décisions sont en conflit ; la plus récente n'a
jamais été reportée sur le pH. **C'est au gate de trancher**, pas à l'implémentation — et la
spec doit obtenir ce verdict AVANT toute modification de libellé.

## ✅ VERDICT `pool-chemistry` — GO SOUS CONDITIONS (2026-10-10, deux passages)

### Passage 1 — la contradiction est levée
Le gate **lève la clause « mot à mot » de bug-135 sur le contenu CHIFFRÉ** des quatre textes.
Motif retenu : les appels étant des edge-triggers, la mesure vaut le seuil à l'instant de
l'écriture — « nulle au mieux (relevé 6.70 sous 6.70), fausse ensuite ».

**Reste verrouillé de bug-135** : la priorité inversée (1025 posé inconditionnellement), la
condition de discriminance `if (!low)` de 994/981, les seuils, l'hystérésis, le latch, le code 250.

### Passage 2 — une décimale (décision utilisateur du 2026-10-10)
GO pour `String(kPhXxx, 1)`. Argument du gate : « un seuil de sécurité n'a pas à s'afficher plus
précisément que la mesure qu'on lui compare — c'est précisément ce qui fabrique la comparaison
impossible (carte 6,7 / bandeau 6,70) ».

### TEXTES VERROUILLÉS MOT À MOT après ce second passage
```cpp
// 916
"pH brut au-dessus de la butée de sécurité (" + String(kPhCeilingRawHard,1) + ") — dosage pH+ suspendu."
// 938
"pH au-dessus du plafond de sécurité (" + String(kPhCeilingCorrected,1) + ") — dosage pH+ suspendu."
// 994 — REMPLACÉ (addendum 2 du gate) : l'ancien texte faisait 156 o pour un
// tampon de 96 et perdait l'instruction finale. Action d'abord, constat ensuite.
"Vérifiez l'offset pH : brut sous " + String(kPhFloorRawHard,1) + ", corrigé au-dessus de " + String(kPhFloorCorrected,1) + " — dosage acide suspendu."
// 1025
"pH sous le plancher de sécurité (" + String(kPhFloorCorrected,1) + ") — dosage acide suspendu."
```
Seuils lus **par constante**, jamais en littéral. `String(x,1)` rend un **point** décimal : ne
pas écrire « 6,7 » en dur.

### (B) Ligne 994 — tranchée : RETIRER les deux mesures
Le sens diagnostique **ne tient pas aux deux nombres** : il tient au fait que la butée brute morde
alors que la garde corrigée ne morde pas (`if (!low)`). L'opposition est **structurelle**, la
phrase l'énonce déjà.
⛔ **Interdit** d'y substituer la valeur de l'offset (elle peut changer pendant le latch — c'est
même l'action demandée — et se périmerait pareil). ⛔ **Interdit** d'y introduire « sonde »,
« calibration » ou « recalibrer » : l'offset corrige un décalage de mesure, la sonde est saine.

### (C) Journaux et MQTT — ils GARDENT la mesure
Horodatés et émis une seule fois au franchissement : un chiffre y est daté, il ne ment pas.
⚠️ **Exception bloquante** : l'alerte périodique du mode Programmé (**1152-1157**) réémet
`_lastRefusalCause[0]` **toutes les heures**. Elle hérite du correctif, et il est **interdit**
d'y injecter une mesure vive (chemin périodique, ADR-0082).

### ⛔ EXCLUSION — NE PAS APPLIQUER LA RÈGLE PAR RÉFLEXE
`kPhGuardReleaseHysteresis = 0.05` n'est **PAS ronde au dixième**. Les messages de **relâche**
(920-923, 943-946, 1003-1006) et les bandes d'hystérésis ORP (1410-1412, 1442-1444) **restent à
DEUX décimales**. Vérifié :

| Relâche | Valeur réelle | Arrondie | Erreur |
|---|---|---|---|
| garde basse | 6,70 + 0,05 = **6,75** | 6,8 | **au-dessus** du vrai point |
| garde haute | 7,60 − 0,05 = **7,55** | 7,5 | **en dessous** du vrai point |

Annoncer une reprise de dosage à un pH que l'appareil n'utilise pas.

### CONDITION BLOQUANTE AJOUTÉE PAR LE GATE
Un **`static_assert`** garantissant que chaque seuil cité est **rond au dixième**. Sans lui, un
futur `kPhSafetyMargin` à 0,45 ferait afficher « 6,8 » pour un seuil réel à 6,75 — **un seuil de
sécurité faux**. C'est la condition qui rend la décimale unique sûre dans la durée, pas
seulement aujourd'hui.

### (D) Frontend — message L4, `data/app.js:4012`
**GO pour `toFixed(1)`** : le centième d'une mesure est du bruit (±0,08 sur 10 min), et ce texte
est **recalculé à chaque rendu** — il ne se périme pas, aucun conflit avec la règle des causes
latchées.
🔒 **Reste verrouillé** : l'affichage en échelle **corrigée**, le test de seuil en échelle
**brute** (offset retranché, ligne 4006). Ne pas « corriger » ce signe.
📝 Le commentaire 4008-4011 **doit être réécrit** : il justifie aujourd'hui les deux décimales.

### 🚨 ADDENDUM 2 — DÉFAUT DE PRODUCTION PRÉEXISTANT : LE TEXTE 994 EST TRONQUÉ

`kDoseBlockedReasonMaxLen = 96` (95 octets utiles). Mesures **après substitution des seuils** :

| Site | Taille | Verdict |
|---|---|---|
| 916 | 75 o | tient |
| 938 | 68 o | tient |
| 1025 | 66 o | tient |
| **994, texte d'origine** | **~160 o** | **tronqué EN PRODUCTION AUJOURD'HUI** |
| 994, 1re proposition du gate | 156 o | tronqué aussi |
| **994, texte final** | **91 o** | tient, marge de 4 o |

Le `strlcpy` (`pump_controller.cpp:610`) coupait à « …reste au-dessus du pla ». **La seconde borne
et l'instruction « vérifiez l'offset pH » n'ont jamais atteint l'écran.** Ce lot répare donc un
défaut antérieur, pas seulement la valeur périmée.

**Agrandir le tampon est ÉCARTÉ** : `char _lastRefusalCause[2][96]` (`pump_controller.h:209`) plus
deux copies sur pile (`web_routes_data.cpp:496-497`, `ws_manager.cpp:892-893`) ; `ws_manager.cpp:667`
note que ces causes saturent déjà la limite — contrainte assumée.

**Règle posée par le gate pour toute cause de garde à venir : ACTION D'ABORD, CONSTAT ENSUITE.**
C'est ce qui survit à une troncature.

**Noms des gardes supprimés, validé** : « butée de dernier recours » et « plancher » sont du
vocabulaire interne. « brut sous X, corrigé au-dessus de Y » porte la structure en entier — deux
échelles, deux inégalités opposées.

**Bénéfice collatéral à documenter** : l'alerte horaire du mode Programmé (`:1152`) réémet
`_lastRefusalCause[0]` ; elle était tronquée elle aussi et **redevient complète**.

### 🔒 CONDITION DE TEST BLOQUANTE (gate, addendum 2)
Un test natif doit **mesurer la longueur de chaque cause APRÈS substitution des seuils** et
échouer au-delà de **95 octets**. Sans lui, le défaut se réintroduit en silence — il a vécu en
production sans être vu.

### Tests de sécurité demandés par le gate
- Cause posée au déclenchement, **inchangée après N minutes** quelle que soit l'évolution du pH.
- **Mutation** : réintroduire `String(ph,2)` dans un libellé → un test doit mourir.
- Non-régression : discriminance 994 (`!low`), inconditionnalité 1025, hystérésis de relâche, code 250.
- Oscillation seuil ± hystérésis : **aucune rafale** de `critical` ni d'alerte MQTT.
- Vérifier que les nouveaux textes tiennent dans `kDoseBlockedReasonMaxLen` (994 est le plus long, il raccourcit).

---

## Comportement attendu

Aucune cause de refus de garde pH ne cite plus une **mesure**. Chaque texte cite le **seuil**
concerné — constante, jamais périmée, et plus utile à l'utilisateur puisqu'il dit où revenir.
La valeur vive reste lue sur la carte du capteur, à quelques centimètres.

Exemple pour le cas signalé (**formulation à faire valider par `pool-chemistry`, non acquise**) :

> « pH sous le plancher de sécurité (6,70) — dosage acide suspendu. »

**Ce qui ne change pas** : le déclenchement des gardes, leurs seuils, l'hystérésis de relâche,
le code de blocage (250), le latch et sa règle de réécriture sur aggravation, et l'affichage
côté frontend (il rend la chaîne telle quelle).

**Journaux et MQTT** : les messages `systemLogger.critical()` et `mqttManager.publishAlert()`
voisins peuvent, eux, continuer de citer la mesure — ils sont **horodatés**, donc un chiffre y
est daté par construction et ne ment pas. Décision à confirmer par le gate : l'arbitrer
explicitement plutôt que de l'aligner par réflexe sur l'affichage.

### Cas limites
- **Ligne 994** : le texte oppose délibérément le brut au corrigé pour pointer un problème
  d'offset. Y retirer les deux mesures retirerait le sens même du message. **Cas à traiter
  spécifiquement** — l'opposition peut peut-être se dire avec les deux SEUILS. Ne pas appliquer
  la règle mécaniquement ici.
- **Aggravation successive** : quand une garde plus grave remplace la précédente, le texte est
  réécrit. Vérifier qu'aucune combinaison ne laisse une chaîne orpheline.
- **Relâche puis re-déclenchement rapide** (oscillation autour du seuil + hystérésis) : le texte
  se repose. Sans mesure dedans, deux déclenchements successifs produisent la même chaîne — ce
  qui est l'effet voulu, et supprime le battement de libellés.
- **Rétrocompatibilité d'affichage** : le frontend affiche `*DoseBlockedReason` tel quel pour le
  code 250 (`data/app.js:6688-6695`). Aucun parsing du texte nulle part — **à vérifier par grep**
  avant de conclure, côté `data/` comme côté intégrations.

## Comportement actuel
Quatre causes de garde pH embarquent une mesure figée à l'instant du déclenchement, affichée
telle quelle pendant toute la durée du blocage (39 minutes observées), à côté d'une carte qui
montre la valeur vive. L'utilisateur voit deux nombres contradictoires et ne peut pas savoir
lequel fait foi.

## Contraintes techniques
- ⚠️ **Aucune `String` reconstruite sur un chemin périodique** (CLAUDE.md § Mémoire, ADR-0082).
  Le correctif retenu va dans le bon sens : des textes sans valeur runtime sont plus courts et
  posés moins souvent.
- ⚠️ **Watchdog actif**, aucune modification du chemin de dosage lui-même.
- ⚠️ **Ne modifier ni seuil, ni hystérésis, ni condition de déclenchement.** La spec ne touche
  QUE le contenu textuel d'une cause déjà posée.
- Constantes de seuil déjà disponibles : `kPhFloorCorrected`, `kPhFloorRawHard`,
  `kPhCeilingCorrected`, `kPhCeilingRawHard` — aucune nouvelle constante attendue.
- Français dans toute l'UI.

## Fichiers potentiellement concernés
- `src/pump_controller.cpp` — lignes 916, 938, 994, 1025. **Elles seules.**
- `docs/subsystems/pump-controller.md` — format des causes de refus.
- `CHANGELOG.md` — changement visible utilisateur.
- `docs/user/src/guide-regulation.html` — **à évaluer** : le message que voit l'utilisateur en
  cas de blocage change de forme.

## Livrables — check-list
- [x] A1 — **Verdict `pool-chemistry` d'ABORD** sur les quatre libellés, et sur la tension entre
      la décision bug-135 (2026-10-05) et celle du 2026-10-08. Rien ne se code avant.
      → consigné ci-dessus (« ✅ VERDICT `pool-chemistry` — GO SOUS CONDITIONS »).
- [x] A2 — Lignes 916, 938, 1025 : la mesure cède la place au seuil.
- [x] A3 — Ligne 994 : traitement spécifique, l'opposition brut/corrigé a survécu (deux seuils,
      zéro mesure) — et son défaut de troncature préexistant est réparé au passage.
- [x] A4 — Vérifier qu'aucun autre appel ne réintroduit le défaut : les huit appels à
      `setGuardRefusalCause()` sont couverts par `test_native_ph_guard_causes` (liste blanche de
      seuils, échec fail-closed sur tout identifiant inconnu).
- [x] D1 — Tests sur la logique pure : `test_native_ph_guard_causes` vérifie que le texte posé
      ne dépend d'aucune mesure runtime (lecture du texte source, substitution des seuils réels).
- [x] D2 — **Validation par MUTATION** : six mutations de relâche tuées, deux rejouées et
      confirmées par le coordinateur (réintroduction de `String(ph, 2)` → le test meurt).
- [x] C1 — `docs/subsystems/pump-controller.md` : règle « une cause de garde cite un seuil,
      jamais une mesure », propagée à la voie pH, avec son motif et le garde-fou `static_assert`.
- [x] C2 — Entrée `CHANGELOG.md` sous `[2.65.0]`.

## Sécurité chimique
- Dosage impliqué : **oui, indirectement** — le code touché est dans `pump_controller.cpp`, sur
  le chemin des gardes pH. Mais **aucune décision de dosage n'est modifiée** : seuils,
  hystérésis, conditions et codes restent identiques. Seul le TEXTE change.
- Limites journalières / horaires : non concernées.
- Filtration requise avant action : inchangé.
- Calibration capteur modifiée : non. Algorithme PID touché : non.
- **Risque identifié** : appauvrir un message de sécurité au point de lui faire perdre son sens
  diagnostique — le risque est concentré sur la **ligne 994**, dont l'utilité tient à
  l'opposition de deux valeurs (elle sert à détecter un offset pH erroné, sujet sur lequel
  l'utilisateur a déjà dû corriger le projet trois fois). Le gate doit statuer sur ce site
  séparément des trois autres.
- **Risque écarté** : aucun. Le correctif ne peut pas provoquer d'injection — il ne touche
  aucune condition d'autorisation.

## ADR attendu ?
- **Probable** : non (estimation initiale)
- **Critère(s) candidat(s)** : aucun (estimation initiale)
- **Sujet pressenti** : aucun. La décision existe déjà, prise le 2026-10-08 et consignée dans
  `pump_controller.cpp:1452-1461` ; cette spec la PROPAGE, elle n'en prend pas de nouvelle. Si
  le gate tranche autrement pour la ligne 994, cet arbitrage-là mériterait un ADR.

**Révisé par `doc-writer` à la clôture** : un ADR a finalement été créé —
[ADR-0095](../../../docs/adr/0095-une-cause-de-garde-cite-un-seuil-pas-une-mesure.md). Motif :
la décision de 2026-10-08 n'avait **jamais été formalisée en ADR**, seulement consignée en
commentaire de code ; cette spec la rend désormais **symétrique** entre les deux voies (pH et
ORP) et y ajoute une contrainte neuve qui verrouille le long terme — **critère 4** (verrouillage) :
tout nouveau seuil pH cité dans une cause de garde devra être rond au dixième, **vérifié à la
compilation** par un `static_assert` dédié. C'est cette contrainte de compilation, absente de
l'estimation initiale, qui a fait basculer le « non » en « oui ».

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- aucun nouveau — `/data` et `/get-system-info` voient le CONTENU de `phDoseBlockedReason`
  changer, pas le contrat.

### Champs WebSocket
- `phDoseBlockedReason` — contenu modifié, type et sémantique inchangés

### Topics MQTT
- `publishAlert("ph_guard", …)` — contenu à arbitrer par le gate (cf. « Comportement attendu »)

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/pump-controller.md` — règle sur le contenu des causes de garde
- `docs/features/page-dashboard.md` — si le message affiché change de forme

#### Docs transverses
- `CHANGELOG.md` — entrée sous 2.65.0
- `docs/user/src/guide-regulation.html` — comportement de la régulation visible utilisateur
- `docs/API.md` — si la description de `phDoseBlockedReason` y figure

#### Docs immuables
- aucun ADR attendu

## Critères d'acceptation
- [x] Aucune cause de refus de garde pH ne contient de mesure vive
- [x] Après 40 minutes de blocage continu, le texte affiché ne contredit plus la carte
      (le texte latché ne cite plus qu'un seuil constant — rien à contredire par construction)
- [x] Les seuils, l'hystérésis et les codes de blocage sont inchangés (vérifié par test)
- [x] La ligne 994 conserve sa valeur diagnostique sur l'offset (opposition des deux seuils)
- [x] `pio run` compile sans nouveau warning
- [x] `pio test -e native` : zéro régression (63 suites / 2 125 cas)
- [x] Verdict `pool-chemistry` **GO** consigné dans la spec avant clôture

## Plan de test manuel
1. **Nominal** — faire descendre le pH sous le plancher, attendre 30 min, comparer le texte du
   bandeau et la valeur de la carte : plus de contradiction possible.
2. **Oscillation** — faire osciller autour du seuil + hystérésis : vérifier qu'aucune rafale de
   `critical` ni d'alerte MQTT n'apparaît (le défaut que la règle de latch protégeait).
3. **Offset** — provoquer le cas de la ligne 994 (brut sous la butée, corrigé au-dessus du
   plancher) : vérifier que le message reste exploitable pour diagnostiquer l'offset.
4. **Aggravation** — enchaîner deux gardes de gravité croissante : vérifier la réécriture.

## Notes pour agent
- ⚠️ **L'offset pH n'est PAS une suspicion de sonde** — il corrige un décalage de MESURE
  (courants de fuite), et la sonde est prouvée saine. L'utilisateur a dû le rappeler trois fois.
  Aucun libellé produit par cette spec ne doit suggérer une sonde défaillante.
- Le latch et sa règle de réécriture sur aggravation sont un ACQUIS protégeant le journal et la
  partition : ne pas les remettre en cause en passant.
- Contexte du constat : épisode de boue d'octobre 2026, eau chargée, pH réellement bas
  (6,5-6,9) et ORP effondré. Le blocage observé est le **fonctionnement normal** de la garde —
  elle fait son travail. Seul l'affichage est en cause.

## ⚠️ CORRECTION DE RÉDACTION (2026-10-10, après clôture)

Cette spec qualifiait la voie ORP de **« saine »** (tableau de recensement et section « Hors
périmètre »). **Le mot est trop large et il a nui.** Une seule chose avait été vérifiée : que ces
causes ne citent pas de **mesure vive**, donc qu'elles ne se périment pas. Exact, et rien de plus.

« Sain » annonçait un examen d'ensemble qui n'avait pas eu lieu. Conséquence concrète : les deux
causes ORP citent leurs seuils **à deux décimales** (`8.10`, `7.60`) alors que les causes pH les
affichent désormais à **une** (`8.1`) — **la même constante `kPhCeilingRawHard`, écrite de deux
façons**, et les deux gardes se déclenchant sur la même condition, le bandeau peut les afficher
**simultanément**. Une fois la voie étiquetée saine, plus personne n'est allé voir.

⇒ Règle de rédaction à retenir : **écrire ce qui a été vérifié, pas une appréciation globale.**
« Ne porte pas de mesure figée » est un fait, « sain » est un jugement — et un jugement dans une
section « hors périmètre » dissuade le lecteur suivant d'examiner le reste.

Traité par **feature-150**. L'utilisateur a relevé lui-même l'attribution fautive : la formule
venait de cette spec, pas de lui.

## Hors périmètre
- Le dédoublonnage bandeau / badge → feature-148.
- Toute modification de seuil, d'hystérésis, de condition de déclenchement ou de code de refus.
- Les causes de la voie ORP — **au seul titre de la mesure figée** : elles citent des seuils,
  pas des mesures, donc elles ne se périment pas. ⚠️ **C'est la seule chose qui a été vérifiée.**
- Le rafraîchissement périodique du texte — option explicitement rejetée le 2026-10-08, motif
  consigné dans le code. Ne pas la rouvrir sans argument neuf.

## Notes d'implémentation (doc-writer, 2026-10-10)

**Mécanique livrée** : les quatre causes de garde pH (`src/pump_controller.cpp:916, 938, 994,
1025`) citent désormais `String(kPhXxx, 1)` — jamais une mesure runtime. Le site 994 (offset)
oppose les deux SEUILS (`kPhFloorRawHard` / `kPhFloorCorrected`), pas les deux mesures. Longueurs
mesurées après substitution : **75 / 68 / 91 / 66 octets**, toutes sous la limite de 95
(`kDoseBlockedReasonMaxLen`) — le site 994, le plus long, passait avant ce lot à ~160 octets et
était **tronqué en production** depuis sa mise en service ; l'instruction finale « vérifiez
l'offset pH » n'atteignait jamais l'écran. Bénéfice collatéral : l'alerte horaire du mode
Programmé (`ph_guard_scheduled`, ligne 1191), qui réémet cette même cause, redevient complète
elle aussi.

**Garde-fou à la compilation** : `static_assert(numfmt::isRoundedToTenth(...))`
(`src/constants.h:1752-1755`) vérifie que les quatre seuils cités sont ronds au dixième. Vérifié
non creux : abaisser `kPhSafetyMargin` de 0,50 à 0,45 casse effectivement la compilation.

**Tests (D1/D2)** : `test/test_native_ph_guard_causes/test_ph_guard_causes.cpp` lit le texte
source des huit appels à `setGuardRefusalCause()`, interdit tout identifiant hors liste blanche
de seuils (fail-closed sur tout autre token), et mesure la longueur en **octets** (pas en
caractères — sentinelle UTF-8 dédiée en tête de suite) après substitution des valeurs réelles.
Suite native complète au moment de cette passe : **63 suites / 2 125 cas, zéro régression**. Six
mutations de relâche tuées, deux rejouées et confirmées par le coordinateur (réintroduction de
`String(ph, 2)` dans un libellé → le test meurt).

**Défaut repéré en documentant, hors périmètre de cette spec, consigné dans
`docs/subsystems/pump-controller.md` et `docs/MQTT.md`, non corrigé ici** : le message
`ph_guard_scheduled` (`pump_controller.cpp:1191`) concatène la cause latchée (qui se termine
désormais par un point) avec `". Le volume quotidien…"`, produisant un **double point**
(`…suspendu.. Le volume…`). Cosmétique, sans effet fonctionnel ni sécurité — à router vers
`code-reviewer` / `embedded-developer`.

**Documentation mise à jour (C1/C2 + transverses)** :
- `docs/subsystems/pump-controller.md` — règle « seuil, pas mesure » propagée à la voie pH,
  table des quatre textes, note sur la réparation de troncature, garde-fou `static_assert`.
- `docs/API.md` (`phDoseBlockedReason`), `docs/MQTT.md` (`ph_guard_scheduled`) — exemples mis à
  jour sur le nouveau format.
- [ADR-0095](../../../docs/adr/0095-une-cause-de-garde-cite-un-seuil-pas-une-mesure.md) (nouveau,
  critère 4 — verrouillage long terme) — indexé dans `docs/README.md`. Consigne aussi
  l'incohérence assumée entre décimales pH (1) et ORP (2) pour une même constante, hors
  périmètre de cette spec.
- `CHANGELOG.md` — nouvelle section `[2.65.0]`, domaines **Firmware** et **Documentation**.
- `src/version.h` bumpé **2.64.0 → 2.65.0** (à la demande explicite du brief de cycle) :
  ⚠️ **le déploiement devra inclure le firmware**, pas seulement la documentation, pour que
  l'appareil annonce la version réelle.
- `docs/user/src/guide-regulation.html` — **évalué, aucune modification nécessaire** : le guide
  décrit les seuils et leur rôle de manière conceptuelle, sans jamais citer le texte exact d'une
  cause de garde ni une mesure figée ; le comportement qu'il décrit (seuils, hystérésis, trois
  refus → bascule manuelle) est inchangé par ce lot. Aucune capture à refaire.
- `docs/features/page-dashboard.md`, `docs/features/page-ph.md` — **vérifiés, aucune mise à
  jour nécessaire** : ni l'un ni l'autre ne cite le texte littéral du message « Chloration
  suspendue » (déjà passé à `toFixed(1)` dans `data/app.js` avant cette passe de documentation).

**Clôture** : tous les critères d'acceptation et la check-list de livrables sont cochés. La spec
**n'est pas déplacée vers `done/`** : au moment de cette passe, les modifications (`src/`,
`data/app.js`, `test/test_native_ph_guard_causes/`, cette spec, l'ADR, les docs transverses) sont
encore **non commitées** (voir `git status`) — la règle de clôture exige des commits référencés,
absents à ce stade. À déplacer dès que le commit (ou la série de commits) couvrant ce lot est
créé.
