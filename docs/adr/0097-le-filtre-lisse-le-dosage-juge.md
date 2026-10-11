# ADR-0097 — Le filtre lisse, le dosage juge : la chaîne de mesure pH/ORP perd ses verrous collants

- **Statut** : Accepté
- **Date** : 2026-10-11
- **Décideurs** : `pool-chemistry` (P1 sur la conception, GO sous 7 conditions ; P2 sur le diff), `architect`, `embedded-developer`, `web-ui-developer`, `ux-ui-designer`, `code-reviewer`, `doc-writer`, utilisateur (exigence de généricité, dimensionnement final de la médiane délégué au gate)
- **Spec(s) liée(s)** : [`feature-152`](../../specs/features/done/feature-152-refonte-de-la-chaine-de-mesure.md), `bug-135`, `feature-069`, `feature-097` (signal pH anti-corrélé, contexte), supersède [ADR-0027](0027-filtre-tendance-centrale.md), supersède partiellement [ADR-0041](0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md), amende [ADR-0049](0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md)

## Contexte

La chaîne de mesure pH/ORP comptait **14 étages** et **13 états pouvant bloquer le dosage**,
accumulés sur **18 specs et 11 ADR** — un seul retrait dans tout cet historique
([ADR-0092](0092-retrait-analyse-stabilite-porte-mesures.md)). Chaque ajout répondait à un
problème réel, mais chacun ajoutait un état où la mesure pouvait être déclarée inutilisable.
La chaîne **température**, dans le même firmware, compte **5 étages** et **1** état bloquant,
pour **zéro incident** — c'est le point de comparaison qui a motivé ce lot.

Le mécanisme du piège, identifié lot par lot : (1) le filtre est rendu méfiant pour écarter les
fausses mesures (rejet de pic isolé) ; (2) méfiant, il se verrouille — la médiane ne bouge que
si des échantillons entrent, donc dès que la grandeur vraie s'éloigne durablement, chaque
échantillon suivant est rejeté à son tour et la médiane reste figée ; (3) on ajoute une sortie
de secours (ré-amorçage) ; (4) cette sortie pouvant être dangereuse, on la met sous trois
gardes cumulatives ; (5) l'une de ces gardes (bande de cohérence 0,20 pH) est calée sur un
bruit qui n'est **pas** celui du terrain (bruit réel mesuré : 0,36) ⇒ la sortie de secours ne
peut **jamais** se déclencher. Retour au point 2.

Deux incidents de production l'ont démontré : un arrêt de dosage pH **et** chlore ≥ 3 h
(v2.20.3, écart légitime de la grandeur mesurée), et un verrouillage à 141 rejets le
2026-10-11 (sonde plongée dans une solution d'étalonnage, médiane figée à 4,62 contre un brut
à 8,46). **Aucun cas n'a été identifié**, par le gate `pool-chemistry` ni par relecture de
l'historique complet du dépôt, où ces mécanismes ont démontrablement évité un dommage réel.

**Exigence posée par l'utilisateur, contraignante pour toute la conception** : « Aucun des
principes ne doit être spécifique à ma piscine. Ça doit être générique pour fonctionner dans
toutes les piscines. » — voir [ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md)
pour la règle de conception qui en découle, et qui dépasse ce seul lot.

## Décision

**Ligne directrice : le filtre lisse, il ne juge pas — c'est la couche de dosage qui juge.**
La chaîne de mesure passe de 14 à **5 étages** (lecture, rejet de l'absurde, médiane +
lissage, offset, fraîcheur), identiques en structure à la chaîne température. La couche de
dosage conserve ses **bornes** (butées de plausibilité, plafonds horaire/journalier) : une
borne compare, elle ne délibère pas, elle ne peut donc pas se verrouiller sur son propre
jugement.

**Le point central : la médiane EST le rejet d'aberrations.** Une médiane de N écarte **par
construction** jusqu'à (N−1)/2 valeurs extrêmes, sans aucun seuil réglable sur une
installation. Un échelon légitime traverse la fenêtre en (N+1)/2 échantillons, **quoi qu'il
arrive** — il n'existe donc **aucune séquence d'entrées** qui verrouille ce module, propriété
qu'aucun seuil de rejet explicite ne peut offrir. Dimensionnement validé par le gate :
**N = 9** (retard total ≈ 67 s à 5 s de cadence, soit 1/13 de `kPhMixingDelayMs` — invisible
pour la régulation), lissage exponentiel de constante de temps **τ = 45 s** recalculée à
chaque échantillon depuis le temps réellement écoulu (indépendante de la cadence).

**Ce qui disparaît** (aucun contre-exemple trouvé par le gate) : rejet de spike isolé, sortie
de secours de ré-amorçage et ses trois gardes cumulatives (cooldown, bande admissible,
cohérence de série), dispersion de fenêtre et son **latch EMI collant** (il se posait sur du
bruit normal — dispersion 0,402 relevée pour un cap à 0,5, il n'inhibait aucun danger réel),
plancher de gel de la porte d'alimentation, tolérance aux trous de la sortie de secours,
grâce de péremption post-ré-amorçage, alarme « filtre non prêt durablement ». **Les deux
verrous collants du projet disparaissent.**

**Ce qui reste, sourcé explicitement** (voir [ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md)) :
rejet NaN / hors plage physique (étage 2, comparaison stricte) ; médiane + lissage (étage 3) ;
**détecteur de sonde figée** (`FrozenDetector`, feature-022, CONSERVÉ — epsilon = ½ LSB
matériel, borne haute de N = décision produit — seul mécanisme qui attrape une sonde morte
rendant une constante plausible, sans lequel une telle sonde serait dosée indéfiniment) ;
fraîcheur (étage 5) ; porte d'alimentation **simplifiée** (vidage à la fermeture, aucune
action à l'ouverture — la fenêtre se remplit seule) ; butées de plausibilité pH et plafonds de
dose (couche de dosage, hors de ce module).

**Une seule valeur officielle.** `getPhFiltered()`/`getOrpFiltered()` deviennent **la** valeur
consommée par l'affichage, l'historique, MQTT et le dosage — identiquement. Le repli silencieux
antérieur (`!isnan(filtered) ? filtered : raw`, présent dans `ws_manager.cpp`,
`web_routes_data.cpp`, `mqtt_manager.cpp`) **disparaît** : pendant la montée en régime, l'écran
affiche l'absence de valeur et le dit, il ne substitue plus une autre mesure sans le signaler.
`getPhFilteredRaw()` est **conservé**, renommé `getPhValueRaw()` — il reste la seule source sur
laquelle les butées de dernier recours peuvent s'appuyer indépendamment d'un offset erroné.

**La voie ORP reste sans butée de plausibilité**, question posée au gate en P1 et tranchée :
pendant l'épisode de boue d'octobre 2026, l'ORP est descendu à −46 mV, une mesure **juste**
(chlore nul + matière organique en décomposition). Une butée aurait refusé une mesure vraie.
Chiffrage retenu : plafond journalier d'usine 500 mL/j de chlore 36 °chl ⇒ 57 g Cl₂ ⇒ **2,85
mg/L/j dans 20 m³** (5,7 mg/L/j dans 10 m³, 8,6 mg/L/j dans 10 m³ en mode Boost) — une
excursion **rattrapable** (irritation, sortie de baignade 24-48 h), aucun dommage matériel. Les
bornes restantes (plafonds horaire/journalier, détecteur de sonde figée, médiane) suffisent ;
`feature-145` (butée ORP) reste ouverte et **non bloquante**.

**Alerte nouvelle, sans nouveau verrou** : le plafond journalier atteint `kDailyLimitStreakAlertDays`
(= 3) jours calendaires consécutifs déclenche une alerte `critical` + MQTT
(`ph_daily_limit_streak` / `orp_daily_limit_streak`) — sous les deux hypothèses possibles
(sonde qui ment / eau réellement mauvaise), poursuivre le goutte-à-goutte au-delà de ce délai
est faux dans les deux cas, et sous le seuil de redox il fabrique des chloramines. C'est une
**alerte**, elle ne bloque rien.

## Alternatives considérées

- **Conserver la chaîne à 14 étages et corriger uniquement la bande de cohérence** (0,20 →
  0,36) — **rejetée** : traite le symptôme du dernier incident, pas la cause. Le mécanisme
  resterait un verrou qui peut se recaler faux sur la prochaine installation — exactement ce
  que l'exigence de généricité interdit.
- **Garder le rejet de spike + sortie de secours, mais retirer seulement le latch EMI** —
  **rejetée** : la sortie de secours reste elle-même verrouillable (cooldown + bande non
  franchissable) ; retirer seulement le symptôme visible laisserait le mécanisme racine intact.
- **Chaîne à 5 étages + médiane comme seul rejet d'aberrations, détecteur de sonde figée
  conservé** — **retenue** : seule option qui élimine structurellement la possibilité même
  d'un verrou (propriété démontrée, pas seulement observée), tout en conservant le seul
  mécanisme dont le retrait exposerait un risque réel identifié par le gate.
- **Ajouter une butée de plausibilité ORP pour compenser le retrait du rejet de spike** —
  **rejetée** : chiffrage du gate (voir Décision) — une butée aurait refusé la mesure VRAIE de
  l'épisode d'octobre. Le risque résiduel (mode commun, feature-137) est borné par les
  plafonds de dose, pas par un jugement sur la valeur.

## Conséquences

### Positives
- Propriété démontrée par construction, pas par test exhaustif : aucune séquence d'entrées ne
  peut laisser le module sans valeur plus longtemps que la traversée normale de la fenêtre
  ((N+1)/2 échantillons) — test central D2 du lot.
- `sensor_filter.{h,cpp}` passe de 959 à **393** lignes ; `SensorFilter::Config` de 13 à **7**
  champs ; les constantes du domaine de ~60 à **12** ; 6 codes `DoseRefusal` retirés,
  **jamais recyclés**.
- L'écran ne ment plus pendant la montée en régime : la valeur affichée et la valeur qui décide
  sont désormais **la même**, ou l'écart est dit explicitement.
- `history.cpp` historise désormais la valeur **officielle** (lissée), cohérente avec ce sur
  quoi la régulation a décidé — fin de la coexistence `point.ph` (brut) / `point.phFiltered`
  (filtré) qui pouvait diverger silencieusement.

### Négatives / dette assumée
- **Discontinuité visible dans les courbes d'historique au déploiement** : les points
  antérieurs portaient le pH brut corrigé instantané, les points postérieurs portent la valeur
  lissée — une petite marche est possible au moment de la bascule. À annoncer explicitement
  (voir CHANGELOG), sous peine d'être lue à tort comme une dérive d'eau.
- **`measures_valid` couple désormais pH et ORP** : une sonde ORP figée fait lire « mesures
  invalides » côté pH aussi. Honnête (aucune des deux grandeurs ne décide alors), mais
  conflatant pour un intégrateur qui voudrait isoler la cause.
- **Le compteur de l'alerte « plafond atteint J jours » n'est pas persisté** (variable RAM) :
  un appareil qui redémarre plus souvent que tous les 3 jours n'atteint jamais ce seuil. Choix
  assumé — persister ajouterait une écriture NVS quotidienne pour un strict gain de visibilité,
  pas de sécurité (voir le commentaire au point d'appel, `pump_controller.cpp`).
- `ADR-0062` (porte de session pH dimensionnée par le bruit de mesure) **n'est pas superseded**
  par ce lot, mais le gate le signale comme un **contre-exemple** de la règle de généricité
  posée par [ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md) — ses chiffres sont
  mesurés sur le filtre qui disparaît ici. Traitement : **spec de suivi distincte**, hors
  périmètre de ce lot.
- `SafetyLimits` reste **explicitement exclu** de la table de réglages par taille de bassin
  (`regulation_defaults.h:24`) — un bassin Small reçoit le plafond journalier d'un Medium. Le
  gate situe ici le vrai défaut résiduel de sécurité ORP (pas l'absence de butée). **Spec
  distincte** ; toute correction devra se formuler comme une **présélection de défaut**, jamais
  comme une entrée de calcul (invariant feature-090).

### Ce que ça verrouille
- Toute future garde de la chaîne de mesure doit être une **borne** (comparaison sans mémoire
  d'état ni jugement cumulatif), jamais un mécanisme qui peut se verrouiller sur sa propre
  méfiance — la leçon du cycle 2 (rejet de pic → sortie de secours → gardes → verrou) ne doit
  pas se rejouer.
- Toute constante nouvelle de ce domaine doit déclarer sa source parmi les trois autorisées
  (physique, décision produit, matériel) — voir [ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md).
- Un champ WS/MQTT/`/data` qui portait un état de jugement du filtre (prêt/instable/alarme) ne
  doit pas être réintroduit sans repasser par le gate `pool-chemistry` : c'est précisément le
  motif de ce retrait.

## Références

- Code : [`src/sensor_filter.h`](../../src/sensor_filter.h), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp), [`src/dosing_logic.h`](../../src/dosing_logic.h) (`DoseRefusal`), [`src/daily_limit_streak_logic.h`](../../src/daily_limit_streak_logic.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp), [`src/history.cpp`](../../src/history.cpp), [`src/ws_manager.cpp`](../../src/ws_manager.cpp), [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp)
- Tests : [`test/test_native_sensor_filter/`](../../test/test_native_sensor_filter/), [`test/test_native_daily_limit_streak/`](../../test/test_native_daily_limit_streak/), [`tools/check_removed_measurement_vocabulary.sh`](../../tools/check_removed_measurement_vocabulary.sh)
- Spec : [`specs/features/done/feature-152-refonte-de-la-chaine-de-mesure.md`](../../specs/features/done/feature-152-refonte-de-la-chaine-de-mesure.md)
- Doc externe : [`docs/API.md`](../API.md), [`docs/MQTT.md`](../MQTT.md), [`docs/subsystems/sensors.md`](../subsystems/sensors.md#chaîne-de-mesure-à-5-étages-feature-152), [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md)
