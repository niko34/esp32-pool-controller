# bug-173 — La réponse GitHub est lue au fil de l'eau, et le parsing la croit finie

## Métadonnées
- **ID** : bug-173
- **Slug** : la-reponse-github-est-lue-au-fil-de-l-eau-et-tronquee
- **Type** : bug — **la mise à jour en ligne échoue**, pour une cause **distincte** de bug-169
- **Statut** : doing — démarrée le 2026-10-18, **implémentée + revue (code + sécurité) le
  2026-10-18** ; reste ouverte faute du test sur l'appareil réel (critère n°1, non coché)
- **Priorité** : **haute** — ⚠️ c'est le **dernier** obstacle : le dépôt est public depuis
  aujourd'hui, l'API répond **200**, les binaires portent les bons noms
- **Version cible** : 2.85.0
- **Dernière mise à jour** : 2026-10-18
- **Spec(s) liée(s)** :
  `todo/bug-169-mise-a-jour-en-ligne-aveugle-sur-depot-prive.md`
  (⭐ **l'obstacle PRÉCÉDENT — levé : le dépôt est public, l'API répond 200**) ·
  ADR-0032 (parse **en flux** avec filtre — ⚠️ **c'est la décision que ce bug remet en cause**) ·
  ADR-0112 (mbedtls obéit à la politique PSRAM — ⭐ **c'est elle qui rend la correction possible**)

## Domaines touchés
- [ ] Capteurs · [ ] Régulation · [ ] Filtration / Éclairage
- [x] **Web — routes HTTP** — `/check-update`
- [ ] MQTT · [ ] UART
- [x] **OTA**
- [ ] Authentification · [ ] Historique · [ ] Frontend

## Agents requis
- [x] spec-reader
- [x] architect — ⚠️ il y a un **arbitrage mémoire** (voir « Options »)
- [ ] pool-chemistry — ⛔ aucun chemin de dosage
- [x] **security-reviewer — OBLIGATOIRE** : ⚠️ chaîne **OTA**, et on remet en RAM une réponse
      **tierce non vérifiée**
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Constat — **mesuré** aujourd'hui, après le passage du dépôt en public

```
GET /check-update → {"error":"JSON parse error"}
```
Journal de l'appareil :
```
[44598413] Vérification des mises à jour GitHub… (PSRAM=4 128 756 o, interne 8 bits libre=54 124 o)
[44600631] /check-update : fenêtre TLS (session ouverte) — libre=44 948, plus gros bloc=32 756
[44600686] ERROR  Erreur parsing JSON GitHub: IncompleteInput
```

⚠️ **55 ms** entre l'ouverture de la session TLS et l'erreur. ⛔ Bien trop court pour avoir lu
**16,7 Ko** sur TLS. ⇒ Le flux n'a **pas** été lu jusqu'au bout.

⚠️ **Ce n'est PAS un manque de mémoire** : le code distingue explicitement les deux cas, et
l'erreur rendue est `IncompleteInput`, ⛔ pas `NoMemory`. Le plus gros bloc interne valait
**32 756 o** au moment du parse.

### La réponse, mesurée
```
content-length: 16756        (⛔ pas de chunked, pas de content-encoding)
body (notes de release) : 3 800 o
assets : 6
```

## Cause racine

`src/web_routes_ota.cpp` parse **en flux** :
```cpp
deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter));
```

⚠️ Le lecteur de flux d'ArduinoJson traite un `read()` qui rend **−1** comme une **fin de
fichier**. Or sur TLS, le tampon réseau se vide **transitoirement** au milieu d'un transfert de
16,7 Ko : les données **arrivent encore**, mais il n'y en a **pas à cet instant**.
⇒ ⛔ **Le parseur conclut que le document est fini** alors qu'il est tronqué.

⭐ **Et ça explique le souvenir de l'utilisateur** — *« au début du projet, la mise à jour via
github fonctionnait »*. Les premières releases avaient **2 fichiers joints et des notes
courtes** : la réponse tenait souvent en **un seul** segment, donc ⛔ aucune fenêtre creuse. Plus
le projet publie, plus la réponse grossit, plus l'échec devient **certain**.
⚠️ ⇒ **Un défaut qui s'aggrave tout seul**, sans qu'aucune ligne de code ne change.

## ⚠️ Ce que ce bug remet en cause — ADR-0032, et il faut le dire

ADR-0032 a **délibérément** choisi le parse **en flux** : l'ancien `getString()` + parse complet
culminait à **~35 Ko** de RAM **interne** en plus des ~48 Ko de la session TLS ⇒ échec
systématique sur tas fragmenté.

⚠️ **Cette contrainte n'existe plus de la même façon** : depuis **ADR-0112**, mbedtls alloue en
**PSRAM**, et l'appareil dispose de **~4,1 Mo** libres. ⇒ 16,7 Ko **en PSRAM** ne coûtent rien.
⛔ Mais la décision d'origine reste **juste dans son contexte** — ⇒ l'ADR de ce lot doit dire
**ce qui a changé**, ⛔ pas qu'elle était fausse.

## Options — ⚠️ à arbitrer

| | Option | Ce qu'on gagne | Ce qu'on paie |
|---|---|---|---|
| **A** | **Mémoriser la réponse en PSRAM**, puis parser depuis le tampon (filtre **conservé**) | ⭐ Supprime la cause **à la racine** ; ⛔ aucune dépendance neuve | Il faut **borner** la taille lue (⚠️ réponse tierce) et **forcer** l'allocation en PSRAM |
| **B** | **Envelopper le flux** dans un lecteur qui **attend** tant que la connexion est ouverte | Mémoire inchangée | ⚠️ Code de synchronisation délicat ; ⛔ un `timeout` mal choisi **rejoue le même défaut** |
| **C** | Bibliothèque tierce (`StreamUtils`) | Peu de code | ⛔ **Dépendance neuve** sur un chemin **OTA** |

⚠️ **Préférence à instruire** : **A**. ⭐ C'est la seule qui **supprime** la classe de défaut au
lieu de la contourner, et la contrainte qui l'interdisait a disparu.

## Comportement attendu
1. `/check-update` **aboutit** sur une réponse de **16,7 Ko**, et ⚠️ **sur des réponses plus
   grosses** — le nombre de fichiers joints et la longueur des notes **croissent**.
2. ⚠️ **Une réponse anormalement grosse est REFUSÉE proprement**, ⛔ jamais tronquée en silence :
   c'est une entrée **tierce**. Message explicite, borne **nommée dans `constants.h`** (ADR-0098).
3. ⛔ **La mémoire interne 8 bits n'est pas sollicitée** pour le tampon : il vit en **PSRAM**.
4. ⚠️ **Si la PSRAM est indisponible** — refus **explicite**, ⛔ pas un repli silencieux qui
   ramènerait le défaut d'ADR-0032.
5. ⛔ **Le filtre est CONSERVÉ** : seuls les champs consommés entrent dans le document.

## Éléments visibles
- [x] **Interface** — la recherche de mise à jour **aboutit**
- [x] **Log** — ⚠️ distinguer « réponse trop grosse » de « lecture incomplète »
- [ ] Endpoint HTTP (⚠️ comportement, ⛔ pas la route) · [ ] UI · [ ] WS · [ ] MQTT

## Critères d'acceptation
- [ ] ⭐ **Sur l'appareil réel** : `/check-update` rend `update_available` et les **deux** URL
      (`firmware.bin`, `littlefs.bin`) — ⛔ **le test qui compte**, ⚠️ **NON ATTESTÉ** : rien ne
      prouve encore que le correctif fonctionne sur le matériel, seulement en natif. **C'est la
      seule raison pour laquelle cette spec reste en `doing/`.**
- [x] Réponse **tronquée** par le pair ⇒ erreur **explicite**, ⛔ jamais un document partiel
      accepté (`OtaBufferedReadResult::kIncomplete` → `500 "GitHub response incomplete"`)
- [x] Réponse **au-delà de la borne** ⇒ refus **nommé** (`kStopTooLarge` → `500 "GitHub response
      exceeds read buffer"`, borne `kOtaCheckResponseMaxBytes` nommée dans `constants.h`)
- [x] ⛔ Aucune allocation du tampon en **RAM interne** (`heap_caps_malloc(…, MALLOC_CAP_SPIRAM)`
      + témoin `esp_ptr_external_ram()`, refus explicite si indisponible — jamais de repli
      silencieux)
- [x] **Mutation** : rétablir le parse en flux doit **tuer** un test (suite
      `test/test_native_ota_response_buffer/`, prédicat pur testé par mutation)
- [x] `pio run` 0 warning · natifs sans régression

## Plan de test manuel
1. `/check-update` sur l'appareil ⇒ ⚠️ **doit** proposer la **2.84.0**.
2. Lancer la mise à jour **en ligne** et vérifier qu'elle **aboutit**.
3. ⚠️ Relever le **plus gros bloc interne 8 bits** avant/après : ⛔ il ne doit **pas** s'effondrer.

## Notes techniques (2026-10-18)

**Option retenue : A** (tampon PSRAM, cf. tableau des options ci-dessus), exactement comme
instruit. Logique de décision extraite en module pur `src/ota_response_buffer_logic.h`
(`otaBufferedReadNextAction`, `otaBufferedReadLengthMismatch`, `otaBufferedReadResultFor`),
testée nativement par `test/test_native_ota_response_buffer/` (ADR-0017). Nouvel ADR
[0119](../../docs/adr/0119-reponse-check-update-bufferisee-en-psram-bornee.md) : trade-off
documenté, trois options tracées (tampon PSRAM retenue, envelopper le flux et bibliothèque
tierce écartées). ADR-0032 **non supersédé** — note datée ajoutée sur son extension 2026-08-04.

**Deux corrections annexes trouvées pendant l'instruction, distinctes de l'objet premier :**

1. Un commentaire de `web_routes_ota.cpp` / `docs/subsystems/ota-manager.md` affirmait que « la
   tâche AsyncTCP n'est pas abonnée au watchdog » — c'était le motif de l'ordre imposé en v2.20.1
   (réarmer le dosage avant de remonter le filesystem). **Vérifié dans la bibliothèque, pas
   supposé** : `CONFIG_ASYNC_TCP_USE_WDT` vaut 1 par défaut (`AsyncTCP.h:41-42`), le `#undef`
   d'`AsyncTCP.cpp:18` est dans un bloc LibreTiny (autre plateforme). **La tâche EST abonnée.**
   L'ordre lui-même n'a **pas** été changé (il reste défendable pour une autre raison : le
   réarmement est un drapeau RAM, le poser en premier ne dépend pas du redémarrage) — mais deux
   revues indépendantes se sont contredites sur ce point pendant l'instruction, et c'est le gate
   `pool-chemistry` qui doit se prononcer sur la base du fait corrigé, pas qui corrige le
   commentaire. Spec de suivi : `bug-175`.
2. Le code `psram_required` était réemployé pour signaler l'échec d'allocation du tampon de
   lecture — une cause **transitoire** (bloc PSRAM momentanément indisponible), alors que
   `docs/API.md` impose au client de traiter `psram_required` comme **définitif** (« ce matériel
   n'a pas de PSRAM », bouton neutralisé durablement). Corrigé en `psram_low`, déjà le code
   existant pour « temporaire, réessayable » (garde TLS sortante, ADR-0036/ADR-0112) — même
   sémantique, second site d'émission, documenté dans `docs/API.md`.

**Correction d'une prémisse du brief initial, par la revue de sécurité** : la session GitHub est
**TLS épinglée** (5 racines auto-signées, ADR-0111) — un DNS détourné produit un **échec de
handshake**, pas un corps hostile reçu avec succès. Le seul pair réellement capable de servir un
contenu arbitraire est celui qui **contrôle la release** elle-même ; seule feature-151 (signature
de l'image) y répond, puisque l'URL et l'empreinte SHA-256 sortent du même JSON.

Suites natives : `73` répertoires `test/test_native_*` mesurés le 2026-10-18
(`ls -d test/test_native_* | wc -l`, ADR-0073 — à remesurer par quiconque le cite ensuite) —
inclut la suite neuve `test_native_ota_response_buffer/`. Le décompte de cas n'a pas été
ré-exécuté par ce passage de documentation (compilation hors périmètre de `doc-writer`).

### Pourquoi cette spec reste en `doing/`
Tous les critères sauf le premier sont remplis et vérifiés dans le code. Le critère n°1 (test sur
l'appareil réel : `/check-update` rend `update_available` + les deux URL) **n'est attesté par
aucune preuve** à ce stade — ni capture, ni log d'appareil post-correctif. Ne pas déplacer vers
`done/` tant qu'il n'est pas coché.

### Fichiers touchés par ce lot
`src/constants.h`, `src/web_routes_ota.cpp`, `src/ota_response_buffer_logic.h` [nouveau],
`test/test_native_ota_response_buffer/` [nouveau], `src/version.h` (2.85.0), `CHANGELOG.md`,
`docs/API.md`, `docs/adr/0032-reponses-http-bornees.md`, `docs/adr/0119-...md` [nouveau],
`docs/adr/README.md`, `docs/subsystems/ota-manager.md`,
`specs/features/todo/bug-174-...md` [nouveau], `specs/features/todo/bug-175-...md` [nouveau],
cette spec.

## Hors périmètre
- Le **téléchargement** du firmware (`/download-update`) — **vérifié, ne porte PAS ce défaut** :
  sa boucle de lecture est pilotée par le `content-length` annoncé (pas un parse JSON en flux) et
  l'intégrité est vérifiée par comparaison SHA-256 **avant** toute activation de l'image. ⚠️ Une
  lacune **distincte** y a toutefois été trouvée pendant cette instruction (aucune échéance
  d'**inactivité** sur sa boucle, seul filet : le watchdog, qui panique plutôt que d'abandonner
  proprement — conséquence mesurée : voie FS, la partition est déjà effacée au moment du panic,
  redémarrage sur FS vide) : spec de suivi ouverte, `bug-174`.
- La signature de l'image OTA — c'est feature-151
- La reconfirmation de l'ordre `setOtaInProgress`/montage FS par `pool-chemistry` (sa prémisse
  s'est révélée fausse pendant cette instruction, voir Notes techniques ci-dessous) — spec de
  suivi distincte, `bug-175`, pour ne pas mélanger un correctif de commentaire avec une décision
  de sécurité qui doit être reconfirmée par le bon agent.
