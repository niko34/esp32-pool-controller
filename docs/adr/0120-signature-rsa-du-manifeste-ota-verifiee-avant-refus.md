# ADR-0120 — La signature OTA porte sur un manifeste RSA-3072 signé en `.sig` séparé, vérifiée avant toute écriture mais sans y conditionner de refus avant preuve terrain

- **Statut** : Accepté
- **Date** : 2026-10-18
- **Décideurs** : Nicolas + Claude (architect, security-reviewer P1)
- **Spec(s) liée(s)** : [`feature-151`](../../specs/features/doing/feature-151-signature-de-l-image-ota.md) (lot D1 ; `feature-118`/[ADR-0096](0096-empreinte-sha256-obligatoire-sur-post-update.md) pour le mécanisme d'empreinte qu'elle complète, sans le remplacer)

## Contexte

`feature-118` a établi qu'une empreinte SHA-256 **fournie par l'appelant** ne prouve que
l'intégrité du **transfert**, jamais l'authenticité de l'**origine** — la règle de sécurité n° 6
de `CLAUDE.md` (« toujours vérifier la signature/CRC avant flash ») restait non satisfaite. Un
compte ou un dépôt GitHub compromis, un jeton `gh`/CI fuité, un CDN ou une autorité de
certification détournée peuvent tous produire un `digest` SHA-256 **conforme** — GitHub le
calcule lui-même à l'upload de l'asset — sans qu'aucun mécanisme existant ne le détecte.

Deux contraintes dominantes cadrent toute solution :

1. **L'OTA est le seul canal de déploiement du projet** (pas de port série, conteneur LXC de
   développement). Toute exigence nouvelle qui casserait `ota_update.sh` ou la page web rendrait
   l'appareil **définitivement non déployable**, sans repli possible.
2. **La page web d'upload ne peut structurellement pas signer** : signer exige la clé privée, qui
   ne doit jamais vivre dans une page servie en HTTP clair sur une IP de réseau local — l'y
   embarquer la publierait à quiconque ouvre la page, annulant l'intérêt même de la signature.

## Décision

### Schéma cryptographique : RSA-3072 / PKCS#1 v1.5 / SHA-256

Retenu après mesure **dans l'ELF existant** : `mbedtls_pk_verify`, `pk_parse_public_key`,
`rsa_pkcs1_verify` sont déjà présents, l'accélération matérielle MPI/SHA est déjà active.
Ed25519 est **absent** de la configuration mbedtls de ce projet (seules les tables X25519
existent) et aurait exigé une dépendance neuve sur le chemin OTA — chemin sur lequel le projet
évite par principe d'en introduire. Coût : signature de 384 octets, budget flash ~0 (la
bibliothèque est déjà liée).

### La signature porte sur un manifeste d'une ligne, jamais sur l'image

```
pool-ota/1 <firmware|filesystem> <MAJ.MIN.PATCH> sha256:<64 hex minuscules>
```

Signer 1,5 Mo directement aurait rendu le geste de signature (`ota_update.sh`) lent et aurait
obligé à répéter l'opération coûteuse à chaque envoi d'une image par ailleurs déjà vérifiée par
empreinte ([ADR-0096](0096-empreinte-sha256-obligatoire-sur-post-update.md)). Le manifeste est
signé une fois, léger, et **lie** explicitement trois informations que la signature doit
garantir ensemble : le genre de l'image (évite qu'un manifeste `firmware` valide une écriture
dans la partition filesystem, ou l'inverse), sa version (nécessaire au plancher anti-retour en
arrière) et son empreinte SHA-256 (seule source faisant autorité — condition 3 du verdict
`security-reviewer` P1 : si l'appareil consultait une autre source d'empreinte que celle signée,
la signature ne lierait plus rien de réel).

L'appareil ne vérifie **jamais** la signature sur la chaîne brute reçue : il la **parse** selon
une grammaire stricte puis la **re-sérialise** depuis les champs extraits — c'est sur ces octets
canoniques que porte la signature. Ce découpage isole la question « la signature est-elle
valide ? » de la question « le texte reçu est-il, au bit près, celui qui a été signé ? » (LF
final, espaces, casse), et permettra à un futur lot d'activer le refus sans toucher à la
grammaire elle-même.

### Fichier `.sig` séparé, jamais un en-tête dans le `.bin`

Embarquer la signature en en-tête du binaire aurait changé le format d'image et l'octet magique
`0xE9` que l'appareil et l'IDF reconnaissent déjà ([ADR-0022](0022-verification-integrite-ota.md)).
Un fichier séparé laisse le binaire intact et la signature se transmet comme un champ de
formulaire supplémentaire (`signature=`), au même titre que le manifeste (`manifest=`) — aucun
changement du format d'image, aucun risque de confondre un binaire non signé avec un binaire
corrompu.

### Réponse à l'impasse « la page web ne peut pas signer » : ni tranchée, ni ignorée — reportée explicitement

Ce lot **ne tranche pas encore** entre les deux options identifiées par la spec (page web
téléversant un `.bin` + un `.sig` signé en amont sur un poste de confiance, ou exemption durable
de la page web). La page web continue, pour l'instant, à n'envoyer ni `manifest` ni `signature` —
ce qui produit un verdict « absente », strictement journalisé, sans aucune conséquence. La
décision réelle est reportée à un lot ultérieur, et **documentée comme reportée** plutôt que
redécouverte : voir [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md#signature-des-envois-ota).

### Ordre de déploiement D0 → D5 : le chemin d'acceptation avant le chemin de refus

Parce que l'OTA est le seul canal de déploiement, une exigence de signature livrée avec un
outillage cassé ou une clé perdue rendrait l'appareil **irrécupérable** — et ce défaut ne
s'éprouve qu'au déploiement **suivant**, trop tard pour revenir en arrière une fois le firmware
en place. La vérification est donc déployée en six étapes, chacune conditionnant la suivante :

| # | Contenu | Risque si sauté |
|---|---|---|
| D0 | Paire de clés générée hors dépôt, clé publique embarquée | — |
| **D1** (ce lot) | Vérifie et **journalise seulement**, ne refuse jamais | — |
| D2 | Preuve terrain : 4 combinaisons (script × page) × (firmware × FS) + 2 essais négatifs, tous journalisés | Sans cette étape, D3 activerait un refus jamais éprouvé en conditions réelles |
| D3 | Refus **activé** ; plancher inerte à 0.0.0 ; interrupteur de secours | Sans D2, un défaut d'outillage découvert ici bloquerait tout déploiement futur |
| D4 | Bump sans effet, premier déploiement réellement filtré | — |
| D5 | Relèvement du plancher (geste explicite) | — |

La fonction de vérification est **rigoureusement identique** entre D1 et D3 ; seul le `if` de
refus change — vérifié par construction (le vérificateur, la clé et la grammaire ne bougent pas
d'un octet entre les deux lots).

### Plancher anti-retour en arrière : geste explicite uniquement, jamais de relèvement automatique

Un relèvement automatique du plancher à chaque flash réussi créerait un **second** mécanisme de
blocage définitif, qui interdirait précisément le retour en arrière dont on a besoin pour sortir
d'un firmware défectueux. Le plancher ne progresse donc que sur un geste humain explicite — en
D1, il est simplement **inerte**, figé à 0.0.0 (jamais franchi par une version réelle du projet).

## Alternatives considérées

- **Ed25519** (écartée) — absent de la configuration mbedtls du projet, aurait exigé une
  dépendance cryptographique neuve sur le chemin OTA.
- **Signature embarquée en en-tête de l'image** (écartée) — changerait le format d'image et
  l'octet magique `0xE9` déjà reconnu par l'IDF et par le pré-contrôle du firmware.
- **Signer directement l'image (1,5 Mo) plutôt qu'un manifeste** (écartée) — plus lent à signer,
  et ne lie pas explicitement le genre et la version attendus, seulement le contenu.
- **`esp_secure_boot` v2 au niveau du bootloader** (écartée, hors périmètre) — apporterait une
  vérification plus forte, mais est **irréversible** une fois activé et exige un **accès USB** et
  la consommation d'**eFuses** : incompatible avec la contrainte dominante « l'OTA est le seul
  canal de déploiement » (pas de port série en environnement de développement).
- **Relèvement automatique du plancher de version** (écartée) — créerait un second mécanisme de
  blocage définitif, exactement symétrique du problème que l'exigence de signature elle-même doit
  éviter.
- **Activer le refus dès ce lot (sans étape D1 séparée)** (écartée) — l'ordre D0→D5 est retenu
  précisément pour éprouver le chemin d'acceptation avant que le chemin de refus n'existe, sur le
  seul canal de déploiement disponible.

## Conséquences

### Positives
- Un attaquant qui compromettrait la chaîne GitHub/TLS (compte, dépôt, jeton CI, CDN, autorité de
  certification) obtient un `digest` SHA-256 conforme gratuitement, mais ne peut produire aucune
  signature valide — il lui manque la clé privée, qui n'a jamais quitté le poste de confiance.
- Le format d'image (`.bin`, octet magique `0xE9`) et le format d'empreinte
  ([ADR-0096](0096-empreinte-sha256-obligatoire-sur-post-update.md)) restent **inchangés** — la
  signature se transmet en parallèle, sans toucher au contrat existant.
- Le retour en arrière vers une version signée légitimement mais depuis reconnue vulnérable sera
  bloqué (une fois le plancher activé en D3/D5), sans attendre `esp_secure_boot`.

### Négatives / dette assumée
- 🔴 **Cette décision ne ferme pas le vecteur « jeton admin volé ».** L'interrupteur de secours
  prévu pour D3 — indispensable contre une clé perdue ou un défaut du vérificateur lui-même,
  puisque le bouton physique d'urgence est au bord du bassin et non dans l'environnement de
  développement — est posable par une requête HTTP authentifiée. Un porteur du jeton admin
  continue donc d'obtenir l'exécution de code arbitraire, au prix de **deux requêtes au lieu
  d'une**. Cette limite doit être redite à chaque fois que cette feature est présentée : le gain
  réel est contre la chaîne GitHub/TLS, pas contre un jeton volé.
- Chaque envoi via `ota_update.sh` exige désormais une phrase de passe saisie interactivement — un
  coût opérationnel **assumé en connaissance de cause** : les alternatives (clé uniquement sur un
  poste externe, déploiement toujours fait par l'utilisateur en personne) imposaient un
  aller-retour à chaque itération, y compris pour un correctif d'une ligne, ce qui aurait fini par
  être contourné.
- L'impasse « la page web ne peut pas signer » reste non tranchée à l'issue de ce lot — la page
  web continue de fonctionner sans signature, avec un verdict journalisé « absente », en attendant
  la décision d'un lot ultérieur.

### Ce que ça verrouille
- Le format du manifeste (`pool-ota/1 <genre> <version> sha256:<hex>`) et sa grammaire stricte
  deviennent la **seule** source d'empreinte faisant autorité pour toute vérification
  d'authenticité OTA future — un futur lot qui introduirait une seconde source romprait le lien
  que la signature est censée garantir.
- Toute évolution future de ce mécanisme (activation du refus, interrupteur de secours,
  relèvement du plancher) doit réutiliser le **même** vérificateur, la **même** clé embarquée et
  la **même** grammaire que ce lot — seul le prédicat de décision doit changer, jamais la logique
  de vérification elle-même.
- Le processus de release doit désormais produire, en plus de `firmware.bin`/`littlefs.bin`, les
  fichiers `.sig` correspondants, signés avec la clé privée tenue hors dépôt.

## Références

- Code : [`src/ota_manifest_logic.h`](../../src/ota_manifest_logic.h)/`.cpp` (grammaire et
  re-sérialisation canonique), [`src/ota_base64_logic.h`](../../src/ota_base64_logic.h)/`.cpp`
  (décodeur strict), [`src/ota_authenticity_logic.h`](../../src/ota_authenticity_logic.h)/`.cpp`
  (combinateur du verdict à sept issues), [`src/ota_signature.h`](../../src/ota_signature.h)/`.cpp`
  (vérification mbedtls RSA-3072/PKCS#1 v1.5/SHA-256), [`src/constants.h`](../../src/constants.h)
  (`kOtaSigningPublicKeyDer`, `kOtaSigningPublicKeyFingerprint`, `kOtaSignatureMaxBytes`,
  `kOtaVersionFloorDefault*`), [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp)
  (`computeOtaAuthenticityVerdict`, `logOtaAuthenticityVerdict`, placement dans `handleOtaUpdate`
  et `handleDownloadUpdate`), [`ota_update.sh`](../../ota_update.sh) (`sign_manifest`,
  `require_signing_tooling`)
- Tests : [`test/test_native_ota_manifest/`](../../test/test_native_ota_manifest/),
  [`test/test_native_ota_base64/`](../../test/test_native_ota_base64/),
  [`test/test_native_ota_authenticity/`](../../test/test_native_ota_authenticity/)
- Spec : [`specs/features/doing/feature-151-signature-de-l-image-ota.md`](../../specs/features/doing/feature-151-signature-de-l-image-ota.md)
- Doc externe : [`docs/API.md`](../API.md#authenticite-manifest-signature-feature-151-d1),
  [`docs/subsystems/ota-manager.md`](../subsystems/ota-manager.md#d1--authenticite-par-signature-journalise-seulement-feature-151-verdict-security-reviewer-p1),
  [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md#signature-des-envois-ota)
- ADR liés : [ADR-0096](0096-empreinte-sha256-obligatoire-sur-post-update.md) (empreinte
  SHA-256, mécanisme complété — pas remplacé — par celui-ci), [ADR-0022](0022-verification-integrite-ota.md)
  (vérification d'intégrité OTA d'origine, octet magique `0xE9`)
