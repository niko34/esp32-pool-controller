# Bug — Garde TLS /check-update trop permissive sur module sans PSRAM

- **Type** : bug (format minimal `specs/modele.md`)
- **Statut** : implémenté (v2.18.6) — tests verts (466/466), revue OK, docs à jour ;
  **validation on-target restante** (voir dernière section)
- **Version cible** : 2.18.6
- **Créée** : 2026-08-12 (constaté sur l'appareil de production juste après le
  flash de la v2.18.5)
- **Domaine** : Firmware (`src/oom_guard_logic.h` prédicat, `src/web_routes_ota.cpp`,
  `src/constants.h`, docs) — aucun chemin de dosage
- **Specs liées** : bug-oom-accept-http-durcissement (v2.18.5, done/ — voir sa
  section « Validation on-target »), bug-checkupdate-tls-core3, ADR-0035, ADR-0032

## Symptôme (mesuré en production, WROOM-32 sans PSRAM, core 3, v2.18.5)

`/check-update` **passe** la garde bicritère puis échoue quand même, après avoir
affamé le tas :

```
INFO  Vérification des mises à jour GitHub... (heap libre=87708 o, plus gros bloc=34804 o, client=192.168.1.15)
ERROR Erreur HTTP GitHub: -1 — TLS: X509 - Certificate verification failed (-9984) [connection refused]
WARN  Allocation heap échouée ×4 (dernière taille demandée=100 o, cumul=5)
```

Une allocation de **100 octets** est refusée alors que 87 Ko restent libres :
fragmentation extrême pendant le handshake. C'est **la fenêtre exacte du crash du
11/08** (une connexion HTTP arrivant à cet instant fait échouer le `new` de
`AsyncWebServerRequest` → `bad_alloc`). La garde protège la survie mais autorise
une tentative **vouée à l'échec** qui met le système en danger pendant ~1 s.

## Cause

Les seuils de la v2.18.5 (`kCheckUpdateMinFreeHeapBytes = 65536`,
`kCheckUpdateMinMaxAllocBytes = 20480`) ont été calibrés sur le budget *agrégé*
du TLS. Or mbedTLS réclame plusieurs blocs **contigus d'environ 16 Ko** : à
34,8 Ko de plus gros bloc, le premier passe, les suivants fragmentent le tas
jusqu'à la famine. Sur ESP32 classique **sans PSRAM**, cet état est le régime
NORMAL après quelques minutes de fonctionnement — la garde ne bloque donc
quasiment jamais alors qu'elle devrait presque toujours bloquer.

## Correctif attendu

Aligner la garde sur le **contrat matériel désormais documenté** (BOM PCB v2 :
ESP32-DevKitC-VE / WROVER-E, PSRAM requise pour la mise à jour en ligne) :

1. **Critère PSRAM** : si `ESP.getPsramSize() == 0`, refuser la session TLS
   sortante avec un 503 explicite orientant vers la mise à jour manuelle
   (« module sans PSRAM : utilisez la mise à jour par upload de fichier »).
   À trancher par l'architect : refus systématique, ou refus seulement sous un
   seuil maxBloc **relevé** (~48-50 Ko, mesuré nécessaire) pour laisser la chance
   d'un heap fraîchement démarré — arbitrage entre « fonction inutilisable » et
   « fenêtre de risque ouverte ~1 s ».
2. **Avec PSRAM** : conserver le comportement actuel (les buffers > 4 Ko partent
   en PSRAM) — seuils à re-mesurer sur le WROVER-E à sa mise en service.
3. Prédicat étendu dans le header pur (testable en natif), pas de logique
   dupliquée dans la route.
4. UI/UX : le message d'erreur doit être exploitable par l'utilisateur (le toast
   affiche déjà `data.error`) — vérifier le rendu, pas de modif `data/` a priori.

## Critères d'acceptation

- [ ] Sur WROOM sans PSRAM : `/check-update` répond immédiatement (aucune session
      TLS ouverte, aucun `Allocation heap échouée` consécutif), message clair
      *(code livré ; mesure on-target restante)*
- [ ] Sur module PSRAM : comportement nominal préservé (à valider au swap WROVER)
- [x] Tests natifs du prédicat étendu (cas PSRAM absente / présente × seuils)
- [x] `pio run` bi-core verts, `pio test -e native` zéro régression
- [x] Docs : `docs/API.md` (condition du 503), `docs/UPDATE_GUIDE.md` (déjà
      annoncé côté matériel), CHANGELOG

## Notes d'implémentation (2026-08-13, v2.18.6)

### Arbitrages

| # | Question | Décision |
|---|---|---|
| **D1** | Refus systématique sans PSRAM, ou seuil maxBloc relevé (~48-50 Ko) ? | **Refus systématique, fail-closed.** Le seuil relevé est illusoire : le parachute OOM immobilise 12 Ko, donc le maxBloc nominal d'un WROOM tombe de ~49 à ~37 Ko — un seuil à 48-50 Ko ne passerait **que parachute désarmé**, précisément quand le filet anti-`bad_alloc` est absent. Et mbedTLS réclame *plusieurs* blocs ~16 Ko : le premier consommerait le gros bloc, les suivants échoueraient. |
| **D2** | Critère sur PSRAM **installée** (`getPsramSize`) ou **libre** (`getFreePsram`) ? | **PSRAM libre** — correctif issu de la revue de code (voir ci-dessous). |
| **D3** | Booléen ou motif en retour du prédicat ? | **Motif** (`uint8_t`) : l'appelant doit distinguer un refus **définitif** (matériel → oriente vers l'upload) d'un refus **temporaire** (mémoire → réessayable). Deux messages, deux sémantiques. |
| **D4** | Étendre la garde à `/download-update` ? | **Oui.** Sans elle, un `POST` direct contourne `/check-update` avec un flash à la clé. Position : après url/allowlist/digest (une requête malformée reste 400/403), **avant** `WiFiClientSecure` / `setOtaInProgress()` / `LittleFS.end()` / `esp_partition_erase_range()` → refus **sans aucun effet de bord**. |

Décision formalisée en **[ADR-0036](../../../docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md)**
(notes additives datées ajoutées à ADR-0035 et ADR-0032, sans réécriture).

### Fichiers livrés

- `src/oom_guard_logic.h` : `oomTlsGateOk` (bool) **supprimé** → `oomTlsGateReason(psramFreeBytes, freeHeapBytes, maxAllocBytes, minPsramBytes, minFreeBytes, minMaxAllocBytes)` retournant `kOomTlsGateOk` / `kOomTlsGateNoPsram` / `kOomTlsGateLowMemory`. **PSRAM évaluée en premier, fail-closed.** Header pur : la PSRAM est mesurée par la route et passée en paramètre (aucun `ESP.*` ici).
- `src/constants.h` : `kOtaTlsMinPsramBytes = 1048576` (1 Mo). Seuils heap `kCheckUpdateMinFreeHeapBytes` (65 536) et `kCheckUpdateMinMaxAllocBytes` (20 480) **inchangés**.
- `src/web_routes_ota.cpp` : fonction commune `refuseIfTlsGateClosed()` appelée par `handleCheckUpdate` **et** `handleDownloadUpdate` ; 503 selon motif ; logs INFO nominal et WARN de refus enrichis (PSRAM libre, deux mesures heap, trois seuils, IP client).
- `data/app.css` + `data/app.js` (correctif UI, hors périmètre initial mais rendu nécessaire par le message long) : `.toast` en `white-space: normal` + `max-width: min(92vw, 560px)` + `overflow-wrap: anywhere` (le message de refus, ~220 caractères, débordait du viewport) ; ajout de `.toast--warning` — **4 appels `showToast(..., 'warning')` existaient sans classe CSS** (toasts invisibles) ; durée adaptative dans `showToast` : 40 ms/caractère borné à [3 s, 9 s] — le plancher de 3 s préserve exactement le comportement historique pour les messages ≤ 75 caractères.
- `src/version.h` → **2.18.6**.

### Correctif de revue — « PSRAM LIBRE » et non installée

La première implémentation testait `ESP.getPsramSize() == 0`. La revue a montré
l'angle mort : cette valeur est **constante et ne décroît jamais**. Avec
`SPIRAM_USE_MALLOC` (allocations > 4 Ko en PSRAM), un module dont la PSRAM est
**saturée** ouvrirait quand même la garde alors que mbedTLS retomberait sur la RAM
interne — exactement le régime de crash que la spec corrige. Le critère porte donc
sur `ESP.getFreePsram()` ; exiger **1 Mo libre** couvre à la fois l'absence de PSRAM
et sa saturation.

### Vérifications

- Tests natifs : **466/466** (+7 dans `test/test_native_oom_guard/`, soit 25 tests dans cette suite) — motifs, **priorité PSRAM sur heap** (PSRAM absente + heap sain → `kOomTlsGateNoPsram`), PSRAM saturée, frontières inclusives des trois seuils. Zéro régression.
- Builds bi-core verts : flash **82,9 % esp32dev** / **92,7 % esp32dev_core3**. `./build_fs.sh` OK.
- Docs : `docs/API.md` (503 des deux routes + rectification de la note « limitation core Arduino 3 » → l'attribution réelle est l'absence de PSRAM), `docs/UPDATE_GUIDE.md`, `docs/subsystems/oom-guard.md`, `docs/features/README.md` (composant `showToast`), `docs/features/page-settings.md`, `CHANGELOG.md`, **ADR-0036**.

### Réserve — seuil PSRAM non mesuré

`kOtaTlsMinPsramBytes` = 1 Mo est un ordre de grandeur **jamais mesuré sur WROVER
réel** (valeur volontairement large : la session TLS y consomme surtout de la
PSRAM). À confirmer à la mise en service du premier module PSRAM.

## Validation on-target restante

- **WROOM (production actuelle)** : après flash de la v2.18.6, cliquer « Vérifier les
  mises à jour » → attendu **503 immédiat**, WARN `refusé (PSRAM libre
  absente/insuffisante)` avec PSRAM libre=0, **aucun** `Allocation heap échouée`
  consécutif, aucune ligne `Erreur HTTP GitHub` ; **toast lisible** (message sur
  plusieurs lignes, ~9 s). Refaire la même vérification par `POST /download-update`
  direct (curl) : même 503, et **aucun effet de bord** (filtration/pompes
  inchangées, FS toujours monté, UI toujours servie).
- **WROVER-E (au swap)** : mesurer la PSRAM libre au moment de l'appel et le
  comportement nominal de `/check-update` + `/download-update` (session TLS
  aboutie) ; re-calibrer `kOtaTlsMinPsramBytes` sur la mesure réelle.

## Validation on-target — 2026-08-13, v2.18.7 en production (WROOM sans PSRAM)

Appareil : version 2.18.7, `sketch_size` = 1 700 976 o — **identique à l'octet
près au binaire publié** dans la release v2.18.7 (donc bien le firmware livré,
pas un build local divergent). PSRAM rapportée : 0 o. Uptime 6 h 34 sans incident.

`GET /check-update` :
- **HTTP 503 en 35 ms** (contre ~900 ms en v2.18.5, où la tentative TLS partait
  quand même) — aucune session TLS n'est ouverte ;
- corps exact : `{"error":"La mise à jour en ligne n'est pas disponible. Votre
  matériel ne dispose pas d'une mémoire PSRAM (module ESP32-DevKitC-VE
  nécessaire). Utilisez la mise à jour manuelle.","code":"psram_required"}` ;
- journal : `WARN /check-update refusé (PSRAM libre absente/insuffisante) :
  PSRAM libre=0 o (min 1048576), heap libre=77148 o (min 65536), plus gros
  bloc=34804 o (min 20480)` — les trois mesures et leurs seuils sont tracés.

**Le résultat décisif** : AUCUNE ligne « Allocation heap échouée » ne suit le
refus, alors que la même requête en v2.18.5 en produisait quatre (dont une
allocation de 100 o refusée). La fenêtre de famine — celle du crash `bad_alloc`
du 11/08 — est bien fermée. Aucun « Réserve OOM consommée » : le parachute n'a
pas eu à intervenir.

Reste ouvert (matériel indisponible) : branche PSRAM présente — seuils
`kOtaTlsMinPsramBytes` / heap à re-mesurer au montage du DevKitC-VE.
