# ADR-0036 — PSRAM libre requise pour toute session TLS sortante (mise à jour en ligne GitHub)

- **Statut** : Accepté
- **Date** : 2026-08-13
- **Décideurs** : Nicolas (verdict architect du cycle `bug-garde-tls-trop-permissive-sans-psram`)
- **Spec(s) liée(s)** : `bug-garde-tls-trop-permissive-sans-psram` (v2.18.6), `bug-oom-accept-http-durcissement` (v2.18.5)

## Contexte

La garde bicritère de la v2.18.5 ([ADR-0035](0035-parachute-oom-new-handler.md)) devait empêcher l'ouverture d'une session TLS GitHub sur heap dégradé. **Mesure terrain du 2026-08-12** (WROOM-32 sans PSRAM, v2.18.5 en production) :

```
INFO  Vérification des mises à jour GitHub... (heap libre=87708 o, plus gros bloc=34804 o, client=192.168.1.15)
ERROR Erreur HTTP GitHub: -1 — TLS: X509 - Certificate verification failed (-9984)
WARN  Allocation heap échouée ×4 (dernière taille demandée=100 o, cumul=5)
```

La garde a **laissé passer** (87 708 ≥ 65 536 **et** 34 804 ≥ 20 480), puis le handshake a affamé le tas jusqu'à faire **refuser une allocation de 100 octets** alors que 87 Ko restaient libres. C'est **la fenêtre exacte du crash `bad_alloc` du 11/08** : une connexion HTTP arrivant pendant cette famine fait échouer le `new AsyncWebServerRequest` (356 o) dans `async_tcp` → `std::terminate` → reboot sec.

Cause du faux positif : les seuils v2.18.5 ont été calibrés sur le budget **agrégé** de la session (~58 Ko), alors que mbedTLS réclame **plusieurs blocs contigus d'environ 16 Ko**. À 34,8 Ko de plus gros bloc, le premier passe et fragmente le tas ; les suivants échouent. Sur ESP32 **sans PSRAM**, cet état est le régime **NORMAL** après quelques minutes de fonctionnement : la garde ne bloquait donc quasiment jamais alors qu'elle aurait dû presque toujours bloquer. En parallèle, `POST /download-update` — qui ouvre la **même** session TLS, puis démonte LittleFS et efface une partition — n'avait **aucune garde**.

Le BOM du PCB v2 documente déjà le module cible **ESP32-DevKitC-VE / WROVER-E (8 Mo de PSRAM)** : il faut trancher entre « fonction de découverte de version disponible mais dangereuse » et « contrat matériel assumé ».

## Décision

Toute **session TLS sortante** (`GET /check-update` **et** `POST /download-update`) exige au moins `kOtaTlsMinPsramBytes` = **1 048 576 o (1 Mo) de PSRAM LIBRE**, évalués **en premier** et **fail-closed**, avant le bicritère heap historique.

- Le prédicat pur partagé `oomTlsGateOk` (booléen) est **remplacé** par `oomTlsGateReason(...)` ([`src/oom_guard_logic.h`](../../src/oom_guard_logic.h)), qui retourne un **motif** : `kOomTlsGateOk` / `kOomTlsGateNoPsram` / `kOomTlsGateLowMemory`. L'appelant doit distinguer un refus **matériel** (définitif, oriente vers l'upload manuel) d'un refus **mémoire** (temporaire, réessayable).
- Le critère porte sur la **PSRAM LIBRE** (`ESP.getFreePsram()`) et **non** sur la taille installée (`ESP.getPsramSize()`, constante et jamais décroissante) : avec `SPIRAM_USE_MALLOC` les allocations > 4 Ko partent en PSRAM, donc une PSRAM **saturée** ne protège de rien — mbedTLS retomberait sur la RAM interne, exactement le régime de crash. Sur un module de 4/8 Mo, exiger 1 Mo libre couvre à la fois l'absence de PSRAM et sa saturation.
- Dans `/download-update`, la garde est placée **après** la validation `url` / allowlist / `digest` (une requête malformée reste un `400`/`403`) et **avant** `WiFiClientSecure`, `setOtaInProgress()`, `LittleFS.end()` et tout `esp_partition_erase_range()` : un refus n'a **aucun effet de bord** (pas de pompes gelées, pas de FS démonté, rien d'effacé).
- Les seuils heap `kCheckUpdateMinFreeHeapBytes` (65 536) et `kCheckUpdateMinMaxAllocBytes` (20 480) sont **inchangés** : ils restent le second filtre, applicable aux modules PSRAM.

## Alternatives considérées

- **Relever le seuil maxBloc à 48-50 Ko au lieu d'exiger la PSRAM** (rejetée) — protection **illusoire** : le parachute OOM immobilise 12 Ko ([ADR-0035](0035-parachute-oom-new-handler.md)), donc le plus gros bloc nominal d'un WROOM tombe de ~49 Ko à ~37 Ko. Un seuil à 48-50 Ko ne passerait **que parachute désarmé**, c'est-à-dire précisément quand le filet anti-`bad_alloc` est absent. Et même à 49 Ko, mbedTLS a besoin de **plusieurs** blocs de 16 Ko : le premier consommerait le gros bloc et les suivants échoueraient — l'échec resterait la règle.
- **Laisser tenter et rattraper l'échec** (rejetée) — c'est le comportement mesuré le 12/08 : la tentative échoue **de toute façon**, mais après avoir mis **tout le système** en famine pendant ~1 s (allocation de 100 o refusée), donc en risque de reboot sec sur la première connexion HTTP entrante. Payer un risque global pour une fonction qui ne peut pas réussir n'a pas de contrepartie.
- **Garder `/download-update` sans garde** (rejetée) — l'UI n'y accède qu'après un `/check-update` réussi, mais la route est publique et authentifiée : un `POST` direct contournerait la garde, avec un **flash à la clé** (FS démonté, partition effacée) pendant une famine mémoire. Une garde qui se contourne par la route voisine n'est pas une garde.
- **Critère sur la PSRAM installée (`getPsramSize() > 0`)** (rejetée) — plus simple à lire, mais aveugle à la **saturation** : une PSRAM installée et pleine renvoie la même valeur qu'au boot et rouvre exactement le régime de crash.
- **PSRAM libre ≥ 1 Mo, fail-closed, sur les deux routes** (retenue) — aligne la garde sur le **contrat matériel du BOM**, transforme un échec aléatoire et dangereux en refus immédiat, explicite et sans effet de bord, et reste correcte sur module PSRAM partiellement consommée.

## Conséquences

### Positives

- Sur WROOM, la fenêtre de famine TLS de ~1 s (le contexte du crash du 11/08) **n'est plus ouverte du tout** : refus immédiat, aucun octet alloué pour le handshake.
- Message utilisateur **actionnable** au lieu d'un « Erreur HTTP GitHub: -1 » opaque : « Mise à jour en ligne indisponible : ce module n'a pas de PSRAM (connexion sécurisée GitHub impossible). Utilisez la mise à jour par upload de fichier. » (`code: "psram_required"`).
- `/download-update` cesse d'être un contournement : les deux routes partagent **le même prédicat pur**, testable en natif ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) — aucune comparaison de seuil dans les handlers.
- Les WARN de refus sont auto-explicatifs on-target (motif, PSRAM libre, les deux mesures heap, les trois seuils, IP client).

### Négatives / dette assumée

- **La découverte de version en ligne est indisponible sur WROOM** (le parc actuel de production) : `/check-update` et `/download-update` répondent `503` systématiquement. Repli documenté et pleinement fonctionnel : **upload de fichier** (`POST /update`, Paramètres > Système) ou `./deploy.sh ota-firmware` / `ota-fs` — aucune de ces voies n'ouvre de session TLS sortante.
- `kOtaTlsMinPsramBytes` = 1 Mo est un ordre de grandeur **jamais mesuré sur WROVER réel** (la session TLS y consomme surtout de la PSRAM, budget attendu très inférieur) : valeur volontairement large, à confirmer à la mise en service du premier module PSRAM.
- La garde ne dit rien de la **qualité** de la PSRAM restante (fragmentation PSRAM non mesurée) — hypothèse acceptée tant qu'aucune mesure ne la contredit.

### Ce que ça verrouille

- **Contrat matériel** : la mise à jour en ligne devient une fonctionnalité *conditionnée au module PSRAM du BOM PCB v2*. Tout retour à un module sans PSRAM en production implique d'assumer l'indisponibilité de cette fonction (ou de rouvrir cet ADR).
- **Toute nouvelle session TLS sortante** (future API externe, webhook, NTP sécurisé…) doit passer par `oomTlsGateReason` — ouvrir une `WiFiClientSecure` sans cette garde est un défaut de revue.
- Les seuils heap restent liés à la réserve du parachute (verrouillage déjà posé par [ADR-0035](0035-parachute-oom-new-handler.md)) ; s'y ajoute désormais `kOtaTlsMinPsramBytes`, à re-mesurer au premier WROVER en service.
- Le contrat de sortie de la garde est un **motif**, pas un booléen : tout nouvel appelant doit gérer explicitement `kOomTlsGateNoPsram` (refus définitif) et `kOomTlsGateLowMemory` (réessayable).

> **Note additive 2026-08-13 (v2.18.7) — présentation utilisateur du refus, décision technique inchangée**
>
> Après essai réel de la v2.18.6, le **texte** du 503 `psram_required` a été reformulé dans le helper partagé (donc sur les deux routes) : « La mise à jour en ligne n'est pas disponible. Votre matériel ne dispose pas d'une mémoire PSRAM (module ESP32-DevKitC-VE nécessaire). Utilisez la mise à jour manuelle. » — l'ancienne formulation citée dans les *Conséquences positives* ci-dessus présentait une caractéristique matérielle comme une erreur et employait un vocabulaire (« upload de fichier ») absent de l'interface. En complément, l'indisponibilité est désormais **annoncée dans l'UI avant toute tentative** : sur `psram_size === 0` (`/get-system-info`), la carte « Mise à jour en ligne (GitHub) » affiche un avertissement permanent renvoyant vers « Mise à jour manuelle » et ses boutons sont neutralisés ; le 503 reste le filet et s'affiche en toast **informatif**, sans préfixe d'erreur.
>
> **Rien de la décision n'est modifié** : critère PSRAM **libre** ≥ `kOtaTlsMinPsramBytes` (1 Mo), évaluation **fail-closed en premier**, application aux **deux** routes, seuils heap, contrat de sortie à motifs, statut `503` et valeur `code: "psram_required"` (seul contrat machine — le champ `error` est un texte utilisateur, reformulable). Détail : [subsystems/oom-guard.md](../subsystems/oom-guard.md), [features/page-settings.md](../features/page-settings.md), spec `specs/features/done/ux-message-psram-mise-a-jour-en-ligne.md`.

> **Note additive 2026-08-13 (v2.18.7, correctif de revue) — le refus se décline en TROIS codes machine, garde inchangée**
>
> La note précédente conservait un code unique `psram_required` pour l'ensemble du motif `kOomTlsGateNoPsram`, qui couvre pourtant deux situations physiquement différentes : PSRAM **absente** (permanent) et PSRAM **saturée** (temporaire). Le contrat HTTP des deux routes compte désormais trois codes :
>
> | `code` | Condition | Nature |
> |---|---|---|
> | `psram_required` | `ESP.getFreePsram() == 0` | permanent (matériel) — neutralise la mise à jour en ligne côté UI |
> | `psram_low` | PSRAM présente, libre < `kOtaTlsMinPsramBytes` | temporaire, réessayable |
> | `tls_low_memory` | motif `kOomTlsGateLowMemory` (texte du message inchangé) | temporaire, réessayable |
>
> La discrimination absente/saturée est faite **dans la route** (`refuseIfTlsGateClosed`) par un test de nullité de la mesure déjà en main — **aucun seuil dupliqué**, aucun quatrième motif : le prédicat pur `oomTlsGateReason()` et ses tests sont **inchangés**. Le contrat de sortie à motifs reste celui posé plus haut ; la scission est une **information de présentation**, pas une décision de garde. Conséquence pour les clients : `code` fait contrat sur les **trois** refus et le front ne reconnaît plus jamais le **texte** du message ; il doit traiter `psram_low` / `tls_low_memory` comme réessayables et se resynchroniser sur `/get-system-info`. **Le reste de la décision est intact** : critère PSRAM libre, ordre fail-closed, seuils heap, position de la garde dans `handleDownloadUpdate`, parachute OOM ([ADR-0035](0035-parachute-oom-new-handler.md)).

## Références

- Code : [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) (`oomTlsGateReason`, motifs `kOomTlsGate*`), [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (`refuseIfTlsGateClosed`, appelée par `handleCheckUpdate` et `handleDownloadUpdate`), [`src/constants.h`](../../src/constants.h) (`kOtaTlsMinPsramBytes`, `kCheckUpdateMinFreeHeapBytes`, `kCheckUpdateMinMaxAllocBytes`)
- Spec : `specs/features/doing/bug-garde-tls-trop-permissive-sans-psram.md` (mesure terrain du 12/08, arbitrages D1-D4)
- Doc : [subsystems/oom-guard.md](../subsystems/oom-guard.md), [API.md](../API.md#get-check-update--write), [UPDATE_GUIDE.md](../UPDATE_GUIDE.md)
- Tests : `test/test_native_oom_guard/` (motifs de la garde, frontières des trois seuils) — 466 tests natifs au total
- ADR liés : [ADR-0035](0035-parachute-oom-new-handler.md) (parachute OOM — la garde v2.18.5 y était un corollaire), [ADR-0032](0032-reponses-http-bornees.md) (session TLS déclarée « hors périmètre, assumée » en 2026-08-04), [ADR-0034](0034-migration-core3-idf5.md) (note WROVER/PSRAM), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable)
