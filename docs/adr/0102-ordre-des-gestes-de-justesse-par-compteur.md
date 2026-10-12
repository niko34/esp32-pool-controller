# ADR-0102 — L'ordre des gestes de justesse se décide par COMPTEUR, jamais par l'horloge ; le verdict est SERVI, pas recalculé

- **Statut** : Accepté
- **Date** : 2026-10-12
- **Décideurs** : `architect` (feature-156), confirmé par le gate `pool-chemistry`
- **Spec(s) liée(s)** : [`feature-156`](../../specs/features/done/feature-156-mode-reglage-de-la-mesure-ph.md)

## Contexte

La correction manuelle de la mesure pH (feature-155, `ph_offset`) et l'étalonnage
de la sonde EZO sont **deux gestes qui touchent à la justesse de la mesure**,
mais un étalonnage modifie le zéro **et** la pente de la sonde : l'état qu'une
correction antérieure compensait **n'existe plus**. Une correction posée avant
le dernier étalonnage réussi est donc **caduque**, et continue pourtant de
s'appliquer en silence à chaque lecture — y compris aux gardes de sécurité et
au dosage.

Pour servir ce verdict, le firmware doit pouvoir répondre à une question
unique : **le dernier étalonnage est-il postérieur à la correction en
vigueur ?** Deux candidats évidents pour trancher cet ordre existaient déjà
dans le code — l'horloge murale (`time(nullptr)`) et `millis()` — et aucun des
deux ne convient :

- **Les epochs seuls** (`time(nullptr)`) sont **indécidables** si l'horloge
  n'est pas synchronisée au moment de l'un des deux événements (NTP/RTC pas
  encore acquis au démarrage, coupure secteur longue) : les deux champs valent
  alors `0`, et comparer deux zéros ne dit rien de l'ordre réel. Un étalonnage
  fait au petit matin avant synchronisation NTP perdrait son droit à rendre
  caduque une correction posée la veille — **l'alerte serait perdue**, sur une
  valeur qui engage le dosage.
- **`millis()`** se replie à **24,8 jours** (dépassement `uint32_t`) et surtout
  **repart de zéro à chaque redémarrage** : un étalonnage fait avant un reboot
  paraîtrait **postérieur** à une correction posée après — l'ordre réel serait
  inversé par un simple redémarrage, le cas le plus fréquent de la flotte.

## Décision

**L'ordre des deux événements est décidé par un compteur entier croissant
(`seq`), jamais par une estampille temporelle.** Chaque écrivain unique
(`setPhOffsetStamped()` pour la correction, `markPhCalibrationStamped()` pour
l'étalonnage) pose `seq = nextJustnessSeq(phOffsetSeq, phCalSeq) = max(a, b) +
1` — une fonction pure, testée en natif (`src/ph_offset.h`). Le verdict
« correction caduque » (`isPhOffsetStale()`) compare `phOffsetSeq < phCalSeq`,
jamais les epochs.

Les deux epochs (`phOffsetEpoch`, `phCalEpoch`) sont **conservés et servis**,
mais **uniquement pour l'affichage** (dater la bannière, dater l'en-tête de
calibration) — **c'est `seq` qui fait foi pour l'ordre**, jamais recalculé
côté client (même principe que [ADR-0100](0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md) :
un verdict qui engage le dosage est servi, pas reconstruit).

## Alternatives considérées

- **Epochs seuls** (rejetée) — indécidable si l'horloge n'est pas
  synchronisée au moment de l'un des deux événements ; l'alerte serait perdue
  précisément dans le cas où l'utilisateur en a le plus besoin (appareil qui
  vient de redémarrer, NTP pas encore acquis).
- **`millis()`** (rejetée) — repli à 24,8 jours, et surtout remise à zéro à
  chaque redémarrage : un étalonnage d'avant-reboot paraîtrait postérieur à
  une correction d'après-reboot, inversant l'ordre réel.
- **Compteur `seq` auto-contenu** (retenue) — monotone par construction
  (`max(a,b)+1`), insensible à l'horloge et au redémarrage, **zéro clé de
  stockage supplémentaire** par rapport à une solution à epoch unique (les deux
  epochs existaient déjà pour l'affichage), et testable en natif sans aucune
  dépendance au temps système.

## Conséquences

### Positives
- Le verdict de caducité reste correct après un rechargement de page, un
  redémarrage de l'appareil, ou des semaines d'écart entre les deux
  événements.
- Aucune dépendance à la synchronisation NTP/RTC pour la **décision** (elle
  reste nécessaire pour l'**affichage** des dates, mais son absence ne casse
  plus le verdict).
- Logique pure, testée par 7 cas natifs (`test_native_ph_offset`), y compris
  le cas limite des deux séquences égales et celui de la rétrocompatibilité
  (0/0 au premier démarrage d'un appareil mis à jour).

### Négatives / dette assumée
- Deux compteurs de plus à persister en NVS (`ph_offset_sq`, `ph_cal_sq`), en
  plus des deux epochs déjà nécessaires à l'affichage — redondance assumée,
  chaque paire a un rôle distinct (ordre vs affichage) et aucune des deux ne
  peut remplacer l'autre.
- Une correction posée avant un étalonnage **antérieur à cette version**
  (compteurs à 0 des deux côtés) ne sera signalée caduque qu'au **prochain**
  étalonnage — rétrocompatibilité assumée : le silence au premier démarrage
  est un choix délibéré (pas de fausse alerte sur tout le parc), validé par le
  gate `pool-chemistry`.

### Ce que ça verrouille
- Tout futur mécanisme de justesse qui doit établir un ordre entre deux
  événements firmware (un troisième geste de justesse, par exemple) devra
  suivre le même patron : un compteur dédié pour l'ordre, un epoch pour
  l'affichage — jamais l'inverse.
- Le verdict de caducité reste une décision **firmware seule** ; un client ne
  doit jamais recevoir les deux compteurs bruts pour les comparer lui-même
  (ils ne sont d'ailleurs **pas exposés** par `/get-config` ni la trame WS —
  seul le verdict booléen `ph_offset_stale` l'est).

## Références

- Code : [`src/ph_offset.h`](../../src/ph_offset.h) (`isPhOffsetStale()`,
  `nextJustnessSeq()`), [`src/config.h`](../../src/config.h) (champs
  `phOffset*`/`phCal*`), [`src/config.cpp`](../../src/config.cpp)
  (`setPhOffsetStamped()`, `markPhCalibrationStamped()`)
- Test : [`test/test_native_ph_offset/test_ph_offset.cpp`](../../test/test_native_ph_offset/test_ph_offset.cpp)
  (section D4)
- Spec : [`specs/features/done/feature-156-mode-reglage-de-la-mesure-ph.md`](../../specs/features/done/feature-156-mode-reglage-de-la-mesure-ph.md)
- Doc externe : [API.md](../API.md), [sensors.md](../subsystems/sensors.md)
