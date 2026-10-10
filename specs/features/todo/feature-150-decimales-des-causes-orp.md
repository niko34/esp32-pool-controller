## Métadonnées
- **ID** : feature-150
- **Slug** : decimales-des-causes-orp
- **Type** : bug
- **Statut** : todo
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
- [ ] A1 — Verdict `pool-chemistry` **d'abord** sur les deux libellés. Rien ne se code avant.
- [ ] A2 — `, 2)` → `, 1)` aux deux sites.
- [ ] A3 — `kPhOrpInhibit` ajouté au `static_assert` de rondeur.
- [ ] D1 — T3 étendu : les causes **ORP** citant un seuil pH doivent être en `, 1)`.
- [ ] D2 — **Mutation** : repasser un site à `, 2)` ⇒ un test doit mourir. Et vérifier que les
      **bandes d'hystérésis** restent protégées (les passer à `,1)` doit toujours tuer).
- [ ] C1 — ADR-0095 : **retirer** la mention « incohérence connue, non traitée ».
- [ ] C2 — `CHANGELOG.md`.

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
- [ ] Les deux causes ORP citent leurs seuils pH à une décimale
- [ ] La même constante s'affiche **de façon identique** dans une cause pH et dans une cause ORP
- [ ] Les bandes d'hystérésis restent à **deux** décimales (vérifié par mutation)
- [ ] `kPhOrpInhibit` est couvert par le `static_assert`
- [ ] `pio run` sans warning nouveau · `pio test -e native` zéro régression

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
