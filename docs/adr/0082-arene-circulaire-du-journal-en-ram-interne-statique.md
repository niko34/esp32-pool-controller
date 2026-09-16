# ADR-0082 — Le journal est stocké dans une arène circulaire `.bss` en RAM interne, pas dans des `String` sur le tas ni en PSRAM

- **Statut** : Accepté
- **Date** : 2026-09-16
- **Décideurs** : utilisateur (arbitrage de l'option B′ et des constantes), `architect` (dossier d'options), `pool-chemistry` (P1 **GO sous conditions** C1-C7 sur le design, P2 sur le diff), `embedded-developer`, `test-engineer` (1 874 cas natifs, validation par mutation), `code-reviewer`, `doc-writer`
- **Spec(s) liée(s)** : [`feature-129`](../../specs/features/doing/feature-129-les-string-du-journal-fractionnent-le-grand-bloc.md) (implémentation, ce lot), `feature-127`/`feature-128` (instruments de preuve), `feature-125` (réserves du lot C, `bug-080`), incident [`2026-09-10-recidive-fragmentation-v2500.md`](../incidents/2026-09-10-recidive-fragmentation-v2500.md)

## Contexte

Le journal (`std::vector<LogEntry>` de `String`, ancien `logger.h`) allouait chaque message et chaque ligne formatée sur le tas interne 8 bits. La preuve produite par `feature-128` (instrument de recensement du tas, ADR-0081) a établi que ces allocations ne se contentent pas d'occuper de la mémoire : elles s'insèrent **au milieu du plus gros bloc libre** et le fractionnent en deux morceaux plus petits, sans jamais le laisser se reformer. Mesuré en production :

| uptime | trou avant | trou après | taille demandée |
|---|---:|---:|---:|
| 18,0 h | 16 752 o | 9 024 o | 144 o |
| 19,4 h | 9 024 o | 7 260 o | 144 o |

**L'amplification est le fait central, pas la fuite** : à 18,0 h, 160 octets alloués ont détruit 7 680 octets de contiguïté. Remonté à l'échelle de l'effondrement initial (démarrage, première sauvegarde d'historique) : **168 octets — les deux `String` d'une seule ligne de journal — ont détruit 23 552 octets de contiguïté**. Ce n'est pas une pénurie d'octets, c'est une pénurie de **position** : le plus gros bloc contigu finit par tomber sous `kStaticFileMinAllocBytes` (8 192 o), ce qui fait refuser le service de fichiers statiques (503) et reporter les sauvegardes d'historique.

Le journal ne peut pas être simplement réduit ou désactivé : c'est l'instrument qui a permis ce diagnostic, et il porte les gardes de dosage chimique (refus d'injection, bascule pH auto→manuel, plancher de sécurité, alertes MQTT). Toute option devait donc préserver — voire améliorer — sa fiabilité, sa latence et son intégrité, sous l'examen obligatoire de `pool-chemistry`.

## Décision

Remplacer le `std::vector<LogEntry>` par **trois objets `.bss` de taille fixe, en RAM interne** (pas de PSRAM) :

| Objet | Taille | Rôle |
|---|---:|---|
| `sLogArena[kLogArenaBytes]` | 16 384 o | texte des messages, file circulaire d'octets |
| `sLogSlots[kMaxLogEntries]` | 4 000 o (200 × 20 o) | descripteurs POD (`timestamp`, `wallSec`, `seq`, `off`, `len`, `level`, `flags`) |
| `sFlushLine[kLogPersistLineMax]` | 832 o | ligne de flush, propriété exclusive du flusher |

Coût mesuré : **+21 240 octets de `.bss`** (21 216 prédits — écart d'alignement structure). `log()` n'exécute plus **aucune allocation dynamique** : un `memcpy` sous le mutex déjà existant, vers un tampon de taille fixe. `_persistBuffer` (le `std::vector<String>` qui doublait le stockage en attendant le flush) est **supprimé** : la ligne de `/system.log` est **dérivée** de l'arène au moment d'écrire.

### Pourquoi la PSRAM a été écartée

La PSRAM était l'intuition de départ, et sur un seul axe elle est strictement meilleure : elle rendrait ~30 Ko de RAM interne au lieu de ~10 Ko (elle ne prélèverait rien sur la ressource dont on diagnostique la pénurie). Mais la contrainte réelle de cette spec est la **contiguïté**, pas le volume — et sur cet axe précis, PSRAM et RAM interne statique sont **équivalentes** : toutes deux retirent 100 % des allocations dynamiques fragmentantes du tas 8 bits. La PSRAM aurait donc payé ~20 Ko qu'on n'a pas besoin de payer, contre trois risques concrets :

1. **Le danger de cache coupé pendant `flushToDisk()`.** `flushToDisk()` s'exécute **sur la tâche qui logue elle-même** (`logger.cpp`, commentaire de tête) — il n'existe pas de « tâche du logger ». Il écrit `/system.log` en flash, et pendant une écriture flash le cache est coupé ; les accès PSRAM passent par ce cache. Lire le texte d'un message depuis la PSRAM **pendant** cette fenêtre transformerait une exposition **quantifiée** (le plancher de 512 o déjà posé pour les caches LittleFS, `constants.h:864`) en exposition **binaire** : soit ça ne fauterait jamais, soit ça fauterait systématiquement selon le timing exact, sans zone intermédiaire observable. C'est le motif qui a fait pencher `pool-chemistry` contre la PSRAM.
2. **La latence.** Les accès PSRAM sont 2 à 4 fois plus lents que la RAM interne, sur un chemin parcouru à chaque ligne de journal, depuis trois tâches (`loopTask`, `mqttTask`, `async_tcp`).
3. **`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` n'est pas activé** dans le sdkconfig épinglé (pioarduino `55.03.37`, ADR-0042/0069) : une réserve `.bss` en PSRAM est **impossible** à ce build. Il faudrait réserver la PSRAM à l'exécution, dans le constructeur du singleton `Logger systemLogger` (`logger.cpp`) — qui tourne **avant** `main.cpp:152` (`heap_caps_malloc_extmem_enable()`, qui active la PSRAM). C'est exactement la cicatrice de `feature-066` : « le parachute anti-OOM était en PSRAM depuis `feature-066`, donc décoratif » — un mécanisme de sécurité réservé avant que sa ressource ne soit disponible.

### `.bss` et tas viennent de la même DRAM : ce n'est pas un ajout, c'est un relogement

`kLogArenaBytes` + `kLogSlots` + `sFlushLine` figent ~21,2 Ko de RAM interne **statique**. Mais cette mémoire venait déjà du tas — le journal y occupait ~30 Ko de `String` mobiles avant ce lot. Le net attendu est donc une **libération**, pas une consommation : ≈ −10 Ko sur le total occupé, et surtout **zéro bloc mobile** là où il y en avait ~300 (200 entrées × (message + ligne formatée)). Le gain réel n'est pas dans le solde en octets — c'est dans la disparition de la capacité à découper le plus gros bloc.

### P-3 : retiré par construction, jamais compté comme corrigé

`logger.cpp` (ancien) faisait `_persistBuffer.reserve(100)` dans le constructeur, puis `toWrite.swap(_persistBuffer)` au flush. `std::vector::swap` échange la **capacité**, pas seulement le contenu : le `reserve()` initial est consommé au premier flush et ne revient jamais — `_persistBuffer` regrandissait géométriquement à chaque cycle, et le commentaire du constructeur affirmait l'inverse de ce que faisait le code. **Décision : ne pas corriger ce défaut séparément.** `_persistBuffer` disparaît entièrement avec ce lot ; corriger deux lignes d'une structure qu'on retire n'aurait protégé personne. Ce défaut est donc **retiré par construction**, et ne doit jamais figurer dans un décompte de « bugs corrigés » de ce lot — il n'a pas été corrigé, il a cessé d'exister.

### L'inversion de propriété exigée par la condition C6 (`pool-chemistry`)

Avant ce lot, `_persistBuffer` était une file **indépendante** de l'arène : une entrée pouvait être évincée du buffer RAM circulaire tout en restant en attente de flush dans `_persistBuffer`, et réciproquement. Dériver la ligne de `/system.log` directement de l'arène **rend les deux solidaires** : une entrée évincée de l'arène avant d'avoir été flushée est désormais **définitivement perdue** pour `/system.log`, pas seulement pour le ring RAM.

- **Gain** : la profondeur de rétention passe de 100 entrées (`_persistBuffer`, éviction FIFO indépendante) à 200 (`kMaxLogEntries`, la profondeur de l'arène elle-même).
- **Perte** : sous rafale d'écriture, une ligne `WARNING` peut désormais être évincée de l'arène **avant** d'avoir atteint `/system.log`, alors que l'ancienne file dédiée la garantissait jusqu'au prochain flush.
- **Ce qui reste couvert** : `ERROR` et `CRITICAL` déclenchent un flush **synchrone** (`logger.cpp`, inchangé) — leur durabilité ne dépendait déjà pas de la profondeur RAM, et ne dépend pas plus aujourd'hui de cette inversion.
- **Observabilité** : le compteur `log_persist_overrun`, **distinct** de `log_arena_evictions`, rend le phénomène mesurable — voir `docs/API.md`. C'est la condition C6 posée par `pool-chemistry` : cette inversion devait être écrite noir sur blanc, pas déduite du diff.

### C7 : rattrapage des lignes de démarrage

Avant l'activation de la persistance (`setPersistenceFs()`, appelé après le montage de la partition `history`), les entrées de journal du tout début du boot n'étaient **jamais** persistées : le curseur de flush démarrait à 0 et refusait tout ce qui précédait. `_persistEnableSeq` est désormais figé à `_nextSeq` au moment de l'activation, et `_flushCursor` réarmé à 0 : le flush qui suit **rattrape tout le démarrage**, filtré à `WARNING`/`ERROR`/`CRITICAL` (jamais `INFO`/`DEBUG` — sinon un boot verse plus de `kMaxLogFileBytes` (8 192 o) d'`INFO` et déclenche une rotation immédiate qui emporte les lignes de sécurité qu'on vient d'écrire).

**Bénéficiaire identifié** : le `CRITICAL` de `config.cpp` — « Régulation pH en MANUEL suite à une bascule automatique non acquittée » — émis **avant** que la persistance ne soit armée. C'était la **seule trace persistante possible** de cet état de sécurité au redémarrage, et elle était perdue avant ce lot. C'est le changement visible utilisateur le plus significatif de cette spec.

### La règle « un pointeur d'arène n'est valide que sous mutex », et son corollaire

Deux types de vue **distincts, sans relation d'héritage** existent désormais :

- `LogRecord` (`logger.h`) : ne pointe **jamais** dans `sLogArena` — soit dans le tampon de l'appelant de `log()` (callback WebSocket, valide pendant tout l'appel synchrone car l'appelant est bloqué dans `log()`), soit dans le tampon fourni par l'appelant de `copyEntryByIndex()`.
- `LogArenaView` (`logger.h`) : pointe **dans** `sLogArena`, produit uniquement par les lecteurs internes exécutés **sous mutex** (`forEachRecentLog`) — valide seulement pendant l'appel du visiteur.

Les deux types sont volontairement **non convertibles implicitement** : toute tentative de faire fuir un pointeur d'arène hors mutex exige une construction explicite champ par champ, ce qui la rend visible en revue.

**Corollaire vérifié** : le callback `broadcastLog()` (WebSocket) reçoit un `LogRecord` dont `message` pointe vers le tampon de l'**appelant** de `log()`, jamais dans l'arène. C'est sûr précisément parce que la tâche appelante est **bloquée** dans `log()` pendant toute la durée du callback synchrone — elle ne peut donc pas réécrire son propre tampon pendant que le callback le sérialise.

### Le mutant non couvrable en natif

`o["message"] = entry.message` (un `const char*` brut) au lieu de `o["message"] = JsonString(entry.message, entry.len)` est un défaut qu'**aucun test natif ne peut faire échouer**. ArduinoJson 7 stocke un `const char*` **par référence** quand on lui passe un pointeur nu ; la sérialisation (`serializeJson`) a lieu **après** que le visiteur de `forEachRecentLog()` a rendu la main et que le mutex a été libéré — le pointeur peut alors être périmé. En test natif, sans concurrence réelle ni réécriture du buffer entre la capture et la sérialisation, ce bug ne se manifeste jamais : il faut une autre tâche qui réécrit l'arène dans la fenêtre exacte entre la capture du pointeur et la sérialisation JSON, une fenêtre de quelques microsecondes. Traité conformément à l'ADR-0073 : **documenté** (ici, dans `logger.h` au site d'appel, et dans `docs/subsystems/logger.md`), **porté par la revue de code** (vérification du site d'appel à chaque modification de `web_routes_data.cpp`) et par une **épreuve on-target sous charge**, mais **jamais compté comme un mutant tué** par la suite native.

### La dette assumée : le clamp UTF-8 dupliqué de `mqtt_manager.cpp`

Deux implémentations de repli UTF-8 (« ne jamais couper au milieu d'un caractère multi-octets ») coexistent dans le firmware : `utf8ClampLen()` (`src/utf8_clamp.h`, module pur partagé, utilisé par `logger.cpp` et `pump_controller.cpp`) et la boucle de recul en ligne de `MqttManager::publishAlert()` (`mqtt_manager.cpp:766`, `while (keep > 0 && (msg.charAt(keep) & 0xC0) == 0x80) keep--;`). Cette spec a créé le module partagé et migré `pump_controller.cpp`, mais **n'a pas migré** `mqtt_manager.cpp:766` : cette boucle est imbriquée dans un chemin d'alerte MQTT déjà validé par `pool-chemistry` en v2.25.0 (refus d'une précédente tentative de troncature d'alerte, qui produisait du JSON invalide côté Home Assistant). Toucher ce chemin exige une revue chimique dédiée, hors périmètre de ce lot. C'est une **implémentation résiduelle avec son motif écrit**, pas un oubli — à traiter par une spec de suivi si une troisième occurrence apparaissait.

### Le post-mortem du défaut de recouvrement (trouvé, corrigé, avant livraison)

Pendant l'implémentation, la place libre de l'arène était calculée depuis `writeOff` (la position où l'on s'apprête à écrire) plutôt que depuis `head` (la position d'écriture courante avant décision). Après un saut de fin d'arène (« pas d'enjambement », condition C4), `writeOff` retombe à 0 et la formule concluait à tort que presque tout l'anneau était libre — alors que la zone vivante commençait peut-être à l'offset 1. Résultat : `evictBytes` sortait à 0, aucune éviction n'avait lieu, et le `memcpy` **recouvrait des messages encore vivants**. Reproduit en simulation à la 508ᵉ et, indépendamment par modèle de référence, à la 853ᵉ écriture, avec les constantes réelles du projet.

Trois enseignements retenus :

1. **Les cinq compteurs de l'arène restaient parfaitement cohérents pendant la corruption.** Aucun d'eux — troncatures, évictions, dépassement de persistance, entrées courantes, octets utilisés — ne bougeait de façon anormale : la comptabilité était juste, seule la géométrie d'écriture était fausse.
2. **Un test unitaire de la phase B attendait une valeur dérivée de la formule fautive** : il confirmait le défaut au lieu de le révéler, parce que le résultat attendu avait été calculé avec le même raisonnement erroné que le code testé.
3. **Seule une comparaison à un modèle de référence indépendant** — une seconde implémentation de la géométrie de l'arène, écrite sans réutiliser la formule suspecte — a permis de trancher.

**Correctif** : la place disponible est mesurée depuis `head`, jamais depuis `writeOff` (commenté au point exact dans `log_arena_logic.cpp`). **Détecteur d'invariant ajouté en production**, indépendant de `logArenaPlan()`/`logRingEvictCount()` : `logArenaWriteOverlapsLive()` teste, juste avant tout `memcpy`, si l'intervalle d'écriture recoupe un créneau encore vivant. Sur déclenchement (compteur `log_arena_overlap_guard_trips`, **doit rester à 0 en exploitation**), le repli est d'évincer **tout** l'anneau plutôt que d'écrire par-dessus un message vivant — perdre le journal vaut mieux que le falsifier (ADR-0072 : un message de garde remplacé par un autre est pire que son absence).

## Alternatives considérées

- **A+B — Pool statique en PSRAM, réservé au démarrage** (la plus complète sur le papier) — **rejetée** : danger de cache coupé pendant `flushToDisk()` (exposition binaire, pas quantifiée), latence ×2-4, et `.bss` PSRAM fermé au sdkconfig épinglé (piège P-5, cicatrice `feature-066`). Détail ci-dessus.
- **A seul — Déporter les `String` en PSRAM sans pool dédié** — **rejetée** : même danger de cache coupé, sans même la garantie « zéro allocation dynamique » de l'option retenue.
- **B seul — Pool statique en RAM interne SANS retirer la seconde `String`** — **rejetée en l'état** : aurait fixé 20 à 30 Ko de `.bss` sans supprimer `_persistBuffer`, donc sans obtenir le gain net de contiguïté. L'option retenue (B′) intègre ce retrait.
- **E — Supprimer seulement la seconde `String` (ligne formatée), garder le vecteur de `LogEntry`** — **rejetée comme solution isolée** : réduit la fréquence de fragmentation sans fermer le mécanisme, la `String` du message principal restant allouée dynamiquement. Absorbée dans l'option retenue.
- **C — Borner la taille des `String` sans changer de structure de stockage** — **rejetée** : `pool-chemistry` avait déjà refusé une troncature de ce type en v2.25.0 (messages d'escalade coupés → JSON invalide → Home Assistant rejetait l'alerte entière). Utilisable seulement comme contrainte *à l'intérieur* d'un pool à taille fixe (ce que fait `kLogMessageMaxBytes`), jamais comme correctif isolé.
- **D — Ne pas journaliser la famine pendant la famine** — **rejetée** : supprime le signal qui a permis ce diagnostic (doctrine explicite de `constants.h` : « un refus hors PANIC prouve que le lot C est dû — c'est le signal voulu »), et n'aurait de toute façon pas suffi : l'effondrement initial (65 % de la perte) vient de la ligne de sauvegarde d'historique, pas de l'avertissement de famine.
- **`kLogMessageMaxBytes = 256`** (comptage initial d'`architect`, 718 sites, max 257 o) — **rejetée** : méthodologiquement fausse pour les messages de garde chimique, construits **avant** l'appel (`char[]` ou concaténation), où l'appel à `systemLogger` ne porte plus qu'un préfixe court. Pire cas réel mesuré par `pool-chemistry` : ≈ 630 o (`mqtt_manager.cpp:1614`, refus de mode pH + `phAutoManualReason`). Un plafond à 256 aurait réinfligé un défaut **déjà commis et corrigé** une fois (`pump_controller.cpp`, un `msg[248]` historique coupait à « …bande mor », perdant l'entrée même du défaut qu'il devait rendre traçable). Retenu : **768**, verrouillé par `static_assert(kLogMessageMaxBytes >= 640, ...)`.

## Conséquences

### Positives
- Le journal n'exécute plus **aucune** allocation dynamique dans `log()` : le mécanisme de fragmentation est fermé à la racine, pas seulement ralenti.
- Coût RAM net négatif (≈ −10 Ko attendu, mesure sur cible en cours) : le journal cesse d'occuper la ressource qu'il fragmentait, sans grossir.
- Les avertissements et erreurs émis **avant** le montage de la persistance atteignent désormais `/system.log` (C7) — un gain d'observabilité, pas seulement un correctif de fragmentation.
- Un détecteur d'invariant indépendant (`log_arena_overlap_guard_trips`) survit en production, là où l'ancien design n'avait aucun filet équivalent.
- Le refus d'une alerte MQTT tronquée (P-1, v2.25.0) est **formellement confirmé non affecté** : `publishAlert()` continue de tronquer selon sa propre logique, indépendante du journal.

### Négatives / dette assumée
- `_persistBuffer` disparaît : la garantie de durabilité pour `WARNING` sous rafale se dégrade de « file dédiée de 100 entrées » à « dérivée de l'arène de 200 entrées, partagée avec le ring RAM » (C6). Compensé par le flush synchrone `ERROR`/`CRITICAL` (inchangé) et par le compteur `log_persist_overrun`.
- Un défaut de conception silencieux — un pointeur d'arène lisible hors mutex — est **structurellement possible** et **non détectable par test natif** (`o["message"] = ptr` au lieu de `JsonString(ptr, len)`). Porté par la revue de code à chaque site consommateur, jamais fermé par la suite automatisée.
- `mqtt_manager.cpp:766` conserve sa propre boucle de clamp UTF-8, non migrée vers `src/utf8_clamp.*` — deux implémentations du même algorithme coexistent dans le firmware.
- ~21,2 Ko de `.bss` sont figés en permanence, même quand le journal est peu chargé — contre une occupation variable (0 à ~30 Ko) de l'ancien design. Un appareil qui logue peu paie le même prix qu'un appareil qui logue beaucoup.

### Ce que ça verrouille
- **Toute évolution future du format d'une entrée de journal doit tenir dans `kLogMessageMaxBytes` (768 o), sous peine de troncature avec marqueur.** Un message de garde chimique plus long qu'aujourd'hui doit être reformulé, pas simplement allongé — le plafond est verrouillé par `static_assert(kLogMessageMaxBytes >= 640, ...)`, qui interdit un retour en arrière silencieux.
- **Aucun code du firmware ne doit retenir un `LogArenaView` (ou son `.message`) au-delà de la portée du visiteur qui l'a reçu.** C'est une règle de revue permanente, pas seulement une règle de ce lot.
- **`kLogArenaBytes` doit rester en RAM interne.** Toute tentative future de le déplacer en PSRAM doit d'abord répondre au danger de cache coupé pendant `flushToDisk()` documenté ci-dessus — ce n'est pas une préférence arbitraire, c'est une contrainte de sécurité vérifiée par `pool-chemistry`.
- **Le journal ne peut plus grandir de façon incrémentale par `reserve()`/`push_back()`.** Toute augmentation de profondeur (`kMaxLogEntries`) ou de capacité (`kLogArenaBytes`) est désormais un changement de taille de `.bss` figée au link, pas un ajustement à chaud.

## Références

- Code : [`src/logger.h`](../../src/logger.h), [`src/logger.cpp`](../../src/logger.cpp), [`src/log_arena_logic.h`](../../src/log_arena_logic.h), [`src/log_arena_logic.cpp`](../../src/log_arena_logic.cpp), [`src/constants.h`](../../src/constants.h) (section « JOURNAL — arène circulaire »)
- Tests : `test/test_native_log_arena/`, `test/test_native_log_arena_integration/`, `test/test_native_log_guard_messages/`, `test/test_native_logger_logic/`
- Spec : [`specs/features/doing/feature-129-les-string-du-journal-fractionnent-le-grand-bloc.md`](../../specs/features/doing/feature-129-les-string-du-journal-fractionnent-le-grand-bloc.md)
- Doc : [`docs/subsystems/logger.md`](../subsystems/logger.md), [`docs/API.md`](../API.md)
- ADR liés : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (extraction en module pur), [ADR-0072](0072-absence-de-valeur-hors-du-domaine.md) (repli sûr = évincer plutôt que falsifier), [ADR-0073](0073-chiffre-consigne-porte-sa-methode.md) (mutants non couvrables), [ADR-0033](0033-budget-espace-partition-history.md) (budget partition `history`), [ADR-0036](0036-psram-requise-mise-a-jour-en-ligne.md), [ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md), [ADR-0078](0078-bascule-psram-cesse-d-etre-chirurgicale.md) (contexte PSRAM du projet), [ADR-0081](0081-recensement-du-tas-et-non-livraison-de-la-lecture-de-contenu.md) (instrument ayant produit la preuve)
