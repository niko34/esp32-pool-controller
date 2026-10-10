## Métadonnées
- **ID** : feature-149
- **Slug** : plus-de-mesure-figee-dans-les-causes-de-garde-ph
- **Type** : bug
- **Statut** : todo
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
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry** — OBLIGATOIRE. Le texte de la ligne 1025 est verrouillé MOT À MOT par
      ce gate (bug-135, 2026-10-05) ; le modifier exige un nouveau passage. Les textes MQTT
      (`publishAlert`) et `critical` associés sont également de son ressort.
- [ ] ux-ui-designer — non : aucune mise en page touchée, aucune surface nouvelle.
- [x] embedded-developer
- [ ] web-ui-developer — le frontend affiche la chaîne telle quelle, sans modification.
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
| 1279 | ORP | « pH indisponible / stabilisation » — aucune valeur | ✅ sain |
| 1429 | ORP | seuil `kPhCeilingRawHard` seul | ✅ sain |
| 1462 | ORP | seuil `kPhOrpInhibit` seul | ✅ **corrigé le 2026-10-08** |
| 1139 | pH | « régulation basculée en manuel » — aucune valeur | ✅ sain |
| **916** | pH | `String(phRaw, 2)` | ❌ **se périme** |
| **938** | pH | `String(ph, 2)` | ❌ **se périme** |
| **994** | pH | `String(phRaw, 2)` **et** `String(ph, 2)` | ❌ **se périme, deux fois** |
| **1025** | pH | `String(ph, 2)` | ❌ **se périme — cas signalé** |

La voie ORP est entièrement assainie ; la voie pH n'a jamais reçu le correctif.

### ⚠️ Tension à trancher par le gate, explicitement
Le texte de la ligne 1025 a été **imposé mot à mot le 2026-10-05** (bug-135), mesure comprise :
commentaire ligne 1016, « Texte imposé MOT À MOT par le gate pool-chemistry — ne pas reformuler
sans nouveau passage ». **Trois jours plus tard**, le même gate établissait sur la voie ORP
qu'inscrire une mesure est un défaut. Les deux décisions sont en conflit ; la plus récente n'a
jamais été reportée sur le pH. **C'est au gate de trancher**, pas à l'implémentation — et la
spec doit obtenir ce verdict AVANT toute modification de libellé.

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
- [ ] A1 — **Verdict `pool-chemistry` d'ABORD** sur les quatre libellés, et sur la tension entre
      la décision bug-135 (2026-10-05) et celle du 2026-10-08. Rien ne se code avant.
- [ ] A2 — Lignes 916, 938, 1025 : la mesure cède la place au seuil.
- [ ] A3 — Ligne 994 : traitement spécifique, l'opposition brut/corrigé doit survivre.
- [ ] A4 — Vérifier qu'aucun autre appel ne réintroduit le défaut (le recensement ci-dessus
      fait foi et doit être re-fait, pas recopié).
- [ ] D1 — Tests sur la logique pure : une cause posée au déclenchement reste vraie N minutes
      plus tard quelle que soit l'évolution de la mesure.
- [ ] D2 — **Validation par MUTATION** : réintroduire `String(ph, 2)` dans un libellé et
      vérifier qu'un test meurt. Sans ça, la non-régression n'est pas prouvée.
- [ ] C1 — `docs/subsystems/pump-controller.md` : règle « une cause de garde cite un seuil,
      jamais une mesure », avec son motif.
- [ ] C2 — Entrée `CHANGELOG.md`.

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
- **Probable** : non
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun. La décision existe déjà, prise le 2026-10-08 et consignée dans
  `pump_controller.cpp:1452-1461` ; cette spec la PROPAGE, elle n'en prend pas de nouvelle. Si
  le gate tranche autrement pour la ligne 994, cet arbitrage-là mériterait un ADR.

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
- [ ] Aucune cause de refus de garde pH ne contient de mesure vive
- [ ] Après 40 minutes de blocage continu, le texte affiché ne contredit plus la carte
- [ ] Les seuils, l'hystérésis et les codes de blocage sont inchangés (vérifié par test)
- [ ] La ligne 994 conserve sa valeur diagnostique sur l'offset
- [ ] `pio run` compile sans nouveau warning
- [ ] `pio test -e native` : zéro régression
- [ ] Verdict `pool-chemistry` **GO** consigné dans la spec avant clôture

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

## Hors périmètre
- Le dédoublonnage bandeau / badge → feature-148.
- Toute modification de seuil, d'hystérésis, de condition de déclenchement ou de code de refus.
- Les causes de la voie ORP, déjà saines.
- Le rafraîchissement périodique du texte — option explicitement rejetée le 2026-10-08, motif
  consigné dans le code. Ne pas la rouvrir sans argument neuf.
