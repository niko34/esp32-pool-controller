# ADR-0048 — Seuils de sécurité pH fixes, dérivés de la plage de consigne, appliqués au pH corrigé

- **Statut** : Accepté
- **Date** : 2026-08-27
- **Décideurs** : `architect`, `pool-chemistry` (GO amont + GO en contre-visite), `code-reviewer` (Approved en 2ᵉ passe), utilisateur (arbitrages du plancher asservi, de la butée 6,40 et des curseurs)
- **Spec(s) liée(s)** : `refonte-seuils-ph-fixes-derives` (v2.25.0), `bug-plancher-ph-offset-legitime-et-securite-desarmee` (même version) ; amende [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md) (offset pH) et le filet de sécurité de feature-067

## Contexte

Le filet de sécurité pH livré en feature-067 refusait toute injection d'acide sous un **plancher évalué sur le pH BRUT**, c'est-à-dire la lecture capteur sans l'offset utilisateur. La prémisse était défendable : le garde-fou doit rester valide même si l'offset est faux.

Elle a produit une **panne de régulation en production le 2026-08-25**. Sur l'installation de référence (offset `+0,50`, consigne `7,20`), la cible brute vaut `6,70`, soit sous le plancher applicable : chaque approche de la consigne déclenchait un refus, et trois refus en 24 h fermaient la régulation (bascule fail-closed en `manual`). Le diagnostic EZO était nominal (pente acide 101,0 %, base 100,0 %, zéro −1,03 mV) : l'offset ne corrigeait pas une dérive de sonde mais un décalage de référence **externe** (courants de fuite de la pompe de filtration, campagne de bruit 22-23/08, offset établi avec trois instruments). Le système punissait une correction légitime et mesurée.

Un premier correctif a été écrit et testé : rendre le plancher **continu et asservi à l'offset**, `max(6,50 ; 7,00 − offset)`, de façon à protéger un pH réel constant. Il était arithmétiquement juste. Il a été **abandonné avant commit**, pour deux raisons qui n'étaient pas des raisons de calcul :

1. **Il était incompréhensible côté utilisateur.** Le seuil annoncé dans l'interface et dans les messages d'alerte bougeait à chaque changement d'offset. Aucune valeur n'était mémorisable, donc aucune n'était vérifiable par l'utilisateur ; le client dupliquait la formule firmware pour pouvoir l'afficher, ce qui créait un miroir de constante à maintenir.
2. **Il avait déjà désactivé la régulation une fois.** La complexité — formule asservie, deux paliers historiques, seuil de bascule, garde de cohérence `checkPhTargetReachable`, bornes de saisie avec refus 400, quatre `static_assert`, 4141 combinaisons de test — était le mécanisme même de la panne. Empiler une couche corrective sur un mécanisme qui vient d'échouer par excès de couches est un pari défavorable.

Il fallait donc trancher la question de fond, laissée en suspens depuis feature-067 : **sur quelle grandeur un seuil chimique porte-t-il ?**

## Décision

**1. Les seuils chimiques portent sur le pH CORRIGÉ ; seules deux butées de dernier recours portent sur le pH BRUT.**

L'offset est une **correction de mesure** : une fois appliqué, la valeur affichée *est* le pH de l'eau. Un seuil chimique (agressivité de l'eau, fraction de HClO) est une propriété de l'eau réelle, donc du pH corrigé. Le pH brut ne conserve qu'un rôle : le **filet contre un offset faux**, via deux butées calibrées pour ne jamais mordre avant leur seuil corrigé en usage légitime.

**2. Tous les seuils sont FIXES et DÉRIVÉS d'une source unique** — la plage de consigne plus trois marges — en `constexpr`. Aucun seuil de sécurité pH n'est écrit en littéral, sauf les deux qui ont une justification chimique propre (`kPhOrpInhibit`, `kPhLowWarn`), commentés comme tels.

| Grandeur | Valeur | Référentiel | Dérivation |
|---|---:|---|---|
| Consigne | `7,20` → `7,50`, pas `0,10` | corrigé | source unique |
| Refus d'ACIDIFIER (`ph_minus`) | `6,70` | corrigé | `kPhTargetMin − kPhSafetyMargin` (0,50) |
| Refus d'ALCALINISER (`ph_plus`) | `7,60` | corrigé | `kPhTargetMax + kPhHighMargin` (0,10) |
| Inhibition du dosage ORP | `7,60` (relâche ≈ 7,55) | corrigé | littéral chimique |
| Avertissement d'eau acide, **non bloquant** | `7,00` | corrigé | littéral chimique |
| Butée basse | `6,20` | **brut** | `kPhFloorCorrected − kPhOffsetMax` |
| Butée haute | `8,10` | **brut** | `kPhOrpInhibit + kPhOffsetMax` |
| Offset | `−0,50` → `+0,50`, pas `0,10` | — | inchangé |
| Hystérésis de relâche | `0,05` | — | commune à **toutes** les gardes |

**3. Chaque butée brute est placée à exactement l'offset maximal de son seuil corrigé.** C'est la propriété qui rend les deux référentiels compatibles : à l'offset extrême, butée et seuil mordent **au même point** et se relâchent ensemble. Aucune ne peut rendre l'autre inopérante, dans aucun des deux sens. Trois `static_assert` de `constants.h` verrouillent l'ensemble :

- la consigne haute plus `kPhOrpInhibitMargin` ne peut pas atteindre `kPhOrpInhibit` — une eau maintenue à la consigne ne peut donc **jamais** inhiber le chlore en permanence ;
- le plafond d'alcalinisation ne peut pas dépasser `kPhOrpInhibit` — l'appareil ne peut pas alcaliniser une eau dont il vient de décider que le chlore y est inactif ;
- chaque butée brute ne peut pas mordre avant son seuil corrigé, pour tout offset du domaine.

**4. Deux bornes visibles sont resserrées.** La consigne maximale passe de `7,60` à **`7,50`** et le refus d'alcaliniser de `7,70` à **`7,60`**. Motif chimique commun : au-dessus de 7,60 la fraction de chlore actif (HClO) tombe sous 25 %. Une consigne à 7,60 pile aurait **inhibé le chlore en permanence** dès que la régulation tient sa consigne — une panne sanitaire silencieuse. Une installation persistée à 7,60 est **clampée au chargement NVS**, avec avertissement journalisé et réécriture immédiate.

**5. La surveillance est séparée de la décision.** `tickPhGuardSurveillance()` et `tickOrpPhInhibitSurveillance()` sont appelées **inconditionnellement**, une fois par tick d'`update()`, hors de toute conjonction d'éligibilité. Elles portent les franchissements, l'hystérésis, les logs et les alertes MQTT ; elles ne comptent rien et n'escaladent jamais. La décision (`isPhGuardBlocking`, `isOrpInhibitedByPh`) ne rend plus que le verdict du tick.

Auparavant, la détection vivait à l'intérieur de la conjonction `phMode == "automatic" && phLimitOk && … && !isPhGuardBlocking(...)`. Le court-circuit `&&` la rendait muette dès qu'un terme amont était faux : quota journalier épuisé, régulation désactivée, **mode `manual`** — c'est-à-dire l'état exact où l'escalade laisse l'appareil. La seule alerte qui désigne la vraie cause ne partait jamais quand elle était le plus nécessaire.

**6. La saisie hors plage devient impossible depuis l'interface.** Consigne = contrôle segmenté à 4 positions ; offset = incrémenteur `−` / valeur / `+`, boutons désactivés aux bornes, sans auto-répétition. Aucun `input[type=range]`, aucun champ libre. Les bornes sont **servies par `/get-config`** (`ph_target_min/_max/_step`, `ph_offset_min/_max/_step`) et non codées dans le JS : élargir la plage ou passer de 4 à 5 positions se fait côté firmware, sans retouche de l'interface. La validation serveur (HTTP 400, refus MQTT, clamp NVS) est **conservée** en défense en profondeur — l'API et MQTT restent ouverts.

**7. L'injection manuelle reste une échappatoire, mais informée.** Quand une butée brute mord, le serveur **obéit quand même** à une injection manuelle (décision utilisateur : l'opérateur doit garder un dernier recours). L'interface affiche un encart d'avertissement au-dessus du bouton et exige une **confirmation** : elle dit que la sécurité est active, pourquoi, que la butée porte sur le pH brut, et que l'injection passera outre.

**8. Suppressions.** `phRawFloorFor()`, `kPhRealFloorTarget`, `kPhRawFloorAbsolute`, `kPhOffsetWideThreshold`, `checkPhTargetReachable()`, `isPhTargetUnreachable()`, `kPhTargetFloorMarginMin` et l'alerte `ph_target_unreachable` **n'existent plus** : la marge devient structurelle, garantie par les `static_assert`, donc il n'y a plus rien à détecter à l'exécution. Les alertes MQTT sont renommées (`ph_raw_floor` → `ph_guard`, `orp_ph_raw_inhibit` → `orp_ph_inhibit`) et deux sont ajoutées (`ph_low_warning`, `ph_guard_scheduled`).

La logique de décision est extraite dans le module **pur** [`src/ph_guard_logic.h`](../../src/ph_guard_logic.h) ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) : deux comparateurs symétriques avec hystérésis (`phGuardBlocksBelow` / `phGuardBlocksAbove`), l'arrondi au pas de l'offset, le diagnostic EZO et le gate d'acquittement.

## Alternatives considérées

- **Plancher continu asservi à l'offset** — `max(6,50 ; 7,00 − offset)`, *livré puis abandonné avant commit*. Arithmétiquement correct : il maintenait un pH réel protégé constant. Rejeté sur deux critères non arithmétiques. **(a) Incompréhensible** : le seuil affiché bougeait avec l'offset, donc aucune valeur n'était mémorisable ni vérifiable par l'utilisateur, et le client devait dupliquer la formule firmware pour l'annoncer. **(b) Il avait désactivé la régulation** : la complexité (formule asservie, paliers historiques, seuil de bascule, garde de cohérence, bornes avec refus 400, 4 `static_assert`, 4141 combinaisons) était le mécanisme de la panne du 25/08, pas son remède. Un seuil de sécurité que l'utilisateur ne peut pas réciter n'est pas un seuil de sécurité, c'est une boîte noire.

- **Escalier à deux paliers** (`6,80` normal, `6,90` puis `6,50` au-delà de `|offset| > 0,30`) — *rejeté*. Le plancher **réel** protégé montait puis redescendait (`6,80` / `7,10` / `7,00` selon la branche) : la grandeur protégée ne peut pas dépendre du côté du seuil administratif où tombe l'offset. Pire, le pire cas tombait juste **avant** le seuil de bascule (à `offset = 0,30` pile, marge `0,10` sous une cible `7,20`, pour un critère de `0,20`).

- **Butée brute basse à 6,40** (proposée par le gate) — *rejetée sur objection de l'utilisateur*. Avec un offset **légitime** de `+0,50`, une butée à 6,40 équivaut à 6,90 corrigé : elle mordrait **avant** le seuil corrigé de 6,70, le rendant inopérant et **réintroduisant par la bande la dépendance à l'offset** qu'on venait de supprimer. La butée est donc dérivée (`6,70 − 0,50 = 6,20`), et le `static_assert` est écrit en **inégalité** : la remonter ne compile plus, l'abaisser reste possible.

- **Conserver la consigne maximale à 7,60** — *rejetée*. À 7,60 pile, la consigne atteint le seuil d'inhibition ORP : dès que la régulation tient sa consigne, le chlore est suspendu **en permanence**. Panne sanitaire silencieuse — l'eau paraît correctement régulée, le désinfectant ne monte jamais. La borne descend à 7,50, et le `static_assert` `kPhTargetMax + kPhOrpInhibitMargin <= kPhOrpInhibit` interdit d'y revenir sans relire la justification.

- **Monter le seuil d'inhibition ORP à 7,70 pour légaliser une consigne à 7,60** — *rejetée*. C'est desserrer une sécurité chimique pour autoriser un réglage plus agressif : l'inverse de l'ordre des priorités. Le seuil de 7,60 a une justification physique propre (fraction de HClO < 25 %), la consigne n'en a pas au-dessus de 7,50. Aucun argument physique ne soutient la zone 7,60-7,70 : remonter le TAC se fait au bicarbonate, hors régulation, jamais en visant un pH plus haut.

- **Plafond d'alcalinisation à `consigne max + 0,20`** (première mouture, soit 7,70) — *rejeté en contre-visite*. Il violait la règle qu'il énonçait : le plafond passait **au-dessus** de `kPhOrpInhibit`, donc l'appareil s'autorisait à alcaliniser une eau dont il venait de décider que le chlore y était inactif. La marge haute descend à `0,10`, ce qui aligne le plafond sur l'inhibition et rend l'invariant de la butée brute haute satisfiable sans constante supplémentaire.

- **Curseurs `input[type=range]` pour la consigne et l'offset** — *rejetés*. Sur mobile, la plage d'offset (11 positions sur ~280 px) donne ~25 px par cran, sur le réglage **le plus sensible de la page** : un cran d'écart vaut 0,10 pH de décalage sur toute la régulation. Un `range` n'affiche pas sa valeur, n'a pas de cran tactile et se déclenche au premier contact — y compris pendant un scroll. Le contrôle segmenté (consigne, 4 positions discrètes) et l'incrémenteur (offset, cible tactile pleine, valeur toujours lisible, bouton désactivé à la borne) rendent l'erreur d'un cran improbable et la saisie hors plage impossible.

- **Bornes de consigne différenciées selon `ph_minus` / `ph_plus`** — *rejetées*. Le produit détermine la **pompe** à actionner, pas la fenêtre de pH acceptable pour l'eau : une eau à 7,80 est aussi mauvaise qu'elle ait été atteinte par excès de base ou par défaut d'acide. Deux fenêtres auraient créé deux invariants à maintenir, et un changement de type de produit aurait pu rendre une consigne déjà persistée soudainement invalide.

- **Écrêter silencieusement une consigne hors bornes sur les chemins de commande** — *rejetée*. Un `number` Home Assistant est un curseur **distant** : écrêter laisserait HA afficher 8,5 pendant que le contrôleur régule à 7,5, et une automation rejouerait indéfiniment la valeur écrêtée. Le refus plus republication de l'état réel rend le désaccord **visible**. L'écrêtage n'est retenu qu'au **chargement NVS**, où il n'y a aucun interlocuteur à informer et où refuser signifierait démarrer sans consigne.

- **Bloquer l'injection manuelle quand une butée brute mord** — *rejetée, décision utilisateur*. L'injection manuelle est le dernier recours de l'opérateur face à un appareil qui se trompe : la fermer transforme un faux positif de sécurité en immobilisation complète. Le compromis retenu est l'**information obligatoire** (encart in situ plus confirmation), pas l'interdiction.

- **Laisser la détection dans la conjonction d'éligibilité et se contenter de réordonner les termes** — *rejetée*. Un réordonnancement est un équilibre instable : le prochain terme ajouté en amont recasse la propriété, silencieusement. Extraire la surveillance hors de toute conjonction rend la propriété structurelle.

## Conséquences

### Positives

- Les sept seuils de sécurité pH sont des **valeurs fixes énonçables** (6,20 / 6,70 / 7,00 / 7,60 / 8,10 et la plage 7,20-7,50) : l'utilisateur peut les vérifier au réactif, les messages d'alerte citent des nombres stables, et l'interface ne duplique plus aucune formule firmware.
- Les seuils chimiques portent enfin sur la grandeur qu'ils prétendent protéger — **le pH de l'eau**, celui que l'utilisateur voit à l'écran.
- L'installation de référence (consigne 7,20, offset +0,50) régule avec **0,50 pH de marge** avant le refus d'acidifier, contre une marge négative avant correctif.
- **Le pH+ est gardé pour la première fois** : jusqu'ici aucun seuil ne l'encadrait, et rien n'empêchait le système d'alcaliniser l'eau dans la zone où il inhibe lui-même le chlore.
- Les alertes de garde partent **quota épuisé et en mode `manual`** — donc dans l'état où l'escalade laisse l'appareil, précisément là où l'ancienne implémentation devenait muette.
- La saisie hors plage est **impossible depuis l'interface**, sans que la validation serveur soit relâchée.
- Une consigne héritée à 7,60 est **corrigée et signalée** au boot, au lieu de piloter silencieusement une régulation qui inhibe le chlore en continu.
- Trois invariants chimiques sont vérifiés **à la compilation** : toute retouche d'une valeur oblige à relire la justification.

### Négatives / dette assumée

- **La fenêtre de consigne se resserre à 0,30 pH** (7,20-7,50, 4 positions). Un utilisateur habitué à réguler à 7,60 verra sa valeur clampée au premier démarrage, sans dérogation possible. C'est l'effet recherché, mais c'est une perte de liberté et un changement visible sur les installations existantes.
- **Politique NaN opposée entre gardes pH et ORP.** Une mesure indisponible **réarme** les mémoires d'hystérésis côté pH (tous les drapeaux repassent à `false`) alors qu'elle les **conserve** côté ORP. Côté pH, c'est un *fail-open borné à 0,05 pH* : après un trou de mesure, la garde peut se relâcher 0,05 pH plus tôt qu'elle ne l'aurait fait. Assumé — l'écart est au niveau du bruit résiduel du filtre, et l'alternative (conserver les drapeaux) ferait persister une hystérésis calée sur un franchissement dont on ne sait plus s'il a eu lieu dans le référentiel courant. Côté ORP la conservation est **nécessaire** : le latch d'inhibition y est armé pour des motifs étrangers à tout seuil (pH indisponible, mesures non représentatives), et confondre les deux rôles avait produit une inhibition permanente du chlore à chaque démarrage de filtration.
- **Les indicateurs exposés gèlent sur leur dernière valeur pendant les sorties anticipées de la boucle** (OTA, mode maintenance, capteurs non initialisés, **filtration arrêtée**, stabilisation post-calibration, assistant de débit). La surveillance n'y tourne pas : `phRawGuardBlocked`, `orpPhRawGuardBlocked` et les causes de refus conservent la valeur du dernier tick complet. Assumé et voulu — en eau stagnante la mesure n'est pas représentative de l'eau du bassin, surveiller là produirait de fausses alertes, et l'appareil n'y dose rien de toute façon. Conséquence pratique : après un arrêt de filtration, l'encart d'avertissement d'injection manuelle peut refléter un état antérieur.
- **L'alerte de refus en mode `programmé` vit encore sous une conjonction d'éligibilité** — même classe de défaut que celui corrigé au point 5. Impact faible : la surveillance couvre la **détection** (franchissement, log `critical`, alerte `ph_guard`), seul le signalement périodique `ph_guard_scheduled` peut manquer. Non traité dans ce cycle.
- Les alertes MQTT sont **renommées** : une automation Home Assistant filtrant sur `ph_raw_floor` ou `orp_ph_raw_inhibit` doit être mise à jour.
- Les symboles publics `getPhDoseBlockedReason()` / `getOrpDoseBlockedReason()` (`String`) et `isPhRawFloorBlocking()` **disparaissent** au profit de `copyPhDoseBlockedReason(char*, size_t)` / `copyOrpDoseBlockedReason(...)` et `isPhGuardBlocking(bool countRefusal)`.
- **La marge de protection en cas d'offset réellement erroné se réduit** à mesure que l'offset augmente : à `+0,50`, la butée brute (6,20) et le seuil corrigé (6,70) coïncident. Assumé : l'offset est borné à ±0,50, la butée interdit toute descente sous 6,20 brut quoi qu'affirme l'offset, et les gardes amont (bandes du filtre, `ph_abnormal`, limites de dosage) sont inchangées.

### Ce que ça verrouille

- **Les seuils chimiques portent sur le pH CORRIGÉ.** Toute évolution qui les ramènerait sur le brut remet en cause cet ADR *et* le rôle assigné à l'offset par [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md).
- **Aucun seuil de sécurité pH ne peut être écrit en littéral** hors des deux exceptions documentées : ils dérivent de la plage de consigne et des marges.
- **Les trois invariants sont des invariants de compilation.** `kPhTargetMin`, `kPhTargetMax`, `kPhSafetyMargin`, `kPhHighMargin`, `kPhOrpInhibitMargin`, `kPhOrpInhibit`, `kPhLowWarn` et `kPhOffsetMin/Max` ne peuvent plus bouger indépendamment.
- **Les butées brutes ne peuvent qu'être abaissées**, jamais remontées : les `static_assert` sont écrits en inégalité.
- **Les bornes de consigne et d'offset sont un contrat d'API public** — HTTP 400, refus MQTT, `min`/`max` de l'auto-discovery HA, et désormais les six champs `ph_target_*` / `ph_offset_*` de `/get-config`. L'interface n'a plus le droit de les coder en dur.
- **La surveillance ne peut plus être remise sous une conjonction d'éligibilité.** C'est la troisième récidive de ce motif dans le projet ; il est désormais structurellement exclu pour les gardes pH et ORP.
- **Parité stricte `/data` ↔ WebSocket.** Tout champ consommé par l'interface doit exister sur **les deux** canaux, avec le même nom et la même valeur — troisième récidive également (pentes de sonde en 2.21.2, calibration de pompe en 2.23.0, indicateurs de garde ici).
- **L'injection manuelle reste une échappatoire.** Aucune garde chimique ne peut la fermer sans une nouvelle décision produit ; le levier autorisé est l'information préalable.

## Références

- Code : [`src/constants.h`](../../src/constants.h) (source unique, seuils dérivés, 3 `static_assert` d'invariant), [`src/ph_guard_logic.h`](../../src/ph_guard_logic.h) (module pur : `phGuardBlocksBelow`, `phGuardBlocksAbove`, `roundPhOffsetToStep`, `isPhProbeHealthy`, `phModeChangeNeedsAck`), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (`tickPhGuardSurveillance`, `tickOrpPhInhibitSurveillance`, `isPhGuardBlocking`), [`src/pump_controller.h`](../../src/pump_controller.h) (`copyPhDoseBlockedReason`, `isPhRawGuardBlocked`), [`src/config.cpp`](../../src/config.cpp) (clamp NVS de `ph_target`, arrondi de l'offset au pas), [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (validation + alignement au pas, bornes servies par `/get-config`), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) / [`src/ws_manager.cpp`](../../src/ws_manager.cpp) (parité des 4 indicateurs), [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) (bornes d'auto-discovery, refus `ph_target`), [`data/app.js`](../../data/app.js) (contrôle segmenté, incrémenteur, encart d'injection manuelle)
- Tests : `test/test_native_ph_guard/`, `test/test_native_ph_offset/`
- Spec : `specs/features/done/refonte-seuils-ph-fixes-derives.md`
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#gardes-de-sécurité-ph-refonte-v2250), [`docs/API.md`](../API.md#post-save-config--critical), [`docs/MQTT.md`](../MQTT.md#alertes), [`docs/features/page-ph.md`](../features/page-ph.md)
- ADR liés : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure), [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md) (offset appliqué en sortie des accesseurs), [ADR-0041](0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md) (mesures non représentatives)
