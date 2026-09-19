# ADR-0083 — L'historique s'écrit en flux et de façon atomique (`.tmp`+`rename`), sans jamais promouvoir son `.tmp`, avec un contrat de format persisté verrouillé par portage

- **Statut** : Accepté
- **Date** : 2026-09-19
- **Décideurs** : utilisateur (arbitrage `.tmp`+`rename` du 2026-09-19), `architect` (dossier d'options, balayage de divergence `dtostrf`/`printf`), `embedded-developer`, `pool-chemistry` (aucun chemin de dosage touché — hors périmètre), `test-engineer` (validation par mutation, fixture de référence), `code-reviewer`, `doc-writer`
- **Spec(s) liée(s)** : feature-131 (la-sauvegarde-de-l-historique-cesse-de-batir-le-json-en-ram)

## Contexte

`HistoryManager::saveToFile()`/`loadFromFile()` construisaient tout le document JSON de `/history.json` en RAM avant de l'écrire, ou après l'avoir lu (`JsonDocument` complet + `String` de sérialisation). C'était le plus gros transitoire **périodique** du firmware, et le site exact du coredump du 05/09/2026. La correction évidente — écrire et lire **en flux**, point par point — allonge mécaniquement la fenêtre pendant laquelle une coupure d'alimentation peut survenir en cours d'écriture, ce qui remet en jeu une décision déjà prise et documentée : [ADR-0033](0033-budget-espace-partition-history.md) avait choisi, en 2026-07-27, de supprimer l'ancien `/history.json` **avant** de réécrire le nouveau, précisément pour éviter la coexistence des deux fichiers sur une partition alors **contrainte à 64 Ko**. Cette même ADR-0033 rejetait explicitement l'option « `.tmp`+`rename` » au motif qu'elle **aggraverait** le pic d'occupation (3× au lieu de 2×) sur cette partition.

Depuis [ADR-0068](0068-partitions-layout-v5-flash-8-mo.md) (05/09/2026), la partition `history` fait **256 Ko**, pas 64 Ko. Le calcul qui justifiait le rejet de `.tmp`+`rename` en 2026-07-27 doit être refait sur la partition réelle, pas sur celle qui n'existe plus.

Trois décisions distinctes devaient être prises dans le même lot :

1. Comment mesurer et écrire le JSON sans jamais le matérialiser en entier ?
2. L'atomicité de l'écriture doit-elle être reconstruite, et comment ?
3. Le format persisté doit rester **identique** (contrat de la spec, consommé par le frontend et les exports) — comment le garantir alors que le mécanisme de sérialisation change entièrement (plus d'ArduinoJson `JsonDocument`, un formatage manuel) ?

## Décision

### 1. Un émetteur unique, deux puits

`HistoryManager::emitPersistedJson(HistoryByteSink sink, void* ctx)` parcourt `memoryBuffer` **une seule fois** et pousse chaque tronçon (préfixe fixe, un point à la fois via `formatPersistedHistoryPoint()`, suffixe fixe) dans un puits dont la nature est choisie **entièrement côté appelant** :

- **puits compteur** (`historyCounterSink`) : passe 1, additionne les octets, **aucun effet de bord FS** — donne `jsonSize` ;
- **puits fichier** (`historyFileSink`) : passe 2, accumule dans un tampon de pile (`kHistoryWriteChunkBytes` = 512 o) et ne déclenche `f.write()` qu'au remplissage.

Le contrat de mesure devient : **une divergence entre la mesure et l'écriture est impossible par construction**, puisque c'est exactement le même code de formatage, le même buffer de point, le même seuil de saut (`kHistoryPersistPointMaxBytes` = 128 o) qui alimentent les deux passes. Ni le pic d'allocation (128 o + 512 o, sur pile, jamais de tas engagé) ni la logique de saut ne peuvent diverger entre « ce qui a été mesuré » et « ce qui a été écrit ».

### 2. Écriture atomique `.tmp` + `rename`, jamais de promotion au boot

`saveToFile()` écrit intégralement `/history.json.tmp`, puis un unique `rename(historyTmpFilePath, historyFilePath)` le fait remplacer l'ancien fichier. **L'ancien `/history.json` n'est plus supprimé avant l'écriture** — il coexiste avec le `.tmp` jusqu'au `rename` final. Conséquence directement recherchée : une coupure d'alimentation en cours d'écriture laisse soit l'ancien fichier intact (`rename` jamais atteint), soit un `.tmp` orphelin sans effet sur `/history.json` — jamais un fichier tronqué en place.

`reconcileHistoryTmp()`, appelée dans `begin()` juste après `setPersistenceFs()` et avant tout chargement, efface l'orphelin **inconditionnellement** (`remove()` seul, ni `open()` ni `exists()` — 0 site fopen sur ce chemin) et journalise un **WARNING sans throttle** (borné à 1 par démarrage par construction) si un orphelin existait : c'est la preuve datée qu'une coupure est survenue pendant une sauvegarde précédente.

**Le `.tmp` de l'historique n'est jamais promu**, à la différence du `.tmp` de rotation du journal ([ADR-0076](0076-reconciliation-du-tmp-de-rotation-au-boot.md)). Le journal peut promouvoir son `.tmp` parce que sa complétude est prouvée par un marqueur de fin écrit **avant** le `remove()` de l'ancien fichier. Rien de tel n'existe ici : la taille attendue du JSON (`jsonSize`, calculée en passe 1) ne vit **qu'en RAM**, jamais persistée sur la flash — au redémarrage, il est structurellement impossible de vérifier qu'un `.tmp` d'historique retrouvé est complet. Et l'ancien `/history.json` reste, par construction du mécanisme, la seule copie **garantie intacte** tant que le `rename` final n'a pas eu lieu : il n'y a donc jamais de raison de préférer le `.tmp` à lui. Le `.tmp` orphelin est **toujours** un déchet.

L'admission mémoire (`fsGuardAdmit`) et l'admission d'espace (`historySaveAdmit`) **fusionnent en une seule**, en tête de fonction. La seconde admission qu'imposait l'amendement du 2026-09-06 à [ADR-0070 §d](0070-fopen-interne-helper-unique-et-classes-de-contention.md) devient historique : elle protégeait le pic de construction du `JsonDocument` complet entre la mesure et l'écriture, un pic que l'écriture en flux fait disparaître. `historySaveAdmit(freeBytes, jsonBytes, marginBytes)` perd son paramètre `oldFileBytes` : l'ancien fichier coexistant avec le `.tmp` jusqu'au `rename`, son espace n'est jamais crédité — `freeBytes` (`totalBytes()-usedBytes()`) le compte déjà comme occupé.

### 3. Contrat d'identité octet à octet, verrouillé par portage de `dtostrf`

Le format ne doit pas changer — contrainte explicite de la spec, le frontend et les exports consomment `/history.json` tel quel. Or l'ancien code appelait `String(float, n)` pour chaque valeur flottante, qui appelle `dtostrf(value, n+2, n, buf)` (`cores/esp32/stdlib_noniso.c`). `dtostrf` **n'appelle pas printf** : il ajoute `0,5·10⁻ⁿ` puis **tronque** chiffre à chiffre par divisions/soustractions successives — ce n'est **pas** équivalent à l'arrondi « au plus proche, pair » d'un `%.*f`. Un balayage exhaustif du pas réel de mesure du DS18B20 (0,0625 °C, architecte, feature-131) a mesuré **174 valeurs divergentes sur 2 881** entre les deux méthodes, soit **6,0 %** — par exemple `-54,75` donne `"-54.7"` avec `dtostrf`/`String`, `"-54.8"` avec `%.1f`.

La décision est de **porter `dtostrf` bit à bit** (`historyFormatFixed()`, [`src/history_persist_format.h`](../../src/history_persist_format.h)) plutôt que d'utiliser `snprintf("%.*f", ...)`. Le nouveau code produit ainsi exactement les mêmes octets que l'ancien, pour toute valeur déjà en usage sur les appareils en production. Vérifié par comparaison **octet à octet, 684 octets, 0 divergence**, contre une fixture (`test/fixtures/history_reference.json`) générée par le code réel de la version précédente (2.52.2, `tools/gen_history_reference_json.cpp`).

**Toute évolution future du format persisté doit passer par `src/history_persist_format.h` et son test-oracle** (le balayage exhaustif contre le port `dtostrf`) — pas réintroduire un `%.*f` ou un pré-arrondi (`roundf(x*10)/10`, comme `history_export_chunk.h` — mesuré **encore plus** divergent) qui romprait silencieusement la compatibilité avec l'historique déjà écrit sur les appareils déployés.

## Alternatives considérées

- **Option A (celle d'ADR-0033, abandonnée sur ce point précis) — `remove()` avant réécriture** : divise le pic de coexistence par 2, mais reperd l'historique entier sur toute coupure entre le `remove()` et la fin de l'écriture — jusqu'à 75 jours d'agrégats journaliers. Le motif qui la justifiait (« diviser le pic copy-on-write par 2 », écrit pour une partition de **64 Ko**) est **dissous** par ADR-0068 : la partition fait désormais **256 Ko**, et la coexistence ancien+nouveau porte le pic d'occupation à ~62 Ko, soit **24 %** de la partition — contre ~38 Ko (**15 %**) avec le `remove()` préalable. Un écart de 9 points de pourcentage sur une partition qui a 76 % de marge dans le pire cas n'est plus un arbitrage serré.
- **Option B (rejetée) — conserver `String(v,n)`/ArduinoJson pour le formatage, streamer seulement la structure** : n'aurait pas supprimé le pic de sérialisation par point (chaque `String(v,n)` reste un `malloc` d'environ 44 o pour un `float` — la voie empruntée par ce code, `point.ph`/`orp`/`temperature`/`phFiltered`/`orpFiltered` étant des `float` dans `DataPoint`), et n'aurait pas permis le test-oracle par balayage exhaustif (ArduinoJson n'étant pas une dépendance de l'environnement natif, [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)). Rejetée en faveur d'un formatage manuel entièrement testable en logique pure.
- **Option C (rejetée) — `snprintf("%.*f", ...)` pour reformater les flottants** : plus simple à écrire et à lire que le portage de `dtostrf`, mais **change le contenu du fichier** pour 6,0 % des valeurs déjà persistées (mesuré). Rejetée : le contrat de la spec est l'identité octet à octet, pas une « équivalence numérique proche ».
- **Option D (retenue) — écriture en flux (un émetteur, deux puits) + atomicité `.tmp`+`rename` sans promotion + portage bit-à-bit de `dtostrf`** : élimine le pic de mémoire, élimine la perte totale sur coupure, préserve le format à l'octet près. Coût : une fenêtre de coexistence `.tmp`/ancien plus longue qu'avant (l'écriture en flux prend le même temps LittleFS qu'avant, la fenêtre de risque n'est donc pas allongée par le flux lui-même — elle l'est par le choix de ne plus `remove()` en tête), absorbée par la marge de la partition 256 Ko.

## Conséquences

### Positives

- **Le pic de mémoire du plus gros transitoire périodique du firmware disparaît** : 128 o + 512 o sur pile, contre « des dizaines de kilooctets » de `JsonDocument`+`String` — site exact du coredump du 05/09/2026 fermé.
- **Plus de perte totale de l'historique sur coupure d'alimentation** : au pire, un `.tmp` orphelin sans effet, jamais un `/history.json` détruit ou tronqué en place.
- **Une admission mémoire au lieu de deux** dans `saveToFile()` : l'amendement de 2026-09-06 à ADR-0070 §d devient historique.
- **Un site `fopen` de moins** (13 au lieu de 14, recensement [acces-fichiers.md](../subsystems/acces-fichiers.md)) : la lecture d'`oldSize` disparaît avec le crédit qu'elle finançait.
- **Format persisté prouvé identique** (684 octets, 0 divergence) — aucune migration, aucune casse du frontend ni des exports.

### Négatives / dette assumée

- **Le `.tmp` de l'historique n'est jamais promu**, contrairement à celui du journal — c'est une limite assumée, pas un oubli : la complétude n'est vérifiable qu'en RAM, jamais sur la flash. Une future évolution qui voudrait promouvoir le `.tmp` devrait d'abord persister la taille attendue quelque part (NVS ou marqueur de fin dans le fichier lui-même) — non fait ici, hors périmètre.
- **Portage de `dtostrf` non trivial à maintenir** : c'est une copie fidèle d'un algorithme externe (`cores/esp32/stdlib_noniso.c`), pas une implémentation naturelle. Toute mise à jour du framework Arduino-ESP32 qui changerait `dtostrf` devrait être re-vérifiée contre ce port (peu probable : le comportement est figé depuis longtemps, mais non garanti par un contrat amont).
- **Fenêtre de coexistence `.tmp`/ancien plus longue en valeur absolue** qu'avec le `remove()` préalable (l'ancien fichier reste ouvert au risque de coupure jusqu'au `rename`, pas jusqu'au `remove()`) — absorbée par la marge de la partition 256 Ko, pas éliminée.
- **Suivi post-déploiement non clos par ce lot** : le `rename()` de LittleFS écrasant une destination existante (`lfs_rename`, confirmé par `nm` sur `libjoltwallet__littlefs.a`) n'a **jamais été exercé sur cible** par ce dépôt (`logger.cpp` fait `remove(dest)` puis `rename` sur son propre chemin de rotation, un chemin différent). Un test de fumée sur cible réelle reste **obligatoire avant tout déploiement long** — voir la spec feature-131, section suivi.

### Ce que ça verrouille

- **Toute évolution du format persisté de `/history.json` passe par `src/history_persist_format.h` et son test-oracle** (balayage exhaustif contre le port `dtostrf`) — un remplacement par `%.*f` ou un pré-arrondi romprait silencieusement la compatibilité avec l'historique déjà écrit sur les appareils déployés.
- **`historySaveAdmit()` ne recrédite plus jamais l'espace d'un fichier qui coexiste avec son remplaçant en cours d'écriture.** Toute nouvelle écriture atomique sur cette partition doit compter le pic de coexistence complet, pas un pic divisé par un `remove()` préalable qui n'a plus lieu.
- **Le `.tmp` de l'historique reste un déchet par construction, jamais une source de vérité.** Un futur mainteneur qui voudrait le promouvoir doit d'abord résoudre le problème de complétude vérifiable (marqueur persisté), pas supposer qu'un `.tmp` trouvé au boot est nécessairement plus récent et complet.
- **Un émetteur unique pour la mesure et l'écriture** : toute future modification du format persisté qui ajouterait un champ doit passer par `emitPersistedJson()`/`formatPersistedHistoryPoint()`, jamais dupliquer la boucle de sérialisation entre une passe de mesure et une passe d'écriture séparées.

## Références

- Code : [`src/history.cpp`](../../src/history.cpp) (`emitPersistedJson`, `saveToFile`, `loadFromFile`, `reconcileHistoryTmp`, puits `historyCounterSink`/`historyFileSink`), [`src/history.h`](../../src/history.h) (contrat `HistoryByteSink`), [`src/history_persist_format.h`](../../src/history_persist_format.h) (`formatPersistedHistoryPoint`, `historyFormatFixed`), [`src/history_logic.h`](../../src/history_logic.h)/`.cpp` (`historySaveAdmit`, signature révisée), [`src/constants.h`](../../src/constants.h) (`kHistoryPersistPointMaxBytes`, `kHistoryWriteChunkBytes`, `kHistoryLoadReservePoints`, `kHistoryLoadMaxPoints`)
- Tests : `test/test_native_history_persist_format/` (oracle bit-à-bit, sweeps pH/ORP/température, comparaison E4), `test/test_native_history_atomicity/` (rename, échec d'écriture, réconciliation), `test/test_native_history_load_stream/` (lecture en flux, troncature), `test/test_native_history/` (admission révisée, budget 256 Ko), `tools/gen_history_reference_json.cpp` (génération de la fixture de référence)
- Spec : `specs/features/done/feature-131-la-sauvegarde-de-l-historique-cesse-de-batir-le-json-en-ram.md`
- Docs : [subsystems/history.md](../subsystems/history.md), [subsystems/acces-fichiers.md](../subsystems/acces-fichiers.md)
- Antécédents : [ADR-0033](0033-budget-espace-partition-history.md) (budget d'espace + `remove`-avant-write, **motif partiellement dissous par cet ADR**), [ADR-0068](0068-partitions-layout-v5-flash-8-mo.md) (partition history 256 Ko, déclencheur de la révision), [ADR-0070](0070-fopen-interne-helper-unique-et-classes-de-contention.md) (point d'admission unique, §d amendé le 2026-09-06 — amendement devenu historique), [ADR-0076](0076-reconciliation-du-tmp-de-rotation-au-boot.md) (le `.tmp` du journal, LUI, est promu — contraste assumé), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure / Humble Object)
