# ADR-0112 — mbedtls alloue selon la politique PSRAM du projet, pas selon la sienne

- **Statut** : Accepté
- **Date** : 2026-10-15
- **Décideurs** : `debugger`, `security-reviewer`, `embedded-developer`, `code-reviewer`, utilisateur (piste de diagnostic décisive)
- **Spec(s) liée(s)** : [`bug-165`](../../specs/features/done/bug-165-recherche-de-mise-a-jour-en-ligne-echoue.md), [`bug-108`](../../specs/features/done/bug-108-garde-tls-ota-sur-metrique-ira-inclusive.md) (absorbé par ce lot — son relevé « impossible à prendre » existe désormais)

## Contexte

`GET /check-update` échouait systématiquement depuis la migration vers core Arduino 3 / mbedtls
3.6.5. **Huit hypothèses** ont cherché la cause du côté des certificats — ancres manquantes,
ordre de lecture du paquet, horloge, courbe P-384 non supportée, chaîne reçue différente de la
chaîne capturée, profil TLS trop restrictif, bundle IDF utilisé par erreur — **toutes tombées**.
Chacune a été testée **réellement** (déploiement, lecture live de `conf->ca_chain`, profil lu sur
la connexion vivante), pas supposée.

La piste décisive est venue d'une question simple de l'utilisateur : *« au début du projet, la
mise à jour via GitHub fonctionnait. Et à un moment, on a changé de framework. »* Mesuré ensuite
sur le chemin de production, sans instrumentation ajoutée, pendant un `/check-update` réel :

| Instant | `free8` | `maxAlloc8` |
|---|---|---|
| Avant ouverture TLS | 60 760 | 57 332 |
| Juste avant le handshake | 11 296 | 10 740 |
| **Pendant la vérification du certificat (ECDSA P-384)** | 4 852 | 🚨 **3 316** |
| 13 ms après l'échec | 59 672 | 47 092 |

**Confirmé par un second système entièrement indépendant** : `fs_guard` (mécanisme préexistant,
sans aucun rapport avec cette investigation) a relevé **le même 3 316** au même instant et a
suspendu ses ouvertures de fichiers 30 secondes.

Cause : les libs mbedtls précompilées livrées par pioarduino portent
`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=1` — mbedtls **force** ses allocations en RAM **interne**,
quelle que soit la politique d'allocation que le projet a déjà posée
(`heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes)`, bug-080 B-1/feature-124). Pendant la
vérification d'une signature ECDSA P-384, mbedtls consomme ~56 Ko de tas interne, pendant que
**4,17 Mo de PSRAM dorment** à côté.

🚨 **bug-165 et bug-108 sont le même problème, vu de deux côtés.** La garde TLS sortante (ADR-0036)
lisait `ESP.getFreeHeap()` = 82 796 o au moment du refus réel et **autorisait** (seuil 65 536),
pendant que le plus gros bloc 8 bits réellement allouable valait 3 316 o — *une garde qui mesure
la mauvaise grandeur ne protège de rien*. Le motif historique de cette garde le disait déjà : « sur
heap dégradé l'échec était lent et aléatoire (BIGNUM alloc en cours de handshake) ». bug-108
décrivait ce défaut depuis septembre sans pouvoir le prouver : le relevé décisif (mémoire pendant
une session TLS active) était **impossible à prendre** tant que le handshake échouait avant
d'allouer quoi que ce soit d'observable. Il existe désormais.

## Décision

1. **mbedtls doit allouer selon la politique PSRAM du projet, pas selon la sienne.** Posé par
   `mbedtls_platform_set_calloc_free(calloc, free)` ([`src/mbedtls_psram_alloc.cpp`](../../src/mbedtls_psram_alloc.cpp)),
   substituant à l'exécution les `calloc`/`free` de la **libc** (déjà PSRAM-aware) à ceux de
   mbedtls — sans recompiler la bibliothèque. Posé **une seule fois**, dans `psramFound()` de
   `main.cpp`, **avant toute utilisation de mbedtls** (Wi-Fi compris).
2. **Aucun allocateur n'est écrit par ce projet.** La politique PSRAM existe déjà
   (`heap_caps_malloc_extmem_enable`) ; mbedtls doit simplement s'y conformer, via les fonctions
   déjà en place.
3. **La pose est vérifiée par un témoin réel, pas supposée réussie.** Une allocation de 2 048 o
   **via le pointeur mbedtls** (`mbedtls_calloc()`) doit être vue en PSRAM
   (`esp_ptr_external_ram()`) ; sinon l'état est `kMbedtlsPsramAllocDecorative`, exposé par
   `/get-system-info` — le projet s'est déjà fait avoir une fois par un parachute anti-OOM
   décoratif, sans que rien ne le signale avant une mesure directe.
4. **La garde TLS sortante (`oomTlsGateReason`, ADR-0036) est recalibrée, pas réécrite** : le
   critère PSRAM passe de « libre totale » (1 Mo, jamais mesuré sur WROVER réel) à « plus gros
   bloc contigu » (40 960 o = 2 tampons SSL de 16 384 + marge JSON, **mesuré** sur WROVER réel) ;
   les deux seuils heap passent de la métrique IRAM-inclusive à la métrique 8 bits (ADR-0058),
   soldant la dette que bug-108 décrivait depuis septembre.

## Alternatives considérées

- **Option A (rejetée)** — modifier la configuration mbedtls (`sdkconfig`,
  `MBEDTLS_EXTERNAL_MEM_ALLOC` ou équivalent) pour qu'elle alloue en PSRAM par défaut. **Impossible
  en pratique** : les bibliothèques mbedtls livrées par pioarduino sont **précompilées** — changer
  un `sdkconfig` sans recompiler la toolchain complète n'a aucun effet sur le binaire embarqué.
- **Option B (rejetée)** — relever encore les seuils de la garde heap interne pour tolérer la
  consommation de mbedtls. Rejetée : la fenêtre de famine (56 Ko, descendant à 3 316 o) est **trop
  massive et trop proche du nominal du tas interne** pour être absorbée par un seuil — un seuil
  honnête couvrant ce cas refuserait l'OTA **en permanence**, même sur un appareil parfaitement
  sain (la spec bug-108 le démontre explicitement : *« une garde juste bloquerait tout — ce qui
  est correct, mais ne répare rien »*).
- **Option C (rejetée)** — écrire un allocateur PSRAM dédié à mbedtls, distinct de la politique
  déjà posée pour le reste du projet. Rejetée : la politique existe déjà et fonctionne pour tout
  le reste du firmware (`heap_caps_malloc_extmem_enable`) — dupliquer sa logique pour mbedtls
  créerait deux politiques PSRAM à maintenir en cohérence, pour aucun bénéfice.
- **Option D (retenue)** — faire obéir mbedtls à la politique **déjà posée**, via le point
  d'extension officiel (`mbedtls_platform_set_calloc_free`), vérifié par un témoin réel.

## Conséquences

### Positives
- `/check-update` et `/download-update` redeviennent fonctionnels sur le matériel cible
  (WROVER-E), sans renoncer à la validation TLS complète de la chaîne de certificats.
- La garde TLS sortante protège enfin contre la grandeur qu'elle est censée surveiller
  (métrique 8 bits, ADR-0058) — la dette de bug-108 est soldée dans le même lot.
- Le mécanisme est **observable en production** sans ouverture de session TLS : `/get-system-info`
  expose `mbedtls_psram_alloc` (0/1/2), et un repli silencieux PSRAM→interne pendant une session
  réelle est désormais journalisé (`logTlsHeapWindow`, condition C4).

### Négatives / dette assumée
- **La chaîne TLS/OTA dépend désormais structurellement de la PSRAM pour fonctionner** — ADR-0036
  passait la mise à jour en ligne de « recommandée » à « nécessaire » matériellement ; ce lot
  resserre cette dépendance : sans PSRAM **active et correctement exploitée par mbedtls**, aucune
  session TLS sortante n'aboutit. Un appareil où le crochet serait `Decorative` (anomalie) perd la
  mise à jour en ligne de la même façon qu'un appareil sans PSRAM physique.
- **Dépendance implicite à `kPsramExtmemThresholdBytes`** : le comportement de mbedtls sous ce
  correctif suit désormais la **même** politique de bascule PSRAM que le reste du firmware — tout
  changement de ce seuil affecte aussi mbedtls, sans qu'aucun code mbedtls ne le déclare
  explicitement (couplage implicite, documenté ici pour qu'il ne soit pas découvert par accident).
- **La PSRAM reste un pool de travail partagé** (ADR-0061) : si elle sature pendant une session
  TLS, mbedtls retombe silencieusement en interne — risque résiduel, couvert par un WARN
  d'observation (`kOtaTlsInternalFallbackWarnBytes`), pas par une garde qui refuserait.
- **Réserve non instruite avant déploiement, signalée par la revue de sécurité** : pendant
  l'écriture en flash (`/download-update`), le cache qui donne accès à la PSRAM est désactivé — si
  mbedtls y détient des données à cet instant, l'appareil plante. Le tampon de pile `buff[512]`
  utilisé pour copier le flux téléchargé reste hors de l'espace mbedtls avant toute écriture
  flash, ce qui rend la bascule sûre pour ce chemin précis ; toute évolution future du chemin de
  téléchargement doit réexaminer cet invariant avant de le modifier.

### Ce que ça verrouille
- Toute bibliothèque tierce précompilée qui expose un point d'extension d'allocateur
  (`platform_set_*`) et qui ne respecte pas nativement la politique PSRAM du projet doit être
  alignée par le **même mécanisme** (substitution à l'exécution + témoin réel), pas par un
  ajustement de seuil qui contournerait le symptôme sans traiter la cause.
- Toute garde mémoire future sur un chemin qui ouvre une session TLS doit mesurer le **plus gros
  bloc contigu** du pool réellement sollicité (PSRAM comme interne), jamais un total — la
  fragmentation est le facteur qui décide si une allocation de plusieurs Ko aboutit.
- La relation arithmétique entre les seuils OTA (`kOtaTlsMinPsramLargestBlockBytes`,
  `kCheckUpdateMinFreeHeapBytes`, `kCheckUpdateMinMaxAllocBytes`, `kOtaCheckJsonBudgetBytes`) est
  verrouillée par `static_assert` dans `constants.h` : un futur changement isolé de l'une sans les
  autres casse la compilation plutôt que de dériver silencieusement.

## Références

- Code : [`src/mbedtls_psram_alloc.h`](../../src/mbedtls_psram_alloc.h) / [`.cpp`](../../src/mbedtls_psram_alloc.cpp) ·
  [`src/main.cpp`](../../src/main.cpp) (pose dans `psramFound()`) ·
  [`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) (`oomTlsGateReason`, sémantique revue) ·
  [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (`refuseIfTlsGateClosed`,
  `logTlsHeapWindow`) · [`src/constants.h`](../../src/constants.h) (seuils + `static_assert`) ·
  [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (`mbedtls_psram_alloc` dans
  `/get-system-info`)
- Specs : [`bug-165`](../../specs/features/done/bug-165-recherche-de-mise-a-jour-en-ligne-echoue.md),
  [`bug-108`](../../specs/features/done/bug-108-garde-tls-ota-sur-metrique-ira-inclusive.md)
- Doc externe : [`docs/subsystems/oom-guard.md`](../subsystems/oom-guard.md#garde-tls-sortante-partagée--oomtlsgatereason-v2186--refondue-v2810-bug-165bug-108) ·
  [`docs/subsystems/ota-manager.md`](../subsystems/ota-manager.md) · [`docs/API.md`](../API.md)
- ADR lié : [ADR-0036](0036-psram-requise-mise-a-jour-en-ligne.md) (PSRAM requise pour l'OTA — amendé par une note additive 2026-10-15, les deux décisions coexistent), [ADR-0058](0058-gardes-heap-metrique-8-bits.md) (métrique 8 bits, échelle des seuils), [ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md) (politique PSRAM que mbedtls suit désormais), [ADR-0111](0111-ancre-de-confiance-tls-toujours-auto-signee.md) (les huit hypothèses certificats, dont les deux premières de cet incident, se sont révélées sans rapport avec la cause réelle)
