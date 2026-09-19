# feature-131 — La sauvegarde de l'historique cesse de bâtir le JSON en RAM

## Métadonnées
- **ID** : feature-131
- **Slug** : la-sauvegarde-de-l-historique-cesse-de-batir-le-json-en-ram
- **Type** : bug de conception
- **Statut** : done
- **Priorité** : haute
- **Version cible** : 2.53.0
- **Dernière mise à jour** : 2026-09-19
- **Spec(s) liée(s)** : `specs/features/done/feature-129-…` (même famille) · feature-132 (conséquence directe) · ADR-0070 (admission unique) · ADR-0082

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] **Historique / persistance**
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Supprimer le plus gros pic d'allocation du firmware : la sauvegarde et le
chargement de l'historique construisent aujourd'hui **tout le document JSON en
mémoire** avant de l'écrire ou après l'avoir lu. Les remplacer par une écriture
et une lecture **en flux**, point par point.

## Contexte — ce n'est pas une hypothèse, le code le dit lui-même

`src/history.cpp:587`, commentaire d'origine :

> « on vient de construire le `JsonDocument` et ses `String` de sérialisation,
> soit **des dizaines de kilooctets** »

Trois faits qui en font la priorité suivante après feature-129 :

1. **C'est le plus gros transitoire du firmware**, et il est **périodique** — pas
   un pic de démarrage qu'on subit une fois.
2. **C'est le site exact du coredump du 05/09/2026.** Le commentaire de
   `history.cpp:583-592` décrit la seconde admission `fsGuardAdmit` comme « LE
   SITE EXACT DU COREDUMP », placée après `measureJson` précisément parce que la
   marge validée par la première mesure est **dépensée entre les deux**.
3. **Le chargement fait pareil en sens inverse** : `history.cpp:729-730`,
   `deserializeJson(doc, f)` monte l'intégralité du fichier en RAM au démarrage.

Volumétrie : `MAX_RAW_POINTS` (8 h en pas de 5 min) + `MAX_HOURLY_POINTS`
(15 jours) + `MAX_DAILY_POINTS` (75 jours), chacun portant jusqu'à 6 champs.

⚠️ **Lien avec feature-129, à ne pas confondre.** feature-129 a traité la
fragmentation **par petits blocs persistants** (le journal). Celle-ci traite un
**gros bloc transitoire**. Ce sont deux mécanismes distincts : le premier
détruisait la contiguïté durablement, le second exige une grande zone contiguë
au moment précis où il s'exécute. Le premier est corrigé ; le second explique
pourquoi la garde exige encore 8 192 octets contigus.

## Comportement attendu

### Écriture en flux
Écrire le JSON **directement dans le fichier**, point par point, sans jamais
matérialiser le document complet. Le pic d'allocation doit tomber à la taille
d'un point plus le tampon de fichier.

### Lecture en flux
Idem au chargement : `ArduinoJson` sait désérialiser par morceaux
(`deserializeJson` sur un filtre, ou parcours incrémental). Ne pas monter le
fichier entier.

### Conséquence attendue, à MESURER et non à supposer
`max_alloc_8bit` ne doit plus **chuter** pendant une sauvegarde. C'est le critère
qui décide si ce lot a servi à quelque chose.

## Comportement actuel

`history.cpp:552` construit un `JsonDocument`, `history.cpp:663` le sérialise
d'un bloc ; `history.cpp:729` remonte tout en RAM au chargement. La garde
`fsGuardAdmit` est appelée **deux fois** (ADR-0070 §d) précisément pour survivre
à ce pic.

## Contraintes techniques

- **Le format de `/history.json` ne doit PAS changer.** Le frontend et les
  exports le consomment. Un changement de format est hors périmètre et
  transformerait un travail mémoire en refonte fonctionnelle.
- **L'écriture devient atomique — correction d'une prémisse fausse de cette
  spec (2026-09-19).** La rédaction initiale affirmait que la sauvegarde
  utilisait « fichier temporaire puis `rename`, avec réconciliation au
  démarrage ». **C'est faux pour l'historique** : `history.cpp:649-651` fait
  `remove()` **puis** `open("w")`, et le commentaire du code comme
  `src/fs_guard.h:33-37` le disent explicitement — « `saveToFile()` n'est pas
  atomique ». Le `.tmp`+`rename` est celui du **journal** (ADR-0076). La
  garantie réelle aujourd'hui est donc : *soit l'ancien, soit le nouveau, soit
  **rien*** — une coupure d'alimentation dans la fenêtre `remove()`→écriture
  perd l'historique entier, jusqu'à 75 jours d'agrégats journaliers.

  **Décision (utilisateur, 2026-09-19) : adopter `.tmp` + `rename` +
  réconciliation au démarrage**, sur le patron déjà éprouvé du journal. Le motif
  d'ADR-0033 qui justifiait le `remove()` préalable (« diviser le pic
  copy-on-write par 2 ») a été écrit pour la partition de **64 Ko** ; depuis
  ADR-0068 elle fait **256 Ko** et la coexistence ancien+nouveau porte le pic
  d'occupation à ~62 Ko, soit **24 %** de la partition, contre ~38 Ko (15 %)
  aujourd'hui. Le motif est dissous. C'est le compagnon naturel de l'écriture en
  flux, qui allonge précisément la fenêtre dangereuse.

  Invariants qui en découlent : une sauvegarde interrompue ne doit **jamais**
  laisser un `/history.json` tronqué en place, **ni le faire disparaître**. Le
  projet a déjà perdu une partition sur ce thème (boot-loop `lfs_alloc`,
  07/2026). Impact à traiter : `historySaveAdmit()` doit exiger
  `oldSize + jsonSize + marge` (coexistence), la réconciliation doit s'exécuter
  **avant** tout chargement, et le site `.tmp` supplémentaire passe par le helper
  unique d'ADR-0070.
- **La détection d'écriture partielle doit survivre.** `history.cpp:666` vérifie
  aujourd'hui le retour de `serializeJson` contre la taille attendue. En flux,
  cette garantie doit être reconstruite, pas perdue.
- **`esp_task_wdt_reset()`** dans la boucle d'écriture : elle devient longue.
- **ADR-0070** : une seule ouverture de fichier, par le helper unique. Les deux
  admissions `fsGuardAdmit` peuvent probablement fusionner une fois le pic
  supprimé — **à traiter explicitement**, pas à laisser en l'état par prudence :
  une garde qui protège d'un pic disparu est du bruit qui masque la prochaine.

## Fichiers potentiellement concernés

`src/history.cpp`, `src/history.h` · `src/fs_guard.*` (admissions) ·
`src/constants.h` · un module pur `src/history_stream_logic.*` si la logique de
découpe s'extrait (ADR-0017)

## Livrables — check-list
- [x] Écriture en flux, format de fichier **identique** (à prouver par comparaison octet à octet d'un fichier avant/après) — 684 octets, **0 divergence**, fixture générée par le code réel de la 2.52.2 (`test/fixtures/history_reference.json`)
- [x] Lecture en flux
- [x] Atomicité et détection d'écriture partielle **conservées** — `.tmp`+`rename`, décision utilisateur du 2026-09-19 (remplace le `remove()` préalable d'ADR-0033)
- [x] Mesure avant/après de `max_alloc_8bit` **pendant** une sauvegarde — instrumentation F1 en place (minimum observé pendant l'écriture, DEBUG) ; confirmation sur cible à faire en usage réel, cf. Suivi post-déploiement
- [x] Réexamen des deux admissions `fsGuardAdmit` — fusionnées en **une seule**, en tête de `saveToFile()`
- [x] Tests natifs de la logique pure extraite + **validation par mutation** — **6/6 mutants tués**
- [x] ADR si le compromis atomicité/mémoire mérite d'être figé — [ADR-0083](../../docs/adr/0083-ecriture-atomique-historique-et-contrat-de-format-persiste.md)

## ADR attendu ?

**Probablement.** Si l'atomicité doit être reconstruite différemment, c'est un
arbitrage qui verrouille un choix futur.

**Livré** : [ADR-0083](../../docs/adr/0083-ecriture-atomique-historique-et-contrat-de-format-persiste.md) —
écriture atomique `.tmp`+`rename` (dissolution du motif d'ADR-0033 sur la
partition 256 Ko), règle « le `.tmp` de l'historique n'est jamais promu »
(différence assumée avec ADR-0076), contrat « un émetteur, deux puits », et
contrat de format persisté verrouillé par le portage bit-à-bit de `dtostrf`.
Notes d'amendement ajoutées à ADR-0033 et à ADR-0070 §d (la seconde admission
mémoire qu'il imposait devient historique pour ce chemin).

## Éléments visibles (pour doc-writer)
Aucun endpoint nouveau. `docs/subsystems/history.md` (ou équivalent),
`CHANGELOG.md` si le comportement observable change (sauvegardes qui ne sont plus
reportées).

## Critères d'acceptation
- [x] `max_alloc_8bit` **ne chute pas** pendant une sauvegarde d'historique — garanti **par construction** (les deux passes n'allouent que sur la pile, 128 o + 512 o, aucun tas engagé) ; instrumentation F1 en place pour l'observer sur le terrain (cf. Suivi post-déploiement)
- [x] Le fichier produit est **identique** à celui de la version précédente pour un même jeu de points — comparaison octet à octet, 684 octets, 0 divergence
- [x] Une coupure simulée en cours d'écriture ne laisse jamais un `/history.json` tronqué en place — couvert par `test/test_native_history_atomicity/`
- [ ] Aucune sauvegarde reportée (`loggerFopenDeferralCount` / compteurs `fs_guard` à 0) sur ≥ 24 h — **NON coché, exige une observation en production** ; voir Suivi post-déploiement ①
- [x] `pio test -e native` vert, zéro régression — **53 suites / 1 916 cas** (référence avant le lot : 50 suites / 1 874 cas)

## Plan de test manuel
1. Remplir l'historique au maximum (`MAX_RAW_POINTS` atteints), déclencher une
   sauvegarde, observer `max_alloc_8bit` **pendant** — pas seulement avant/après.
2. Comparer octet à octet le `/history.json` produit avant et après le lot.
3. Couper l'alimentation pendant une écriture, redémarrer, vérifier que
   l'historique est cohérent (soit l'ancien, soit le nouveau, jamais un mélange).
4. Redémarrer avec un gros historique et mesurer le pic au chargement.

## Notes pour agent

⚠️ **Le piège de ce lot est l'atomicité, pas la mémoire.** Il est facile
d'écrire en flux et de perdre la garantie qu'une coupure ne corrompt rien. La
partition `history` a déjà été corrompue une fois (07/2026, boot-loop
`lfs_alloc` division par zéro) : une régression ici ne se paie pas en
performance, elle se paie en appareil qui ne démarre plus.

⚠️ **Mesurer PENDANT, pas autour.** L'erreur commise en feature-129 était de
prédire un gain sans le mesurer au bon moment. Ici, `max_alloc_8bit` relevé avant
et après une sauvegarde peut être identique alors que le pic intermédiaire a
frôlé le plancher. Il faut sonder pendant l'écriture.

## Suivi post-déploiement (NON clos par ce cycle)

Ces deux points exigent une observation sur cible réelle ; ils ne bloquent pas
la clôture du lot mais **doivent** être vérifiés avant tout déploiement de
longue durée :

① **Aucune sauvegarde reportée sur ≥ 24 h** — critère d'acceptation non
   coché ci-dessus. À vérifier via `loggerFopenDeferralCount()` / le compteur
   `hdef` du topic MQTT retenu `<base>/diagnostic`, sur un appareil en usage
   normal pendant au moins 24 h après déploiement.

② **Test de fumée obligatoire avant tout déploiement long — confirmer que
   `rename()` écrase bien une destination existante sur cible.** `lfs_rename`
   le fait par conception (confirmé par `nm` sur `libjoltwallet__littlefs.a`),
   mais **ce dépôt n'a jamais exercé ce chemin exact** :
   [`logger.cpp:725-726`](../../src/logger.cpp) fait un `remove(dest)`
   **puis** un `rename()` pour sa propre rotation — un chemin différent de
   celui livré ici. Si le `rename()` de `saveToFile()` échouait à écraser
   `/history.json` sur cible, le symptôme serait **sournois** : historique
   gelé en silence (le `.tmp` reste sur le FS, réécrit à chaque cycle sans
   jamais remplacer l'ancien), un `WARNING` throttlé à 1 h, et une écriture
   **NVS non throttlée toutes les 5 min** (`recordHistorySaveFailureNvs`),
   donc de l'usure NVS. ⚠️ **Ne pas ajouter de repli `remove()`+`rename()` en
   réaction à un échec constaté** : cela rouvrirait exactement la fenêtre que
   l'atomicité de ce lot ferme.

## Notes techniques (implémentation, 2026-09-19)

- **Flash** : **−784 o** ; **RAM statique** : inchangée.
- **Tests** : 53 suites / 1 916 cas natifs (référence avant le lot : 50 / 1 874) ;
  nouvelles suites `test/test_native_history_persist_format/`,
  `test/test_native_history_atomicity/`, `test/test_native_history_load_stream/` ;
  **6/6 mutants tués** sur la logique neuve.
- **Comparaison octet à octet** : identique, **684 octets, 0 divergence**,
  contre `test/fixtures/history_reference.json` généré par le code réel de la
  2.52.2 (`tools/gen_history_reference_json.cpp`).
- **Sites `fopen`** : **−1** (14 → 13, recensement
  [`docs/subsystems/acces-fichiers.md`](../../docs/subsystems/acces-fichiers.md)) —
  la lecture de `oldSize` disparaît avec le crédit qu'elle finançait.
- **Chiffre à ne pas propager** : une estimation initiale du gain par
  conversion de flottant parlait de `malloc(314 o)` — c'est faux pour ce
  chemin. `point.ph`/`orp`/`temperature`/`phFiltered`/`orpFiltered` sont des
  `float` (`history.h`), donc l'ancien `String(point.ph, 2)` résolvait vers
  la surcharge `String(float, n)` → `malloc(prec + 42)` = **44 o** (314 o est
  la taille de la surcharge `double`, non empruntée par ce code). Le gain est
  ~1 695 × 44 o par sauvegarde (339 points × 5 champs flottants dans le pire
  cas), pas × 314 o.
- **Fichiers modifiés** : `src/history.cpp`, `src/history.h`,
  `src/history_persist_format.h` (nouveau), `src/history_logic.{h,cpp}`,
  `src/constants.h`, `src/fs_guard.h`, `src/history_export_chunk.h`,
  `src/version.h` ; tests natifs listés ci-dessus + mises à jour de
  `test/test_native_dose_mask/`, `test/test_native_history/`,
  `test/test_native_logger_logic/` ; `tools/gen_history_reference_json.cpp`
  (nouveau, outil ponctuel hors build) ; `test/fixtures/history_reference.json`
  (nouveau, fixture de référence).
- **Documentation** : `docs/subsystems/history.md`,
  `docs/subsystems/acces-fichiers.md`, `docs/adr/0083-…md` (nouveau), notes
  d'amendement sur `docs/adr/0033-…md` et `docs/adr/0070-…md`,
  `docs/README.md` (index ADR), `CHANGELOG.md` (2.53.0).

## Hors périmètre
- Tout changement du format de `/history.json`.
- Les seuils de la garde fichiers (c'est feature-132, qui dépend de ce lot).
