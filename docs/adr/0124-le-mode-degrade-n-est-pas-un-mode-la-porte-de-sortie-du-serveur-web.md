# ADR-0124 — Le mode dégradé n'est pas un mode : la porte de sortie du serveur web

- **Statut** : Accepté
- **Date** : 2026-11-15
- **Décideurs** : Nicolas + Claude (`architect`, `security-reviewer` P1 — GO sans condition en fin de lot —, `ux-ui-designer`, `code-reviewer`)
- **Spec(s) liée(s)** : [`feature-151`](../../specs/features/done/feature-151-signature-de-l-image-ota.md) (sous-lot D3-b1 « la porte » ; complète [ADR-0122](0122-socle-decision-ota-genre-empreinte-relais-sig.md), [ADR-0123](0123-une-seule-session-tls-sortante-par-requete-http.md))

## Contexte

`WebServerManager::begin()` **retournait** quand `LittleFS.begin()` échouait (ancien `web_server.cpp:50-54`). Après un redémarrage avec la partition `spiffs` détruite (mise à jour du filesystem interrompue, partition corrompue), **aucune route HTTP n'existait**, pas même `POST /update`, et le serveur n'était jamais démarré. L'appareil était **injoignable par le réseau tout en continuant de doser** : la régulation tourne dans `loop()`, indépendamment du serveur.

Réparer exigeait un câble USB. Or le plan de feature-151 (D3-c : refuser toute image non signée) rend ce cas plus coûteux : plus on durcit l'OTA, plus il faut que la voie de réparation sans câble soit **sûre et toujours disponible**.

Réserve de fond posée avant toute conception : *« le mode dégradé ne doit pas être un mode »*. Un serveur qui se comporte autrement selon l'état du filesystem (routes en moins, authentification assouplie, drapeau `degraded`) crée une seconde surface de sécurité, mal testée, que personne ne relit.

## Décision

### 1. Retrait du retour anticipé, table de routes unique

- Le `CRITICAL` « LittleFS non disponible » est **conservé** ; le `return` est **supprimé**. `setupRoutes()` s'exécute **toujours**, `server->begin()` aussi.
- La table de routes est **identique** dans les deux états. Aucun enregistrement conditionné au montage, aucun drapeau « dégradé ».
- Les routes JSON, l'authentification, `POST /update` et le WebSocket fonctionnent comme d'habitude. `/get-history`, `/get-logs` et `/download-logs` vivent sur `historyFs` (partition `history` séparée) : intactes.

**Preuve que rien ne s'ouvre dangereusement sans filesystem** (lue dans la bibliothèque, pas supposée) : `VFSImpl::exists()` et `open()` (`vfs_api.cpp:24-28`, `:89-93`) **sortent avant tout `fopen`** quand `_mountpoint == nullptr`, et un `begin()` raté ne le pose **jamais** (`LittleFS.cpp:84-88`). Aucun accès fichier n'a lieu.

⚠️ **Correction de ce que la conception affirmait** — « 404 par construction » était **faux**. Sans filesystem, `beginResponse(FS&, path)` rend `NULL` (`WebRequest.cpp:1209`) et `_send()` répond **501** « Handler did not handle the request » (`WebRequest.cpp:1036`) ; **404 seulement** si la requête porte `If-None-Match`. Même nuance pour « aucune allocation » : une `String gzPath` est construite — sans danger, mais le critère exact est « aucun `fopen` ». **Aucun test, script ou client ne doit dépendre du 404.**

`⛔ LittleFS.begin(true)` (formatage automatique) reste interdit : il détruirait les données au lieu de les laisser réparer.

### 2. Une page de secours embarquée dans le firmware

Quand l'interface web ne peut plus être servie, `/` et `/index.html` rendent une **page de secours** qui permet de réinstaller le filesystem (connexion, deux fichiers `.bin` + `.sig`, envoi vers `POST /update`).

- **`GET /recovery`** : publique, **toujours** servie, enregistrée **avant la garde 503** (servie depuis la flash, elle répond même en famine mémoire).
- **`GET /ui`** : enregistrée **après** la garde 503 (elle ouvre un fichier), sert `/index.html` **sans condition**. C'est l'échappatoire quand le secours s'arme à tort (voir § 4).
- 🔴 Servie par la surcharge `beginResponse(code, type, const uint8_t*, size_t)` ⇒ `AsyncProgmemResponse`, **zéro copie en tas**. La surcharge `const char*` construirait une `AsyncBasicResponse` qui copie ~16 Ko dans une `String`, pour un `maxBloc8` nominal d'environ 21 Ko. Pas de pré-gzip : la page pèse ~16 Ko, soit moins de 1 % de la mémoire libre ; la complexité ne se justifie pas.
- La page n'insère du texte que par **`textContent`**, avec des messages **fixes** (jamais le texte du serveur ni du `.sig`). Le contrôle de génération rougit sur `innerHTML`, `insertAdjacentHTML`, `document.write`, `eval`, `location.search`, `location.hash` et toute URL absolue.

### 3. La page est générée, et VERSIONNÉE

`tools/gen_recovery_page.js` (node, via `test/js_extract.js`) produit `src/ota_recovery_page.cpp` à partir de `src/ota_recovery_page.html.in` et d'extraits de `data/app.js`. Le fichier généré est **commité** : **exception assumée** à « on ne versionne pas les générés ».

**Raison** : le firmware doit compiler depuis un clone **sans node**. Une génération au build aurait imposé node au gate `pio run` de la CI. Contreparties : un garde-fou de fraîcheur (`tools/check_recovery_page_generated.sh` : re-génère puis `diff`, plus `node --check` du JS produit), lancé en CI **et** par `tools/check_release_artefacts.sh` (donc par `/release`, qu'il arrête — la CI n'étant pas bloquante pour une release).

### 4. Arbitrage du SHA-256 : une seule source, extraite

`POST /update` **exige** le paramètre `sha256` (`web_routes_ota.cpp:807`, feature-118 lot B), et `crypto.subtle` est **indisponible** en HTTP clair sur IP de LAN : la page **doit** calculer l'empreinte elle-même.

| | (a) copie manuelle | **(a′) extraction à la génération** | (b) manifeste = source d'empreinte |
|---|---|---|---|
| Implémentations du SHA-256 | 2 | **1** | 1 |
| Dérive | silencieuse | **détectée par le garde-fou** | — |
| `/update` modifié | non | **non** | oui |
| Acceptation élargie | non | **non** | **oui, pour tous les appelants** |

**Retenue : (a′).** Le SHA-256 (`SHA256_K`, `createSha256`, `sha256OfFile`) et le parseur de `.sig` (`parseOtaManifestLine`, `parseSigFile`) restent écrits **une fois** dans `data/app.js` ; l'**autonomie du code extrait a été prouvée par exécution** (6 392 o, aucune dépendance au reste du fichier, même empreinte que `node:crypto` sur 300 000 o aléatoires). Le risque d'une copie défectueuse est la **disponibilité** (envois refusés le jour où l'on a besoin du secours — échec fermé), pas la confidentialité.

**(b) écartée** : le `sha256` déclaré par l'appelant n'est pas signé alors que l'empreinte du manifeste l'est, donc (b) serait même plus fort cryptographiquement ; mais elle **élargit ce que `/update` accepte pour tous les appelants** — une acceptation neuve, exactement la classe de défaut sur laquelle D3-a s'est fait prendre. ⇒ Renvoyée à **D3-c**.

### 5. Le déclencheur : marqueur NVS + montage + index

Décision pure (`decideOtaRecoveryReason`, `src/ota_recovery_logic.*`), entrées établies par l'appelant. **Priorité** : marqueur → montage → index.

| Entrée | Motif | Rôle |
|---|---|---|
| Marqueur NVS `pool-ota`/`fsupd` | `kFsUpdateInterrupted` | seul signal d'un montage **réussi** sur un contenu incohérent |
| `LittleFS` non monté | `kMountFailed` | cas initial du défaut |
| ni `index.html.gz` ni `index.html` | `kIndexMissing` | FS monté mais vidé |

- **Marqueur** : namespace dédié `pool-ota` (⛔ pas `poolctrl` : `config.cpp` et son mutex, interdits depuis `async_tcp`), clé `fsupd`. La NVS vit à `0x9000`, **hors `spiffs`** : elle survit à la destruction de la partition. Posé avant les **deux** `LittleFS.end()` de `web_routes_ota.cpp` (`/update` et `/download-update`), levé après les **deux** succès. **Conditionné au genre filesystem** (S3) : le point de succès est partagé, un succès *firmware* ne doit jamais lever le marqueur du filesystem (défaut trouvé en revue, figé par un test natif tué par mutation).
- **Deux modes d'échec, tous deux vers « comme avant »** : écriture NVS en échec ⇒ copie RAM inchangée, entrée ignorée ; lecture NVS en échec (`markerReadable == false`) ⇒ jamais lue comme « en cours ». S'y ajoute `indexProbed == false` (admission mémoire refusée) : **n'arme jamais** le secours.
- **Sonde d'index** : `fsPathResolvableIfHeapAllows()` (`fs_guard.*`, seul fichier autorisé à ouvrir, [ADR-0070](0070-fopen-interne-helper-unique-et-classes-de-contention.md)), miroir de `send(FS&, path)` : **`.gz` d'abord**, car `minify.js` n'écrit **que** les variantes `.gz` (feature-048). Une sonde sur `/index.html` nu aurait armé le secours en permanence sur tout appareil sain — défaut trouvé **avant** tout code, que les tests natifs n'auraient pas vu (le module pur reçoit un booléen ; c'est l'appelant qui mentait). `tools/check_fs_probe_paths.sh` lie désormais le sondeur au producteur.
- **Réévaluation à chaque transition** (S4) : pose, levée, remontage réussi ou raté — pas seulement au démarrage. Le secours s'arme et se lève **sans redémarrage**.
- 🔴 **Faux positif collant assumé** : une coupure de courant entre la pose du marqueur et `LittleFS.end()` laisse le marqueur posé sur un filesystem **intact**. Le secours masque alors l'interface. **Réparation** : `GET /ui` (lien présent sur la page de secours), puis un envoi réussi lève le marqueur. Cosmétique : `login.html` redirige vers `/`, donc le lien `/ui` est à redonner après connexion — d'où une page unique, sans redirection.

### 6. Le secours n'est pas une garde — et un outil le tient

`otaRecoveryActive()` n'a **qu'un seul lecteur** (`web_routes_recovery.cpp`). `auth.*`, `otaUploadAuthorized`, `REQUIRE_AUTH` et le prédicat de signature ne la lisent jamais. `web_routes_ota.cpp` n'a qu'une interface d'**écriture** du marqueur. `tools/check_degraded_mode_absent.sh` le vérifie **sur les symboles** et non sur les fichiers (les sites du marqueur vivent *dans* `web_routes_ota.cpp`, donc « aucune mention dans ce fichier » serait faux d'emblée — l'argument « vert par construction » était erroné).

## Alternatives considérées

- **Laisser le `return`, réparer par USB** (rejetée) — c'est le défaut. Injoignable et dosant.
- **Mode dégradé avec table de routes réduite / drapeau `degraded`** (rejetée) — crée une seconde politique de sécurité, c'est la réserve de fond.
- **`LittleFS.begin(true)`** (rejetée) — formate : détruit des données récupérables, et efface l'interface sans qu'on ait de quoi la remettre.
- **Page de secours stockée dans le filesystem** (rejetée) — absente exactement quand elle est nécessaire.
- **Génération de la page au build** (rejetée) — impose node à `pio run`.
- **Copie manuelle du SHA-256 dans la page** (rejetée) — deux implémentations, dérive silencieuse.
- **Manifeste comme source d'empreinte** (rejetée ici, renvoyée à D3-c) — acceptation élargie pour tous les appelants.
- **Sonde sur `/index.html` seul** (rejetée) — armement permanent (voir § 5).

## Conséquences

### Positives
- L'appareil reste **joignable** et réparable sans câble, y compris avec `spiffs` détruite.
- Aucun prédicat de décision OTA touché : `/update` inchangé (refus `:807` intact), aucun site `handleOtaUpdate` neuf, aucune route `POST` neuve.
- Quatre garde-fous CI neufs ou étendus (`degraded-mode-absent`, `recovery-page-generated`, `fs-probe-paths`, exemption nommée de `check_fopen_guard.sh`).

### Négatives / dette assumée
- Fichier généré **versionné** (~16 Ko) : il réapparaît dans le diff à chaque évolution du SHA-256 ou du parseur de `.sig`.
- Surcoût flash mesuré : **+19 120 o** (Flash 51,8 % ; 1 935 273 o).
- Faux positif collant (§ 5), réparable par `/ui`.
- ⚠️ **Risques résiduels, écrits comme tels** :
  - mot de passe et jeton stockés **en clair en NVS, sans sel** ;
  - le **premier démarrage est ouvert** : mot de passe d'usine `admin` en Basic tant que l'assistant n'est pas terminé (`/auth/change-password` rejouable sans secret dans cette fenêtre) ;
  - transport en **HTTP clair** sur le LAN ;
  - le porteur d'un jeton valide peut **masquer l'interface de façon persistante** en armant le marqueur (entamer un envoi filesystem puis couper) — réparable par `/ui`, et **inférieur** à son pouvoir existant de détruire réellement la partition.
  ⇒ **Acceptable pour un réseau domestique privé.**
- ⛔ **Cette décision ne ferme PAS le vecteur « jeton admin volé »** (voir [ADR-0120](0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md)) : il reste ouvert, et ne doit jamais être présenté comme fermé par cette feature.
- **Hors périmètre assumé (B13)** : « internet nulle part » (ni l'appareil ni le poste). Jamais résolu par une acceptation non signée ; voir le guide de mise à jour.

### Ce que ça verrouille
- **Table de routes identique avec ou sans filesystem, aucun drapeau « dégradé »** — outillé par `check_degraded_mode_absent.sh`. Toute évolution qui conditionne une route ou une décision d'auth à l'état du montage doit d'abord superséder cet ADR.
- Le nombre de lecteurs de `otaRecoveryActive()` est figé à **1**.
- `src/ota_recovery_page.cpp` ne s'édite **jamais à la main** ; `/release` s'arrête s'il est périmé.
- Toute évolution du SHA-256 ou du parseur de `.sig` dans `data/app.js` oblige à **régénérer** la page.
- Le marqueur vit en NVS `pool-ota`/`fsupd` ; pose et levée restent conditionnées au genre filesystem.
- `/recovery` reste **avant** la garde 503 ; `/ui` reste **après**.

## Références

- Code : [`src/web_server.cpp`](../../src/web_server.cpp) (`begin()`, `setupRoutes()`), [`src/web_routes_recovery.cpp`](../../src/web_routes_recovery.cpp), [`src/ota_recovery_logic.h`](../../src/ota_recovery_logic.h), [`src/ota_recovery_marker.h`](../../src/ota_recovery_marker.h), [`src/ota_recovery_page.html.in`](../../src/ota_recovery_page.html.in), [`src/fs_guard.h`](../../src/fs_guard.h)
- Outils : [`tools/gen_recovery_page.js`](../../tools/gen_recovery_page.js), [`tools/check_recovery_page_generated.sh`](../../tools/check_recovery_page_generated.sh), [`tools/check_degraded_mode_absent.sh`](../../tools/check_degraded_mode_absent.sh), [`tools/check_fs_probe_paths.sh`](../../tools/check_fs_probe_paths.sh)
- Spec : [`feature-151`](../../specs/features/done/feature-151-signature-de-l-image-ota.md) — « D3-b1 — CONCEPTION ARRÊTÉE », « REVUE DE SÉCURITÉ P1 (S1-S9) », « ÉTUDE ERGONOMIE RENDUE »
- Doc : [`docs/subsystems/web-server.md`](../subsystems/web-server.md), [`docs/subsystems/ota-manager.md`](../subsystems/ota-manager.md), [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md)
