# ADR-0119 — La réponse `/check-update` est bufferisée en PSRAM bornée, plus jamais parsée en flux

- **Statut** : Accepté
- **Date** : 2026-10-18
- **Décideurs** : Nicolas + Claude (architect, security-reviewer, code-reviewer)
- **Spec(s) liée(s)** : `bug-173-la-reponse-github-est-lue-au-fil-de-l-eau-et-tronquee`

## Contexte

Incident constaté en production le 2026-10-18, **le jour même du passage du dépôt en public** (l'API GitHub répond enfin `200`, après `bug-169`) : `/check-update` échouait systématiquement avec `ERROR Erreur parsing JSON GitHub: IncompleteInput`, **55 ms** après l'ouverture de la session TLS — bien trop court pour avoir lu les **16 756 o** de la réponse.

Cause : `deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter))` ([ADR-0032, extension 2026-08-04](0032-reponses-http-bornees.md#extension-2026-08-04--check-update-parse-en-flux-entrant-v2161)) parse **en flux**. Le lecteur de flux d'ArduinoJson traite un `read()` qui rend `-1` comme une **fin de document**. Sur TLS, le tampon réseau se vide **transitoirement** au milieu d'un transfert de 16,7 Ko — les données arrivent encore, il n'y en a simplement pas à cet instant précis — et le parseur conclut à tort que le document est fini.

Ce défaut **s'aggrave tout seul, sans qu'aucune ligne de code ne change** : plus le projet publie (assets, notes de release plus longues), plus la réponse GitHub grossit, plus la probabilité d'une fenêtre TLS creuse pendant le transfert augmente. Cela explique le souvenir utilisateur (« au début du projet, la mise à jour via GitHub fonctionnait ») : les premières releases, avec 2 assets et des notes courtes, tenaient souvent en un seul segment TLS.

**Ce lot révise, sans l'annuler, l'extension du 2026-08-04 d'[ADR-0032](0032-reponses-http-bornees.md)** (voir la note datée ajoutée à cet ADR). Le choix du parse en flux n'était **pas une erreur** : `https.getString()` + parse complet culminait à **~35 Ko de RAM interne**, en plus des ~48 Ko de la session TLS, sur un tas qui n'avait alors aucun moyen de placer ce volume ailleurs. C'est le **contexte** qui a changé — [ADR-0112](0112-mbedtls-obeit-a-la-politique-psram-du-projet.md) a fait obéir mbedtls à la politique PSRAM du projet, libérant ~4,1 Mo mesurés le même jour. 16,7 Ko en PSRAM ne coûtent plus rien.

## Décision

La réponse GitHub n'est **plus parsée en flux**. Elle est :

1. **Mémorisée dans un tampon alloué explicitement en PSRAM** (`heap_caps_malloc(…, MALLOC_CAP_SPIRAM)`, jamais de repli implicite vers la RAM interne), dimensionné sur le `content-length` annoncé (entrée **tierce**, jamais de confiance aveugle — ne sert qu'à **dimensionner**, capé à une borne nommée `kOtaCheckResponseMaxBytes` = 128 Ko, soit ×8 la taille mesurée le jour de l'incident).
2. **Lue par une boucle bornée** à la fois en taille (`kOtaCheckResponseMaxBytes`) et en durée (`kOtaCheckResponseReadTimeoutMs` = 15 s — large marge volontaire : un timeout agressif rejouerait le symptôme corrigé pour une connexion simplement lente). La **décision** de continuer/arrêter à chaque itération est une fonction **pure** (`otaBufferedReadNextAction`, `src/ota_response_buffer_logic.h`), testée nativement et par mutation ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) — c'est elle qui porte l'invariant central : une absence de donnée **transitoire** (`available() == 0`) ne doit **jamais** être confondue avec une fin de document, même connexion ouverte.
3. **Vérifiée dans son placement réel** par `esp_ptr_external_ram()` avant tout usage — témoin anti-décoratif (même idiome que `mbedtls_psram_alloc.cpp`) : le projet a déjà eu un parachute « en PSRAM » resté décoratif faute de vérification.
4. **Parsée depuis ce tampon** une fois complète (`deserializeJson(doc, buffer, len, Filter(filter))`), filtre ArduinoJson **conservé à l'identique**, document résultat toujours sur l'allocateur **plafonné net** `BudgetJsonAllocator` (`kOtaCheckJsonBudgetBytes` = 8192 o, inchangé).

Trois issues anormales, **distinguées explicitement** (jamais un refus muet ou ambigu) :
- **trop gros** (> `kOtaCheckResponseMaxBytes`) → `500`, entrée tierce anormalement grosse refusée ;
- **trop lent** (> `kOtaCheckResponseReadTimeoutMs`) → `500`, abandon propre ;
- **incomplet** (connexion fermée/EOF avant la fin attendue du `content-length`) → `500`, jamais un document partiel accepté silencieusement.

L'échec d'allocation PSRAM (bloc contigu de 128 Ko — ou moins si `content-length` le permet — indisponible à cet instant) répond `503` avec le code `psram_low` (condition **transitoire**, réessayable) et **jamais** `psram_required` (contrat **permanent**, « ce matériel n'a pas de PSRAM ») : la PSRAM existe bel et bien sur ce module, c'est un bloc contigu qui manque passagèrement.

## Alternatives considérées

- **Envelopper le flux dans un lecteur qui attend tant que la connexion reste ouverte** (rejetée) — mémoire inchangée, mais code de synchronisation délicat : un timeout mal calibré à l'intérieur de ce lecteur rejoue exactement le défaut corrigé (conclure à une fin de lecture sur une absence de donnée transitoire).
- **Bibliothèque tierce (`StreamUtils`)** (rejetée) — peu de code à écrire, mais introduit une dépendance neuve sur un chemin **OTA**, ce que le projet évite par principe sur cette surface.
- **Mémoriser la réponse en PSRAM puis parser depuis le tampon** (retenue) — seule option qui supprime la **classe** de défaut (un lecteur de flux qui ne peut pas distinguer « vide transitoirement » de « fini ») au lieu de la contourner, rendue possible par la disparition de la contrainte mémoire qui avait motivé le parse en flux en 2026-08-04.

## Conséquences

### Positives
- Supprime la cause racine à la source, plutôt que de la contourner (pas de patch sur le timeout du flux, pas de retry côté client).
- Le pic mémoire du handler croît avec la taille de la réponse GitHub (future-proof : plus d'assets, notes plus longues) sans jamais retoucher ce fichier avant le prochain facteur ×8.
- `BudgetJsonAllocator` et le filtre ArduinoJson — la partie de l'extension 2026-08-04 qui n'était pas en cause — sont **conservés à l'identique** : seule l'étape d'acquisition du corps change.

### Négatives / dette assumée
- Un bloc PSRAM contigu de 128 Ko (ou moins si `content-length` le permet) est désormais exigé à chaque appel de `/check-update` — négligeable face aux ~4,1 Mo mesurés, mais ce n'est plus zéro.
- Le code `psram_low` est désormais émis par **deux** points de garde distincts (la garde TLS sortante d'[ADR-0036](0036-psram-requise-mise-a-jour-en-ligne.md)/[ADR-0112](0112-mbedtls-obeit-a-la-politique-psram-du-projet.md), et l'allocation du tampon de réponse ci-dessus). La sémantique côté client (« temporaire, réessayable ») reste identique, mais un client qui voudrait distinguer les deux causes n'a aucun moyen de le faire depuis le seul code — acceptable car aucun client n'a ce besoin aujourd'hui.
- Le tampon mémorise une réponse **tierce** avant toute vérification de son contenu. ⚠️ Nuancé par le fait que la session est **TLS épinglée** (5 racines auto-signées, [ADR-0111](0111-ancre-de-confiance-tls-toujours-auto-signee.md)) : un DNS détourné produit un **échec de handshake**, pas un corps hostile livré avec succès. Le seul pair réellement capable de servir un contenu arbitraire est celui qui **contrôle la release GitHub elle-même** — seule [feature-151](../../specs/features/todo/feature-151-signature-de-l-image-ota.md) (signature de l'image) répond à ce cas, puisque l'URL et l'empreinte SHA-256 proviennent du **même** JSON (une release compromise fournirait un couple URL/empreinte cohérent entre eux, mais faux).

### Ce que ça verrouille
- **Tout parse d'une réponse d'API externe reçue via un `Stream` réseau (TLS ou non) doit bufferiser avant de parser**, jamais parser en flux directement sur le `Stream` — le lecteur de flux d'ArduinoJson (et plus généralement tout lecteur qui traite `read() == -1` comme EOF) ne distingue pas une absence de donnée transitoire d'une fin de document.
- Toute bufferisation de ce type doit : (a) être bornée en taille **et** en durée par des constantes nommées, (b) être allouée en PSRAM avec repli **explicite** (refus nommé), jamais un repli silencieux vers la RAM interne, (c) **vérifier** son placement réel plutôt que le supposer.
- Les patrons (a)/(b)/(c) d'[ADR-0032](0032-reponses-http-bornees.md) restent la référence pour les réponses **sortantes** (construites par ce firmware) ; ce lot ajoute un patron symétrique pour les réponses **entrantes** lues depuis un flux réseau tiers — ADR-0032 n'est pas supersédé, sa note datée du 2026-10-18 renvoie ici.

## Références

- Code : [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) (`handleCheckUpdate`), [`src/ota_response_buffer_logic.h`](../../src/ota_response_buffer_logic.h) (`otaBufferedReadNextAction`, `otaBufferedReadLengthMismatch`, `otaBufferedReadResultFor`), [`src/constants.h`](../../src/constants.h) (`kOtaCheckResponseMaxBytes`, `kOtaCheckResponseReadTimeoutMs`)
- Tests : [`test/test_native_ota_response_buffer/`](../../test/test_native_ota_response_buffer/)
- Spec : `specs/features/doing/bug-173-la-reponse-github-est-lue-au-fil-de-l-eau-et-tronquee.md`
- ADR liés : [ADR-0032](0032-reponses-http-bornees.md) (patrons de réponses bornées, note datée 2026-10-18 ajoutée pour ce lot — **non supersédé**), [ADR-0112](0112-mbedtls-obeit-a-la-politique-psram-du-projet.md) (contexte PSRAM qui rend cette décision possible), [ADR-0036](0036-psram-requise-mise-a-jour-en-ligne.md) (garde TLS sortante, code `psram_required`/`psram_low` préexistant), [ADR-0111](0111-ancre-de-confiance-tls-toujours-auto-signee.md) (TLS épinglé, pourquoi un DNS détourné ne livre pas de corps hostile)
- Doc externe : [`docs/API.md`](../API.md) (contrat `/check-update`), [`docs/subsystems/ota-manager.md`](../subsystems/ota-manager.md)
