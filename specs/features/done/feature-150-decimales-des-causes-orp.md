## Métadonnées
- **ID** : feature-150
- **Slug** : decimales-des-causes-orp
- **Type** : bug
- **Statut** : livré — tests verts, revue OK (non commité, voir Notes d'implémentation)
- **Priorité** : basse
- **Version cible** : 2.65.1
- **Dernière mise à jour** : 2026-10-10
- **Spec(s) liée(s)** : feature-149 (a posé la règle et laissé ce résidu) · ADR-0095

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
- [ ] architect — **non** : aucune conception. Deux caractères, une constante ajoutée à un
      `static_assert` existant, deux sites ajoutés à un test existant. Le motif est posé par
      ADR-0095, l'outillage est livré par feature-149.
- [x] **pool-chemistry** — OBLIGATOIRE. Le texte de 1514 est verrouillé mot à mot (verdict du
      2026-10-08). Passage court attendu : **quatrième** sur le même sujet, et le gate a déjà
      tranché deux fois dans ce sens.
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Deux causes de garde de la voie ORP citent des seuils **pH** à deux décimales alors que les
causes pH les affichent désormais à une. Il s'agit de **la même constante**, écrite de deux
façons, et les deux messages peuvent s'afficher en même temps.

## Contexte

### Le défaut, mesuré
| Ligne | Texte | Constante |
|---|---|---|
| 1481-1482 | « pH brut en butée haute (butée **8.10**) — dosage chlore suspendu » | `kPhCeilingRawHard` |
| 1514 | « pH trop élevé (seuil **7.60**) — dosage chlore suspendu » | `kPhOrpInhibit` |

Depuis feature-149 (v2.65.0), la cause pH correspondante affiche
« pH brut au-dessus de la butée de sécurité (**8.1**) » — **`kPhCeilingRawHard` également**.

### ⚠️ Ce n'est pas théorique : les deux peuvent s'afficher ENSEMBLE
Les deux gardes se déclenchent sur **la même condition** — un pH brut trop haut : la pH refuse
l'acide, l'ORP refuse le chlore. Le bandeau du tableau de bord ayant **une ligne par voie**,
l'utilisateur peut lire, l'une sous l'autre, **le même seuil écrit 8.1 et 8.10**.

### Pourquoi feature-149 l'a laissé passer — la vraie raison
Sa section « Hors périmètre » qualifiait la voie ORP de **« saine »**. Or une seule chose avait
été vérifiée : que ces causes ne citent pas de **mesure vive**, donc qu'elles ne se périment
pas. Exact, et rien de plus. **Le mot annonçait un examen d'ensemble qui n'avait pas eu lieu**,
et il a dissuadé d'aller voir le reste.
Corrigé dans la spec de feature-149 le 2026-10-10 (section « Correction de rédaction »).
⚠️ L'attribution fautive a été relevée par l'utilisateur : **la formule venait de la spec, pas
de lui.** Ne pas lui prêter ce cadrage.

### La décision produit qui s'applique
Utilisateur, 2026-10-10 : **une décimale pour toute valeur mesurée de pH ou pour un seuil pH**,
deux réservées aux **réglages** et aux **bandes de diagnostic**. Formalisée par ADR-0095.
Rien dans cette décision ne la limite à la voie pH — c'est l'étiquette « saine » qui a borné le
lot, pas la règle.

## ✅ RECENSEMENT EXHAUSTIF DES CITATIONS DE SEUIL pH (vérifié le 2026-10-10)

Fait **après** l'implémentation, pour ne pas reproduire l'erreur de feature-149 — où une
appréciation globale (« voie ORP saine ») avait dispensé d'un examen site par site.
`grep` de toutes les occurrences de `String(kPh<Seuil>` dans `pump_controller.cpp` :

| Décimales | Sites | Nature | Statut |
|---|---|---|---|
| **1** | 923, 953, 1023-1024, 1060 | causes de garde **pH** latchées | feature-149 ✅ |
| **1** | 1488, 1524 | causes de garde **ORP** latchées | **feature-150** ✅ |
| 2 | 910, 942, 1001, 1043 | `systemLogger.critical` + `publishAlert` au franchissement | correct — horodatés |
| 2 | 932, 963, 1034, 1069 | messages de **relâche** | correct — **ne pas toucher** |
| 2 | 1460, 1463-1464, 1498, 1501-1502 | **bandes d'hystérésis ORP** (8,05 / 7,55) | correct — **ne pas toucher** |
| 2 | **884** | `systemLogger.warning` + `publishAlert("ph_low_warning")` | **examiné** — journal/MQTT horodaté ⇒ garde 2 décimales |
| 2 | **1385** | `systemLogger.info` de relâche ORP | **examiné** — journal ⇒ garde 2 décimales |

Les deux dernières lignes ne figuraient dans aucune analyse antérieure. Elles sont conformes
à la règle du gate (« les journaux et MQTT, horodatés, gardent la mesure ») — mais c'est
désormais **constaté**, pas supposé.

⇒ **Après ce lot, toute cause de garde latchée citant un seuil pH est à une décimale, sur les
deux voies.** Aucun site restant.

## Comportement attendu
Les deux causes citent leurs seuils pH à **une** décimale : « butée **8.1** », « seuil **7.6** ».
**Aucun autre mot ne change.** Les deux seuils sont ronds au dixième (`kPhCeilingRawHard` = 8.10,
`kPhOrpInhibit` = 7.60) : l'affichage est **sans perte**.

### Cas limites
- **Longueur** : les textes **raccourcissent** d'un octet chacun. Re-mesurer tout de même après
  substitution — c'est la discipline que feature-149 a dû apprendre à ses dépens.
- **`kPhOrpInhibit` doit rejoindre le `static_assert`** : il n'y figure pas aujourd'hui (le verrou
  couvre les quatre seuils cités par les causes pH). Sans lui, la règle serait vérifiée pour les
  unes et pas pour les autres — exactement le genre d'asymétrie qui a produit ce défaut.
- **Ne PAS toucher** aux bandes d'hystérésis ORP (~1451, ~1483) : elles citent `seuil −
  kPhGuardReleaseHysteresis`, soit **8,05** et **7,55**, qui ne sont **pas ronds**. Les arrondir
  annoncerait une reprise de dosage à un pH que l'appareil n'utilise pas. Protégées par test
  depuis feature-149 — vérifier que ce verrou tient toujours après le lot.

## Comportement actuel
`8.10` et `7.60` dans les causes ORP ; `8.1` dans la cause pH pour la même constante.

## Contraintes techniques
- Seuils lus **par constante**, jamais en littéral. `String(x,1)` rend un **point** décimal.
- Aucune condition de déclenchement, aucun seuil, aucune hystérésis, aucun code de blocage,
  aucun latch modifié. **Seul le nombre de décimales change.**
- `constants.h` reste self-contained (pas de `<cmath>`).

## Fichiers potentiellement concernés
- `src/pump_controller.cpp` — lignes ~1482 et ~1514. **Elles seules.**
- `src/constants.h` — ajouter `kPhOrpInhibit` au `static_assert` existant.
- `test/test_native_ph_guard_causes/test_ph_guard_causes.cpp` — étendre T3 aux deux sites ORP.
- `docs/subsystems/pump-controller.md`, `docs/adr/0095-*.md` (retirer l'incohérence consignée),
  `CHANGELOG.md`.

## Livrables — check-list
- [x] A1 — Verdict `pool-chemistry` **d'abord** sur les deux libellés. Rien ne se code avant.
      (quatrième passage sur le même sujet, GO conforme aux trois précédents)
- [x] A2 — `, 2)` → `, 1)` aux deux sites (`pump_controller.cpp:1487, 1524`).
- [x] A3 — `kPhOrpInhibit` ajouté au `static_assert` de rondeur (`constants.h:1752-1756`,
      cinq seuils couverts).
- [x] D1 — T3 étendu : les causes **ORP** citant un seuil pH sont désormais en `, 1)`, couvertes
      par `test_native_ph_guard_causes` (six causes latchées : quatre pH + deux ORP).
- [x] D2 — **Mutation** : repasser un site ORP à `, 2)` fait mourir le test. Les **bandes
      d'hystérésis** restent protégées — les passer à `,1)` continue de tuer (vérifié,
      non régressé par ce lot).
- [x] C1 — ADR-0095 : mention « incohérence connue, non traitée » **retirée**, remplacée par le
      constat que la règle couvre désormais les deux voies.
- [x] C2 — `CHANGELOG.md` sous `[2.65.1]`.

## Sécurité chimique
- Dosage impliqué : **non** — aucune décision modifiée, aucun seuil, aucune garde.
- **Risque identifié** : arrondir par mégarde une **bande d'hystérésis** (8,05 / 7,55), qui n'est
  pas ronde. C'est le seul risque réel du lot, et il est déjà couvert par un test de feature-149.
- Risque écarté : aucune injection possible, aucune condition d'autorisation touchée.

## ADR attendu ?
- **Probable** : non — ADR-0095 porte déjà la règle. Ce lot **supprime une exception** qui y est
  consignée, il n'en crée pas.

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- aucun
### Champs WebSocket
- `orpDoseBlockedReason` — contenu modifié, contrat inchangé
### Topics MQTT
- `orp_ph_inhibit` — contenu du message
### Documentation à mettre à jour
- `docs/subsystems/pump-controller.md`, `docs/adr/0095-*.md`, `CHANGELOG.md`

## Critères d'acceptation
- [x] Les deux causes ORP citent leurs seuils pH à une décimale
- [x] La même constante s'affiche **de façon identique** dans une cause pH et dans une cause ORP
      (`kPhCeilingRawHard` → `8.1` des deux côtés)
- [x] Les bandes d'hystérésis restent à **deux** décimales (vérifié par mutation)
- [x] `kPhOrpInhibit` est couvert par le `static_assert`
- [x] `pio run` sans warning nouveau · `pio test -e native` zéro régression (63 suites / 2 126 cas)

## Plan de test manuel
1. Provoquer un pH brut haut : vérifier que le bandeau affiche **8.1** sur les deux lignes.
2. Vérifier qu'un message de relâche affiche toujours **8,05** et non 8,1.

## Notes pour agent
- Lot **très petit**. Le piège n'est pas la difficulté, c'est la **proximité** : deux caractères à
  changer, deux voisins à ne surtout pas toucher. Lire les encadrés d'exclusion posés par
  feature-149 **avant** d'éditer.
- ⚠️ L'offset pH n'est **pas** une suspicion de sonde ; aucun libellé ne doit le suggérer.

## Hors périmètre
- Toute modification de seuil, d'hystérésis, de condition ou de code de refus.
- Les causes pH, traitées par feature-149.
- Les **bandes d'hystérésis**, qui restent à deux décimales **par décision**, pas par oubli.

## Notes d'implémentation (doc-writer, 2026-10-10)

**Mécanique livrée** : les deux causes de garde ORP (`pump_controller.cpp:1487, 1524`) citent
désormais `String(kPhCeilingRawHard, 1)` et `String(kPhOrpInhibit, 1)` — plus de deux décimales
pour des constantes déjà affichées à une décimale côté pH. `kPhOrpInhibit` a rejoint le
`static_assert(numfmt::isRoundedToTenth(...))` (`constants.h:1752-1756`), qui couvre
désormais **cinq** seuils. Les bandes d'hystérésis ORP (`kPhCeilingRawHard −
kPhGuardReleaseHysteresis` = 8,05 ; `kPhOrpInhibit − kPhGuardReleaseHysteresis` = 7,55) et les
messages de relâche **n'ont pas été touchés** — non ronds, ils restent à deux décimales.

**Tests (D1/D2)** : `test_native_ph_guard_causes` étend sa liste blanche de seuils et son
recensement à **six** causes latchées (quatre pH + deux ORP, lignes 922/952/1022/1059/1487/1524).
Quatre mutations jouées : deux sur les sites ORP eux-mêmes (repasser `kPhCeilingRawHard`/
`kPhOrpInhibit` à `,2)` → le test meurt), et **deux sur les bandes d'hystérésis voisines**
(forcer `,1)` sur `8,05`/`7,55` → le test meurt aussi) — ces deux dernières prouvent que le
verrou posé par feature-149 sur les bandes tient toujours après l'extension du périmètre à
l'ORP ; l'une d'elles a été rejouée par le coordinateur. Suite native complète : **63 suites /
2 126 cas, zéro régression**. `pio run` : SUCCESS, aucun warning nouveau.

**Recensement complet, effectué après l'implémentation** (pour ne pas reproduire l'erreur
d'appréciation globale de feature-149, qui avait qualifié la voie ORP de « saine » sans examen
site par site) : voir le tableau de la section « Recensement exhaustif » ci-dessus, repris dans
`docs/subsystems/pump-controller.md`. Deux sites n'avaient **jamais** été examinés avant ce lot
— `publishAlert("ph_low_warning", …)` (~884) et le `systemLogger.info` de relâche ORP (~1385) —
et le sont désormais : tous deux à deux décimales, **à bon droit** (journal/MQTT horodaté, pas
une cause latchée). Documenté explicitement dans `pump-controller.md` pour qu'un futur lot ne
les « corrige » pas par symétrie avec les six causes latchées.

**Documentation mise à jour (C1/C2 + transverses)** :
- `docs/subsystems/pump-controller.md` — table des deux causes ORP mise à jour (avant correctif
  v2.57.1 / après v2.57.1 deux décimales / après feature-150 une décimale), portée de la règle
  « seuil, pas mesure » explicitement étendue aux deux voies, recensement complet des
  citations de seuil pH du fichier.
- [ADR-0095](../../../docs/adr/0095-une-cause-de-garde-cite-un-seuil-pas-une-mesure.md) — la
  section « incohérence connue, assumée hors périmètre » est **retirée** et remplacée par une
  section « portée étendue aux deux voies (feature-150) » qui consigne la résolution sans
  effacer l'historique (l'ADR reste par ailleurs inchangé dans sa Décision/Alternatives/
  Conséquences — seule l'annexe de statut est mise à jour).
- `docs/API.md` — exemple de `orpDoseBlockedReason` mis à jour sur le nouveau format.
  `docs/MQTT.md` — vérifié : le type `orp_ph_inhibit` décrit les messages **journal/MQTT**
  (`String(ph,2)`/`String(phRaw,2)`, horodatés), qui n'ont pas changé — rien à corriger là.
- `CHANGELOG.md` — nouvelle section `[2.65.1]`, domaine **Firmware** + **Documentation**.
- `src/version.h` bumpé **2.65.0 → 2.65.1** : ⚠️ **le déploiement devra inclure le firmware**
  pour que l'appareil annonce la version réelle.
- `docs/user/src/guide-regulation.html` — **évalué, aucune modification nécessaire** : les
  occurrences de `7,60` et `8,10` qui y figurent décrivent les seuils eux-mêmes en prose
  (nombres physiques), pas le texte littéral d'un message applicatif — le format de décimales
  interne à `setGuardRefusalCause()` ne s'y reflète pas. Aucune capture à refaire.

**Clôture** : tous les critères d'acceptation et la check-list de livrables sont cochés. La spec
**n'est pas déplacée vers `done/`** : les modifications (`src/`, `test/`, cette spec, l'ADR, les
docs transverses) sont encore **non commitées** — la règle de clôture exige des commits
référencés, absents à ce stade. À déplacer dès que le commit couvrant ce lot est créé.
