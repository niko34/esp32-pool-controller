# Bug + UX — Agrégat journalier fantôme (points futurs) et lisibilité des courbes

- **Type** : bug (firmware historique) + amélioration UX (frontend) — format minimal
- **Statut** : done (2026-08-10 — revue Approved, builds bi-core + 441 tests + build_fs verts ; validation on-device : log de purge du g2 + tooltips/trous à contrôler au premier flash)
- **Version cible** : 2.18.4
- **Créée** : 2026-08-10
- **Domaine** : Historique (`src/history*`) + Frontend (`data/app.js`) — aucun chemin de dosage
- **Spec liée** : bug-history-doublons-troncature (la garde anti-futur y avait été
  proposée en revue puis écartée du périmètre — elle vient de prouver sa nécessité)

## Anomalie constatée (production, 2026-08-10)

`/get-history` contient un point **granularité 2 (JOURNALIER)** daté du créneau
du 10/08 00:00 UTC — doublement impossible : l'affamage documenté empêche la
formation des journaliers, et un journalier daté du jour COURANT ne devrait
naître que 15 jours plus tard. Mécanisme retenu : des points horodatés dans le
FUTUR (excursion d'horloge pendant les reboots des essais 8-9/08) + wrap uint32
de `isOlderThan` (un point futur paraît vieux de ~136 ans) → consolidation
immédiate HOURLY→DAILY avec bucket futur. Le point fantôme pollue le bord droit
des courbes (l'un des deux « 02:00 » constatés).

## Correctifs firmware

1. **Garde anti-futur** (`history_logic`, pure + tests) :
   `historyIsExpired(now, ts, maxAge)` = `ts <= now && isOlderThan(...)` —
   utilisée par la purge 90 j ; `historyBucketConsolidable` durcie de même
   (dernière seconde du bucket dans le futur → jamais consolidable).
   L'invariant de wrap d'`isOlderThan` lui-même reste inchangé (documenté,
   verrouillé par tests).
2. **Assainissement au chargement** : purge des points `ts > now + 
   kHistoryFutureToleranceSeconds` (3600 s) quand l'horloge est disponible
   (RTC), avec log INFO du nombre purgé — le g2 fantôme disparaît au premier
   boot.

## Améliorations frontend (demandes utilisateur du 09-10/08)

3. **Tooltip complet** sur tous les graphiques : date + heure TOUJOURS (plus
   d'heure seule pour « aujourd'hui ») + type de point (« mesure », « moyenne
   horaire », « moyenne journalière ») — lève l'ambiguïté des étiquettes.
4. **Mini-graphiques de l'accueil sur 24 h** (au lieu de 3 j) ; écrans
   détaillés inchangés.
5. **Coupure de courbe visible** quand deux points consécutifs sont espacés de
   plus de 90 min (null injecté → uPlot lève le crayon) — les arrêts de
   filtration se voient ; appliqué aux mini-graphiques ET aux écrans détaillés,
   y compris le chemin incrémental (?since=).

## Critères d'acceptation

- [x] Tests natifs : 5 nouveaux (441 au total), dont LE cas du bug (wrap dit
      « vieux », la garde dit « non expiré ») ; suites existantes intactes
- [ ] Au boot sur l'appareil : log de purge du point futur, g2 fantôme absent
      de /get-history
- [x] Tooltip : date+heure+type sur mini et détail ; mini = 24 h ; trous
      > 90 min (mini) / jour manquant (détail), y compris raccord incrémental
- [x] node --check + build_fs + builds bi-core verts ; revue Approved
      (note mineure consignée : faux trou théorique possible au raccord
      horaire→brut, cosmétique)

## Hors périmètre
- L'affamage DAILY structurel (7 j < 15 j) — spec future dédiée (inchangé)
- La purge des journaliers légitimes éventuels (aucun ne peut exister à ce jour)

## Ajout de dernière minute (même lot, 2026-08-10)
- Item 6 — Widget Éclairage de l'accueil : la ligne « Horaires » est masquée
  quand la programmation est désactivée (demande utilisateur du 10/08 — des
  horaires inertes contredisaient le mode affiché). data/app.js, ~8 lignes.
