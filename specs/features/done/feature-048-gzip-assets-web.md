# Feature-048 — Pré-compression gzip des assets web

- **Type** : feature (build + serving, format minimal)
- **Statut** : done (reste la validation on-target manuelle)
- **Version cible** : 2.12.1
- **Créée** : 2026-07-06 (audit de taille web)
- **Domaines** : build FS (`minify.js`, `build_fs.sh`), serving statique (`src/web_server.cpp` — vérification, a priori sans modif), aucune logique métier

## Contexte / mesures

Le FS embarque ~445 KB dont ~389 KB de texte servi non compressé. Gzip -9 mesuré sur les fichiers minifiés : app.js 153,5→39,9 K, index.html 79,9→14,0 K, uPlot 50,9→21,9 K, app.css 49,2→9,8 K, wizard+wifi+login 55,9→16,7 K. **Gain total ~287 KB** (payload 445 → ~158 KB, partition 82 % → ~27 %) et chargement ~4× plus rapide (le débit de l'ESP32 est le facteur limitant de l'UI mobile).

ESPAsyncWebServer gère nativement les assets pré-compressés : `serveStatic` et `request->send(fs, path)` cherchent `<path>.gz` et servent avec `Content-Encoding: gzip`.

## Comportement attendu

1. `minify.js` : après minification, **gzipper (zlib niveau 9) les fichiers texte** (`.js`, `.html`, `.css`) dans `data-build/` et ne conserver QUE la variante `.gz` pour ces fichiers (les images/icônes restent telles quelles). `uPlot.iife.min.js` et `uPlot.min.css` inclus.
2. `build_fs.sh` : inchangé (il empaquette `data-build/`).
3. **Vérification firmware exhaustive** : recenser TOUS les chemins servant des fichiers (`serveStatic("/")` + index par défaut, `request->send(LittleFS, ...)` explicites — wizard, login, wifi, captive portal AP) et confirmer dans la source de la lib (`.pio/libdeps/`) que chacun résout la variante `.gz`. Si un chemin ne la résout pas → l'adapter (ou conserver la variante non compressée pour CE fichier uniquement, documenté).
4. Aucun changement des routes API/JSON (non concernées).

## Cas limites
- Client sans `Accept-Encoding: gzip` : tous les navigateurs le supportent ; un `curl` brut sur un fichier statique recevra du gzip (documenté, sans impact API).
- OTA FS : `littlefs.bin` inchangé dans son flux (image complète).
- Mode AP / portail captif : wizard/wifi/login servis gzippés — navigateurs captifs OK.

## Critères d'acceptation
- [x] `data-build/` ne contient plus de `.js/.html/.css` non compressés (hors exceptions documentées), les `.gz` sont présents.
- [x] `./build_fs.sh` SUCCESS, taille de l'image FS mesurée avant/après consignée (~158 KB attendu).
- [x] Revue firmware : tous les chemins de serving résolvent `.gz` (preuve : références lib).
- [ ] On-target : les 4 pages (app, wizard, wifi, login) se chargent, devtools montre `Content-Encoding: gzip` ; API inchangée.
- [x] `pio run` sans warning si src touché ; `node --check` sur minify.js n/a (script build).

## Notes d'implémentation (2026-07-06, v2.12.1, code-reviewer : Approved)

- **`minify.js`** : nouvelle fonction `writeGzip()` (zlib niveau 9) appliquée après minification HTML/JS/CSS — seule la variante `.gz` est écrite dans `data-build/`, y compris sur les chemins d'erreur (fallback = source non minifiée mais gzippée). Images/icônes copiées telles quelles. `build_fs.sh` et `src/web_server.cpp` **inchangés**. `src/version.h` → 2.12.1.
- **Mesures** : payload FS **443 → 155 KB**, occupation partition `spiffs` (576 KB) **78 % → 27 %**. `data-build/` : 8 `.gz` + 4 images. `./build_fs.sh` SUCCESS, `pio run` 0 warning.
- **Vérification serving — références lib (ESPAsyncWebServer v3.6.0, vérifiées ×2)** :
  - `serveStatic("/", LittleFS, "/")` (`web_server.cpp:100`) → `AsyncStaticWebHandler` teste `path + ".gz"` si le fichier nu est absent (`WebHandlers.cpp:150-178`) ;
  - `request->send(LittleFS, path)` — routes `/`, `/index.html`, `/wizard.html`, `/login.html`, `/wifi.html`, portail captif inclus → `AsyncFileResponse` bascule sur `path + ".gz"` et pose `Content-Encoding: gzip` (`WebResponses.cpp:642-644`).
- **Mineure préexistante (hors périmètre, notée en revue)** : les routes `/config` et `/config.html` (`web_server.cpp:91-97`) servent `/config.html` qui n'existe pas dans `data/` — chemin mort antérieur à la feature, comportement inchangé (404 identique avant/après).
- **Reste à valider on-target** : chargement des 4 pages (app, wizard, wifi, login) avec `Content-Encoding: gzip` visible en devtools ; routes API/JSON inchangées.
