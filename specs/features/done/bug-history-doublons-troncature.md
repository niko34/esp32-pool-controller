# Bug — Graphiques vides : agrégats horaires dupliqués + réponse /get-history tronquée

- **Type** : bug (format minimal `specs/modele.md`)
- **Statut** : done (2026-08-09 — revue Approved après correction kMaxRawDataPoints 72→96, builds bi-core + 436 tests verts ; validation on-device restante : json.tool + retour des graphiques après flash)
- **Version cible** : 2.18.1
- **Créée** : 2026-08-09 (constaté en production ; présent depuis v2.14.0 pour la
  troncature, antérieur pour la duplication)
- **Domaine** : Historique (`src/history.*`, `src/history_logic.*`) + route
  `/get-history` (`src/web_routes_data.cpp`) — aucun chemin de dosage
- **Spec liée** : bug-heap-toctou (v2.14.0, origine de la route chunked)

## Symptômes observés (production, 2026-08-08/09)

1. Dashboard et écrans pH/ORP : aucun graphique. Console Safari :
   `SyntaxError: The string did not match the expected pattern` dans
   loadHistoricalData/loadMiniChartData = `resp.json()` sur corps invalide.
2. `/get-history?range=3d` : réponse **TRONQUÉE** à 26 478 octets — se termine
   après un point complet, SANS le pied `],"count":N}` (vérifié json.tool :
   « Expecting ',' delimiter — position 26478 sur 26478 »).
3. `/get-history?range=all` : chaque créneau horaire présent en ~5 exemplaires
   (même timestamp, valeurs différentes, granularity:1).

## Causes racines (deux bugs qui se renforcent)

**A. Duplication des agrégats horaires/journaliers** (`history.cpp::consolidateData`) :
les points RAW d'une même heure franchissent le seuil `RAW_MAX_AGE` (6 h)
PROGRESSIVEMENT ; la consolidation (toutes les 5 min) crée un agrégat par
passage pour le même créneau à partir du seul sous-ensemble déjà « vieux »
(jusqu'à 12 doublons/heure), et l'insertion ne fusionne jamais avec un agrégat
existant du même (timestamp, granularité). Idem HOURLY→DAILY. Effet : buffer
gonflé, consolidations longues (mutex tenu), doublons persistés.

**B. Troncature sous contention mutex** (`web_routes_data.cpp::handleGetHistory`) :
sur timeout mutex sans octet en attente, le callback chunked renvoie
`RESPONSE_TRY_AGAIN`. La lib (fork 3.11.2, WebResponses.cpp:454) le gère en ne
rien émettant… mais ne re-sonde le callback qu'à l'ack TCP suivant — s'il n'y a
RIEN en vol, aucun ack ne viendra : silence → timeout TCP → connexion close →
JSON tronqué sans pied. B est déclenché d'autant plus souvent que A rallonge
les consolidations.

## Correctif

**A1 — Consolidation par créneau COMPLET** : ne convertir un créneau (heure/jour)
que lorsque le créneau ENTIER a dépassé l'âge limite (prédicat pur
`historyBucketConsolidable(now, bucketStart, bucketLen, maxAge)`), condition
appliquée à l'identique au groupement ET à la suppression des sources — un seul
agrégat par créneau, de meilleure qualité (l'heure complète, plus des tranches
de 5 min).

**A2 — Déduplication/fusion** : après chaque tri du buffer (loadFromFile +
consolidateData), fusion des entrées adjacentes de même (timestamp,
granularité≠RAW) : moyennes équipondérées champ à champ via
`historyMergeMeanFinite(a,b)` (fini+fini→moyenne, un seul fini→lui, sinon NaN),
booléens en OR. Le passage au chargement ASSAINIT d'un coup les doublons déjà
persistés sur les appareils (pas d'effacement d'historique).

**B1 — Espace de maintien de flux** : sur timeout mutex sans octet en attente,
émettre UN espace (légal entre deux éléments JSON) au lieu de
`RESPONSE_TRY_AGAIN` — le flux TCP reste vivant, le réessai se fait au cycle
suivant, la borne existante `kHistoryChunkMaxRetries` (→ clôture propre avec
pied) est conservée.

## Critères d'acceptation

- [x] Logique pure testée en natif : 8 tests (dont LE cas du bug : premier
      point vieux mais créneau incomplet → refusé)
- [x] Un créneau horaire ne produit plus qu'UN agrégat (A1) ; fusion au
      chargement/import/consolidation (A2, revue ligne à ligne)
- [x] `/get-history` n'émet plus `RESPONSE_TRY_AGAIN` (seuls des commentaires
      le mentionnent) ; espaces de maintien + borne 200 → pied garanti
- [ ] Sur l'appareil : `python3 -m json.tool` valide `range=3d` ET `range=all`,
      les graphiques réapparaissent, les doublons existants ont fusionné
- [x] `pio run` + `esp32dev_core3` verts ; 436/436 natifs ; revue Approved

## Hors périmètre

- La qualité des moyennes des doublons déjà persistés (fusion équipondérée =
  approximation assumée, les poids d'origine ne sont pas stockés)
- `/download-logs` et `/get-logs` (patrons différents, aucun TRY_AGAIN — grep vérifié)

## Notes d'implémentation (clôture 2026-08-09)
- Revue « Request changes » PERTINENTE : kMaxRawDataPoints=72 (6h pile) aurait
  affamé l'agrégation avec A1 (survie ~7h requise) → porté à 96 (8h). Budget
  partition pire cas 13→14 blocs/16 (note additive ADR-0033), garde
  historySaveAdmit inchangée en filet.
- SUIVI CONNU (préexistant, hors lot, relevé en revue) : kMaxHourlyDataPoints
  (168 = 7 j) < HOURLY_MAX_AGE (15 j) → les agrégats JOURNALIERS ne se forment
  jamais (les horaires sont évincés avant d'y être éligibles). Documenté dans
  docs/subsystems/history.md — à traiter dans une spec future (aligner l'âge ou
  la capacité).
- Validation on-device au premier flash 2.18.1 : log « X agrégats dupliqués
  fusionnés » au boot, json.tool valide sur range=3d et all, graphiques
  de retour.

## ADDENDUM — Cause racine C, la vraie (lot 2.18.2, 2026-08-09)

La « cause B » ci-dessus (RESPONSE_TRY_AGAIN/dead-air) était un DIAGNOSTIC
ERRONÉ : le fix « espace de maintien » n'a pas résolu la troncature. La cause
réelle, démontrée par capture du flux chunked BRUT sur l'appareil (curl --raw :
chunks = émis exact SANS pied + terminateur propre) :

**Le cas kFooter de la machine à états chargeait le pied `],"count":N}` dans le
carry ET passait phase=kDone dans la même itération — la condition de boucle
`while (… && phase != kDone)` sortait AVANT d'émettre le carry. Le pied n'a
JAMAIS été envoyé, sur AUCUNE réponse, depuis la v2.14.0** (le cas kHeader ne
déclenchait pas le bug : il passe en kPoints et la boucle continue). Les
graphiques étaient donc morts depuis le déploiement du 2026-08-05 — l'anomalie
« pas de données » du 06/08 était ce bug, pas l'invalidation du bypass.

Correctif (2.18.2) : condition de boucle étendue (`carry non vide OU phase !=
kDone`) + refactor de robustesse : instantané filtré en UNE prise de mutex
(`snapshotFiltered`, 100 ms, 503 rare) puis streaming sans mutex — le mécanisme
timeout/espaces/TRY_AGAIN disparaît entièrement (getPointByIndex,
kHistoryPointReadTimeoutMs, kHistoryChunkMaxRetries retirés). Validé on-device
(pied présent, JSON valide). Deux revues étaient passées à côté — leçon
consignée : la machine à états chunked doit être extraite en logique pure
testable (ADR-0017) avec un test « le flux se termine par le pied » (dette
documentée dans history.md).

Pourquoi 3 itérations : le bug était masqué par des couches successives
(doublons réels → contention réelle → comptabilité de diagnostic elle-même
faussée par les early-returns → variable shell vide côté client). La percée =
mesurer AU CÂBLE (flux brut) au lieu de raisonner sur les couches hautes.
