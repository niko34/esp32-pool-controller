## Métadonnées
- **ID** : bug-165
- **Slug** : recherche-de-mise-a-jour-en-ligne-echoue
- **Type** : bug
- **Statut** : doing (correctif + documentation livrés 2026-10-14, clôture par l'orchestrateur — non déplacée vers `done/`)
- **Priorité** : **haute** — ⚠️ c'est la **voie de remédiation** : le chemin par lequel on déploie un correctif
- **Version cible** : à déterminer (constaté en **2.80.0**)
- **Dernière mise à jour** : 2026-10-14
- **Spec(s) liée(s)** : ⚠️ **[`bug-108`](bug-108-garde-tls-ota-sur-metrique-ira-inclusive.md) DÉPEND de celle-ci** — il corrige la garde qui protège ce chemin ; corriger la garde d'un chemin cassé n'a pas de sens tant qu'on ignore pourquoi il est cassé. Voir aussi [ADR-0036](../../../docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md) et [ADR-0111](../../../docs/adr/0111-ancre-de-confiance-tls-toujours-auto-signee.md) (créé pendant ce lot).

## Notes d'implémentation (2026-10-14, doc-writer)

### Cause racine identifiée et corrigée

GitHub sert désormais, pour `api.github.com`, une chaîne terminant sur **Sectigo Public Server
Authentication Root E46**, dans sa variante **CROISÉE** (signée par USERTrust ECC) — mbedtls 3.x
(core Arduino 3) refuse structurellement les ancres de confiance cross-signées, exactement le
mécanisme déjà rencontré le 2026-08-08 (`bug-checkupdate-tls-core3`, racines USERTrust AAA
cross-signées). **La règle « toutes auto-signées » avait été posée en commentaire à l'époque, mais
rien ne la réappliquait quand la chaîne réelle a évolué.** Corrigé : ajout des racines Sectigo
E46 (ECC) et R46 (RSA, chaîne de repli réelle pour les clients sans ECDSA — vérifié, pas anticipé),
toutes deux auto-signées — 3 → 5 racines. Voir [ADR-0111](../../../docs/adr/0111-ancre-de-confiance-tls-toujours-auto-signee.md)
et [docs/subsystems/ota-manager.md](../../../docs/subsystems/ota-manager.md#règle--une-ancre-de-confiance-est-toujours-la-variante-auto-signée-bug-checkupdate-tls-core3-bug-165-adr-0111).

### ⚠️ Les quatre hypothèses écartées — consignées pour qu'on ne les refasse pas

Table déjà posée par le `debugger` ci-dessus (section « Contexte »), confirmée et **reprise sans
modification** — avec les chiffres exacts, pour qu'un futur diagnostic ne reparte pas sur ces
quatre pistes sans élément neuf :

| Hypothèse | Mesure | Verdict |
|---|---|---|
| Mémoire 8 bits insuffisante | `free_heap_8bit` **54 224**, `max_alloc_8bit` **45 044** | ❌ écartée |
| PSRAM insuffisante | `psram_free` **4 174 860** / 4 194 304 — seuil exigé : 1 048 576 | ❌ écartée |
| Certificats inadaptés (anciens, les 3 d'origine) | `openssl s_client -CAfile <les 3>` ⇒ **`Verify return code: 0 (ok)`** contre `api.github.com` **au moment du test** — la chaîne a changé depuis | ❌ écartée (constat ponctuel, la chaîne GitHub a évolué après coup — cause réelle trouvée ailleurs) |
| Pas d'accès Internet | NTP synchronisé au démarrage (journal appareil) | ❌ écartée |

⚠️ **Nuance sur la 3ᵉ ligne, à ne pas perdre** : l'hypothèse « certificats inadaptés » a été
écartée parce que les 3 certificats de l'époque validaient **alors** la chaîne observée — ce n'est
pas devenu faux rétroactivement, c'est la chaîne GitHub qui a changé **après** ce test. La cause
racine réelle (ancre cross-signée) n'a été trouvée qu'en **comparant** la chaîne réellement servie
en pratique (`openssl s_client -showcerts`) à celle couverte par le fichier embarqué — pas en
reniant ce test.

### Contrat API changé

`GET /check-update` renvoie désormais `step`/`detail` sur un échec (voir
[docs/API.md](../../../docs/API.md#️-contrat-derreur-enrichi-bug-165-v280x--step-et-detail)) — le
404 « aucune release publiée » reste un `200` avec `no_release: true`, désormais **atteignable**
(avant ce correctif, la connexion échouait toujours avant d'atteindre GitHub).

### ⚠️ Risque identifié, NON corrigé, à tester après déploiement

Le chemin de **téléchargement** (`objects.githubusercontent.com`) présente une structure de
chaîne **identique** (maillon croisé) — voir [docs/subsystems/ota-manager.md](../../../docs/subsystems/ota-manager.md)
pour le détail. Non vérifié sur appareil réel, hypothèse à tester une fois une release publiée.

### Non-régression de la voie de secours

`POST /update` et `./deploy.sh ota-firmware` ne passent pas par `handleCheckUpdate` ni par
`GITHUB_ROOT_CA` côté upload manuel — aucun chemin de code partagé modifié par ce lot (vérifié par
lecture, le diff ne touche que `handleCheckUpdate`, `github_root_ca.h` et `ota_check_logic.h`).

## Domaines touchés
- [x] Web — routes HTTP / WebSocket
- [x] OTA

## Agents requis
- [x] spec-reader · [x] architect · [x] **debugger** — ⚠️ c'est le cœur : la cause n'est **pas identifiée**
- [x] **security-reviewer** — ⚠️ OBLIGATOIRE : le lot touche `web_routes_ota.cpp` (règle de `CLAUDE.md`)
- [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] pool-chemistry — aucun code de dosage

## Description
`GET /check-update` échoue systématiquement avec `{"error":"GitHub API error","code":-1}`.
Le code **−1** d'`HTTPClient` signifie **échec de connexion** — la requête n'aboutit pas.

## Contexte — constaté le 2026-10-14 sur l'appareil réel, firmware 2.80.0

La garde TLS **ne refuse pas** (aucun 503) : la requête passe, puis la connexion échoue.

### ⚠️ Quatre hypothèses VÉRIFIÉES et ÉCARTÉES

| Hypothèse | Mesure | Verdict |
|---|---|---|
| Mémoire 8 bits insuffisante | `free_heap_8bit` **54 224**, `max_alloc_8bit` **45 044** | ❌ écartée |
| PSRAM insuffisante | `psram_free` **4 174 860** / 4 194 304 — seuil exigé : 1 048 576 | ❌ écartée |
| Certificats inadaptés | Les **3** certificats de `src/github_root_ca.h` (USERTrust RSA, USERTrust ECC, **ISRG Root X1**) valident la chaîne de `api.github.com` : `openssl s_client -CAfile <les 3>` ⇒ **`Verify return code: 0 (ok)`** | ❌ écartée |
| Pas d'accès Internet | **NTP synchronisé** au démarrage (journal appareil) | ❌ écartée |

⚠️ **La cause n'est donc PAS identifiée.** ⛔ Ne pas repartir sur ces quatre pistes sans élément neuf.

### ⚠️ Observation qui oriente, et qui est contre-intuitive
Pendant **14 relevés à 0,7 s** couvrant toute la tentative, la mémoire **n'a pas bougé**
(`max_alloc_8bit` constant à 45 044, `free_heap_8bit` oscillant entre 53 780 et 57 120, planchers
`mf`/`mb` **inchangés**).
⇒ Le chiffrement **n'a jamais commencé à allouer** : l'échec est **antérieur au handshake**.
⚠️ **MAIS** — et c'est le piège : **une allocation qui ÉCHOUE ne laisse AUCUNE trace dans la
mémoire libre**, puisqu'elle n'a jamais lieu. ⛔ L'absence de creux **n'exclut pas** un échec
d'allocation ; elle exclut seulement une allocation **réussie puis libérée**.

### Un fait annexe, à ne pas confondre avec le défaut
Depuis le réseau local, `https://api.github.com/repos/niko34/esp32-pool-controller/releases/latest`
répond **404** : le dépôt n'a **aucune release publiée**. ⚠️ **Ce n'est pas la cause** — l'appareil
rapporte **−1** (connexion échouée), pas 404. Mais cela signifie qu'un correctif **ne peut pas être
validé de bout en bout** tant qu'aucune release n'existe.

## Comportement attendu
- `GET /check-update` aboutit : soit une version disponible, soit « à jour », soit un **message
  d'erreur qui NOMME la cause** — ⛔ pas un code numérique opaque.
- ⚠️ **L'échec actuel est muet** : `code: -1` ne dit ni l'étape, ni la raison. Le correctif doit
  rendre le diagnostic possible **sans sonde externe** — c'est le reproche récurrent du projet,
  *l'appareil ne dit pas ce qu'il fait*.

### Cas limites
- Aucune release publiée (cas actuel) ⇒ **404 attendu**, à distinguer d'un échec de connexion.
- GitHub limite le débit (`rate limit`) ⇒ message distinct.
- Résolution de nom qui échoue ⇒ message distinct.

## Comportement actuel
`{"error":"GitHub API error","code":-1}` — systématique, sans aucune trace exploitable au journal.

## Contraintes techniques
- ⛔ **Aucune régression de la VOIE DE SECOURS** : `POST /update` (téléversement manuel) et
  `./deploy.sh ota-firmware` ⚠️ **ne passent PAS** par cette route et doivent le rester — c'est la
  voie qu'on emprunte **quand tout va mal**.
- ⛔ Ne pas modifier la garde TLS : c'est l'objet de [`bug-108`](bug-108-garde-tls-ota-sur-metrique-ira-inclusive.md).
- Aucun handler bloquant > 50 ms.
- ⚠️ **Journaliser sans ouvrir une fuite** : la route journalise déjà l'IP cliente ; le projet a
  **déjà détruit un post-mortem par sa propre journalisation** (15/08/2026).

## Fichiers potentiellement concernés
`src/web_routes_ota.cpp` (~860-970 : `handleCheckUpdate`), `src/github_root_ca.h`,
`src/constants.h` (délais d'attente), `docs/API.md`, `docs/subsystems/ota-manager.md`.

## Pistes pour le `debugger` — ⚠️ aucune n'est privilégiée
1. **Quelle étape échoue exactement ?** Résolution de nom, connexion TCP, handshake, réponse ?
   ⇒ Instrumenter **avant** de corriger : aujourd'hui le code **−1** couvre tous ces cas.
2. **`setCACert()` n'accepte-t-il qu'UN certificat ?** Le fichier en contient **trois** concaténés.
   ⚠️ Vérifier que l'API utilisée les prend **tous** — un seul pris en compte expliquerait l'échec
   malgré des certificats valides.
3. **Délai d'attente** : un handshake TLS sur ESP32 peut dépasser un délai court.
4. **Mémoire au moment précis de l'allocation mbedTLS** — ⚠️ rappel : une allocation échouée est
   **invisible** dans la mémoire libre. Instrumenter le **retour** de l'allocation, pas le niveau.

## Critères d'acceptation
- [ ] La **cause racine est identifiée et écrite** — ⛔ pas contournée.
- [ ] `GET /check-update` aboutit sur l'appareil réel.
- [ ] L'échec, quand il survient, **nomme son étape et sa raison** au journal **et** dans la réponse.
- [ ] Les quatre hypothèses déjà écartées sont **consignées**, pour qu'on ne les refasse pas.
- [ ] ⛔ `POST /update` et `./deploy.sh ota-firmware` **sans aucune régression** — vérifié, pas supposé.
- [ ] Test natif sur la **logique de classification de l'erreur**, validé par **mutation**.

## Plan de test manuel
1. `GET /check-update` **sans release publiée** ⇒ message distinguant « aucune release » d'un échec réseau.
2. Avec une release publiée ⇒ la version est détectée.
3. Nom de domaine injoignable (bloquer au pare-feu) ⇒ message **nommant** la résolution de nom.
4. ⛔ `./deploy.sh ota-firmware` ⇒ fonctionne, **inchangé**.

## Hors périmètre
- ⛔ La **garde TLS** et ses seuils : [`bug-108`](bug-108-garde-tls-ota-sur-metrique-ira-inclusive.md).
- ⛔ La **publication d'une release** GitHub : décision de l'utilisateur, jamais automatique.

## Clôture — la règle existait, la chaîne a changé, personne ne l'a réappliquée

**Version 2.80.1.** Mesuré (ADR-0073) : `pio run -e esp32dev_core3` **SUCCESS**, 0 warning ·
`pio test -e native` **2 202 cas / 71 suites** · **19/19** contrôles d'hygiène · **5 678** liens ·
flash **+3 868 octets**.

### Verdicts
`security-reviewer` **GO sous 2 conditions** — toutes deux levées · `code-reviewer` **Approved**,
1 mineure appliquée · **5 mutations tuées**.

### ⚠️ LE FAIT MARQUANT : la règle était écrite, et elle n'a pas tenu

Le commentaire de `src/github_root_ca.h`, daté du **2026-08-08**, disait déjà :
> « mbedtls 3.x **n'accepte plus les ancres de confiance cross-signées** […] provoquaient un échec
> de handshake (**Erreur HTTP GitHub: -1**) »

⇒ **Exactement le symptôme d'aujourd'hui.** La règle « toutes auto-signées » avait été posée — mais
**en commentaire**. GitHub a migré sa chaîne ; rien n'a rappelé qu'il fallait réappliquer la règle.
⚠️ **Une règle écrite dans un commentaire n'est pas une règle : c'est un souvenir.**
⇒ Correctif de fond : `tools/check_github_root_ca.sh`, **câblé en CI**, qui vérifie
**cryptographiquement** l'auto-signature de chaque ancre. **ADR-0111**.

### Ce qui a coûté le plus de temps : l'échec était MUET
`code: -1` dans la réponse HTTP. ⚠️ **Et le détail était DÉJÀ au journal** — `X509 - Certificate
verification failed (-9984)` — **mais n'était pas remonté**. ⇒ L'information existait et n'était
pas servie. `/check-update` renvoie désormais `step` et `detail`.

### Quatre hypothèses écartées, avec leurs chiffres — ⛔ ne pas les refaire
| Hypothèse | Mesure | Verdict |
|---|---|---|
| Mémoire 8 bits | `free_heap_8bit` 54 224, `max_alloc_8bit` 45 044 | écartée |
| PSRAM | 4 174 860 / 4 194 304, seuil 1 048 576 | écartée |
| Délai d'attente | échec en **613 ms** | écartée |
| Courbe P-384 non gérée | `CONFIG_MBEDTLS_ECP_DP_SECP384R1_ENABLED=y` | écartée |

⚠️ **Et un piège de méthode, à retenir** : ma première vérification des certificats a conclu « ✔ »
**à tort**, parce que `openssl s_client -CAfile` consulte **aussi le magasin système**. ⇒ Le test
ne reproduisait pas la situation de l'appareil. **Isoler, sinon on ne teste pas ce qu'on croit.**

### Ce que la revue a rattrapé
L'empreinte d'**ISRG Root X1** n'était **pas** contrôlée — ⚠️ or c'est l'ancre du chemin de
**TÉLÉCHARGEMENT**, la seule dont la corruption casserait l'OTA **sans casser la recherche**, donc
**sans se voir**. Couverture portée à **5/5**, mutation vérifiée.

### ⛔ Restant à l'humain
- ⚠️ **Le déploiement EST le test** : la validation TLS réelle ne se prouve pas hors cible.
- Après déploiement : relancer `/check-update`, puis **tenter un téléchargement** — c'est ce qui
  dira si le risque ci-dessous est réel.

### ⚠️ Risque identifié, NON corrigé
`objects.githubusercontent.com` sert `*.github.io ← YR1 ← **Root YR (CROISÉE par ISRG Root X1)**`.
**Même structure** que celle qui vient de casser la recherche. ⛔ Ajouter « ISRG Root X2 » ne
réglerait rien — racine sans rapport. ⇒ Spec de suivi si le test après déploiement le confirme.

---

# ⛔ ROUVERT le 2026-10-14 — le correctif a été déployé et N'A PAS fonctionné

⚠️ **La clôture ci-dessus était PRÉMATURÉE.** Le correctif (2 racines auto-signées) a été commité,
poussé, déployé en **2.80.1**, puis **constaté sans effet** :
```json
{"code":-1,"step":"tls_handshake",
 "detail":"X509 - Certificate verification failed, e.g. CRL, CA or signature check failed (-9984)"}
```
⚠️ **Le CHANGELOG annonçait « refonctionne » — c'était FAUX**, rectifié dans la même entrée.
⛔ **Ce qui reste vrai et acquis** : le diagnostic. L'appareil **nomme** désormais l'étape et la
cause, au lieu d'un `-1` muet. C'est ce qui a permis de fermer six pistes en une soirée.

## ⚠️ SIX HYPOTHÈSES FERMÉES PAR L'EXPÉRIENCE — ⛔ NE PAS LES REFAIRE

| # | Hypothèse | Test réellement exécuté | Résultat |
|---|---|---|---|
| 1 | Racine auto-signée manquante | 2 racines Sectigo ajoutées, **déployé** | ❌ même erreur |
| 2 | Seul le 1er certificat du paquet est lu | Sectigo **placée en tête**, déployé | ❌ même erreur |
| 3 | Horloge fausse | Journalisée : **2026-10-14 20:32 UTC**, correcte | ❌ |
| 4 | Courbe P-384 non supportée | `mbedtls_ecp_curve_list()` à l'exécution : **secp384r1 présente (id=4)**, mbedtls **3.6.5** ; auto-vérification de E46 ⇒ **ret=0** | ❌ |
| 5 | Chaîne reçue ≠ chaîne capturée | **Callback `mbedtls_ssl_conf_verify()` activé** : 3 maillons, **identiques** à la capture | ❌ |
| 6 | Profil TLS plus restrictif | Profil **lu sur la connexion vivante** (`mbedtls_x509_crt_profile_default`, **secp384r1 autorisée**) ; `verify_with_profile()` avec **ce pointeur** + `cn="api.github.com"` ⇒ **ret=0, flags=0x0** | ❌ |

⚠️ **Également écarté** : le paquet de certificats intégré à l'IDF n'est **pas** utilisé —
`ssl_client.cpp:199-208` prend la branche `rootCABuff != NULL` ; le bundle n'est attaché que dans
le `else if`.

## 🚨 LE FAIT CENTRAL, à méditer avant de reprendre

**Tout ce qui est reproduit hors ligne RÉUSSIT. Le handshake réel ÉCHOUE. Même binaire, à 1,2 s
d'intervalle.** Même chaîne, mêmes ancres, **même pointeur de profil**, même nom d'hôte.

⚠️ Et une observation qui n'a **pas** été exploitée : le callback live ne reçoit **jamais de
`depth=3`**. La documentation dit qu'il est appelé pour chaque maillon **y compris l'ancre
trouvée** ⇒ **mbedtls abandonne avant d'essayer l'ancre**.
⚠️ **Autre observation non expliquée** : les drapeaux par profondeur ont **varié entre deux
exécutions** (une fois les 3 niveaux à `0x8`, une fois `depth=0` à `0x0`). ⛔ Non interprété.

## ⚠️ LA PISTE SUIVANTE, non testée — et c'est la plus prometteuse

⛔ **Le comptage « 5 ancres chargées » portait-il sur le BON objet ?**
Le test hors ligne parse `GITHUB_ROOT_CA` **lui-même**. Le handshake, lui, utilise
`conf->ca_chain`, rempli par `mbedtls_x509_crt_parse()` **dans `start_ssl_client()`**
(`ssl_client.cpp:201`).
🚨 **Et cette ligne ne vérifie que `ret < 0`** : `mbedtls_x509_crt_parse()` renvoie, quand il est
**positif**, le **NOMBRE DE CERTIFICATS QUI ONT ÉCHOUÉ À ÊTRE LUS** — et ce cas est **ignoré en
silence**.
⇒ **Vérifier en priorité** : parcourir **`conf->ca_chain`** (⛔ pas une analyse refaite à côté) et
compter/nommer les ancres **réellement** chargées côté handshake, plus la valeur de retour du parse.
⚠️ Si ce nombre est **inférieur à 5**, tout s'explique d'un coup.

## Matériel conservé
L'instrumentation complète (514 lignes : callback de vérification, profil, courbes, bissection
hors ligne) est conservée en **`docs/incidents/bug-165-instrumentation-tls.patch`** — ⛔ elle n'est
**pas** dans le firmware livré. `git apply` pour la reprendre.

## Contournements disponibles — ⚠️ l'appareil reste maintenable
⛔ **Aucune urgence opérationnelle** : `./deploy.sh ota-firmware` (réseau local) et le téléversement
manuel `POST /update` **fonctionnent**. Seule la recherche de mise à jour **en ligne** est hors
service.

---

# 🚨 CAUSE RACINE TROUVÉE — 2026-10-15 : ce n'est PAS un problème de certificats

⚠️ **Les huit hypothèses précédentes cherchaient au mauvais endroit.** Piste apportée par
l'utilisateur : *« au début du projet, la mise à jour via GitHub fonctionnait. Et à un moment, on a
changé de framework. »*

## LA MESURE — chemin de production, sans instrumentation

| Instant | `free8` | **`maxAlloc8`** |
|---|---|---|
| Avant ouverture TLS | 60 760 | **57 332** |
| Juste avant le handshake | 11 296 | **10 740** |
| **Pendant la vérification du certificat** | 4 852 | 🚨 **3 316** |
| 13 ms après l'échec | 59 672 | **47 092** |

⇒ **mbedtls consomme ~56 Ko de tas INTERNE** et ne laisse que **3 316 octets contigus** au moment
précis où il doit vérifier une signature **ECDSA P-384**. C'est **temporaire et massif**, ⛔ pas un
manque permanent.

## ⚠️ CONFIRMÉ PAR UN SECOND SYSTÈME, INDÉPENDANT
`fs_guard` — mécanisme **préexistant** du projet, sans aucun rapport avec l'instrumentation — a
relevé **le même chiffre (3316)** au même instant :
> `Ouvertures de fichiers internes suspendues — maxBloc8=3316 o`
> puis 30 s plus tard : `Ouvertures de fichiers rétablies — 5 refus en 30 s`

⇒ **Deux systèmes différents, la même pénurie, sur le chemin de production.** Et
`Alloc heap échouée x3` apparaît **même sur un firmware 100 % propre**.

## CE QUE ÇA EXPLIQUE — tout, d'un coup

| Observation | Explication |
|---|---|
| Ça marchait avant le changement de plateforme | mbedtls **2.28** consommait bien moins que **3.6.5** |
| Tout réussit **hors ligne** | la vérification s'y fait avec **47 Ko** disponibles, pas 3 |
| Les drapeaux **variaient** entre deux exécutions | ⚠️ une pénurie n'est **pas déterministe** |
| Aucune racine ajoutée n'y changeait rien | ⛔ le problème n'a **jamais** été les certificats |
| La garde TLS n'a pas refusé | 🚨 **c'est [`bug-108`](bug-108-garde-tls-ota-sur-metrique-ira-inclusive.md)** |

## 🚨 bug-165 ET bug-108 SONT LE MÊME PROBLÈME
La garde a lu `ESP.getFreeHeap()` = **82 796** (métrique IRAM-inclusive) contre un seuil de 65 536
⇒ **elle a autorisé**. Pendant ce temps le bloc 8 bits réellement utilisable tombait à **3 316**.
⚠️ C'est **exactement** le défaut décrit par bug-108 : *« une garde aveugle à la famine qu'elle
vise »*. Et son motif historique le disait déjà : *« sur heap dégradé l'échec était lent et
aléatoire (BIGNUM alloc en cours de handshake) »*.

## LE REMÈDE — identifié, non encore appliqué

⚠️ **`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y`** : mbedtls **force** ses allocations en RAM interne.
⇒ Il s'étrangle dans ~50 Ko **pendant que 4,17 Mo de PSRAM dorment à côté** (`psram_free` mesuré).

| Voie | Praticable ? |
|---|---|
| Changer la configuration (`MBEDTLS_EXTERNAL_MEM_ALLOC`) | ⛔ **non** — bibliothèques **précompilées** par pioarduino |
| **`mbedtls_platform_set_calloc_free()`** à l'exécution | ✅ **oui**, depuis le code du projet |

### ⚠️ TROIS RÉSERVES À INSTRUIRE AVANT DE CODER
1. **PSRAM plus lente** — sans importance pour une mise à jour.
2. Le détournement doit être posé **avant toute utilisation** de mbedtls — ⚠️ le Wi-Fi s'en sert
   peut-être dès le démarrage. **À vérifier.**
3. 🚨 **LA PLUS SÉRIEUSE** : pendant l'écriture en flash, **le cache qui donne accès à la PSRAM est
   désactivé**. Si mbedtls y détient des données **à cet instant**, l'appareil **plante**. ⛔ Ça ne
   concerne pas `/check-update`, mais **bien `/download-update`**. ⚠️ `pool-chemistry` n'est pas
   concerné, mais **`security-reviewer` l'est** : c'est la chaîne OTA.

## ⇒ CONSÉQUENCE POUR bug-108
⛔ **Ne plus le traiter isolément.** Son relevé « impossible » (mémoire pendant une session TLS)
**existe désormais** : **`maxAlloc8 = 3 316`**, mesuré. ⚠️ Et il change la donne : un seuil
calibré sur ce chiffre **refuserait toute mise à jour** tant que la PSRAM n'est pas utilisée.
**Les deux lots doivent être pensés ensemble.**

---

# ✅ CORRECTIF APPLIQUÉ ET VÉRIFIÉ — 2026-10-15 (v2.81.0), doc-writer

## Le remède identifié a été codé, et bug-108 absorbé dans le même lot

`mbedtls_platform_set_calloc_free(calloc, free)` posé dans `psramFound()` de `main.cpp`
([`src/mbedtls_psram_alloc.{h,cpp}`](../../../src/mbedtls_psram_alloc.h)), **vérifié par un témoin
réel** (allocation via le pointeur mbedtls, confirmée en PSRAM) — ⛔ **aucun allocateur écrit**,
substitution des `calloc`/`free` de la libc, déjà PSRAM-aware. État exposé par `/get-system-info`
(`mbedtls_psram_alloc` : 0 absent / 1 actif / 2 décoratif).

**Les trois réserves posées à la clôture précédente ont été instruites** :
1. PSRAM plus lente — sans importance pour une mise à jour (assumé, non reconsidéré).
2. Pose avant toute utilisation de mbedtls — **vérifiée** : posée dans `psramFound()`, juste après
   `heap_caps_malloc_extmem_enable`, avant `setupWiFi()`. Aucune utilisation de mbedtls entre les
   deux (invariant déjà vrai, préservé).
3. Cache PSRAM désactivé pendant l'écriture flash — **traité pour `/download-update`** : le tampon
   de pile `buff[512]` copie le flux **hors de l'espace mbedtls** avant toute écriture, ce qui rend
   la bascule sûre sur ce chemin (commentaire dédié posé dans `web_routes_ota.cpp`).

**bug-108 absorbé, pas traité isolément** : la garde TLS sortante (`oomTlsGateReason`) est
recalibrée dans le même lot — critère PSRAM sur le **plus gros bloc contigu** (40 960 o, mesuré
sur WROVER réel) au lieu de la PSRAM libre totale (1 Mo, jamais mesurée), seuils heap sur la
métrique **8 bits** (ADR-0058) au lieu de `ESP.getFreeHeap()`/`getMaxAllocHeap()`. Dette « hors
périmètre consigné » d'ADR-0058 **soldée**.

## Vérifié sur l'appareil réel, après déploiement

- `GET /check-update` → **200**.
- Plus gros bloc allouable **pendant le handshake** : **40 948 o** (contre 3 316 avant correctif).
- `fs_guard` : **aucune suspension** d'ouverture de fichiers pendant la même fenêtre.
- Témoin anti-décoratif : **= 1** (actif, confirmé en PSRAM).
- Voie de secours re-testée : téléversement manuel accepté par le firmware corrigé — **aucune
  régression**.

## Documentation produite dans cette passe

`docs/subsystems/oom-guard.md` (cause racine, correctif, garde recalibrée, témoin, observabilité
du repli), `docs/subsystems/ota-manager.md` (rectification de la note WROVER devenue fausse),
`docs/API.md` (conditions 503, nouveau champ `/get-system-info`), [ADR-0036](../../../docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md)
amendé par note additive (immuable, non réécrit), [ADR-0112](../../../docs/adr/0112-mbedtls-obeit-a-la-politique-psram-du-projet.md)
créé, `CHANGELOG.md` [2.81.0] + rectification de la lecture de [2.80.1].

⛔ Spec non déplacée vers `done/` — clôture prise en charge par l'orchestrateur.

---

# ✅ CLÔTURE — 2026-10-15, v2.81.0 : RÉSOLU, et bug-108 avec lui

**Mesuré** (ADR-0073) : `pio run` **SUCCESS**, 0 warning · `pio test -e native` **2 208 cas /
71 suites** · **4 mutations sur 4 tuées** · **19/19** contrôles · **5 727** liens.

## LA PREUVE — sur l'appareil, vérifiée indépendamment

| | Avant | Après |
|---|---|---|
| `/check-update` | `code:-1` | ✅ **200**, `{"no_release":true}` |
| `maxAlloc8` pendant la session | 🚨 **3 316** | ✅ **40 948** |
| `fs_guard` | suspension 30 s | ✅ **aucune** |
| Témoin anti-décoratif | — | ✅ **1** (PSRAM réelle) |
| Voie de secours | — | ✅ **re-prouvée** : le firmware corrigé accepte un téléversement |

## ⚠️ CE QUE CE BUG AURA ENSEIGNÉ

**1. Huit hypothèses sur neuf cherchaient au mauvais endroit.** Toutes supposaient un problème de
**certificats** — parce que le message d'erreur disait « vérification de certificat échouée ».
⚠️ **Le message était exact et trompeur** : la vérification échouait bien, **faute de mémoire**.

**2. La bonne question n'était pas « pourquoi ça ne marche pas » mais « qu'est-ce qui a changé
depuis que ça marchait ».** ⇒ Posée par l'**utilisateur** : *« au début du projet, la mise à jour
fonctionnait. Et à un moment, on a changé de framework. »* C'est elle qui a tout débloqué.

**3. Le remède aussi vient de l'utilisateur** : *« il n'est pas possible de déplacer ces
allocations en PSRAM ? »* ⇒ **4,17 Mo dormaient** pendant que mbedtls s'étranglait dans 3 Ko.

**4. ⚠️ Ce qui a VRAIMENT coûté du temps : l'échec était MUET.** `code: -1`, alors que le détail
était **déjà au journal**. ⇒ Une fois le diagnostic remonté, **six pistes ont été fermées en une
soirée** — chacune par une **mesure**, ⛔ jamais par un raisonnement.

**5. Deux témoins indépendants valent mieux qu'un.** `fs_guard` — sans aucun rapport avec
l'enquête — a vu **le même 3 316**. ⚠️ C'est ce qui a écarté l'idée que l'instrumentation
fabriquait elle-même la pénurie qu'elle mesurait.

**6. ⛔ Et une erreur de méthode, à ne pas répéter** : ma première vérification des certificats a
conclu « ✔ » **à tort**, parce que l'outil consultait **aussi le magasin système**. ⇒ **Isoler,
sinon on ne teste pas ce qu'on croit tester.**

## Matériel conservé
`docs/incidents/bug-165-instrumentation-tls.patch` (514 l.) et
`bug-165-instrumentation-heap-handshake.patch` (125 l.) — ⛔ hors firmware livré.
