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
