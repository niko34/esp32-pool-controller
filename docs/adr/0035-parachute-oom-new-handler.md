# ADR-0035 — Parachute OOM global : `std::set_new_handler` + réserve d'urgence libérable + callback IDF flag-seul

- **Statut** : Accepté
- **Date** : 2026-08-12
- **Décideurs** : Nicolas (verdict architect du cycle `bug-oom-accept-http-durcissement`)
- **Spec(s) liée(s)** : `bug-oom-accept-http-durcissement` (v2.18.5)

## Contexte

Crash de production le **2026-08-11 21:47** (v2.18.4, core 3, WROOM) : `std::bad_alloc` **non rattrapé** dans la tâche `async_tcp` — `operator new(356)` = `new AsyncWebServerRequest` (fork ESP32Async, `WebServer.cpp:50`) à l'acceptation d'une connexion HTTP → `std::terminate` → abort. Cinétique (ring heap RTC) : heap stable, puis **−59 904 o en ~5 s** par un consommateur autonome resté **non attribué** (hypothèses save historique / clients gelés / rafale de page **réfutées par mesure** ; candidat restant : buffers RX du driver WiFi en tempête).

Le problème structurel dépasse ce crash : sur core Arduino, les **exceptions C++ sont activées** — un `new` qui échoue **lance**, il ne retourne jamais `nullptr`. Toutes les gardes `if (x == NULL)` des libs (dont ESPAsyncWebServer) sont **mortes**, et le `new` fautif peut être **n'importe où** (libs externes non patchables, `String`, conteneurs). Aucun `try/catch` localisé ne peut couvrir tous les points d'allocation ; le comportement d'échec d'allocation est **GLOBAL et invisible au point d'appel** — il faut trancher au niveau global, maintenant, car chaque famine ponctuelle coûte un reboot sec sans trace applicative.

## Décision

Le firmware installe un **parachute OOM hybride** (module `oom_guard`, logique pure `oom_guard_logic.h`) :

1. **`std::set_new_handler`** + **réserve d'urgence libérable** de `kOomReserveBytes` = 12 288 o, pré-allouée **au plus tôt du `setup()`** (bloc contigu avant toute fragmentation). À la première famine, le handler libère la réserve — le `new` en cours est **re-tenté par libstdc++ et réussit** (contrat new_handler) au lieu d'aborter. Chemin d'urgence strict : **aucune allocation, aucun log, aucun mutex** — extraction atomique + `free` + compteurs ; le log CRITICAL « Réserve OOM consommée » est différé dans la loop (one-shot par épisode). Réserve épuisée → désenregistrement du handler : le `new` suivant lève `bad_alloc` = **statu quo ante** (coredump en filet ultime).
2. **Réarmement à hystérésis** depuis la loop (`kOomRearmMinMaxAllocBytes` = 32 Ko, cadence 10 s), critère fondé sur l'**absence de réserve** (pointeur nul) — pas sur l'état — pour converger malgré une course handler/réarmement (durcissement de revue).
3. **`heap_caps_register_failed_alloc_callback` en COMPLÉMENT, mode flag-seul** (diagnostic) : compte les échecs d'allocation **y compris les `malloc` C** (lwIP, mbedTLS) que le new_handler ne voit pas ; drainé en WARN throttlé (30 s) depuis la loop.

En corollaire, une **garde bicritère** protège `/check-update` **avant** la session TLS (~58 Ko) : refus 503 si heap libre < 65 536 o OU maxBloc < 20 480 o (prédicat pur partagé `oomTlsGateOk`).

> **Note additive 2026-08-13 (v2.18.6)** — le corollaire ci-dessus est **complété et durci** par [ADR-0036](0036-psram-requise-mise-a-jour-en-ligne.md) : la mesure terrain du 12/08 a montré que le bicritère seul laissait passer le régime NORMAL d'un WROOM (free=87 708 / maxBloc=34 804) puis affamait le tas jusqu'à refuser une allocation de 100 o. Le prédicat `oomTlsGateOk` (booléen) est **remplacé** par `oomTlsGateReason(...)` (motifs `kOomTlsGateOk` / `kOomTlsGateNoPsram` / `kOomTlsGateLowMemory`), qui exige d'abord `kOtaTlsMinPsramBytes` = 1 Mo de **PSRAM libre** (fail-closed) et s'applique **aussi** à `/download-update`. Les deux seuils heap et l'ensemble de la décision de cet ADR (réserve, handler, réarmement, callback flag-seul) restent **inchangés**.

## Alternatives considérées

- **Callback IDF seul** (`heap_caps_register_failed_alloc_callback`) (rejetée comme mécanisme principal, **retenue en complément flag-seul**) — le callback est purement informatif : il n'offre **aucun contrat de re-tentative** de l'allocation (l'échec suit son cours, le `new` lève quand même `bad_alloc`). Il apporte en revanche ce que le new_handler ne voit pas : les échecs des `malloc` C internes.
- **`try/catch` localisés autour des points d'allocation à risque** (rejetée) — le point de crash réel est dans **ESPAsyncWebServer** (lib externe épinglée, non patchable sans fork), et le prochain `bad_alloc` frappera ailleurs : couverture par énumération impossible, la protection doit être globale.
- **`-fno-exceptions`** (faire retourner `nullptr` aux `new`) (rejetée) — les libs de l'écosystème ne sont **pas null-safe partout** : un `new` retournant `nullptr` sur du code qui suppose une exception (ou qui déréférence sans garde) = **comportement indéfini** ; changement de flag global invasif et non testé sur toute la chaîne.
- **Hybride new_handler + réserve + callback flag-seul** (retenue) — seule option qui donne un **contrat de re-tentative** (l'allocation vitale réussit), reste sûre en contexte handler (aucune alloc), couvre le diagnostic des `malloc` C, et dégrade exactement vers le comportement antérieur quand la réserve est épuisée.

## Conséquences

### Positives

- Une famine ponctuelle est **absorbée** (l'accept HTTP du 11/08 aurait réussi) et produit un **CRITICAL exploitable** au lieu d'un abort sec ; l'auto-guérison ([ADR-0029](0029-auto-guerison-heap.md)) garde ~1,5× son seuil de marge (12 288 = 1,5 × 8 192 o) pour déclencher un reboot **propre** (pompes coupées, cumuls persistés).
- Instrumentation d'attribution pour la récidive : compteur/taille des échecs `malloc` C (WARN throttlé), IP client sur `/check-update`, dump ring enrichi (uptime absolu + reset_reason brut).
- Dégradation exacte vers le statu quo ante (réserve épuisée → `bad_alloc` → coredump) : le parachute ne peut pas faire **pire** que l'existant.

### Négatives / dette assumée

- **Tout `new` du firmware peut désormais « réussir en consommant la réserve »** : le comportement d'allocation est global et invisible au point d'appel — un code qui « marche » peut vivre sur la réserve pendant un épisode (le CRITICAL one-shot est la seule trace). Ce n'est **pas** une solution à la famine : le containment de fond reste l'auto-guérison [ADR-0029](0029-auto-guerison-heap.md).
- **Limite `malloc` C** : lwIP/mbedTLS/drivers échouent toujours en `NULL` — le parachute ne les re-tente pas (diagnostic seulement via le callback).
- 12 Ko de RAM immobilisés en permanence (relâchés précisément quand le système en a le plus besoin).

### Ce que ça verrouille

- **Les seuils de garde mémoire supposent la réserve** : `kCheckUpdateMinFreeHeapBytes` / `kCheckUpdateMinMaxAllocBytes` (et tout futur seuil de garde) sont dimensionnés avec 12 Ko soustraits du heap visible — modifier `kOomReserveBytes` impose de re-vérifier les gardes (et réciproquement).
- Le new_handler est un **point global unique** : tout futur mécanisme OOM (pools dédiés, seconde réserve) doit composer avec lui, pas le remplacer silencieusement.
- Le chemin handler reste **interdit d'allocation/log/mutex** — toute évolution du module doit préserver ce contrat (logs différés via la loop uniquement).

## Références

- Code : [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h), [`src/oom_guard.h`](../../src/oom_guard.h) / [`src/oom_guard.cpp`](../../src/oom_guard.cpp), [`src/main.cpp`](../../src/main.cpp) (init + tick), [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (garde 503), [`src/constants.h`](../../src/constants.h) (6 constantes `kOom*` / `kCheckUpdateMin*`)
- Spec : `specs/features/done/bug-oom-accept-http-durcissement.md` (investigation mesurée + addendum « honnêteté documentaire »)
- Doc : [subsystems/oom-guard.md](../subsystems/oom-guard.md), [API.md](../API.md#get-check-update--write)
- Tests : `test/test_native_oom_guard/` (18 tests)
- ADR liés : [ADR-0029](0029-auto-guerison-heap.md) (containment de fond), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure), [ADR-0034](0034-migration-core3-idf5.md) (note WROVER/PSRAM sur les seuils TLS)
