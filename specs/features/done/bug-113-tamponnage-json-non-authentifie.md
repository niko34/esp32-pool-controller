# bug-113 — 16 Ko allouables sans authentification sur trois routes

## Métadonnées
| | |
|---|---|
| Type | bug (sécurité / mémoire) |
| Statut | **done (2026-09-07)** — **entièrement close par le lot `bug-101`**, livrée en **v2.48.0** (tests verts, revue OK) |
| Priorité | Modérée — dégradation propre, mais vecteur réel pendant la famine de `bug-080` |
| Version cible | **2.48.0** |
| Relevé par | `code-reviewer`, 2026-09-07, audit de `bug-save-config-applique-avant-authentification` |
| Specs liées | `bug-080` (famine active), `bug-101` (le plafond de 16 Ko sur l'import), `bug-112` |

## ✅ CETTE SPEC EST CLOSE PAR LE LOT `bug-101` — la contradiction apparente est levée

Les deux specs semblaient s'opposer : celle-ci veut **abaisser** le plafond de corps
(16 Ko allouables sans authentification), `bug-101` en voulait **davantage** pour
restaurer un historique complet.

**La contradiction n'existe que tant que le découpage est fait côté appareil.** Une fois
le découpage porté **chez le client** (décision de `bug-101`), `/history/import` n'a plus
besoin de capacité du tout, et le plafond peut **descendre à la moitié** de l'actuel.

Plafonds retenus, **par route**, chacun justifié sur la charge réelle et non « par ordre
de grandeur » :

| Route | Plafond | Justification |
|---|---|---|
| `/history/import` | **8 192 o** | le client découpe par budget d'octets ; ≤ ~74 points par lot |
| `/wifi/connect` | **1 024 o** | `ssid` ≤ 32 o + `password` ≤ 63 o, échappés `\uXXXX` au pire (×6) ⇒ ≈ 600 o, soit 1,7× de marge. **Pas 512** : un SSID UTF-8 pathologique aurait été refusé |
| `/sensors/onewire/identify` | **256 o** | `address` 16 hex + `role` ≤ 7 ⇒ ≈ 45 o, soit 5,7× de marge |
| **Total exposé sans authentification** | **9 472 o** | contre 49 152 o aujourd'hui — **−80,7 %** |

Et le refus devient **propre** : le garde prioritaire (patron ADR-0067) rend un `413` en
JSON maison **et** un `503` + `Retry-After` en famine, via
`httpLowHeapShouldRefuse(heapLargestFreeBlock8Bit(), kStaticFileMinAllocBytes)` — donc
**sans inventer de seuil** : constante et prédicat existants, déjà couverts par
`test_native_http_lowheap_guard`. Le critère « jamais d'`abort()` » est satisfait.

⚠️ **Une prémisse de cette spec est également corrigée** : l'idée que ce tamponnage
menace le tas 8 bits interne est **fausse au-delà de 4 095 o**. `calloc` passe par
`heap_caps_malloc_default`, qui bascule sur **PSRAM d'abord** dès que la taille dépasse
`kPsramExtmemThresholdBytes` (établi par désassemblage, 2026-09-07). Le motif du
plafond bas n'est donc pas « ça va planter » mais « la sûreté reposerait entièrement sur
une **préférence** d'allocateur, dont le repli interne est le contre-exemple ». Un
plafond bas est vrai indépendamment de l'état de la PSRAM.

**Corollaire contre-intuitif** : découper **trop fin** ramènerait l'allocation **dans**
le tas affamé (≤ 4 095 o ⇒ interne). Le budget de lot client est donc choisi
**au-dessus** de ce seuil, à dessein.

Les quatre critères d'acceptation de cette spec sont couverts par le lot `bug-101` :
plafond justifié par route ✓ · 503 explicite au lieu d'un `abort()` ✓ ·
`/history/import` fonctionnel ✓ · flux de premier démarrage (`/wifi/connect`) préservé ✓.

## Domaines touchés
- [x] Web — routes HTTP (`src/web_routes_data.cpp`, `src/web_routes_config.cpp`, `src/web_routes_sensors.cpp`)
- [ ] Régulation — [ ] Capteurs — [ ] MQTT — [ ] Frontend — [ ] OTA

## Agents requis
- [x] architect · [x] web-ui-developer · [x] code-reviewer · [x] doc-writer
- [ ] `pool-chemistry` — sans objet.

## Description
Trois routes utilisent `AsyncCallbackJsonWebHandler`, qui **tamponne le corps entier**
avant d'appeler le callback : `/history/import`, `/sensors/onewire/identify`,
`/wifi/connect`. Le tamponnage a donc lieu **avant** toute authentification.

**Ce n'est pas** le motif « corps appliqué avant vérification » — les trois vérifient
bien avant toute mutation (audit du 07/09). Mais un client non authentifié peut faire
**allouer de la mémoire** à l'appareil.

## Contexte — les chiffres
- `_maxContentLength` vaut **16384 o** par défaut et n'est **jamais surchargé**.
- `handleBody` refuse `total > 16384` **avant** allocation, puis fait
  `calloc(total + 1)` avec test de nullité et `abort()` propre — **pas** de
  `bad_alloc`, la dégradation est correcte.
- Soit **16 Ko par requête en vol, sur 3 URI, sans authentification**.

Sur un serveur ordinaire ce serait négligeable. Ici, `bug-080` documente une **famine
mémoire active** : le 07/09, **trois chargements de page** ont suffi à produire
**21 échecs d'allocation** de tampons WiFi (1 622 o, `caps=0x080C`), avec un plus gros
bloc effondré à 832 o. 16 Ko réclamables sans identifiants ne sont plus négligeables
dans ce contexte.

## Comportement attendu
- Abaisser `setMaxContentLength()` au strict nécessaire **par route** (l'ordre de
  grandeur suggéré par l'audit est ~4 Ko ; à chiffrer sur les charges réelles —
  ⚠️ `/history/import` a besoin de place, cf. `bug-101` qui traite justement son
  plafond : **coordonner les deux specs, ne pas les traiter en aveugle**).
- Ajouter une garde sur le plus gros bloc disponible avant d'accepter le tamponnage,
  sur le patron de la garde `fopen` (ADR-0070) : refuser proprement en famine plutôt
  que d'aggraver.

## Contraintes techniques
- **Tension explicite avec `bug-101`** : cette spec veut *baisser* le plafond,
  `bug-101` constate qu'il est *trop bas* pour une restauration d'historique. La
  résolution n'est pas un nombre unique mais un plafond **par route**. À trancher
  ensemble.
- La garde mémoire doit utiliser la métrique **8 bits** (`heapLargestFreeBlock8Bit()`),
  jamais `ESP.getMaxAllocHeap()` — cf. la cause racine documentée de `bug-080`.

## Critères d'acceptation
- [x] Plafond justifié **par route**, chiffré sur les charges réelles — `/history/import`
      **8 192 o**, `/wifi/connect` **1 024 o**, `/sensors/onewire/identify` **256 o** ;
      total exposé sans authentification **9 472 o** contre 49 152, **−80,73 %**,
      **asserté à la compilation** (toute révision casse le build).
- [x] Un tamponnage refusé en famine renvoie un 503 explicite, jamais un `abort()` —
      `registerJsonBodyGuard()` : `503 {"error":…}` + `Retry-After: 5` via
      `httpLowHeapShouldRefuse(heapLargestFreeBlock8Bit(), kStaticFileMinAllocBytes)`,
      **sans inventer de seuil** (prédicat et constante existants).
- [x] `/history/import` reste fonctionnel pour une restauration réelle — **oui, et sans
      condition de taille** : le découpage client de `bug-101` supprime le besoin de
      capacité côté appareil. ⚠️ Recette **on-target** de `bug-101` non jouée.
- [x] Le flux de premier démarrage (`/wifi/connect`) reste fonctionnel — charge nominale
      **58 o**, **595 o** au pire cas *tout échappé* `\uXXXX`, sous 1 024. **Pas 512**,
      délibérément : un SSID UTF-8 pathologique y aurait été refusé.
- [x] `pio run` et `pio test -e native` sans régression — **41 suites / 1 607 cas**
      (était 40 / 1 535), `pio run` après `-t clean` **SUCCESS**, **6 warnings tous
      préexistants**, Flash **49,6 %**.

## Hors périmètre
- Le journal non borné : c'est `bug-112`.

## Notes techniques — passage `test-engineer` (2026-09-07)

Couvert par la suite `test/test_native_history_import/` (partie 6), en même
temps que bug-101 : les deux specs partagent le plafond
`kHistoryImportMaxBodyBytes` et le garde `registerJsonBodyGuard()`.

**Vérifié pour les TROIS routes** (`/history/import`, `/wifi/connect`,
`/sensors/onewire/identify`), pas seulement la première : en garder deux ne
diviserait le total exposé que d'un tiers. Le test assert l'appel exact
`registerJsonBodyGuard(server, "<uri>", <constante>)` **et** qu'il précède
`addHandler(...)` dans le fichier — l'ordre **est** la substance de la garde
(`AsyncWebServer::_attachHandler` retient le **premier** handler dont `filter()`
et `canHandle()` passent, WebServer.cpp:147). Un test qui se contenterait de
constater la **présence** de l'appel serait vert sur un garde totalement inerte,
d'où la mutation M7 (garde déplacé après le handler) : elle meurt.

**Off-by-one d'un corps de 8 192 o exactement — vérifié dans la bibliothèque.**
`AsyncJson.cpp` refuse sur `contentLength() > _maxContentLength` (ligne 158) et
`total > _maxContentLength` (ligne 203) : le plafond est le nombre d'octets
**autorisés**. Le garde utilise la **même** comparaison stricte, deux fois (une
au filtre, une au handler). Un `>=` ferait diverger les deux plafonds **d'un
octet** : le garde refuserait ce que la bibliothèque accepte. Le budget client
(7 168 o) laissant 1 024 o de marge, le cas ne surviendrait qu'avec un client
tiers ou une page en cache — **introuvable en recette, et un 413 inexplicable en
production**. `test_sentinelle_garde_compare_strictement_le_plafond` compte
exactement 2 × `contentLength() > capBytes` et 0 × `>= capBytes` (mutation M5 :
elle meurt).

**Également verrouillé** : le garde ne journalise pas et ne construit aucune
`String` (il tourne sur `async_tcp`, et un `WARNING` déclencherait un flush
synchrone du journal — donc un `fopen`, l'`abort()` même qu'il prévient) ; il
mesure le tas **8 bits** (`heapLargestFreeBlock8Bit()`, jamais
`ESP.getFreeHeap()`, la métrique fausse du projet) via le prédicat pur
**existant** `httpLowHeapShouldRefuse` ; il rend un `Retry-After` sur le 503 ; et
`importHandler->setMaxContentLength()` reste posé **en plus** du garde — le garde
lit le `Content-Length` **annoncé**, la bibliothèque compte les octets
**réellement reçus**, et un client qui sous-déclare son corps ne serait arrêté
que par la seconde.

**Total exposé sans authentification** : 8 192 + 1 024 + 256 = **9 472 o** contre
3 × 16 384 = 49 152, soit −80,7 %. Le test le recompte à partir du **source** de
`constants.h` et exige que chaque plafond reste **strictement** sous 16 384 —
à 16 384 il ne surcharge plus le défaut de la bibliothèque et la garde devient
décorative, comme l'a été le parachute anti-OOM logé en PSRAM depuis
feature-066.

## Clôture (2026-09-07, v2.48.0)

**Close par le lot `bug-101`** — les deux specs partagent `kHistoryImportMaxBodyBytes`
et `registerJsonBodyGuard()`. Décision d'ensemble et résolution de la contradiction
(`bug-113` voulait **moins** de capacité, `bug-101` **plus**) :
[ADR-0075](../../../docs/adr/0075-decoupage-client-contre-streaming-firmware.md).

**Les trois routes sont couvertes**, pas seulement la première : en garder deux
n'aurait divisé le total exposé que d'un tiers.

**Documentation** : [`docs/subsystems/web-server.md`](../../../docs/subsystems/web-server.md)
(section `registerJsonBodyGuard()`), [`docs/API.md`](../../../docs/API.md) (plafonds sur
les trois routes, `413`/`503` en JSON maison), `CHANGELOG.md` `[2.48.0]`.

**Le motif du plafond a changé, la décision non** — la prémisse « ce tamponnage menace
le tas 8 bits interne » est **fausse au-delà de 4 095 o** (désassemblage du 2026-09-07 :
`calloc` → `heap_caps_malloc_default` → **PSRAM d'abord**). Le plafond bas reste juste
parce que, sans lui, la sûreté reposerait **entièrement sur une préférence d'allocateur**
dont le repli interne est le contre-exemple.

### 📋 Consigné comme ouvert
- Le test qui interdit les secrets dans les fixtures ne mord que sur une **liste de
  mots** (`ssid`, `mqtt`, `192.168.`) : un broker en `10.x` passerait. **Garde-fou
  faible, pas faux.**
- Les défauts préexistants de `saveToFile()` (appelée **par lot**, et `void` donc
  capable de sauter en silence) et le `continue` sur timestamp invalide sont consignés
  dans la fiche `bug-101`.
