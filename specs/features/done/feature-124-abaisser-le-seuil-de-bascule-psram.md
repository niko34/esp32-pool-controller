## Métadonnées
- **ID** : feature-124
- **Slug** : abaisser-le-seuil-de-bascule-psram
- **Type** : feature — **changement de politique d'allocation**, assumé comme tel
- **Statut** : **livré en v2.50.0** — code, tests (verts), revue **Approved**, doc et
  [ADR-0078](../../../docs/adr/0078-bascule-psram-cesse-d-etre-chirurgicale.md).
  ⚠️ **T2 reste OUVERT** : le critère de succès est binaire et ne se mesure que **sur
  cible**, après déploiement. La spec reste donc en `doing/` jusqu'à ce relevé.
- **Priorité** : **haute** — scénario S7, premier pas de la stratégie arrêtée le 08/09
- **Version cible** : 2.50.0
- **Spec(s) liée(s)** : `bug-080` lot C (le dossier), `feature-122` (l'instrument qui mesurera l'effet), `feature-123` (**gelée** — reléguée au dernier recours par l'analyse du 08/09)
- **Dernière mise à jour** : 2026-09-09

## Domaines touchés
- [ ] Capteurs · [ ] Régulation · [ ] Filtration / Éclairage
- [ ] Web · [ ] MQTT · [ ] UART · [ ] OTA · [ ] Authentification
- [x] **Politique d'allocation mémoire** (transverse à tout le firmware)
- [ ] Frontend

## Agents requis
- [x] spec-reader · [x] architect
- [x] **pool-chemistry — OBLIGATOIRE** : la politique d'allocation est transverse, donc elle touche aussi les chemins de dosage
- [ ] ux-ui-designer
- [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer

## Description

Abaisser `kPsramExtmemThresholdBytes` de **4 095 o** à une valeur nettement plus
basse, pour que les allocations ordinaires de taille moyenne préfèrent la PSRAM
et laissent le tas interne 8 bits respirer.

## Contexte — la stratégie du 08/09

Sept scénarios ont été évalués pour clore définitivement `bug-080`. Résultat :

| Scénario | Verdict |
|---|---|
| **S7 — ce lot** : abaisser le seuil PSRAM | **7/10** — très faible coût, réversible en une ligne |
| S2 : sérialiser le service des fichiers statiques | 8/10 — attaque la **cause**, mais demande la mesure du fragmenteur |
| S4 : moins de fichiers par page | 6/10 |
| S6 : servir depuis la PSRAM | 5/10 |
| **S1 : réserve DMA (`feature-123`)** | **3/10** — protège le WiFi mais **aggrave le 503** |
| S3 : tampons WiFi statiques | 2/10 — ~25 Ko sur un tas de 57 |
| S5 : abaisser le seuil du 503 | 1/10 — masque le symptôme |

Séquence retenue par l'utilisateur : **S7, puis la mesure du fragmenteur, puis S2**.

## ⚠️ CE QUE CE LOT NE FERA PAS — à lire avant tout

**Il ne déplacera AUCUN tampon WiFi.** Le seuil ne gouverne que
`heap_caps_malloc_default`, c'est-à-dire `malloc`/`new` ordinaires — le projet le
documente déjà (`constants.h`, désassemblage de `libheap.a`). Or les allocations
qui échouent demandent `caps=0x080C` **explicitement** : elles **contournent** le
seuil, et le contourneront toujours.

⇒ Ce lot ne corrige donc pas la cause. Il **fait de la place** au fragmenteur en
déportant tout ce qui peut l'être. C'est une promesse plus faible, et elle doit
être énoncée ainsi partout — pas comme un correctif.

**Effet attendu, à vérifier et non à postuler** : un plancher de plus gros bloc
plus haut sous la même rafale de 6 requêtes parallèles. ⚠️ **La « référence à
battre » de 2 932 o écrite ici était fausse** — c'est le seuil de déclenchement du
refus 503, observé au moment où la garde refuse. Le creux réel, mesuré finement le
09/09, vaut **1 076 o** ; le critère retenu est **1 622 o** (cf. plus bas).

## ⚠️ L'INVARIANT À RÉVISER, ET L'INTENTION À ASSUMER

`constants.h` porte ce verrou :

```cpp
static_assert(kPsramExtmemThresholdBytes + 1 == kVfsStdioBufferBytes, …);
```

Il épingle le seuil à **exactement 4 095**, pour garantir l'intention du lot B-1 :
« 4 095 est CHIRURGICAL — la seule classe d'allocations qui change de camp est
*exactement 4 096 o*. On ne déplace pas une politique d'allocation, on ferme une
inégalité non stricte. »

**Ce lot abandonne délibérément cette prudence.** L'assert doit donc être
**réécrit**, non supprimé : il doit continuer à garantir que le tampon stdio VFS
(4 096 o) bascule bien — donc `seuil < kVfsStdioBufferBytes` strictement — mais
cesser d'imposer l'égalité à 4 095. Et le commentaire de B-1 doit être **annoté**,
pas effacé : il documente une décision qui était juste à son heure.

## Le risque principal — PSRAM et cache coupé

Une donnée résidant en PSRAM et touchée pendant que le cache flash/PSRAM est
**désactivé** (écriture NVS, écriture LittleFS) provoque un plantage. Le projet est
déjà exposé pour tout ce qui dépasse 4 095 o ; abaisser le seuil **élargit
substantiellement** cette exposition.

**C'est le point central du gate `pool-chemistry`** : quelles structures touchées
depuis une interruption, une section critique, ou pendant une écriture flash,
pourraient basculer en PSRAM ? Les piles de tâches sont allouées avec caps
explicites (`pvPortMalloc` → `MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL`) et ne
basculeront pas — à **vérifier**, pas à supposer.

## ⭐ RÉFÉRENCE AVANT S7 — relevée le 09/09/2026 à 07:10, à comparer après

Reproduction volontaire par l'utilisateur sur la **v2.49.3**, instrument complet
pour la première fois (planchers fins + attribution de contexte corrigée).

| Grandeur | Valeur |
|---|---|
| **Plancher `0x0804`, mesure FINE** | **1 076 o** |
| **Régime au creux** | **`0x02` = RAFALE HTTP** — attribution sans ambiguïté |
| Sondes d'échec | **948**, 1 204, 1 268 o |
| Planchers `0x080C` / `0x1804` (résolution 8 s) | **20 468 o** — ils n'ont **rien vu** (facteur **19**) |
| Épisode 503 | déclenché à 2 932, **24 refus en 35 s** |
| Cumul d'échecs d'allocation | 45 |
| Déconnexions MQTT sur la période | 6 |
| Régimes vus depuis le démarrage (`ctxb`) | `0x0F` — les quatre |
| Durée de parcours | max **550 µs**, médiane ≤ 512 |

### ⚠️ Le chiffre de 2 932 était le SEUIL DE DÉCLENCHEMENT, pas le creux

Toute la stratégie des sept scénarios avait été bâtie sur « le creux vaut
2 932 o ». **C'est faux** : 2 932 est la valeur que la garde 503 *observe au moment
où elle refuse*, donc le seuil de déclenchement. Le creux réel, mesuré finement,
est **1 076 o** — près de **trois fois plus bas**.

Et c'est ce chiffre qui explique le défaut : **1 076 < 1 622**, la taille du tampon
TX WiFi. Les allocations `caps=0x080C` ne peuvent donc pas aboutir, ce que les
sondes d'échec confirment à 948 o.

### Le critère de succès de ce lot devient binaire

**Faire remonter le plancher fin `hf.i8.mb` au-dessus de 1 622 o** sous une
reproduction volontaire identique. Ce n'est plus une amélioration à apprécier :
c'est un seuil franchi ou non.

⇒ Corollaire sur la valeur du seuil : pour déplacer les pbufs lwIP d'environ
1 600 o, il faut un seuil **strictement inférieur** — la règle codée étant
« taille ≤ seuil ⇒ interne ». **1 024** le fait ; 2 048 ne l'aurait pas fait, et on
l'aurait découvert après déploiement.

## ⭐⭐ RÉSULTAT SUR CIBLE — 09/09/2026 08:23, LE CRITÈRE EST FRANCHI

Reproduction volontaire par l'utilisateur sur la **v2.50.0**, rafale **presque
deux fois plus forte** que celle de la référence.

| | Avant S7 (v2.49.3) | **Après S7 (v2.50.0)** |
|---|---|---|
| Requêtes GET servies | 176 | **327** |
| **Plancher fin `hf.i8.mb`** | **1 076 o** | **8 692 o** |
| Épisodes 503 | 1, **24 refus en 35 s** | **0** |
| Échecs d'allocation | 25 | **1** |
| Déconnexions MQTT | 6 | **0** |
| Pushes WS sautés | — | **0** |
| `ph_anom` / `orp_anom` (gate C2) | 0 / 0 | **0 / 0** |

**Critère binaire : franchi.** Objectif 1 622 o (tampon TX WiFi), obtenu
**8 692 o** — facteur **8**. Le plancher dépasse même les **8 192 o** de
`kStaticFileMinAllocBytes`, ce qui explique le rapport de l'utilisateur : **plus
aucun message « mémoire insuffisante »**, la garde 503 n'ayant plus eu de raison
de se déclencher.

**Condition C2 du gate satisfaite** : anomalies de comptabilité de dosage à zéro
sur les deux voies. Aucun retour arrière.

### Ce qui subsiste, et ne doit pas être tu

**Un** échec d'allocation : demande de **1 612 o** refusée avec un plus gros bloc
à **1 588 o** — il s'en est fallu de **24 octets**. Un seul contre 25, soit
**−96 %**. La marge est devenue positive, mais **étroite sur ce cas précis**.

### Le « Too many requests » observé par l'utilisateur est une AUTRE garde

Journal : `Rate limit dépassé pour 192.168.1.15 (122 req/min)`. C'est le limiteur
de débit d'authentification, intentionnel et **sans rapport avec la mémoire**. Il
a refusé **proprement**, écran de login correctement rendu, et s'est relâché en
quelques secondes. L'utilisateur juge le comportement acceptable — c'est la
différence entre une **protection** et une **panne**, là où l'ancien
comportement était un 503 sur une page cassée.

### La mesure du fragmenteur est répondue par le résultat lui-même

Elle devait nommer ce qui remplit le tas interne pendant une rafale. La réponse
est arrivée autrement : déplacer la classe **[1 025 ; 4 095]** vers la PSRAM
produit un facteur 8 sur le plancher. Le fragmenteur était donc **dans cette
classe** — et la seule chose qui y grandit avec le nombre de requêtes
simultanées, ce sont les **structures lwIP, une par connexion TCP**. La mesure ne
confirmerait que ce que le résultat démontre.

## Comportement attendu

- Le tas interne 8 bits présente un **plus gros bloc plus élevé** sous charge égale.
- Aucune régression fonctionnelle : la bascule est une **préférence**, pas une
  contrainte — si la PSRAM refuse, l'allocateur retombe en interne.
- Aucun ralentissement perceptible sur la régulation ni sur le service web.

### Cas limites
- **Module sans PSRAM** (WROOM) : `psramFound()` faux → l'appel est sauté, seuil
  sans effet. Binaire unique, comportement inchangé.
- **Écriture flash concomitante** : cf. le risque ci-dessus.
- **PSRAM saturée** : repli interne silencieux, donc dégradation vers l'ancien
  comportement — acceptable, mais à dire.

## Comportement actuel

`heap_caps_malloc_extmem_enable(4095)` posé à `main.cpp` juste après
`initOomGuard`. Seule la classe « exactement 4 096 o » bascule.

## Contraintes techniques
- Chemin de démarrage : l'appel précède `systemLogger.begin()` → aucun log sur place
- ADR-0058 : la métrique de référence reste le pool 8 bits
- Le seuil doit rester **strictement inférieur** à `kVfsStdioBufferBytes` (4 096)
- `kOomReserveBytes > seuil` : l'assert existant doit rester vrai
- Ne jamais lancer `pio pkg`

## Fichiers potentiellement concernés
- `src/constants.h` — la valeur, les deux `static_assert`, le commentaire de B-1 à annoter
- `src/main.cpp` — le site d'appel (probablement inchangé)
- `docs/subsystems/system-health.md`, ADR (révision de la doctrine B-1), `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — Seuil à **1 024**, et le raisonnement à écrire est un **encadrement** :
      strictement **sous 1 600** pour que les pbufs lwIP basculent (règle codée :
      « taille ≤ seuil ⇒ interne », donc seul ce qui dépasse le seuil part en
      PSRAM) ; et **au-dessus de 512** pour laisser en RAM interne les caches
      LittleFS (512 o) et le pool FILE de la libc (428 o), qui côtoient les
      écritures flash. Fenêtre utile : **[512 ; 1 599]**.
      ⚠️ **2 048 avait d'abord été retenu sur un raisonnement INVERSÉ** — « au-dessus
      de 1 622 pour capturer la classe lwIP » conclut le contraire de ce qu'il
      énonce, puisque 1 622 ≤ 2 048 resterait interne. Erreur rattrapée par le gate
      `pool-chemistry` avant tout code. Ne pas graver le raisonnement faux.
- [x] A2 — `static_assert` réécrit : `seuil < kVfsStdioBufferBytes` strictement,
      et le message explique pourquoi l'égalité à 4 095 a été abandonnée.
      ⚠️ **TROISIÈME assert à annoter**, oublié dans la première rédaction et relevé
      par le `spec-reader` : `kHistoryImportChunkBudgetBytes > kPsramExtmemThresholdBytes`
      (`constants.h`) reste **vrai**, mais son commentaire — « un lot ≤ 4 095 o
      repartirait dans le tas interne affamé » — devient **faux** et doit être
      corrigé. Et **5 assertions du test `test_native_oom_guard` épinglent 4 095**.
- [x] A3 — Commentaire de B-1 **annoté**, jamais effacé
- [x] A4 — Vérifier que les piles de tâches et les structures touchées sous cache
      coupé utilisent des caps **explicites** et ne basculent donc pas
- [x] C1 — `docs/subsystems/system-health.md` : le changement de politique, son
      motif, et **ce qu'il ne fait pas** (aucun tampon WiFi déplacé)
- [x] C2 — ADR : la doctrine « chirurgical » de B-1 est révisée. Alternatives à
      tracer : garder 4 095, descendre à 512 d'un coup, procéder par paliers
- [x] C3 — `CHANGELOG.md`
- [x] T1 — Tests : le seuil respecte les deux invariants ; mutation ramenant à 4 095
      (le lot doit être visible) et mutation à 4 096 (le tampon stdio ne basculerait plus)
- [x] T2 — ✅ **SATISFAIT le 09/09/2026 à 08:23** : plancher fin **8 692 o** contre 1 622 visés et 1 076 avant, sous une rafale de 327 requêtes (deux fois la référence). Zéro épisode 503, zéro déconnexion MQTT, un seul échec d'allocation contre 25. **Relevé sur cible avant/après**, sous reproduction volontaire identique
      (rechargements multiples en onglets parallèles) : le plancher fin `hf.i8.mb`
      doit **remonter au-dessus de 1 622 o** (le gabarit du tampon TX WiFi).
      ⚠️ **La référence de 2 932 o écrite ici d'abord était fausse** : c'est le seuil
      de déclenchement du refus 503, pas le creux. Le creux relevé le 09/09 vaut
      **1 076 o**, et le critère est calé sur 1 622. **Si ce n'est pas le cas, le lot n'a pas marché et il
      faut le dire** — le seuil ne gouverne pas les allocations à caps explicites,
      donc l'échec est un résultat possible

## Conditions du gate `pool-chemistry` (GO SOUS CONDITIONS, 09/09)

- [x] **C1** — Ne pas graver le raisonnement inversé ; seuil à **1 024**, encadrement écrit.
- [x] **C2** — ✅ **SATISFAIT** : `ph_anom` et `orp_anom` à **0** avant comme après déploiement. Aucune régression de la comptabilité de dosage, aucun retour arrière. **Latence** : marge réelle de deux ordres de grandeur (`kPulseMinMs` = 10 s,
      `kAcctDeltaAnomalyMs` = 5 000 ms contre une loop de quelques dizaines de ms).
      Mais T2 doit publier le **compteur d'anomalies de comptabilité de dosage**
      avant/après : **toute hausse non nulle = retour arrière immédiat**.
- [x] **C3** — `static_assert` ajouté sur `kOomFallbackReserveBytes` (1 024) face au
      seuil, sinon un palier futur sous 1 024 le ferait basculer sans alerte.
- [x] **C4** — Le témoin `kHeapWitness4095*` devient **mensonger** : renommer, et
      conserver un témoin de contrôle à **seuil + 1** plutôt qu'à une valeur figée.
- [x] **C5** — L'ADR doit dire que l'innocuité sous cache coupé repose sur la copie
      interne d'`esp_flash` **et** sur l'**absence d'ISR IRAM applicative** (zéro
      `IRAM_ATTR`, zéro `attachInterrupt` dans `src/`) — invariant à **réénoncer**,
      pas un acquis.

**Ce que le gate a contre-vérifié et confirmé** : `pump_controller.cpp` ne contient
aucun `malloc`/`new`/`JsonDocument` (uniquement des `snprintf` sur pile, arrêt de
pompe par écriture de registre) ; compteurs journaliers en NVS et non LittleFS ;
parachute protégé aux trois sites par l'entonnoir unique avec `#pragma GCC poison
malloc` — la récidive de feature-066 est **structurellement fermée** ; aucune garde
de dosage ne lit de métrique de tas.

**Méthode de paliers** : l'exposition est **quantifiée** au-dessus de 512 et
**binaire en dessous** — les caches LittleFS et le pool FILE sont un changement de
*nature*. **Ne jamais planifier de palier sous 512.**

## ADR attendu ?
- **Probable** : oui — critères 1, 2, 3
- **Sujet** : « la bascule PSRAM cesse d'être chirurgicale » — révision assumée de
  la doctrine du lot B-1, avec le motif (le tas interne est la ressource rare, et
  la PSRAM dort à 4,18 Mo libres)

## Notes

Ce lot est le **premier pas** d'une stratégie en trois temps, et il a été choisi
pour son rapport bénéfice/risque, pas pour son efficacité attendue. Il peut très
bien ne rien changer : c'est pourquoi le critère T2 exige un relevé comparatif et
prévoit explicitement l'échec comme résultat publiable.

⚠️ **`feature-123` est GELÉE par cette analyse**, pas abandonnée. Elle protégerait
les tampons WiFi en retirant de la mémoire au pool général — donc en rendant le
503 **plus** probable, alors que c'est le symptôme que l'utilisateur voit. Elle
redevient pertinente si, et seulement si, S7 puis S2 laissent subsister des échecs
d'allocation WiFi.

---

## Notes d'implémentation (2026-09-09, v2.50.0)

### Ce qui a été livré

| Fichier | Changement |
|---|---|
| [`src/constants.h`](../../../src/constants.h) | seuil **4 095 → 1 024** ; commentaire de B-1 **conservé mot pour mot** + annotation feature-124 (encadrement, deux bornes, motif lwIP, ce que le lot ne fait pas, mise en garde contre le raisonnement inversé) ; **cinq** `static_assert` |
| [`src/main.cpp`](../../../src/main.cpp) | site d'appel inchangé (juste après `initOomGuard`, sous `psramFound()`) ; témoin A7 exprimé en **`seuil + 1` / `seuil`** ; message PSRAM du boot annonce le nouveau seuil |
| [`src/heap_metrics.h`](../../../src/heap_metrics.h) | `kHeapWitness4096*` / `4095*` → `kHeapWitnessAbove*` / `AtThreshold*` — **mêmes bits, même sens** |
| [`src/web_routes_config.cpp`](../../../src/web_routes_config.cpp) | clés JSON **`w96` / `w95` conservées**, recâblées sur les constantes renommées |
| [`src/oom_guard.cpp`](../../../src/oom_guard.cpp), [`src/heap_caps_logic.h`](../../../src/heap_caps_logic.h) | commentaires réalignés sur le seuil relatif |

### Compteurs **mesurés** (jamais recopiés — [ADR-0073](../../../docs/adr/0073-chiffre-consigne-porte-sa-methode.md))

| Grandeur | Valeur | Comment |
|---|---|---|
| Suites natives | **44** | `ls -d test/test_native_* \| wc -l` |
| Cas de test | **1 686** | dernière ligne de `pio test -e native` |
| Flash | **1 857 425 o** | `pio run -e esp32dev_core3` |
| Warnings | **exactement 6, tous préexistants** | aucun warning nouveau |

### Les 6 mutants tués

| # | Mutation | Ce qui la tue |
|---|---|---|
| 1 | Seuil ramené à **4 095** (annule le lot en une ligne) | borne haute de `test_psram_extmem_threshold_window_v2500` (les pbufs lwIP de 1 600 o redeviennent internes) **+** ligne `4095` de la table des paliers (`fenetreOk = false`) **+** sentinelle qui interdit le retour de l'égalité `seuil + 1 == kVfsStdioBufferBytes` |
| 2 | Seuil à **2 048** (le raisonnement inversé) | même borne haute : `1 600 ≤ 2 048` et `1 622 ≤ 2 048` restent internes ⇒ le lot ne déporte **rien**. Inutile mais non dangereux, donc **invisible sans cette table** |
| 3 | Seuil sous la borne basse (**511**, **256**) | `static_assert(kPsramExtmemThresholdBytes >= 512)` et `static_assert(kOomFallbackReserveBytes <= seuil)` — arrêt à la **compilation** ; la table des paliers en publie le motif ligne par ligne |
| 4 | Témoin A7 ramené à une **taille figée** (`malloc(kVfsStdioBufferBytes)`) | `test_the_psram_witness_is_relative_to_the_extmem_threshold` (compte **0** occurrence de la forme figée, exige `seuil` et `seuil + 1`, interdit `heap_caps_malloc(`) |
| 5 | **Suppression silencieuse** d'un des cinq `static_assert` | `test_the_five_psram_threshold_static_asserts_are_present` — lit `constants.h` **réel, dépouillé de ses commentaires** (une simple mention ne suffit pas : forme corrigée de `bug-114`) |
| 6 | Déplacement d'un **bit** du témoin, ou recâblage d'une clé `w96`/`w95` | `test_the_psram_witness_bits_and_json_keys_stay_comparable` — les 7 positions de bits sont épinglées **en littéraux** et le câblage des deux clés est vérifié dans `web_routes_config.cpp` |

### ⚠️ Ce qui reste NON PROUVÉ

**Le critère de succès binaire** : faire remonter le plancher fin `hf.i8.mb`
**au-dessus de 1 622 o** sous reproduction volontaire identique. Il **ne se mesure
pas hors cible** — aucun test natif, aucune compilation ne peut y répondre. Les
44 suites prouvent l'**encadrement** du palier et le **câblage** du témoin, pas
l'**effet** de la politique.

Restent également dus, avec le même relevé (condition **C2** du gate
`pool-chemistry`) : le **compteur d'anomalies de comptabilité de dosage**
avant/après — ⚠️ **toute hausse non nulle = retour arrière immédiat** — et le
verdict A7 relu dans `hf.w96` / `hf.w95`.

### ⚠️ Deux incidents de ce cycle, qui valent plus que le correctif

**1. `src/constants.h` a été PERDU par un `git checkout` sur un lot non commité.**
Le `test-engineer` a exécuté un `git checkout` sur `src/constants.h` pour annuler
une mutation, alors que **le lot entier n'était pas commité** : la commande a donc
restauré la version d'**avant feature-124** et effacé tout le travail du lot sur ce
fichier. Reconstitué depuis le transcript de la session, puis vérifié **bit à bit**
par la taille de flash (**1 857 425 o**, à l'octet près) et le md5 du binaire.

> ⚠️ **La leçon est plus importante que l'incident** : une flash identique ne
> prouve que le **code**, jamais les **commentaires** — or **toute la valeur de ce
> lot est dans les commentaires** (l'encadrement, les bornes, le raisonnement
> inversé à ne pas regraver). C'est la **revue** qui a dû les auditer un par un.
> Corollaire de méthode : ne jamais utiliser `git checkout <fichier>` pour annuler
> une mutation sur un lot non commité — la mutation se défait à la main, ou le lot
> se commite d'abord.

**2. Un commentaire de test justifiait un assert par un exemple FAUX.** Il
affirmait : « sans cet assert, un palier à 256 compilerait sans un mot ». C'est
**faux** — l'assert **C3** (`kOomFallbackReserveBytes <= seuil`) arrête déjà 256,
et la **table des paliers du même lot** le démentait explicitement (ligne `256` :
« plancher, repli et borne basse violés à la fois »). Corrigé : le commentaire dit
maintenant que le plancher `>= 512` garde un **axe distinct** de C3 et ne
redevient effectif que si `kOomFallbackReserveBytes` descend sous 512.

> Dans un lot dont **la valeur est dans les commentaires**, une justification
> démentie par le lot lui-même est **exactement le défaut combattu**.
