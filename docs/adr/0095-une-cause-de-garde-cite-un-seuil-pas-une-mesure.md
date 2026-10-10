# ADR-0095 — Une cause de garde cite un seuil, pas une mesure : action d'abord, une décimale

- **Statut** : Accepté
- **Date** : 2026-10-10
- **Décideurs** : `pool-chemistry` (GO sous conditions, deux passages, 2026-10-10)
- **Spec(s) liée(s)** : [feature-149](../../specs/features/done/feature-149-plus-de-mesure-figee-dans-les-causes-de-garde-ph.md) (plus de mesure figée dans les causes de garde pH) ; propage une décision déjà prise côté ORP le 2026-10-08 (commentaire `pump_controller.cpp:1452-1461`, non formalisée en ADR à l'époque) ; référence [bug-135](../../specs/features/todo/bug-135-butee-brute-ph-se-declenche-sur-une-lecture-isolee.md) (priorité des causes pour l'acide, non remise en cause ici)

## Contexte

`setGuardRefusalCause()` (`src/pump_controller.cpp:759`) est un **latch** : le texte qu'il
pose n'est réécrit qu'en cas d'aggravation de gravité, pas à chaque tour — délibérément, pour
éviter qu'une mesure oscillant autour d'un seuil ne fasse battre deux libellés contradictoires,
un `critical` et une alerte MQTT par oscillation, et ne chasse le journal système de sa
partition (ADR-0082). Les appels à cette fonction sont des **edge-triggers** : `if (low &&
!_phLowBlockActive)`.

Conséquence structurelle, pas accidentelle : à l'instant précis où l'edge-trigger se déclenche,
la mesure vaut **par construction** le seuil qui vient d'être franchi (à la résolution
d'affichage près). Un texte qui inscrit cette mesure n'apporte donc **aucune information au
moment où il est écrit**, puis devient **faux** à mesure que la mesure évolue pendant toute la
durée du blocage — alors que la carte capteur, elle, reste vive à côté.

**Deux relevés de production l'ont montré, à six jours d'écart et sur deux voies distinctes :**

- **2026-10-08, voie ORP** : le bandeau affichait *« pH trop élevé (8.28) — dosage chlore
  suspendu »* pendant que la carte pH affichait **8,1** au même instant.
- **2026-10-10, voie pH, après correctif partiel** : le bandeau affichait *« pH **6.70** sous
  le plancher de sécurité (**6.70**) »* pendant que la carte affichait **6,67** — **le même
  nombre apparaît deux fois**, et la phrase s'auto-contredit (un pH de 6,70 ne peut pas être
  « sous » 6,70).

Un défaut de production **préexistant et non lié à la mesure** a été découvert en creusant le
second relevé : le texte de la cause « offset » (site `994`, garde `rawHard` côté acide, cas
non discriminant) pesait **~160 octets** une fois les mesures substituées, pour un tampon
`_lastRefusalCause[2][96]` de **95 octets utiles** (`kDoseBlockedReasonMaxLen`). `strlcpy`
coupait silencieusement la fin, et l'instruction « vérifiez l'offset pH » — la seule information
actionnable du message — **n'a jamais atteint l'écran** depuis la mise en service de ce texte.

## Décision

**Une cause de garde latchée (`setGuardRefusalCause`) cite un seuil, jamais une mesure.**
Cette règle, déjà posée côté ORP le 2026-10-08, est désormais **symétrique** entre les deux
voies pH et ORP. Trois précisions l'accompagnent, issues du gate `pool-chemistry` :

1. **Seuil, pas mesure.** Le seuil est une constante (`kPhFloorCorrected`,
   `kPhFloorRawHard`, `kPhCeilingCorrected`, `kPhCeilingRawHard`) : il ne périme jamais, et
   c'est de toute façon le chiffre utile à l'utilisateur — il dit **où redescendre**. La
   mesure vive reste disponible à quelques centimètres, sur la carte capteur du tableau de
   bord, rafraîchie à chaque tour.
2. **Action d'abord, constat ensuite.** Le tampon fait 95 octets utiles et `strlcpy` coupe la
   fin sans avertissement : une phrase qui annonce l'action requise avant d'éventuels détails
   survit à une troncature, une phrase qui se termine sur l'instruction la perd.
3. **Une décimale pour un pH de mesure ou de seuil ; deux pour un réglage ou une bande de
   diagnostic.** Les quatre seuils cités sont tous ronds au dixième, donc une décimale ne perd
   aucune information. ⛔ Exception explicite : `kPhGuardReleaseHysteresis = 0,05` n'est **pas**
   ronde au dixième — les messages de relâche et les bandes d'hystérésis ORP restent à **deux**
   décimales, faute de quoi un point de relâche réel de 6,75 s'afficherait arrondi à 6,8 (ou
   7,55 à 7,5, 6,25 à 6,3, 8,05 à 8,1) : une reprise de dosage annoncée à un pH que l'appareil
   n'utilise pas.

**Condition bloquante posée par le gate** : un `static_assert(numfmt::isRoundedToTenth(...))`
(`src/constants.h:1752-1755`) garantit à la compilation que les quatre seuils cités dans les
causes pH sont ronds au dixième. Sans lui, un futur réglage de `kPhSafetyMargin` à 0,45 (au lieu
de 0,50) ferait afficher « 6,8 » pour un seuil réel à 6,75 — un **seuil de sécurité faux** livré
en silence. Vérifié par mutation : abaisser `kPhSafetyMargin` à 0,45 casse effectivement la
compilation.

**Hors décision** : les **journaux et alertes MQTT** (`systemLogger.critical()`,
`mqttManager.publishAlert()`) voisins continuent de citer la mesure. Ils sont **horodatés** et
émis **une seule fois** au franchissement : un chiffre y est daté, il ne ment pas — contrairement
au texte latché, relu en continu pendant toute la durée du blocage.

## Alternatives considérées

- **Rafraîchir le texte latché à chaque tick** (rejetée le 2026-10-08, motif consigné dans le
  code et reconduit ici) — réintroduit exactement le défaut que le latch avait été conçu pour
  éviter : libellés contradictoires en rafale, un `critical` et une alerte MQTT par oscillation,
  journal chassé de la partition `history` (64 Ko, rétention déjà de 25-35 min). Rien de neuf
  n'a été apporté qui justifierait de rouvrir cette option.
- **Agrandir le tampon `_lastRefusalCause[2][96]`** — écartée. Le tampon est dupliqué sur la
  pile à deux endroits (`web_routes_data.cpp:496-497`, `ws_manager.cpp:892-893`), et
  `ws_manager.cpp:667` documente que ces causes saturent déjà la limite du budget WebSocket.
  Le correctif retenu va dans le sens inverse : des textes sans valeur runtime sont plus courts
  et posés moins souvent — pas besoin de plus de place.
- **Remplacer la mesure par la valeur de l'offset, sur le site 994** (opposition brut/corrigé,
  utilisée pour diagnostiquer un offset mal saisi) — écartée. L'offset peut changer pendant le
  latch (c'est même l'action demandée au lecteur), et se périmerait pareil. Le sens diagnostique
  ne tient pas aux deux valeurs mais à l'**opposition structurelle** des deux seuils (la butée
  brute mord, la garde corrigée ne mord pas) — elle survit sans aucune mesure.

## Conséquences

### Positives
- Les deux voies (pH, ORP) suivent désormais la **même** règle pour toute cause latchée :
  plus de lecture de code au cas par cas pour savoir si une valeur citée est fiable.
- Le texte le plus long (`994`, offset) passe de ~160 octets (tronqué en production depuis sa
  mise en service) à 91 octets — la seule instruction actionnable du message atteint enfin
  l'écran.
- Les quatre textes sont désormais protégés par une suite native dédiée
  (`test/test_native_ph_guard_causes/`) qui lit le texte source des appels, interdit toute
  réintroduction d'une mesure (liste blanche de seuils) et mesure la longueur en **octets**
  après substitution.

### Négatives / dette assumée
- Un futur site de cause latchée doit désormais connaître cette règle pour l'appliquer
  correctement (seuil, une décimale, exception hystérésis à deux) — rien ne l'impose
  mécaniquement hors du `static_assert` de complétude des seuils ; un nouveau seuil **non**
  cité dans une cause ne serait pas détecté comme manquant.
- Défaut cosmétique découvert en documentant cette décision, **non corrigé par cet ADR** : le
  message `ph_guard_scheduled` (`pump_controller.cpp:1191`) concatène la cause latchée (qui se
  termine désormais par un point) avec `". Le volume quotidien…"`, produisant un double point
  (`…suspendu.. Le volume…`). À router vers `code-reviewer` / `embedded-developer`.

### Ce que ça verrouille
- **Tout nouveau seuil pH cité dans une cause de garde devra être rond au dixième, ou la
  compilation casse.** Le `static_assert` de `src/constants.h:1752-1755` n'est pas optionnel :
  étendre la liste des seuils couverts est la seule voie pour ajouter un cinquième seuil cité,
  et retirer un seuil de la liste sans retirer son usage dans une cause romprait la garantie
  que cet ADR établit.
- **Aucune cause latchée ne doit plus jamais inscrire une mesure**, sur aucune voie. Un
  contributeur qui voudrait réintroduire `String(ph, 2)` dans un texte de
  `setGuardRefusalCause()` doit d'abord faire tomber le test de mutation dédié
  (`test_native_ph_guard_causes`) — s'il n'y parvient pas, c'est que la règle tient.

## Référence — incohérence connue, assumée hors périmètre

Les causes **ORP** (`orp_ph_inhibit`, section « Inhibition du dosage ORP sur pH élevé ») citent
leurs seuils à **deux** décimales (ex. « butée 8.10 »), alors que les causes **pH** affichent
désormais **8.1** pour exactement la **même constante** (`kPhCeilingRawHard`). Cette
incohérence n'est **pas** un oubli de cet ADR : le périmètre de feature-149 portait
explicitement sur la **mesure figée**, pas sur l'harmonisation du nombre de décimales entre les
deux voies. Elle est consignée ici pour qu'elle ne soit pas perdue, et resterait à reprendre par
une spec dédiée si l'utilisateur le souhaite — pas par une extension silencieuse de celle-ci.

## Références

- Code : [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — `setGuardRefusalCause()`
  (ligne 759), sites 916/938/994/1025 (voie pH) ; [`src/constants.h:1695-1756`](../../src/constants.h) —
  `numfmt::isRoundedToTenth()` et le `static_assert` associé
- Spec : [feature-149](../../specs/features/done/feature-149-plus-de-mesure-figee-dans-les-causes-de-garde-ph.md)
- Tests : [`test/test_native_ph_guard_causes/test_ph_guard_causes.cpp`](../../test/test_native_ph_guard_causes/test_ph_guard_causes.cpp)
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#règle--une-cause-latchée-ne-porte-jamais-une-mesure-vive-v2571), [`docs/API.md`](../API.md#get-data--write), [`docs/MQTT.md`](../MQTT.md), [`CHANGELOG.md`](../../CHANGELOG.md)
