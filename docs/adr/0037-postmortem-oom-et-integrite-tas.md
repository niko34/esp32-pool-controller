# ADR-0037 — Post-mortem d'allocation en RAM RTC + contrôle d'intégrité périodique du tas

- **Statut** : Accepté
- **Date** : 2026-08-14
- **Décideurs** : Nicolas (verdict architect du cycle `bug-parachute-oom-jamais-rearme-et-postmortem`, gate `pool-chemistry` GO en P1 sous conditions puis GO en P2 sur le diff)
- **Spec(s) liée(s)** : `bug-parachute-oom-jamais-rearme-et-postmortem` (v2.18.8)

## Contexte

**Deux crashs de production en trois jours**, tous deux par échec d'allocation d'une **petite** structure :

- **11/08/2026** (v2.18.4) — `operator new(356)` (`new AsyncWebServerRequest`, accept HTTP) après un effondrement du tas de ~60 Ko en ~5 s. Réponse : le parachute OOM ([ADR-0035](0035-parachute-oom-new-handler.md), v2.18.5).
- **13/08/2026** (v2.18.7, uptime 12,9 h) — `new AsyncFileResponse` (**156 o**) en servant un fichier statique, puis échec de l'allocation de l'objet exception (**100 o**) → `std::terminate` → abort. **L'anneau RTC montre un tas SAIN 2 s avant** : `free = 81 408`, `maxBloc = 27 584`, **stables à l'octet près depuis 78 s**. Un `malloc` de 156 o ne peut pas échouer dans cet état.

Le 13/08 laisse **deux hypothèses non départageables** avec l'instrumentation d'alors : **(a)** consommateur très rapide dans l'angle mort de 2 s entre deux échantillons de l'anneau ; **(b)** **corruption du tas** — la comptabilité annonce 81 Ko libres alors que l'allocateur ne sert plus 156 octets.

Trois contraintes cadrent toute réponse :

1. **Les traces différées sont perdues.** Le CRITICAL « Réserve OOM consommée » est émis par la loop → jamais atteint quand l'abort suit immédiatement ; le journal persistant l'est aussi (flush toutes les 10 min). **Seule la RAM RTC noinit survit.**
2. **Le chemin d'échec d'allocation est réentrant-interdit.** `heap_caps_register_failed_alloc_callback` est appelé **depuis l'intérieur de l'allocateur**, qui **détient déjà le verrou du composant heap** : tout appel `heap_caps_*` (donc `ESP.getFreeHeap()` / `ESP.getMaxAllocHeap()`) y est un **deadlock**. S'y ajoutent les interdits déjà posés par [ADR-0035](0035-parachute-oom-new-handler.md) : aucune allocation, aucun mutex, aucun log.
3. **Le parachute lui-même était inopérant.** Pour que libstdc++ lève, il faut `get_new_handler() == nullptr` : le handler s'était désenregistré et le réarmement exigeait `maxBloc ≥ 32 768 o`, alors que le plus gros bloc **réel** de l'appareil vaut **29 684 o à 33 min** et **27 584 o à 12,9 h**. Le seuil était **au-dessus du régime normal** → la réserve n'était **jamais** ré-allouée : parachute **one-shot** en pratique. Erreur de calibrage introduite en v2.18.5.

## Décision

Le firmware **instrumente pour trancher** (il ne prétend pas réparer une cause racine encore inconnue) et **rétablit** la protection perdue, en quatre volets solidaires :

1. **Recalibrage du réarmement sur le régime mesuré** : `kOomRearmMinMaxAllocBytes` = **24 576 o** (= 2 × `kOomReserveBytes`), plus un **palier de repli** `kOomFallbackReserveBytes` = 2 048 o armé dès `kOomFallbackRearmMinMaxAllocBytes` = 12 288 o. Surclassement repli → nominal autorisé, **déclassement interdit**. Principe : « mieux vaut un petit parachute qu'aucun » — 2 Ko couvrent 8× le duo fautif du 13/08 (156 + 100 o).
2. **Post-mortem d'allocation en RAM RTC noinit** : 3ᵉ zone (magic `0xF00DFA11`, **32 octets**), écrite par les chemins d'échec en **stores bruts alignés uniquement**. Elle porte taille du 1ᵉʳ et du dernier échec, `caps`, compteur, uptimes, drapeaux et compteur d'épisodes. **Le drapeau « réserve consommée » EST l'octet `flags`** de cette zone : posé immédiatement par le handler, il survit à un abort qui suit dans la microseconde. La zone est **dumpée au boot suivant**, en **une ligne unique**, juste après le dump de l'anneau.
3. **Miroir de l'anneau comme seule source « free / maxBloc »** : la contrainte n°2 interdisant toute mesure depuis le chemin d'échec, le post-mortem consomme un **miroir atomique du dernier échantillon de l'anneau** (`oomGuardNoteHeapSample()`), à la **cadence inchangée de 2 s**, avec des valeurs **déjà mesurées** — **aucun parcours de tas supplémentaire**. Le miroir et le chemin d'échec partagent la même horloge (`xTaskGetTickCount()`, sans verrou) pour que le calcul d'âge reste juste.
4. **Verdict + contrôle d'intégrité périodique** : la fonction pure `oomPostMortemVerdict()` conclut **FAMINE MÉMOIRE** si `failCount > kOomCorruptionMaxFails` (8, **strict**) **et** que les échecs forment une **rafale** (span ≤ `kOomCorruptionBurstSpanS` = 60 s), ou si la comptabilité est écroulée ; **SUSPICION CORRUPTION** si la comptabilité est saine (`maxBloc ≥ 4 ×` la taille demandée) ; **INDÉTERMINÉ** sinon. En parallèle, `heap_caps_check_integrity_all()` s'exécute toutes les **5 min** (durée attendue 1-3 ms sous `CONFIG_HEAP_POISONING_LIGHT`), **autolimité** (WARN > 5 ms, désactivation définitive > 20 ms) et **sous 8 gardes de sécurité chimique cumulatives** (mode maintenance, OTA, dosage pH, dosage ORP, injection manuelle pH, injection manuelle ORP, **pompe énergisée**, uptime minimal), appelé **après** `PumpController.update()`. Une corruption détectée coupe et inhibe le dosage puis sort par le **chemin propre existant** (`performFailSafeRestart`, alerte MQTT `heap_corruption`) ou par le mode maintenance si le plafond de redémarrages est atteint.

## Alternatives considérées

- **Échantillonner le tas toutes les 200-500 ms au lieu de 2 s** (rejetée) — c'était la réponse « évidente » à l'angle mort de 2 s. Mais `heap_caps_get_largest_free_block()` / `ESP.getMaxAllocHeap()` s'exécutent **interruptions coupées** : quadrupler à décupler leur fréquence **injecterait de la gigue dans le phénomène même qu'on mesure** (et dans la régulation), pour **zéro pouvoir discriminant** — un échantillon plus fin ne dit toujours pas si l'allocateur ment. Le post-mortem, lui, capture l'état **au moment exact** de l'échec sans aucune mesure supplémentaire.
- **Activer `CONFIG_HEAP_POISONING_COMPREHENSIVE`** (rejetée) — c'est le détecteur de corruption le plus fin (remplissage et vérification octet par octet de chaque bloc), mais son coût est **prohibitif** : surcoût sur **chaque** `malloc`/`free` du firmware, y compris dans les chemins temps réel (`async_tcp`, LEDC, MQTT), et durée du contrôle global sans commune mesure avec les 1-3 ms observées en `LIGHT`. Le mode `LIGHT` déjà en place suffit à détecter en-têtes et canaris corrompus.
- **Ne rien changer au réarmement et se contenter d'observer** (rejetée) — le parachute serait resté **inopérant après son premier déclenchement** : on aurait instrumenté un système sans protection. Le recalibrage est indissociable de l'instrumentation.
- **Stocker le post-mortem dans le journal persistant / NVS** (rejetée) — écrire sur LittleFS ou en NVS depuis le chemin d'échec est impossible (allocations, verrous, latence) ; et c'est justement la persistance différée qui a échoué le 13/08. La RAM RTC noinit est la **seule** mémoire écrivable en quelques stores bruts et survivant au reset.
- **`ESP.restart()` nu sur corruption détectée** (rejetée) — **viole la sécurité chimique** : un redémarrage sans coupure préalable des pompes ni persistance des cumuls journaliers peut laisser une pompe énergisée jusqu'au reset et fausser l'anti-sur-dosage au reboot. La sortie passe obligatoirement par `performFailSafeRestart()` ([ADR-0029](0029-auto-guerison-heap.md)) ou le mode maintenance.
- **Retenue : post-mortem RTC + miroir + verdict + intégrité périodique + recalibrage** — seule combinaison qui capture l'état au **point d'échec**, sans réentrer dans l'allocateur, sans ajouter de charge de mesure, tout en rétablissant une protection réellement armée et en ajoutant un détecteur **direct** de l'hypothèse de corruption.

## Conséquences

### Positives

- **Une seule récidive suffira à trancher** famine (a) vs corruption (b) : la ligne de post-mortem donne taille demandée, `caps`, uptime, état comptabilisé du tas, drapeaux et **verdict**, avec la cinétique de l'anneau juste au-dessus.
- Le parachute est **réellement armé en régime établi** (24 Ko atteignables ; 27-30 Ko mesurés), et **toujours armé au moins au palier de repli** dès 12 Ko de plus gros bloc.
- Le drapeau « réserve consommée » **survit à un abort immédiat**, ce que le CRITICAL différé ne faisait pas.
- Détection **directe** de la corruption du tas, avec sortie **fail-safe chimique** et alerte MQTT dédiée.
- **Aucune charge de mesure ajoutée** : la cadence de l'anneau reste 2 s, le contrôle d'intégrité coûte 1-3 ms toutes les 5 min, pompes à l'arrêt.

### Négatives / dette assumée

- **Trace indicative, pas comptable** : `failCount` et les uptimes sont mis à jour en lecture-modification-écriture **sans verrou** (interdits sur ce chemin) — un incrément peut être perdu si deux échecs tombent simultanément sur les deux cœurs.
- **Perte possible du post-mortem n-2** : l'instantané pris par `initOomGuard()` vit en RAM ordinaire ; un crash entre l'init et le dump l'emporte. Compromis accepté contre le risque inverse (un échec pendant `setup()` polluant la zone du boot précédent).
- **Uptimes tronqués sur 16 bits** (repliement toutes les 18 h 12) : les durées sont calculées modulo, et la validité de la zone ne dépend **pas** de l'ordre des uptimes — un contrôle d'ordre rendait le dump muet au-delà de 18 h, c'est-à-dire exactement dans le régime des deux crashs.
- **Le verdict est une heuristique** : il oriente le diagnostic, il ne prouve rien. La fenêtre de rafale a été ajoutée en revue précisément parce que la règle initiale concluait « famine » sur le scénario du 13/08.
- **32 octets de RAM RTC** immobilisés en permanence (budget total des 3 zones : 1073 o sur 8192).
- **Ce cycle n'explique pas le crash du 13/08** : la cause racine reste **inconnue**.

### Ce que ça verrouille

- **Une échelle de 5 seuils interdépendants** sur le plus gros bloc contigu : 8 192 (auto-guérison) < 12 288 (réarmement repli) < 16 384 (push WS) < 20 000 (WARN heap) < 24 576 (réarmement nominal). Aucun ne se modifie isolément : leur ordre **est** la sémantique de la dégradation.
- **Une 3ᵉ zone RTC noinit** avec magic propre : tout futur usage de la RAM RTC doit composer avec les 1073 o déjà engagés et ne jamais réutiliser `0xA5C0FFEE`, `0xB10C4EA9` ni `0xF00DFA11`.
- **L'interdiction réentrante devient un contrat écrit** : aucun `heap_caps_*` ni aucune API mesurant le tas depuis le callback d'échec d'allocation ou depuis le `new_handler` — toute donnée de tas y arrive nécessairement par un **miroir** alimenté ailleurs.
- **Le contrôle d'intégrité est subordonné à la sécurité chimique** : ses 8 gardes et sa position **après** `PumpController.update()` ne peuvent pas être assouplies sans un nouveau passage `pool-chemistry` (retard maximal admissible : 65 ms).
- **`heap_corruption` est un contrat MQTT public** : type unique, publié par les deux branches (redémarrage propre et mode maintenance).

## Références

- Code : [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) (format, verdict, paliers, cadence d'intégrité — logique pure), [`src/oom_guard.cpp`](../../src/oom_guard.cpp) (zone RTC, chemins d'échec, dump, `oomGuardIntegrityTick`), [`src/heap_ring.cpp`](../../src/heap_ring.cpp) (miroir), [`src/main.cpp`](../../src/main.cpp) (dump au boot, 8 gardes, `handleHeapCorruptionDetected`), [`src/pump_controller.h`](../../src/pump_controller.h) (`isAnyPumpEnergized`), [`src/constants.h`](../../src/constants.h)
- Spec : `specs/features/doing/bug-parachute-oom-jamais-rearme-et-postmortem.md`
- Doc : [subsystems/oom-guard.md](../subsystems/oom-guard.md), [subsystems/system-health.md](../subsystems/system-health.md), [MQTT.md](../MQTT.md#alertes)
- Tests : `test/test_native_oom_guard/` (66 tests, +41)
- ADR liés : [ADR-0035](0035-parachute-oom-new-handler.md) (parachute OOM — contrat complété par le présent ADR), [ADR-0029](0029-auto-guerison-heap.md) (redémarrage propre fail-safe), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable en natif)
