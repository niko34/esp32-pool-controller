## Métadonnées
- **ID** : bug-106
- **Slug** : journee-archivee-a-zero-sur-compteur-en-recul
- **Type** : bug (indicateur informatif — aucune incidence sur le dosage)
- **Statut** : **livré — tests verts, revue OK** (2026-09-07)
- **Priorité** : basse
- **Version cible** : **2.47.0** (livrée)
- **Dernière mise à jour** : 2026-09-07
- **ADR produits** : [ADR-0072](../../../docs/adr/0072-absence-de-valeur-hors-du-domaine.md), [ADR-0073](../../../docs/adr/0073-chiffre-consigne-porte-sa-methode.md) — conjoints avec `bug-098`
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
- [x] **Une journée invalidée par un recul de compteur est distinguable d'une journée
      atteinte à `t=0`, dans la donnée archivée comme à l'affichage** — piste 2
      retenue (drapeau distinct), `kTttFlagCounterReset = 0x20` dans `TttDay::flags`.
      Archivé en NVS `pool-kpi` et servi par `/get-kpi`. À l'affichage : pointillé
      estompé, **aucune durée**, phrase « durée non mesurable » — et **non** la classe
      des bonnes journées. Le bit ajoute par ailleurs l'astérisque, les hachures et
      la cause « compteur de filtration remis à zéro » dans la note sous la bande.
- [x] **Test natif couvrant les deux cas, validé par mutation** — les deux
      situations, jusqu'ici indiscernables, sont séparées par assertion : recul du
      compteur ⇒ `minutes == 0` **et** `flags & 0x20` ; cible atteinte d'emblée ⇒
      `minutes == 0` **et** `flags & 0x20 == 0`. Mutants tués aux **trois** sites de
      pose (retirer le `|=` de l'un quelconque des trois tue un test). Couverture
      **lignes et fonctions 100 %** sur `time_to_target_logic`.
- [x] **Aucune régression sur la base courante** — base **39 suites / 1 451 cas**
      à la clôture (38 / 1 418 à l'ouverture + `test_native_dose_mask` et les cas
      ajoutés aux trois suites touchées). Mesuré le 2026-09-07 par
      `ls -d test/test_native_* | wc -l` et `grep -rn 'RUN_TEST(' test/ | wc -l`.
      ⚠️ `CLAUDE.md` annonce encore « 33 suites / 1 291 cas » : **à recaler**
      (motif de l'[ADR-0073](../../../docs/adr/0073-chiffre-consigne-porte-sa-methode.md) — une fausse base de non-régression lue par
      chaque session).
- [x] **Aucun impact sur `pump_controller` ni sur aucune limite de dosage** —
      confirmé : `pump_controller.cpp` n'inclut pas `time_to_target*`, le flux reste
      à sens unique et en lecture seule. Aucun chemin de dosage, aucun budget
      horaire ou journalier ne lit ces drapeaux.

## Notes techniques de clôture (2026-09-07, v2.47.0)

### Pourquoi une sentinelle dans `minutes` a été écartée

La piste 1 (`0xFFFF`) aurait **recommis, dans le champ voisin, la faute que
`bug-098` corrige** : le domaine d'un `uint16_t` de minutes redevenait porteur
d'une information qui n'est pas une durée. Le champ `flags` existait déjà, à trois
octets de là, précisément pour ça. La piste 3 (ne pas archiver) échouait pour une
autre raison : `tttHistoryCloseOrphans()` rouvre la case le lendemain et en fait
une « non atteinte à 0 minute » — le mensonge revenait par un autre chemin, **sans
trace de sa cause**. Formalisé par l'[ADR-0072](../../../docs/adr/0072-absence-de-valeur-hors-du-domaine.md).

### Le troisième site, celui que la spec ne nommait pas

La spec pointait `time_to_target_logic.cpp:295-297` — l'**archivage**. Le drapeau
est en réalité posé à **trois** sites, et le plus important n'était pas celui-là :

| Site | Ligne | Chemin |
|---|---:|---|
| `tttUpdateElapsedMinutes()` | **128** | le chemin **VIF**, celui qui alimente l'instantané servi à l'UI |
| `tttTick()` — tenue relancée | **307** | recul **entre `t0` et le début de tenue** : `tttUpdateElapsedMinutes` ne le voit pas (`filtTotalMin >= t0FiltMin`), et c'est pourtant le même événement |
| `tttTick()` — archivage | **326** | le site nommé par la spec |

### Une forme validée comme précédent

`tttUpdateElapsedMinutes(TttLive& st, uint32_t)` prend son entrée par **référence
non const** et la **mute**. La revue a retenu la forme : ce module est une
**machine à états** dont tout le contrat est de muter `TttLive&` ; l'ADR-0017 porte
sur l'absence de dépendance **matérielle**, pas sur l'immutabilité ; et rendre le
drapeau à l'appelant aurait déplacé sa pose à **trois sites dont un dans une autre
unité de compilation** (`time_to_target.cpp`, reprise après coupure) — l'oubli même
qui a produit ce bug.

⚠️ **Condition de copie**, inscrite dans l'en-tête et dans l'[ADR-0072](../../../docs/adr/0072-absence-de-valeur-hors-du-domaine.md) : l'effet de
bord doit rester un **OR IDEMPOTENT**. La fonction tourne jusqu'à **1×/s/voie** et
plusieurs fois par tick à la clôture — un `++`, un horodatage ou toute écriture non
idempotente casserait la forme **sans faire rougir un test**, seulement une valeur
qui dérive avec la fréquence d'appel.

### Réserve OUVERTE — trou résiduel, non corrigé

Le bit vit dans `TttLive` (RAM RTC) et n'atteint la NVS qu'à l'**archivage** de la
journée. Un recul **suivi d'une remontée** du compteur au-dessus de `t0`, **avec
une coupure secteur entre les deux**, perd donc le bit : la journée se rouvre depuis
l'anneau sans lui. **Atténuation** : cette même coupure pose
`kTttFlagPowerCycle` — la journée reste signalée, mais **pour le mauvais motif**.
**Non corrigé**, motif : le remède (une écriture NVS à chaque recul) coûterait de
l'**usure de flash** pour un scénario à **trois conditions simultanées**.

### Fichiers modifiés

`src/time_to_target_logic.h`, `src/time_to_target_logic.cpp`,
`src/time_to_target.cpp`, `data/app.js`,
`test/test_native_time_to_target/test_time_to_target.cpp`.

### Documentation

`docs/adr/0072-absence-de-valeur-hors-du-domaine.md` (créé),
`docs/adr/0073-chiffre-consigne-porte-sa-methode.md` (créé), `docs/adr/README.md`,
`docs/README.md`, `docs/subsystems/time-to-target.md`, `docs/features/page-ph.md`,
`docs/features/page-orp.md`, `docs/user/src/guide-regulation.html`, `CHANGELOG.md`.
Aucun PDF régénéré ; aucune capture d'écran affectée par ce bug (le cas ne se
produit pas sur les captures existantes — la pastille concernée n'apparaît qu'après
une remise à zéro du compteur).

## Hors périmètre
- Les **demi-branches défensives** relevées par `feature-105` et laissées découvertes
  (`pulse_dosing_logic` 52, 79, 98, 109, 143-144 ; `time_to_target_logic` 149, 208, 239,
  241, 274, 300 ; `measure_validity_logic` 34, 83, 87). Elles n'ont pas de rapport avec ce
  défaut.
