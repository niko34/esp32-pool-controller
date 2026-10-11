# ADR-0096 — Empreinte SHA-256 obligatoire sur `POST /update` : intégrité de transport, pas authenticité d'origine

- **Statut** : Accepté
- **Date** : 2026-10-10
- **Décideurs** : `security-reviewer` (P1 sur les trois issues, GO sous 9 conditions ; P2 sur le diff livré), `architect` (arbitrage entre les trois issues), `embedded-developer`, `web-ui-developer`, `pool-chemistry` (impact : `setOtaInProgress`), `code-reviewer`, `doc-writer`
- **Spec(s) liée(s)** : [`feature-118`](../../specs/features/doing/feature-118-authenticite-de-l-image-ota.md) (ce lot : lot A), [`feature-151`](../../specs/features/todo/feature-151-signature-de-l-image-ota.md) (suivi : signature), [`bug-117`](../../specs/features/done/bug-117-upload-ota-sans-verification-d-authentification.md) (prérequis, authentification de l'upload)

## Contexte

`POST /update` (upload manuel firmware/filesystem) acceptait une image **sans empreinte** : la comparaison SHA-256 était conditionnée à `g_uploadHasExpected`, donc au bon vouloir de l'appelant. `POST /download-update` (téléchargement GitHub), elle, rendait l'empreinte **obligatoire** — une incohérence relevée comme telle dans la spec d'origine.

L'audit `security-reviewer` (P1, design) a **recadré** cette incohérence plutôt que de la confirmer telle quelle : le paramètre `digest` de `/download-update` est **lui aussi fourni par l'appelant** (relayé depuis `/check-update`, lui-même une donnée GitHub non signée), et `isUrlAllowed()` ne contrôle que l'**hôte** — n'importe quel dépôt du domaine `github.com` passe. Les deux routes sont donc au **même niveau de preuve** ; ce qui les distinguait n'était pas leur nature, seulement leur **obligation**.

Le point dur, trouvé en revue : **la page web d'upload ne peut pas calculer l'empreinte avec `crypto.subtle`**, cette API exigeant un contexte sécurisé (HTTPS ou `localhost`) — l'appareil sert du HTTP en clair sur une IP de réseau local, un contexte non sécurisé par construction. Rendre l'empreinte obligatoire sans résoudre ce point **casserait la page d'upload**, qui est l'un des deux canaux de déploiement réels de cet appareil (l'autre étant `ota_update.sh`) — il n'existe **aucun port série** dans l'environnement de développement (conteneur LXC).

## Décision

**Issue 1 retenue : empreinte obligatoire, avec une implémentation SHA-256 pure JavaScript embarquée dans `data/app.js`.** Mise en œuvre en deux lots :

- **Lot A** (ce lot, v2.66.0) : la page web et `ota_update.sh` **calculent et envoient systématiquement** l'empreinte. Le firmware la **journalise** et indique ce qu'un futur refus aurait décidé (`otaUploadWouldLotBReject()`), **sans encore refuser** une empreinte absente ou non conforme. Seul changement de refus : l'empreinte **malformée**, déplacée à la réception du paramètre (avant tout effet de bord), au lieu d'être détectée en toute fin d'upload après effacement et réécriture de la partition.
- **Lot B** (à venir, hors périmètre de cet ADR) : transforme ce verdict journalisé en refus effectif pour absente/non conforme, alignant enfin l'**obligation** de `/update` sur celle de `/download-update`.

**Ce que ce mécanisme prouve, et ce qu'il ne prouve pas — à ne jamais diluer.** L'empreinte est **calculée par l'appelant** : elle prouve que l'image écrite est celle qu'il a envoyée, **pas qui l'a produite**. Pour le **firmware**, l'IDF vérifie déjà l'image à l'activation (`Update.end(true)` → `esp_ota_set_boot_partition()`, `ESP_ERR_OTA_VALIDATE_FAILED` sur une image invalide) ; l'empreinte ajoute la détection du **mauvais fichier**, pas d'une image corrompue. Pour le **filesystem** (`U_SPIFFS`), **rien d'autre** ne vérifie l'intégrité de l'image reçue — l'empreinte y est la **seule** protection, et c'est là qu'est le gain réel de ce lot.

**La règle de sécurité n° 6 de `CLAUDE.md` (« toujours vérifier la signature/CRC avant flash ») n'est PAS satisfaite par ce lot.** Elle ne peut l'être que par une signature à clé publique — reportée à [feature-151](../../specs/features/todo/feature-151-signature-de-l-image-ota.md), dont le périmètre inclut explicitement le cas où la page web ne peut, cette fois non plus, pas signer (la clé privée n'y vit pas). Le terme à employer pour ce lot est **« intégrité du transfert »** ; le mot « sécurité » ne qualifie jamais ce mécanisme dans la documentation du projet.

## Alternatives considérées

- **Issue 1 — Empreinte obligatoire + SHA-256 JS embarqué dans `data/`** (**retenue**). Coût mesuré : `app.js.gz` passe de **73 597 à 75 257 octets** (+1 660 o, ~2,3 %), sur une partition `spiffs` de 512 Kio (524 288 o, layout v5, [ADR-0068](0068-partitions-layout-v5-flash-8-mo.md)) dont la marge libre reste de **324 181 octets** après ce lot — très loin d'un plafond. Coût de complexité : une implémentation SHA-256 pure JS à maintenir, couverte par des vecteurs extraits du code réellement livré (jamais une copie) pour empêcher toute divergence silencieuse. **Ce qu'elle prouve réellement** : l'intégrité du transfert sur les **deux** canaux de déploiement, sans exception.

- **Issue 2 — Empreinte obligatoire sauf pour la page web, qui reste dispensée** (**écartée, pour une raison plus forte que son coût affiché**). Le coût apparent (« un fail-closed avec un trou nommé, au moins honnête ») est trompeur : **le firmware ne peut structurellement PAS distinguer la page web d'un `curl` quelconque.** Tout ce que voit le handler d'upload est un corps multipart et des en-têtes — rien n'identifie l'origine « page officielle » par opposition à n'importe quel client HTTP qui imiterait ses champs. Une exemption « page web » serait donc en réalité une exemption **universellement invocable** par quiconque omet le champ `sha256` : elle ne dispenserait pas un usage légitime précis, elle désarmerait le mécanisme pour tout le monde. C'est le pivot de cet arbitrage, pas un simple détail de coût.

- **Issue 3 — Signature à clé publique, empreinte devenue sans objet** (**écartée pour ce lot, reportée**). Coût le plus élevé (génération et gestion de clé, vérification avant `Update.end()`, plancher de version anti-retour en arrière) — et **seule** option qui réponde pleinement à la règle n° 6. Écartée *pour ce lot*, pas rejetée : elle devient [feature-151](../../specs/features/todo/feature-151-signature-de-l-image-ota.md), avec le même point dur que l'issue 2 à trancher explicitement (la page web ne peut pas signer non plus — il lui faudrait un fichier `.sig` à téléverser en plus du `.bin`, ou une exemption, ce qui ramène exactement au dilemme ci-dessus). `esp_secure_boot` v2 est noté comme option **distincte**, à évaluer séparément : irréversible, exige un accès USB et la consommation d'eFuses, donc incompatible en l'état avec la contrainte « OTA seul canal de déploiement ».

## Conséquences

### Positives
- Les **deux** canaux de déploiement (page web, script) envoient désormais une empreinte sans exception codée en dur — aucune « voie officielle » n'est tentée d'être reconnue par son origine, ce qui aurait ouvert la faille décrite dans l'issue 2 écartée.
- Le refus « empreinte malformée » se déplace **avant tout effet de bord** (avant `g_uploadOwner`, `setOtaInProgress`, `LittleFS.end()`, `Update.begin()`) — gain immédiat, indépendant du lot B.
- Le passage du lot A au lot B est le changement d'**une seule branche** (`otaUploadWouldLotBReject()` déjà centralisé), pas une réécriture.
- Couverture par mutation : les tests natifs tuent les mutants introduits sur le bourrage SHA-256 (25/25 vecteurs échouent si l'implémentation est altérée) et sur le prédicat pur `parseOtaSha256Param`/`otaUploadIntegrityVerdict`.

### Négatives / dette assumée
- Ce lot ne satisfait **pas** la règle de sécurité n° 6 — seulement une étape intermédiaire vers elle. Toute lecture qui présenterait ce mécanisme comme une mesure de sécurité serait une erreur de communication, pas un fait technique nouveau.
- `+1 660 o` gzippés sur `app.js` à chaque mise à jour du filesystem, pour un mécanisme qui ne referme encore aucun refus (lot A journalise seulement).
- Le lot B reste à livrer pour que l'obligation soit réelle ; tant qu'il ne l'est pas, une image **sans** empreinte, ou avec une empreinte **non conforme**, continue d'être flashée sur `/update`.

### Ce que ça verrouille
- Toute future route qui écrit une partition depuis une entrée réseau devra, par cohérence avec cet ADR, soit exiger une preuve d'origine **vérifiable indépendamment de l'appelant**, soit documenter explicitement qu'elle ne le fait pas — ne jamais présenter une empreinte auto-fournie comme une authentification.
- Le régime de R2 (recensement du tas, [ADR-0081](0081-recensement-du-tas-et-non-livraison-de-la-lecture-de-contenu.md)) reste conditionné à l'absence d'authenticité OTA **jusqu'à la livraison de la signature** ([feature-151](../../specs/features/todo/feature-151-signature-de-l-image-ota.md)) — pas jusqu'à celle de ce lot, qui ne change pas l'argument du plafond OTA (un porteur d'identifiants CRITICAL a déjà l'exécution de code arbitraire via `/update`).
- L'exemption par origine perçue (« c'est la page officielle, donc on lui fait confiance ») est explicitement écartée comme motif recevable dans ce projet : elle a été nommée, chiffrée, et rejetée ici — une future relecture ne doit pas la réintroduire sans relire cette section.

## Références

- Code : [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (handler d'upload), [`src/ota_integrity_logic.h`](../../src/ota_integrity_logic.h)/[`.cpp`](../../src/ota_integrity_logic.cpp) (`parseOtaSha256Param`, `otaUploadIntegrityVerdict`, `otaUploadWouldLotBReject`), [`data/app.js`](../../data/app.js) (`createSha256`, `sha256OfFile`), [`ota_update.sh`](../../ota_update.sh) (`compute_sha256`), [`src/constants.h`](../../src/constants.h) (`kOtaSha256MissingMessage`, préparée pour le lot B)
- Tests : `test/test_native_ota_integrity/`, [`test/test_native_ota_sha256_param/`](../../test/test_native_ota_sha256_param/), [`test/test_native_ota_order_guard/`](../../test/test_native_ota_order_guard/), [`test/sha256_vectors.js`](../../test/sha256_vectors.js) + [`tools/check_sha256_vectors.sh`](../../tools/check_sha256_vectors.sh)
- Spec : [`specs/features/doing/feature-118-authenticite-de-l-image-ota.md`](../../specs/features/doing/feature-118-authenticite-de-l-image-ota.md) (verdict `security-reviewer` P1, les 9 conditions), [`specs/features/todo/feature-151-signature-de-l-image-ota.md`](../../specs/features/todo/feature-151-signature-de-l-image-ota.md)
- Doc externe : [`docs/API.md`](../API.md#post-update--critical), [`docs/subsystems/ota-manager.md`](../subsystems/ota-manager.md#lot-a-feature-118-empreinte-envoyee-systematiquement-verdict-journalise), [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md)
