# feature-144 — Le détecteur de chute de contiguïté est éteint depuis juillet

## Métadonnées
- **ID** : feature-144
- **Slug** : detecteur-de-chute-de-contiguite-remis-en-service
- **Type** : feature — **observabilité**, remise en service d'un instrument désactivé
- **Statut** : implémenté — tests verts, revue OK, documentation faite (doc-writer, 2026-10-08). Reste la fenêtre d'observation manuelle post-déploiement (voir « Plan de test manuel »). Laissée dans `doing/` par consigne explicite du doc-writer (cf. note de clôture en fin de fichier).
- **Priorité** : **haute** — pas pour le risque immédiat (la marge est confortable), mais parce
  que **sans cet instrument, le prochain épisode sera invisible lui aussi**. C'est une spec qui
  rend les autres possibles.
- **Version cible** : 2.60.0
- **Dernière mise à jour** : 2026-10-08
- **Spec(s) liée(s)** : `feature-125` (fragmentation, rouverte le 10/09) · `feature-133`
  (« identifier le second mécanisme de fragmentation ») · `specs/features/done/bug-080-…`
  (métrique 8 bits) · ADR-0058

## Domaines touchés
- [ ] Capteurs · [ ] Régulation · [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant · [ ] UART · [ ] OTA · [ ] Authentification
- [x] **Historique / persistance** — le journal système, sa cadence et son volume
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — **non requis** : aucun chemin de dosage, aucune limite touchée
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Le bloc de `src/main.cpp:1231-1241` qui émet une trace dès que le plus gros bloc contigu chute
de plus de `kHeapLogDropBytes` est **intégralement commenté** depuis le **2026-07-23**. Les deux
constantes `kHeapLogIntervalMs` et `kHeapLogDropBytes` (`constants.h:340-341`) ne sont plus
référencées **que dans des commentaires** : elles sont mortes.

Conséquence mesurée le 2026-10-08 : une chute de **18 432 octets (−41 %)** n'a produit **aucune
trace**. Elle n'a été découverte qu'au relevé périodique suivant, **trente minutes plus tard**,
et son déclencheur reste **inconnu** faute d'horodatage.

## Contexte — l'incident du 2026-10-08

### Ce qui s'est passé
| Heure | `libre8` | **`maxBloc8`** |
|---|---|---|
| 11:56 | 56 900 | **45 044** |
| 12:26 | 57 048 | **45 044** |
| **12:56** | 56 488 | **26 612** ⬅ |
| 13:26 | 53 500 | 26 612 (`ws=1`) |
| 14:56 | 56 368 | 26 612 |

La mémoire **libre** n'a pas bougé. C'est la signature exacte que le commentaire du code —
celui-là même qui est commenté — annonce :

> *« Voir la fragmentation : maxBloc s'effondre pendant que libre reste correct. »*

### Pourquoi l'instrument avait été éteint, et pourquoi c'était défendable
Le commentaire est honnête : *« Trace INFO périodique désactivée (2026-07-23, demande
utilisateur) »*. Le bloc émettait **toutes les `kHeapLogIntervalMs` = 5 minutes**, soit
~288 lignes par jour. Sur une partition `history` de 256 Ko, ce bruit chassait le reste du
journal. La demande était légitime.

**Mais le détecteur d'ÉVÉNEMENT est parti avec le bruit.** Or les deux déclencheurs étaient dans
le même `if` : la cadence périodique **et** la chute. Désactiver le bloc a supprimé les deux.

### Ce qui a été écarté EXPÉRIMENTALEMENT le 2026-10-08
Trois candidats testés **sur l'appareil de production**, avec l'accord de l'utilisateur, par
recensement du tas (`POST` puis `GET /debug/heap_census?kind=free`) avant et après :

| Test | Plus gros bloc |
|---|---|
| Référence | 27 320 |
| **4 bascules de filtration en 40 s** (reproduction exacte du geste utilisateur) | **27 320** |
| **Fichiers statiques servis sur connexions neuves** (`/`, `/app.js`, `/app.css`) | **27 320** |
| **Connexion WebSocket neuve** (client non authentifié, 20 s) | **27 320** |

**Delta nul dans les trois cas.** Et le journal porte **une douzaine de bascules de filtration
antérieures** (11:35 → 12:12) alors que `maxBloc8` valait encore 45 044 à 11:56 **et** à 12:26.

⚠️ Détail relevé : la connexion WebSocket a consommé **5 blocs libres** et n'en a rendu que 4
(45 → 40 → 44). Fragmentation réelle mais marginale, cohérente avec ce que `feature-125`
documente de la pile réseau (structures `memp` jamais rendues à l'allocateur).

### L'hypothèse restante, NON prouvée
Une allocation **unique depuis le démarrage**, jamais rendue et jamais répétée : coût de 18 Ko
payé une fois, puis plus rien ne bouge. **Cohérente avec les quatre mesures**, mais invérifiable
depuis le palier — il faudrait mesurer **depuis un démarrage frais**.

## Comportement attendu

**Réactiver le détecteur de CHUTE, et lui seul.** La cadence périodique de 5 minutes reste
désactivée : elle est déjà couverte par la trace « Heap 8bits » (`kHeap8LogIntervalMs` = 30 min,
`main.cpp:1271`), qui elle est **vivante** et suffit à la tendance.

Le gain est donc **gratuit en volume de journal** : en régime normal, aucune ligne
supplémentaire. Une ligne n'apparaît que lorsqu'une chute réelle survient — c'est-à-dire
exactement quand on en a besoin.

### Ce que la trace doit porter
Au minimum : `maxBloc8` avant et après, l'ampleur de la chute, et `libre8` — parce que c'est
l'écart entre les deux qui **nomme le phénomène** (fragmentation vs consommation).

### ⚠️ LE POINT DUR, découvert au cadrage — la spec initiale était FAUSSE sur ce point

**Retirer la cadence périodique sans rien mettre à la place livre un instrument MORT.**

Dans le bloc d'origine, `lastHeapLogMaxBloc` n'est rafraîchi **qu'à l'émission**, et seule une
**baisse** déclenche. La cadence de 5 minutes jouait donc un **second rôle, non documenté** :
**réarmer la référence vers le haut**.

Sans elle, la référence devient un **cliquet monotone descendant** : le détecteur émet au plus
quelques lignes par démarrage, puis **se tait définitivement**. Une remontée suivie d'une
nouvelle chute depuis un palier haut serait **manquée** — c'est-à-dire précisément le scénario
qu'on cherche à attraper.

➜ La question « faut-il tracer une remontée ? » n'est donc **pas cosmétique** : c'est la
**condition de réarmement du détecteur**. À trancher entre réarmement **silencieux**
(`ref = max(ref, courant)` avec hystérésis) et réarmement **tracé** (ligne de remontée).

⚠️ **C'est le scénario d'échec le plus probable de ce lot** : livrer quelque chose de conforme à
la lettre de la spec et d'inutile en pratique.

### ⚠️ CORRECTION — le budget du journal n'est pas 256 Ko, c'est 8 Ko

La spec initiale raisonnait sur la partition `history` (256 Ko). **Faux.** Le plafond réel est
`Logger::kMaxLogFileBytes = 8192` avec `kRotateKeepBytes = 6144` (`src/logger.h:117-118`),
rétention observée **~25-35 min**. La partition porte surtout `history.json`.

L'argument de la spec s'en trouve **renforcé**, pas affaibli — mais tout chiffrage doit se faire
sur **8 Ko**.

⚠️ **Pire cas aggravant** : sous `kLowHeapWarnBytes`, `checkHeapHealth()` passe à **5 secondes**
(`kHeapCheckFastIntervalMs`, `main.cpp:614`). Un détecteur sans anti-rafale émettrait **720
lignes/h précisément pendant la famine** — c'est-à-dire **au moment où le journal est le seul
témoin**. Précédent documenté : le crash du 15/08/2026.

### ⚠️⚠️ CONTRE-CORRECTION — le sondage ci-dessous était FAUX, l'architect l'a refait

**Ne pas se fier aux percentiles de la section suivante.** Ils ont été produits sur un
échantillon biaisé deux fois, et l'architect a refait la mesure proprement le 2026-10-08 :

| | relevés | transitions | **baisses > 0** | p50 | p90 | max |
|---|---|---|---|---|---|---|
| sondage initial (faux) | « 7 116 » | — | — | 5 120 | 30 720 | **82 944** |
| **mesuré, dédupliqué, segmenté par boot** | **585 uniques** | **546** | **19** | 7 680 | 33 792 | **36 864** |

Deux biais : les 240 fichiers de `/root/pool-captures/` sont des captures **quotidiennes d'un
journal glissant**, donc massivement redondantes (7 116 → **585**) ; et les sauts de démarrage
fabriquaient des baisses **fantômes** (max 82 944 → 36 864).

➜ **`maxBloc8` est un ESCALIER** : 546 transitions, **19 baisses seulement**, 527 plates ou
montantes. **Un seuil absolu de 8 192 ne ramène PAS le bruit** — il produirait 12 lignes sur
11,4 jours.

**Le vrai défaut de 8 192 n'est donc pas le bruit, c'est l'ASYMÉTRIE DE SENSIBILITÉ** : il vaut
**18 % quand il reste 45 Ko** et **80 % quand il n'en reste que 10**. Il est aveugle précisément
dans la bande où le redémarrage d'auto-guérison approche — et il **manque** la chute réelle du
**13/09 à 12:21, 23 540 → 16 372 (−30 %)**, à quatre paliers de `kRestartMaxAllocHeapBytes`.

➜ Règle retenue : **`max(4096, ref >> 2)`** — un quart de la référence, plancher 4 Ko
(= `kRestartMaxAllocHeapBytes`, échelle déjà posée du projet). Simulée sur 35 boots :
**10 lignes / 273,5 h**, soit **0,88 ligne/jour**, et **toutes des chutes de ≥ 30 %**.

*La section ci-dessous est conservée pour mémoire — elle documente une erreur de méthode
(ne pas dédupliquer, ne pas segmenter par boot), pas un résultat.*

### ⚠️ CORRECTION (ERRONÉE — voir la contre-correction ci-dessus) — le seuil de 8 192 o ramènerait le bruit

Instrumentable **hors appareil** : `/root/pool-captures/*.txt` contient **7 116 lignes
« Heap 8bits »** réelles (lecture seule). Sondage indicatif des baisses entre relevés consécutifs
(fichiers et démarrages **mélangés — à refaire par boot**) :

| | p50 | p90 | p99 | max |
|---|---|---|---|---|
| baisse de `maxBloc8` | **5 120** | **30 720** | 55 296 | 82 944 |

➜ **À 8 192, une part importante des variations naturelles franchit le seuil.** Le critère
d'acceptation « zéro ligne supplémentaire en régime normal » **n'est pas acquis** avec la valeur
reprise telle quelle. C'est la matière du livrable A2, et elle est disponible sans toucher à
l'appareil.

À arbitrer : seuil **absolu**, **relatif** (% de la référence), ou **hybride** — sachant qu'un
seuil absolu vaut 18 % à 45 Ko et **80 % à 10 Ko**.

### ⚠️ CORRECTION — `check_memory_discipline.sh` ne protège PAS `main.cpp`

Ses motifs stricts (toute `String`, toute allocation) ne visent que `logger.{h,cpp}` et
`log_arena_logic.{h,cpp}`. Sur le reste de `src/`, il ne ferme que `String tab[N]` et les
conteneurs STL de `String`.

➜ **Une `String` ajoutée dans `checkHeapHealth()` passerait la CI au vert.** L'invariant A6 doit
tenir **par conception** (`snprintf` sur tampon de pile, comme les lignes voisines) et être
vérifié par une **sentinelle de structure** (patron `test/test_native_heap_telemetry_guard/`),
jamais par le seul script.

### Cas limites à trancher
- **Seuil** : `kHeapLogDropBytes = 8192` est-il le bon ? Trop bas ⇒ le bruit revient par une
  autre porte. Trop haut ⇒ on rate les chutes progressives. À instruire sur les données réelles.
- **Anti-rafale** : une chute franchie puis refranchie ne doit pas produire une ligne par tick.
  Le bloc d'origine mémorisait `lastHeapLogMaxBloc` — comportement à conserver et à vérifier.
- **Remontée** : faut-il tracer aussi une **remontée** significative ? Elle distinguerait une
  fragmentation durable d'un creux transitoire. À arbitrer.
- **Budget journal** : plafond réel **8 192 o** (`Logger::kMaxLogFileBytes`), rétention
  ~25-35 min. Chiffrer le pire cas **à la cadence rapprochée de 5 s**.

## Comportement actuel
`src/main.cpp:1231-1241` : bloc entièrement commenté. L'échantillonnage de `maxAllocHeap`
(`main.cpp:1223`) reste **actif** — il alimente l'auto-guérison — mais **rien ne le journalise
sur chute**.

## Contraintes techniques
- ⚠️ **Le chemin de journalisation n'alloue pas** (ADR-0082, `check_memory_discipline.sh`).
  Ironie à ne pas rater : on instrumente la mémoire, l'instrument ne doit pas la consommer.
- L'échantillonnage existant alimente `SelfHealInputs::maxAllocHeap` et la cadence adaptative du
  timer (`lastHeapMaxAllocSample`) : **ne rien casser de ce chemin**.
- Le bloc tourne dans `checkHeapHealth()`, appelé périodiquement : pas de boucle longue, pas
  d'allocation, pas de `String` construite à chaque tick même quand rien n'est émis.
- Ne pas réactiver la cadence périodique de 5 minutes : c'est elle qui avait motivé l'extinction.

## Fichiers potentiellement concernés
- `src/main.cpp` (~1211-1245) — le bloc à remettre en service
- `src/constants.h` (340-341) — `kHeapLogDropBytes` à recalibrer ; `kHeapLogIntervalMs`
  devient-elle morte pour de bon ? Si oui, **la retirer** plutôt que la laisser mentir
- `docs/subsystems/logger.md` ou `docs/subsystems/system-health.md`
- `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Détecteur de chute réactivé, **sans** la cadence périodique de 5 min
- [x] A2 — Seuil `kHeapLogDropBytes` instruit et justifié sur données réelles (pas repris tel quel)
      — remplacé par un seuil **relatif** `max(kHeapDropFloorBytes, ref >> kHeapDropRefShift)`,
      chiffré sur 35 boots réels (contre-correction du 2026-10-08)
- [x] A3 — Anti-rafale vérifié **à la cadence rapprochée de 5 s** : une chute ne produit **pas**
      une ligne par tick. **ET règle de RÉARMEMENT de la référence explicite** (sans elle, le
      détecteur devient un cliquet et se tait après quelques lignes — voir « LE POINT DUR »)
- [x] A4 — La trace porte `maxBloc8` avant/après, l'ampleur, et `libre8` (+ `ctx`, non demandé à
      l'origine mais ajouté — nomme le contexte au moment du relevé)
- [x] A5 — `kHeapLogIntervalMs` : **supprimée** de `constants.h` (définitivement morte),
      remplacée par `kHeapDropMinIntervalMs` (sémantique inversée : plancher, pas plafond)
- [x] A6 — Chemin de journalisation **sans allocation** (ADR-0082) — vérifié par
      `check_memory_discipline.sh`
- [x] D1 — Logique de décision extraite en module **pur** si elle le mérite (ADR-0017) ; sinon
      le dire explicitement — FAIT (`src/heap_drop_logic.h`, déjà extrait avant ce passage de
      test). Suite native `test/test_native_heap_drop_logic/` (15 cas) : franchissement incident
      réel, non-franchissement, frontière additive inclusive, plancher dominant le seuil relatif,
      asymétrie 23540→16372, plateau 1 ligne, cliquet chute/remontée/chute (2 lignes), amplitude
      cumulée sur chute progressive, non-ré-ancrage pendant la suppression, anti-rafale
      adversarial ≤12/h, repliement `millis()`, 1er tick silencieux, constantes épinglées, et un
      **rejeu de données réelles embarquées** (boot du 2026-09-12 15:19:35, captures
      `/root/pool-captures/`, dédupliqué et segmenté par démarrage réel — 3 lignes attendues,
      dont l'incident nommé du 13/09 12:21).
- [x] D2 — **Mutation** : neutraliser la comparaison de chute ⇒ un test doit mourir — FAIT, tue
      10/14 tests.
- [x] D3 — **Mutation** : neutraliser la mise à jour de la référence (anti-rafale) ⇒ un autre
      meurt — FAIT, deux mutations distinctes :
      D3a (ré-ancrage à l'émission neutralisé) tue le test de plateau (nommé par la spec) + le
      test de franchissement + le rejeu réel ; D3b (réarmement silencieux neutralisé) tue
      **exactement** le test du scénario de cliquet nommé par la spec
      (`test_chute_remontee_nouvelle_chute_deux_lignes`). Deux mutations optionnelles
      supplémentaires essayées aussi (seuil absolu au lieu de relatif ; ré-ancrage de `ref`
      pendant la suppression) : toutes deux tuées par les tests existants/dédiés. Les 6 mutations
      ont été **restaurées et vérifiées par `md5sum`** après chaque essai.
- [x] D4 — **Sentinelle de structure** prouvant l'absence de `String` sur le chemin d'émission —
      `check_memory_discipline.sh` ne couvre PAS `main.cpp` pour ce motif — FAIT, 6 tests ajoutés
      à `test/test_native_heap_telemetry_guard/` : (a) zéro `String`/concaténation `+` dans le
      bloc `if (dropResult.emit) { ... }`, (b) tampon `char ligneChute[96]` présent, nom
      distinct du tampon `ligne` historique (ne casse pas
      `test_native_heap_floors::test_the_historic_heap_8bits_line_is_untouched`, qui identifie
      cette ligne par le PREMIER tampon nommé `ligne`), (c) `heapDropEvaluate()` appelée
      EXACTEMENT 1 fois dans tout `main.cpp`, (d) `heapLargestFreeBlock8Bit()` appelée
      EXACTEMENT 1 fois dans `checkHeapHealth()` (aucune mesure de tas supplémentaire), (e) le
      site d'appel passe bien `maxAllocHeap` (mesure fraîche) et jamais
      `lastHeapMaxAllocSample` (voir bug ci-dessous), (f) les lignes « Heap 8bits: » (déjà
      protégée ailleurs) ET « Heap masq boot/fen: » (AUCUNE sentinelle avant ce passage) restent
      mot pour mot identiques. ⚠️ Piège rencontré en écrivant (d)/(e) : `extractFunctionBody(src,
      "void checkHeapHealth()")` matchait la **déclaration anticipée** (`main.cpp:157`, qui se
      termine par `;`) et non la définition (`:1212`), capturant par erreur le corps de
      `setup()` — corrigé en cherchant `"void checkHeapHealth() {"` (avec l'accolade).
- [x] C1 — `docs/subsystems/system-health.md` : nouvelle section « Détecteur de chute de
      contiguïté, remis en service (v2.60.0) » — pourquoi il avait été éteint, pourquoi sa remise
      en service ne ramène pas le bruit, format exact, les trois règles de conception et leur
      motif. Corrigé aussi `system-health.md` (section « Instrumentation du tas », table des
      constantes) et `logger.md` (entrée `Heap: …` au site/date faux, nouvelle entrée
      `Chute maxBloc8: …`).
- [x] C2 — `CHANGELOG.md` : entrée `[2.60.0]` ajoutée, bloc « Ce qui change pour vous » honnête
      (rien de visible, une ligne de journal nouvelle et rare)

## Sécurité chimique
- Dosage impliqué : **non**. Aucun plafond, aucune garde, aucun chemin de `pump_controller`.
- Effet **indirect** : une famine de contiguïté finit par refuser les fichiers de l'interface
  (`kStaticFileMinAllocBytes = 8192`) puis par déclencher un redémarrage d'auto-guérison
  (`kRestartMaxAllocHeapBytes = 4096`). Voir cet effondrement **à l'instant où il se produit**
  sert indirectement la disponibilité de la régulation.

## ADR attendu ?
- **Probable** : non — remise en service d'un mécanisme existant, pas un arbitrage structurant.
- À reconsidérer si le seuil retenu encode un compromis non évident entre bruit et détection.
- **Contredit par l'architect, puis par doc-writer** : un ADR **a été créé** —
  [ADR-0090](../../../docs/adr/0090-detecteur-de-chute-maximum-glissant-auto-rearme.md). Le seuil
  retenu encode exactement le compromis anticipé ci-dessus (relatif vs absolu, chiffré sur 35
  boots), et la thèse « un instrument de tendance et un détecteur d'événement ne partagent jamais
  leur interrupteur » verrouille un choix de conception futur — les deux critères de déclenchement
  (alternatives crédibles chiffrées, contrainte verrouillant un choix futur) sont remplis.

## Éléments visibles (pour doc-writer)
- Aucun endpoint, champ WS ni topic MQTT nouveau.
- **Une ligne de journal nouvelle**, conditionnelle et rare. Format à documenter.

## Critères d'acceptation
- [x] Une chute supérieure au seuil produit **une** ligne, horodatée, avec l'ampleur — prouvé par
      `test/test_native_heap_drop_logic/` (15 cas + rejeu de données réelles) et les mutations
      D2/D3 qui tuent les tests quand ce comportement est cassé. Confirmation **sur l'appareil
      réel** laissée au « Plan de test manuel » ci-dessous (fenêtre post-déploiement, non encore
      écoulée au moment de cette clôture documentaire).
- [x] En régime normal, **zéro ligne supplémentaire** par rapport à aujourd'hui — aucune cadence
      périodique dans `heap_drop_logic.h` ; prouvé structurellement par les sentinelles D4
      (un seul site d'appel, pas de mesure de tas supplémentaire)
- [x] L'échantillonnage qui alimente l'auto-guérison est **inchangé** — vérifié par
      `test_d4d_heapLargestFreeBlock8Bit_called_once_in_checkHeapHealth` et
      `test_d4e_call_site_uses_fresh_sample_not_the_sentinel_prone_static`
- [x] `check_memory_discipline.sh` vert — vérifié 2026-10-08 (ainsi que `check_specs.sh`,
      `check_doc_links.sh`, `check_fopen_guard.sh`, `check_ws_broadcast_targeting.sh`, tous verts)
- [x] `pio run` sans nouveau warning ; `pio test -e native` vert, mutants tués — vérifié
      2026-10-08 : `pio run -e esp32dev_core3` SUCCESS (RAM 29,6 %, Flash 50,2 %), seuls
      warnings = `OneWire.cpp` (tiers, préexistant, hors périmètre). `pio test -e native` :
      **60 suites / 2103 cas, zéro échec** (mesuré via `ls -d test/test_native_* | wc -l` et la
      dernière ligne de `pio test -e native`, ADR-0073 — repère avant ce passage : 59/2082).

## Notes techniques — test-engineer (2026-10-08)

### ⚠️ Bug réel trouvé (NON corrigé ici, hors périmètre test-engineer) : débordement additif sur `courant` proche de `UINT32_MAX`

`heapDropEvaluate()` revendique (commentaire de tête de `heap_drop_logic.h`) d'être une fonction
**TOTALE**, « aucune branche indéfinie quelles que soient les valeurs fournies ». C'est **faux**
dans un cas précis : la comparaison `courant + threshold <= state.ref` **déborde** quand
`courant` est proche de `UINT32_MAX` (l'addition wrap autour de 2^32), ce qui peut produire un
**FAUX POSITIF** — une émission avec `after = courant` (une valeur proche de 4 Go, absurde pour
« plus gros bloc libre ») et `dropAmount` incohérent, **et** corrompt durablement `state.ref` (qui
reste bloquée à cette valeur absurde), ce qui ferait réémettre indéfiniment (au rythme de
l'anti-rafale) tant qu'aucune lecture réelle ne redépasse cette fausse référence — ce qui,
numériquement, n'arrivera jamais.

**Reproduit isolément** : `ref=20000` (ancré), puis `courant=UINT32_MAX` à `t=300000` →
`seuil=5000`, `courant+seuil` déborde à `4999`, `4999 <= 20000` est VRAI ⇒ `emit=true`,
`after=4294967295`, `dropAmount=20001` (incohérent).

**Gravité réelle, nuancée** : en production, `courant` vient **toujours** de
`heapLargestFreeBlock8Bit()` (quelques dizaines de Ko au plus — le tas interne de l'ESP32 ne
dépasse jamais quelques centaines de Ko), donc cette valeur **n'est physiquement jamais proche de
UINT32_MAX** par la voie normale. Le risque réel est un **tas déjà corrompu** (lecture de mémoire
invalide produisant un entier garbage) — précisément le genre d'événement que cette
instrumentation sert à diagnostiquer ; ironiquement, c'est l'un des seuls scénarios où le
détecteur pourrait lui-même produire une trace trompeuse au moment où elle compte le plus.

**Documenté par un test qui PASSE** (pas un test rouge laissé en l'état) :
`test/test_native_heap_drop_logic/test_heap_drop_logic.cpp::
test_sentinelle_uint32_max_revele_un_debordement_additif` — le test asserte le comportement
RÉEL (le faux positif), avec un commentaire explicite indiquant qu'il documente un défaut connu
et doit être **inversé** (`TEST_ASSERT_FALSE(r.emit)`) le jour où le module est corrigé. Protégé
aussi, côté structure, par `test_d4e_call_site_uses_fresh_sample_not_the_sentinel_prone_static`
qui vérifie que le site d'appel dans `main.cpp` ne passe jamais la statique
`lastHeapMaxAllocSample` (qui, elle, VAUT `UINT32_MAX` avant le premier échantillon) à
`heapDropEvaluate()` — seule `maxAllocHeap` (mesure fraîche) y entre, ce qui ferme la porte
d'entrée réaliste de ce défaut.

**Piste de correctif, non appliquée ici** : garder la comparaison additive pour la LECTURE
(cohérent avec le commentaire « jamais soustractive »), mais ajouter une garde `courant <=
state.ref` AVANT l'addition (qui ne peut alors jamais déborder, puisque `threshold` est borné par
`state.ref` dans les grandeurs réalistes du projet) — ou, plus simple, garder le test en l'état et
accepter le risque résiduel comme **non exploitable en pratique** (à trancher par
`pool-chemistry`/`code-reviewer`/`debugger`, pas par ce passage de test).

### Honnêteté de périmètre
- Les tests de `test_native_heap_drop_logic/` prouvent le **comportement de la fonction pure**
  pour toute séquence d'appels. Ils ne prouvent PAS le câblage réel dans `checkHeapHealth()`
  (impur, non compilable en natif), ni la fraîcheur de `heapRingLastCtx()`/`heapFloorsRead()` au
  moment de l'émission, ni le volume réel observé en production.
- Le rejeu de données réelles embarqué est **mesuré le 2026-10-08 avec une méthode distincte** de
  celle citée plus haut dans cette spec (dédup sur la ligne complète, segmentation par le
  marqueur de démarrage réel `=== Démarrage ESP32 Pool Controller vX.Y.Z ===`, pas par fichier) :
  **585 → 1420 lignes uniques**, **13 boots segmentés** (et non les chiffres cités plus haut dans
  cette spec, eux-mêmes déjà signalés FAUX par la contre-correction) ; rejoué sur l'ensemble des
  13 boots, le module émet **27 lignes au total**, TOUTES des chutes ≥ 26 % — cohérent avec
  l'intention du seuil relatif. Ne pas comparer ces deux comptages bruts entre eux : fenêtres
  temporelles et méthodes différentes. Un seul boot (13 points, 2026-09-12 15:19:35, 3 émissions)
  est embarqué en dur dans le test pour rester **portable** (aucune dépendance à
  `/root/pool-captures/`, absent de la CI).

## Plan de test manuel — ⚠️ FENÊTRE À NE PAS MANQUER
**Le prochain déploiement redémarre l'appareil, donc repart de la contiguïté pleine
(45 044 au boot du 2026-10-08).** C'est la seule occasion d'attraper le phénomène depuis son
origine.

1. Déployer **avec** cet instrument actif.
2. Relever `maxBloc8` au démarrage (trace « Heap 8bits », ou `POST /debug/heap_census`).
3. Laisser vivre, et **attendre la première ligne de chute** : elle datera l'événement et
   permettra de corréler avec ce qui se passait à cet instant.
4. Croiser avec le journal : si l'intervalle est de nouveau vide, c'est que l'événement est
   **silencieux par conception** — connexion WebSocket ou requête HTTP, aucun des deux ne
   journalise (vérifié le 2026-10-08). Ce serait alors le sujet d'une spec de suivi.

## Notes pour agent

⚠️ **Ne pas réactiver la cadence périodique.** C'est elle, et elle seule, qui avait motivé
l'extinction de juillet : ~288 lignes par jour sur une partition de 256 Ko. Le détecteur de
chute, lui, n'émet qu'en cas d'événement. Les deux étaient dans le **même `if`** — c'est
précisément le défaut de conception à ne pas reproduire : **un instrument de tendance et un
détecteur d'événement n'ont pas la même cadence et ne doivent pas partager leur interrupteur.**

⚠️ **Trois candidats ont déjà été écartés expérimentalement** (bascules de filtration, fichiers
statiques, connexion WebSocket) : ne pas refaire ce travail, et ne pas les réintroduire comme
hypothèses. Les chiffres sont dans « Contexte » ci-dessus.

⚠️ L'instrument mesure la mémoire : **il ne doit pas en consommer**. Pas de `String` construite
à chaque tick quand rien n'est émis.

## Hors périmètre
- **Trouver la cause** de la chute du 2026-10-08 : c'est ce que cette spec rend *possible*, pas
  ce qu'elle fait. Suivi dans `feature-133`.
- Le silence des connexions WebSocket et HTTP dans le journal — vrai sujet, mais distinct :
  l'ouvrir séparément si l'étape 4 du plan de test le confirme.
- Toute modification des seuils d'auto-guérison ou de refus de fichiers statiques.

## Notes techniques — doc-writer (2026-10-08)

- Documentation mise à jour : `docs/subsystems/system-health.md` (nouvelle section dédiée +
  correction de la section « Instrumentation du tas » et de la table des constantes, désormais
  fausse depuis l'extraction en module pur), `docs/subsystems/logger.md` (entrée `Heap: …`
  corrigée — site et date étaient faux — et nouvelle entrée `Chute maxBloc8: …`),
  `CHANGELOG.md` (entrée `[2.60.0]`).
- **ADR créé** : `docs/adr/0090-detecteur-de-chute-maximum-glissant-auto-rearme.md` (numéro
  vérifié contre `ls docs/adr/`, 0089 était le dernier pris). L'architect avait contredit le pari
  initial de cette spec (« pas d'ADR ») — formalisé ici. Indexé dans `docs/README.md`.
- Aucune doc transverse supplémentaire : pas d'endpoint HTTP, pas de topic MQTT, pas de champ
  WebSocket nouveau — `docs/API.md`, `docs/MQTT.md`, `docs/subsystems/ws-manager.md` non touchés.
  Pas de doc utilisateur (`docs/user/`) : rien de visible dans l'interface, **aucune capture
  d'écran à refaire**.
- **Spec non déplacée vers `done/`** : le « Plan de test manuel » (fenêtre post-déploiement) n'est
  pas encore écoulé au moment de cette passe documentaire — critère de clôture explicite de la
  spec elle-même. À déplacer une fois la fenêtre observée (ou son échéance explicitement
  abandonnée).

---

## Clôture — agent principal, 2026-10-08

### Défaut réel trouvé par le test-engineer, et corrigé

`heapDropEvaluate()` se revendiquait fonction totale. **Faux** : la forme additive
`courant + seuil <= ref` **déborde** quand `courant` approche `UINT32_MAX` — la somme se replie,
la garde devient vraie, et l'émission fantôme **corrompt `ref` durablement**.

Le test-engineer l'a documenté par un test qui **passait** (assertant le comportement réel), avec
le commentaire disant de l'inverser après correction. C'était la bonne manière de le signaler
sans sortir de son rôle.

**Corrigé** : garde `courant < ref` **avant** toute arithmétique, puis soustraction sûre.
**Test inversé** : il verrouille désormais la garde et meurt si la forme additive revient.

**Pourquoi ne pas l'avoir jugé théorique** — c'est l'argument qui a décidé : `courant` vient de
`heapLargestFreeBlock8Bit()` et n'approche jamais 4 Go en fonctionnement normal. Mais le scénario
réaliste est une **lecture corrompue pendant une corruption de tas déjà en cours**, c'est-à-dire
**exactement ce que cet instrument sert à diagnostiquer**. Il aurait menti au seul moment qui
compte.

### Garde ajoutée après revue
`static_assert(kHeapDropRefShift < 32)` — `ref >> refShift` serait un comportement indéfini
au-delà. Inoffensif aujourd'hui (site d'appel unique, constante 2), zéro octet, cas fermé.

### Ce que la revue a vérifié elle-même, sans croire les rapports
Le `code-reviewer` a **rejoué les cinq mutants** et restauré le fichier à l'identique :

| Mutant | Tests tués |
|---|---|
| D2 — comparaison de chute neutralisée | 5 |
| D3a — ré-ancrage à l'émission retiré | 3 |
| **D3b — réarmement silencieux retiré (le cliquet)** | **2** |
| forme additive restaurée | 1 |
| émission au premier tick | 3 |

Et il a jugé que le **rejeu de données réelles est une vraie garde de calibrage**, pas une
décoration : il épingle un comptage exact sur 110 points d'un boot réel et vérifie nommément
l'incident `23540 → 16372` que l'ancien seuil absolu ratait. Tout déplacement du seuil le casse.

### Sur la sentinelle de la ligne historique
Elle identifie la ligne par le **premier tampon nommé `ligne`** — couplage positionnel. Jugé
**acceptable en l'état** parce qu'un test voisin assère `countOccurrences("char ligne[") == 1` :
l'hypothèse implicite est devenue un invariant testé, et un ajout casse bruyamment avant de
pouvoir détourner la sentinelle en silence.
➜ **Lot de suivi recommandé** (non bloquant) : accrocher la sentinelle au **littéral de format**
plutôt qu'au nom du tampon. C'est le contrat réel — 152+ relevés de référence — dont le nom
n'est qu'un proxy.

### Résultats mesurés
- `pio run -e esp32dev_core3` : SUCCESS, aucun warning neuf — Flash 50,2 %, RAM 29,6 %,
  **+236 octets** pour l'instrument complet
- `pio test -e native` : **2 103 cas / 2 103**, **60 suites** (+1 : `test_native_heap_drop_logic`,
  et +6 cas de sentinelle structurelle)
- Les cinq scripts d'hygiène verts · `code-reviewer` **Approved**
- ADR-0090 créé ; quatre emplacements de doc périmée corrigés, dont
  `logger.md:867` qui renvoyait à `main.cpp:782` — une ligne qui n'a plus rien à voir depuis
  longtemps

### ⚠️ Ce qui N'EST PAS prouvé — la fenêtre à ne pas manquer
Les tests prouvent le **comportement du module pur** et la **forme structurelle** du câblage.
Ils ne prouvent **pas** le comportement d'exécution sur cible : fraîcheur du masque `ctx` à
l'émission, volume réel en production. `main.cpp` n'est pas compilable nativement.

**Le prochain déploiement redémarre l'appareil et repart de la contiguïté pleine (45 044 au boot
du 2026-10-08).** C'est **la seule occasion** d'observer le phénomène depuis son origine : au
palier actuel (26 612), la chute a déjà eu lieu et **rien ne la reproduit** — trois candidats
testés sur l'appareil, delta nul dans les trois cas.

Étapes : relever `maxBloc8` au boot · laisser vivre · attendre la **première ligne de chute** ·
croiser son horodatage avec le journal de la minute précédente.
Si aucune ligne n'apparaît malgré une chute visible au relevé de 30 min, l'événement est
**silencieux par conception** (connexion WS ou requête HTTP, aucune des deux ne journalise —
vérifié le 2026-10-08) : ouvrir une spec de suivi, ne pas modifier ce lot.

Une **ligne par démarrage** est structurellement attendue (stabilisation post-boot), sans intérêt
diagnostique. Si le débit réel dépasse nettement 0,88 ligne/jour, c'est la première chose à
regarder.
