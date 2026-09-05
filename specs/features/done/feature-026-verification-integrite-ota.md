## Métadonnées
- **ID** : feature-026
- **Slug** : verification-integrite-ota
- **Type** : feature
- **Statut** : done
- **Priorité** : critique
- **Version cible** : **2.11.0**
- **Dernière mise à jour** : 2026-07-06 (livrée en v2.11.0 — voir Notes d'implémentation ; validation on-target restante via la check-list ci-dessous)
- **Spec(s) liée(s)** : aucune — issue de la revue de code projet (2026-06-20)

> **Annotations d'audit (2026-07-06)** — spec confrontée au code v2.10.1, entièrement valide (zéro empreinte dans le code OTA, `static lastLog` l.124 toujours non réinitialisé) :
> 1. **Opportunité de design inconnue de la spec** : l'API REST GitHub expose un champ **`digest` (SHA-256) par asset** dans la réponse déjà interrogée par `handleCheckUpdate`. L'alternative « SHA-256 incrémental (mbedtls, dispo IDF) comparé au digest GitHub » domine probablement le MD5 (`Update.setMD5`) : zéro changement du process de release, empreinte issue de la source déjà utilisée. À trancher dans l'ADR (architect vérifie la présence effective du champ `digest` dans la réponse API utilisée et le budget RAM/CPU du SHA-256 par chunk).
> 2. **Périmètre précisé** : `littlefs.bin` (flux FS OTA) est couvert au même titre que `firmware.bin`.
> 3. **Politique par chemin** : GitHub = fail-closed strict (digest disponible) ; upload manuel USB/HTTP = pas d'empreinte naturelle → calcul + log de l'empreinte, flash autorisé (l'utilisateur est physiquement présent), comportement documenté — conforme à l'AC2 « ou explicitement documenté ».
> 4. Version cible réalignée : 2.11.0 (base réelle 2.10.1).

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [x] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [ ] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Ajouter une **vérification d'intégrité explicite (MD5/CRC)** de l'image avant tout flash OTA, conformément à la règle de sécurité absolue #6 du projet (« OTA : toujours vérifier la signature/CRC avant flash »).

## Contexte
La revue de code (2026-06-20) a relevé que l'OTA repose **uniquement** sur la validation implicite de `Update.end(true)` (vérification du magic byte de l'image ESP32), sans empreinte MD5/CRC fournie par la source. Une image tronquée par une coupure réseau, corrompue en transit ou partiellement téléchargée peut donc passer la validation minimale et être flashée. La règle #6 de `CLAUDE.md` impose une vérification d'intégrité, aujourd'hui non satisfaite. C'est le point le plus critique de la revue car il touche une règle non négociable et peut briquer la carte.

## Comportement attendu
- La source de mise à jour (asset de release GitHub et/ou manifeste OTA) fournit l'empreinte **MD5** (ou taille + CRC) de l'image firmware attendue.
- Avant l'écriture, le firmware appelle `Update.setMD5(<empreinte attendue>)` afin que la stack `Update` rejette l'image en cas de mismatch.
- En cas d'empreinte absente ou invalide : le flash est **refusé** (fail-closed), avec un log `CRITICAL` explicite et un code HTTP/réponse d'erreur clair côté UI.
- À la fin, vérifier que `Update.end(true)` confirme l'intégrité ; toute incohérence → rollback / pas de redémarrage sur l'image suspecte.
- Le chemin **upload manuel** (USB/HTTP) doit au minimum calculer et logger l'empreinte de l'image reçue ; idéalement comparer à une empreinte saisie/transmise.

### Cas limites
- Téléchargement interrompu (coupure WiFi) → empreinte ne correspond pas → flash refusé, état précédent conservé.
- Empreinte non fournie par le serveur → refus explicite plutôt que flash « best effort ».
- Image de taille 0 ou tronquée → refus avant écriture flash.
- Double upload sans reboot (cf. `web_routes_ota.cpp:124` `static size_t lastLog` non réinitialisé) → réinitialiser l'état de progression entre deux tentatives.

## Comportement actuel
- `ota_manager.*` / `web_routes_ota.cpp` flashent l'image en s'appuyant sur la seule validation `Update.end(true)`.
- Aucune empreinte MD5/CRC n'est demandée ni vérifiée avant écriture (`web_routes_ota.cpp:135`).
- `handleCheckUpdate` (chemin GitHub) ne récupère pas d'empreinte de l'asset.

## Contraintes techniques
- ESP32, offline first — la vérification ne doit pas introduire de dépendance cloud obligatoire au-delà de la source de release déjà utilisée.
- AsyncWebServer : le handler d'upload ne doit pas bloquer > 50 ms par chunk (calcul MD5 incrémental, pas en une passe).
- Watchdog actif pendant tout le flux OTA.
- Compatibilité ascendante : si une ancienne source ne fournit pas d'empreinte, documenter le comportement (refus vs warning configurable).

## Fichiers potentiellement concernés
- `src/ota_manager.*` — appel `Update.setMD5()`, récupération/validation empreinte, gestion fail-closed [modif]
- `src/web_routes_ota.cpp` — chemin upload manuel + check update GitHub, réinit état progression [modif]
- `src/constants.h` — éventuelles constantes (timeouts, mode strict/lax) [modif]
- `docs/UPDATE_GUIDE.md` — procédure OTA avec empreinte, comportement en cas de mismatch [doc]
- `docs/API.md` — format de réponse d'erreur d'intégrité [doc]

## Nouvelles signatures / API (optionnelle)
- Fonctions publiques : `bool OtaManager::beginWithChecksum(size_t size, const String& md5);` (ou équivalent)
- Champs de réponse HTTP : `{"error":"integrity_mismatch", ...}` sur échec de vérification

## Sécurité chimique (optionnelle)
- Dosage impliqué : non.

## ADR attendu ?
- **Probable** : oui — **créé** : [ADR-0022](../../../docs/adr/0022-verification-integrite-ota.md) (2026-07-06)
- **Critère(s) candidat(s)** : 1, 4 (contrainte structurante sur le format de release + verrouillage compat OTA long terme)
- **Sujet pressenti** : stratégie de vérification d'intégrité OTA (MD5 fourni par release vs CRC embarqué vs signature) et comportement en l'absence d'empreinte (strict fail-closed).

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Routes OTA existantes dans `web_routes_ota.cpp` (réponse d'erreur enrichie sur mismatch)

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/ota-manager.md` — vérification d'intégrité, fail-closed (créer si absent)

#### Docs transverses
- `docs/UPDATE_GUIDE.md` — procédure + cas mismatch
- `docs/API.md` — réponse d'erreur d'intégrité
- `CHANGELOG.md` — entrée sécurité OTA

#### Docs immuables
- `docs/adr/NNNN-verification-integrite-ota.md` — si ADR confirmé

## Critères d'acceptation
- [ ] AC1 — Une image dont le SHA-256 ne correspond pas à l'empreinte attendue est **refusée** avant/pendant le flash, sans redémarrage sur l'image suspecte. *(implémenté + revu ; validation finale on-target : scénarios 2, 4 et 6)*
- [x] AC2 — En l'absence d'empreinte fournie, le comportement est fail-closed (refus) ou explicitement documenté comme tel. *(GitHub : fail-closed strict `400 integrity_digest_missing` + bouton Installer désactivé ; upload manuel : empreinte loggée, comportement documenté — [ADR-0022](../../../docs/adr/0022-verification-integrite-ota.md), docs/UPDATE_GUIDE.md)*
- [ ] AC3 — Un log `CRITICAL` est émis sur tout échec d'intégrité, avec un message clair côté UI. *(implémenté + revu ; validation on-target : scénarios 2 et 6)*
- [ ] AC4 — Un OTA nominal (empreinte correcte) réussit comme avant. *(validation on-target : scénario 1)*
- [ ] AC5 — Deux tentatives d'upload successives sans reboot n'altèrent pas l'état de progression/log. *(partiellement vérifié : fix `lastLog` → `g_uploadLastLog` réinitialisé à `index==0`, revu ; validation on-target : scénario 5)*
- [x] `pio run` compile sans nouveau warning. *(0 warning, Flash 84,5 %, build_fs OK)*
- [x] Tous les libellés utilisateur sont en français. *(messages UI et logs vérifiés en revue)*

## Plan de test manuel
1. OTA nominal avec empreinte correcte → succès + reboot sur nouvelle version.
2. OTA avec image volontairement tronquée → refus, version précédente conservée.
3. OTA sans empreinte fournie → refus explicite (mode strict).
4. Coupure WiFi pendant le download → refus, pas de brique.
5. Deux uploads consécutifs sans reboot → progression correcte au 2ᵉ.

## Check-list de validation on-target

> Ajoutée par le test-engineer (2026-07-05). Tests **manuels** à exécuter sur l'ESP32 réel
> (matériel + réseau requis, non couverts par `pio test -e native`). Les tests unitaires
> natifs de la logique pure sont dans `test/test_native_ota_integrity/` (22 tests, verts).

| # | Scénario | Étapes | Résultat attendu |
|---|---|---|---|
| 1 | OTA nominal GitHub | Page Mise à jour → « Vérifier » → « Installer » sur la dernière release GitHub (assets avec digest) | Téléchargement + vérification SHA-256 OK, reboot sur la nouvelle version, version affichée à jour |
| 2 | Image tronquée | Servir un `firmware.bin` tronqué (ou couper le serveur en cours de download) avec un digest de l'image complète | Refus (mismatch SHA-256), log `CRITICAL`, message d'erreur clair côté UI, **version précédente conservée**, pas de reboot sur l'image suspecte |
| 3 | Digest absent (vieille release) | Pointer « Vérifier » sur une release GitHub dont l'asset n'expose pas de champ `digest` (si une telle release existe encore) | Bouton « Installer » **désactivé** avec message explicite (fail-closed, AC2) |
| 4 | Coupure WiFi pendant download | Lancer l'installation GitHub puis couper le WiFi (ou l'AP) en cours de téléchargement | Refus propre (timeout/mismatch), pas de brique : la carte reste sur la version courante et reste joignable après reconnexion |
| 5 | Deux uploads manuels consécutifs | Upload manuel `/update` d'un firmware valide, **sans reboot**, relancer immédiatement un second upload | Progression/logs corrects au 2ᵉ upload (état `lastLog` réinitialisé, AC5) |
| 6 | Param `sha256` optionnel de `/update` avec empreinte fausse | `curl -F "sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" -F "firmware=@.pio/build/esp32dev/firmware.bin" http://<ip>/update` (champ POST, empreinte volontairement fausse — 64×'a', avec ou sans préfixe `sha256:` ; auth requise) | Réponse FAIL (`integrity_mismatch`), log `CRITICAL`, flash refusé, version conservée |

## Notes pour agent
- Règle de sécurité #6 non négociable : ne pas livrer un mode « best effort » par défaut.
- Le calcul MD5 doit être incrémental (par chunk) pour ne pas bloquer le handler async.
- Vérifier l'interaction avec `web_routes_ota.cpp:124` (`static size_t lastLog`) et le throttle de progression.

## Hors périmètre
- Signature cryptographique asymétrique (clé publique embarquée) — pourra faire l'objet d'une feature ultérieure si besoin.
- Rollback automatique multi-partitions au-delà du mécanisme `Update` standard.

## Notes d'implémentation (2026-07-06, v2.11.0)

### Design retenu ([ADR-0022](../../../docs/adr/0022-verification-integrite-ota.md))

**SHA-256** (et non MD5) : hachage incrémental mbedtls (`OtaStreamHasher`, accélération matérielle, O(len) par chunk) comparé au champ **`digest`** (`"sha256:<64hex>"`) exposé par l'API GitHub `releases/latest` pour chaque asset — présence **vérifiée sur la release réelle v2.5.2** du projet, zéro changement du process de release. `Update.setMD5` écarté : ne couvre pas le flux filesystem écrit via `esp_partition_write()`.

- **Transit du digest par l'UI** : `/check-update` renvoie les champs additifs `firmware_digest`/`filesystem_digest`, `app.js` les repasse en POST `digest=` à `/download-update` — même modèle de menace que l'URL déjà relayée (whitelist hôtes GitHub + auth CRITICAL). Le re-fetch API côté firmware a été écarté (double le pic RAM TLS + `getString`).
- **Fail-closed strict chemin GitHub** : digest absent → `400 integrity_digest_missing` (refus AVANT connexion) ; malformé → `400 integrity_digest_invalid` ; mismatch → firmware : `Update.abort()` avant `end()` (aucun boot sur image suspecte), FS : `422` sans restart, remontage LittleFS best-effort, re-tentative requise (partition FS suspecte, firmware intact bootable). UI : messages français distincts, bouton **Installer désactivé** si digest vide (fail-closed visible).
- **Upload manuel `/update`** : empreinte toujours calculée et loggée ; param POST `sha256` **optionnel** (mismatch ou malformé → abort) ; **verrou anti-upload-concurrent** `g_uploadOwner` (409 pour le 2ᵉ client, `onDisconnect` → abort + réarmement dosage, owner périmé repris si `!Update.isRunning()`) ; fix `lastLog` réinitialisé à `index==0` (AC5).
- **ArduinoOTA/espota** : déjà protégé par le MD5 du protocole, non modifié.
- **Logique pure** `parseSha256Digest` / `sha256Equal` (temps constant) / `sha256ToHex` (pattern ADR-0017).

### Fichiers créés / modifiés

- **Créés** : `src/ota_integrity.h/.cpp` (OtaStreamHasher), `src/ota_integrity_logic.h/.cpp` (logique pure), `test/test_native_ota_integrity/test_ota_integrity_logic.cpp` (22 tests), `docs/adr/0022-verification-integrite-ota.md`.
- **Modifiés** : `src/web_routes_ota.cpp` (vérification par chemin, verrou d'upload, réponses 400/409/422), `src/constants.h` (`kOtaSha256HexLen`, `kOtaSha256Prefix`), `src/version.h` (2.11.0), `data/app.js` (transit digest, messages d'erreur, bouton désactivé), docs (`API.md`, `UPDATE_GUIDE.md`, `subsystems/ota-manager.md`, `adr/README.md`, `CHANGELOG.md`).

### Historique de revue

- **Request changes #1** — upload concurrent sur `/update` : deux requêtes simultanées corrompaient les statiques du handler (le `Update.begin()` du 2ᵉ échouait APRÈS réinitialisation du hasher et effacement de l'empreinte du 1ᵉʳ). Corrigé : verrou `g_uploadOwner` + marqueur `_tempObject` → 409 + `onDisconnect` de nettoyage.
- **Request changes #2** — fuite `setOtaInProgress(true)` sur échec `Update.begin()` : pompes doseuses inhibées jusqu'au reboot. Corrigé : appariement `setOtaInProgress(false)` sur tous les chemins terminaux.
- **Approved final.** Mineure cosmétique consignée (non bloquante) : après reprise d'un owner périmé, l'ancien client peut recevoir un faux « OK » — sans impact sécurité.

### Tests

- 22 tests natifs `test/test_native_ota_integrity/` — **185/185 verts** au total. `pio run` : 0 warning, Flash 84,5 % ; `build_fs.sh` OK.
- Validation matérielle/réseau restante : **check-list on-target ci-dessus** (6 scénarios, écrite par test-engineer le 2026-07-05) — couvre AC1, AC3, AC4 et la fin d'AC5.
