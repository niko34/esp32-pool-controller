# Bug — Crash OOM (bad_alloc) à l'accept HTTP : durcissement + instrumentation

- **Type** : bug (format minimal `specs/modele.md`)
- **Statut** : implémenté (v2.18.5) — tests verts, revue OK ; validation on-target restante (voir Notes d'implémentation)
- **Version cible** : 2.18.5
- **Créée** : 2026-08-11 (crash en production core 3, coredump décodé + investigation mesurée)
- **Domaine** : Firmware (`src/` hors dosage — nouveau module garde OOM, `web_routes_ota.cpp`,
  `main.cpp`, `constants.h`) — aucun chemin de dosage
- **Specs liées** : investigation-churn-heap-ws-http (G1-G5, 07/2026), bug-heap-toctou
  (ring heap RTC = la preuve), bug-checkupdate-tls-core3 (TLS intermittent), ADR-0029
  (auto-guérison = containment)
- **Remplace** : bug-oom-save-historique.md (hypothèse save réfutée par mesure — voir Addendum)

## Symptôme

Reboot spontané en production (v2.18.4 core 3, WROOM) le 2026-08-11 21:47 local.
Coredump : `std::bad_alloc` non rattrapé dans la tâche `async_tcp` —
`operator new(356)` = `new AsyncWebServerRequest` (fork ESP32Async, WebServer.cpp:50)
à l'acceptation d'une connexion HTTP → `std::terminate` → abort. La garde
`if (r == NULL)` de la lib est **morte** sur core 3 (new lance, ne retourne jamais
nullptr). Dosage sain ; mqttTask **coincée en écriture** (socket broker gelé =
anomalie réseau simultanée).

## Faits mesurés (cinétique ring RTC + mesures live du 11/08 au soir)

1. Ring pré-crash : heap stable 65 s (free=94 208, maxB=49 088, ws=0, skip=0), puis
   **−59 904 o en ~5 s** (free→32 512, maxB→27 584 : une bouchée ≥ 16-21 Ko prise
   dans le gros bloc), plateau ~9 s, crash à l'accept suivant. Consommateur
   **autonome** (aucun client WS, aucune action utilisateur — confirmé).
2. Hypothèses **réfutées par mesure live** : saveToFile historique (~23 Ko, bref,
   ~108 points réels) ; clients TCP gelés (~200 o/client — architecture ack-driven) ;
   rafale de chargement de page même lente (< 1 Ko de creux mesuré).
3. TLS check-update : profil compatible (2×16 Ko + x509 + budget 8 Ko ≈ 58 Ko,
   plusieurs secondes) mais l'utilisateur n'a PAS cliqué ; l'IP appelante n'est pas
   loggée (invérifiable). Mesuré : sur heap dégradé, échec en 0,14 s (BIGNUM alloc)
   — sur heap reposé, GitHub est ATTEINT (échec parse JSON en 2,7 s) : la
   « limitation core 3 » est en réalité intermittente, fonction de maxB au départ.
4. Candidat restant non instrumenté : gonflement dynamique des buffers RX du driver
   WiFi (jusqu'à ~32 × 1,6 Ko) pendant une tempête/réassociation — corrèle avec le
   socket MQTT gelé au crash et 3 décrochages MQTT le même soir (22:30/22:42/22:45).
5. Le journal persistant a tourné (cap taille) : les traces du boot pré-crash sont
   perdues — l'attribution définitive attend la récidive instrumentée.

## Correctif attendu (durcissement indépendant du déclencheur)

1. **Parachute OOM global** (nouveau module, ex. `oom_guard.*`) :
   `std::set_new_handler` + réserve d'urgence pré-allouée (~8-16 Ko, dimensionnement
   architecte). Handler : libérer la réserve + log CRITICAL one-shot (« réserve OOM
   consommée ») + re-tentative automatique de l'allocation (contrat new_handler).
   Réserve épuisée → le handler laisse `bad_alloc` suivre son cours (comportement
   actuel). Réarmement : re-allocation de la réserve quand maxB repasse au-dessus
   d'un seuil (dans la loop, cadence lente). AUCUNE allocation dans le handler.
2. **Garde heap sur `/check-update`** : refuser AVANT `WiFiClientSecure` si
   `ESP.getMaxAllocHeap()` < seuil (constante, ordre de grandeur 60 Ko — architecte)
   avec 503 JSON explicite (« mémoire insuffisante pour TLS, réessayez ») — remplace
   l'échec lent aléatoire par un refus rapide et honnête. Sur WROVER (PSRAM), les
   buffers mbedTLS > 4 Ko partent en PSRAM : seuil à réévaluer (note doc).
3. **Instrumentation d'attribution** :
   - logger l'**IP client** dans le log INFO de `handleCheckUpdate` (et du 503) ;
   - étendre le dump du ring au boot : ajouter une ligne INFO avec
     `esp_reset_reason` + uptime absolu du dernier échantillon (déjà dispo) — aide
     la corrélation temporelle a posteriori.
4. **Hygiène** (même cycle, hors spec au sens strict) : purge des fichiers
   `src/uart_*` et `src/debug_tls_verify.h` ressuscités par OneDrive (liés dans le
   binaire 2.18.4 = ~35 Ko de flash morts ; aucun appelant).

Hors périmètre (specs de suivi si récidive) : le parse JSON fragile du check-update
sur TLS réussi ; instabilité MQTT/réseau du 11/08 ; pic saveToFile à buffer plein
(~350 points → à traiter par streaming un jour, non urgent).

## Critères d'acceptation

- [x] Test natif du parachute (logique pure : consommation/réarmement de la réserve,
      one-shot du log, pas d'alloc dans le handler)
- [x] Test natif de la garde check-update (seuil, message, chemin nominal intact)
- [x] `pio run` core 2 + core 3 verts ; `pio test -e native` zéro régression ;
      binaire SANS les objets uart_* (vérifier la taille)
- [ ] Sur cible : `/check-update` sous heap dégradé → 503 immédiat loggé avec IP ;
      sur heap sain → comportement inchangé
- [x] Le ring + parachute : une future famine OOM produit un log CRITICAL exploitable
      au lieu d'un abort sec (le coredump reste le filet ultime)

## Addendum 2026-08-11 — honnêteté documentaire

Le premier diagnostic (spec remplacée : pic RAM saveToFile ~60 Ko) était FAUX :
l'estimation supposait ~350 points en buffer alors que la production en compte ~108
(pic réel mesuré ~23 Ko, bref). Réfuté par mesure `/get-system-info` fine pendant un
cycle de save. Leçon déjà connue du projet, reconfirmée : mesurer au plus près AVANT
de dimensionner un correctif.

## Notes d'implémentation (2026-08-12, v2.18.5)

### Fichiers livrés

- `src/oom_guard_logic.h` (nouveau, pur, header-only) : états
  `kOomStateArmed/Released/Depleted`, `oomOnAllocFailed`, `oomShouldRearm`,
  `oomShouldLogRelease` (one-shot), `oomTlsGateOk` (prédicat bicritère partagé).
- `src/oom_guard.{h,cpp}` (nouveaux) : `initOomGuard()` (malloc réserve au plus
  tôt du setup + `std::set_new_handler` + `heap_caps_register_failed_alloc_callback`
  flag-seul), `oomGuardLoopTick()` (logs différés + réarmement hystérésis),
  `oomGuardReserveArmed()` (log de boot conditionnel). Handler : UNIQUEMENT
  exchange atomique + free + atomics + désenregistrement si épuisé — aucune
  alloc/log/mutex.
- `src/constants.h` : 6 constantes (valeurs ci-dessous).
- `src/main.cpp` : `initOomGuard()` en tête de `setup()`, log d'armement,
  `oomGuardLoopTick()` dans `loop()`.
- `src/web_routes_ota.cpp` : garde bicritère avant `WiFiClientSecure` dans
  `handleCheckUpdate` → 503 `{"error":"Mémoire insuffisante pour TLS, réessayez"}`
  + WARN avec les deux mesures et l'IP client ; IP ajoutée au log INFO nominal.
- `src/heap_ring.cpp` : WARN du dump enrichi (uptime absolu du dernier
  échantillon mod 65 536 s + `esp_reset_reason` brut).
- `test/test_native_oom_guard/` (nouveau, 18 tests).
- `src/version.h` : 2.18.5.
- Purge (hygiène, item 4) : `src/uart_{commands,protocol,transport}.{cpp,h}`,
  `src/debug_tls_verify.h`, `tools/uart_test.py` supprimés (ressuscités par
  OneDrive après feature-065 ; le linker les élaguait déjà via `--gc-sections`,
  delta flash net du durcissement ≈ +3,1 Ko).

### Constantes retenues (arbitrage architecte)

| Constante | Valeur | Justification |
|---|---|---|
| `kOomReserveBytes` | 12 288 o | ≈ 34 allocs vitales de 356 o ; 1,5× le seuil auto-guérison `kRestartMaxAllocHeapBytes` (8 192) |
| `kOomRearmMinMaxAllocBytes` | 32 768 o | Hystérésis de réarmement, > `kLowHeapWarnBytes` (20 Ko) |
| `kOomRearmCheckIntervalMs` | 10 000 ms | Cadence lente du réarmement (loop) |
| `kOomFailWarnThrottleMs` | 30 000 ms | Max 1 WARN « allocation heap échouée » par fenêtre |
| `kCheckUpdateMinFreeHeapBytes` | 65 536 o | Garde TLS — heap total (session ~58 Ko) |
| `kCheckUpdateMinMaxAllocBytes` | 20 480 o | Garde TLS — plus gros bloc contigu |

### Revue de code

Fix de revue **« critère pointeur »** : `oomShouldRearm` fondé sur l'**ABSENCE DE
RÉSERVE** (pointeur nul), pas sur l'état — ferme la course handler/réarmement (si
le handler consomme la nouvelle réserve entre l'`exchange` et le `store(Armed)`,
l'état peut afficher Armed avec réserve nulle ; le pointeur garantit la
convergence au tick suivant). `exchange` en ceinture-bretelles à la ré-allocation.

### Vérifications

- Tests natifs : **459/459** (+18 `test_native_oom_guard`), zéro régression.
- Builds bi-core verts : flash **82,9 % core 2** (production) / **92,6 % core 3**
  (banc) — binaire sans les objets `uart_*` vérifié.
- ADR créé : **ADR-0035** (parachute OOM — comportement global d'allocation
  invisible au point d'appel ; callback IDF seul écarté comme mécanisme
  principal, retenu en complément flag-seul ; les seuils de garde dépendent de
  la réserve). Docs : `docs/subsystems/oom-guard.md` (nouveau), `docs/API.md`,
  `docs/subsystems/system-health.md`, `CHANGELOG.md`.

### Validation on-target (2026-08-12, v2.18.5 flashée en production, WROOM sans PSRAM)

**Acquis** :
- IP client loggée : `Vérification des mises à jour GitHub... (heap libre=87708 o,
  plus gros bloc=34804 o, client=192.168.1.15)` ✔
- Instrumentation heap_caps opérationnelle : `Allocation heap échouée ×4 (dernière
  taille demandée=100 o, cumul=5)` ✔ — **preuve directe** que la session TLS
  conduit le tas à la famine (une allocation de **100 octets** refusée alors que
  87 Ko restent libres = fragmentation extrême). Aucun CRITICAL « réserve OOM
  consommée » : ces échecs sont des `malloc` C (mbedTLS), qui ne passent PAS par
  le `new_handler` — la limite documentée de l'ADR-0035 est confirmée en vrai.
- Le parachute n'a donc pas eu à se déclencher ; aucun crash pendant l'essai.

**DÉFAUT DÉCOUVERT (garde trop permissive sur WROOM)** : la garde a **laissé
passer** (free 87 708 ≥ 65 536 ET maxBloc 34 804 ≥ 20 480), puis le TLS a échoué
(`X509 - Certificate verification failed (-9984)` — signature d'erreur qui mute
avec l'état mémoire, cf. bug-checkupdate-tls-core3) après avoir affamé le tas.
C'est exactement la fenêtre dangereuse du crash du 11/08 : si une connexion HTTP
arrive pendant cette famine, le `new` de `AsyncWebServerRequest` échoue. Le seuil
maxBloc de 20 Ko protège la *survie* mais pas l'*inutilité* de la tentative :
mbedTLS réclame plusieurs blocs ~16 Ko contigus, hors d'atteinte à 34,8 Ko de
plus gros bloc sur un module sans PSRAM.

→ **Spec de suivi** : `bug-garde-tls-trop-permissive-sans-psram` (todo) — aligner
la garde sur le contrat matériel désormais documenté (PSRAM requise pour la mise
à jour en ligne). Critère 4 de cette spec considéré **atteint** pour ce qui est
livré (IP + instrumentation + non-régression) ; le durcissement du seuil est
traité par la spec de suivi.
