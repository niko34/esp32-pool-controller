## Métadonnées
- **ID** : bug-106
- **Slug** : journee-archivee-a-zero-sur-compteur-en-recul
- **Type** : bug (indicateur informatif — aucune incidence sur le dosage)
- **Statut** : doing (2026-09-07)
- **Priorité** : basse
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-09-06
- **Spec(s) liée(s)** : `feature-105` (c'est en la traitant que le défaut a été trouvé)

## Domaines touchés
- [ ] Régulation — **explicitement non** : `pump_controller.cpp` n'inclut pas
      `time_to_target*`, le flux est à sens unique et en lecture seule
      (constat `pool-chemistry`, 2026-09-06). Aucune incidence sur le dosage.
- [x] Historique / persistance (valeur archivée)
- [x] Web — la valeur est affichée

## Agents requis
- [x] spec-reader — [x] architect — [x] test-engineer — [x] code-reviewer — [x] doc-writer
- [ ] pool-chemistry — indicateur informatif, aucun chemin de dosage

## Description
Quand le compteur de filtration recule sous `t0FiltMin`, la journée s'archive avec
`minutes = 0` — valeur **indistinguable** d'un « déjà à la cible au départ ».

## Contexte
Trouvé par `test-engineer` pendant `feature-105`, en écrivant le test de la branche
« compteur en recul » (`time_to_target_logic.cpp:288-290`). Le défaut est **voisin** de
cette branche, pas dedans : lignes **295-297**.

Le ternaire `holdStartFiltMin >= t0FiltMin` protège correctement du sous-débordement en
arithmétique non signée — mais il rend **0**, qui est déjà une valeur légitime de
l'indicateur (« la cible était atteinte dès le départ »). Les deux situations deviennent
donc indiscernables une fois la journée archivée.

Le compteur recule lors d'un effacement NVS `pool-daily`, d'une restauration d'usine ou
d'une restauration de configuration en cours de tenue.

⚠️ **Aucune assertion n'a été posée sur ce comportement** dans les tests de `feature-105`,
délibérément : poser un test dessus l'aurait **entériné**. La branche voisine, elle, est
testée et son mutant est tué.

## Comportement attendu
Une journée dont le compteur a reculé doit être **distinguable** d'une journée atteinte
d'emblée. Trois pistes, à arbitrer par `architect` :
1. Une valeur sentinelle d'archivage (`0xFFFF` ou équivalent) signifiant « mesure
   invalidée par un recul de compteur », avec le rendu correspondant côté UI.
2. Un drapeau distinct dans l'enregistrement archivé.
3. Ne pas archiver la journée du tout.

## Comportement actuel
`minutes = 0`, silencieusement.

## Fichiers potentiellement concernés
- `src/time_to_target_logic.cpp` (l. 295-297) — la logique pure, testable nativement
- `src/time_to_target_logic.h` — si le type archivé change
- `data/app.js` + `docs/features/page-*.md` — si le rendu change
- `test/test_native_time_to_target/` — le test qui manque

## Critères d'acceptation
- [ ] Une journée invalidée par un recul de compteur est distinguable d'une journée
      atteinte à `t=0`, dans la donnée archivée comme à l'affichage
- [ ] Test natif couvrant les deux cas, **validé par mutation**
- [ ] Aucune régression sur la base courante : **38 suites / 1 418 cas** (le « 1291 »
      d'origine était périmé — mesuré le 2026-09-07, v2.46.2)
- [ ] Aucun impact sur `pump_controller` ni sur aucune limite de dosage

## Hors périmètre
- Les **demi-branches défensives** relevées par `feature-105` et laissées découvertes
  (`pulse_dosing_logic` 52, 79, 98, 109, 143-144 ; `time_to_target_logic` 149, 208, 239,
  241, 274, 300 ; `measure_validity_logic` 34, 83, 87). Elles n'ont pas de rapport avec ce
  défaut.
