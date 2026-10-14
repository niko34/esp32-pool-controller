# ADR-0111 — Une ancre de confiance TLS embarquée est toujours la variante auto-signée, et un contrôle automatique l'impose

- **Statut** : Accepté
- **Date** : 2026-10-14
- **Décideurs** : `debugger`, `security-reviewer`, `embedded-developer`, `code-reviewer`
- **Spec(s) liée(s)** : [`bug-165`](../../specs/features/done/bug-165-recherche-de-mise-a-jour-en-ligne-echoue.md) ; incident antérieur `bug-checkupdate-tls-core3` (2026-08-08, non tracé par un ADR à l'époque)

## Contexte

Le firmware embarque des certificats racine (`src/github_root_ca.h`) pour valider en HTTPS les
connexions sortantes vers GitHub (`/check-update`, `/download-update`) — seule voie de
remédiation automatique du projet. **mbedtls 3.x** (core Arduino 3 / IDF 5, environnement de
production depuis [ADR-0042](0042-core3-environnement-par-defaut.md)) refuse structurellement les
ancres de confiance **cross-signées**, que mbedtls 2.28 (core 2) tolérait.

Cette contrainte a mordu **deux fois** :

- **2026-08-08** (`bug-checkupdate-tls-core3`) : GitHub servait des racines USERTrust
  **cross-signées par « AAA Certificate Services »**. Remplacées par leurs variantes
  auto-signées, avec la règle posée **en commentaire** : « toutes auto-signées ».
- **2026-10-14** (`bug-165`) : la chaîne réelle servie par `api.github.com` a changé — GitHub sert
  désormais une chaîne terminant sur **Sectigo Public Server Authentication Root E46**, dans sa
  variante **cross-signée** (signée par USERTrust ECC). Le commentaire d'août disait la règle,
  mais **rien ne la réappliquait** à l'évolution réelle de la chaîne. `/check-update` a échoué
  **systématiquement** pendant un temps indéterminé, sans que rien ne le distingue d'un problème
  mémoire ou réseau (code `-1` opaque).

Un piège aggrave le diagnostic : une racine cross-signée porte **le même nom** et **la même clé
publique** que sa variante auto-signée — seule la **signature de l'émetteur** diffère. Un contrôle
qui se contenterait de vérifier la **présence** d'un certificat du bon nom, ou même la
**concordance de l'empreinte attendue**, ne détecte pas ce défaut si l'empreinte attendue a
elle-même été mal choisie une fois.

## Décision

1. **Toute ancre de confiance embarquée pour une validation TLS sortante doit être la variante
   AUTO-SIGNÉE** (`subject == issuer`, signature vérifiable avec sa propre clé publique) —
   **jamais** une variante cross-signée, même si son nom et sa clé publique concordent avec la
   bonne autorité.
2. **Cette règle est imposée par un contrôle automatique en CI**
   ([`tools/check_github_root_ca.sh`](../../tools/check_github_root_ca.sh) →
   [`tools/github_root_ca_vectors.py`](../../tools/github_root_ca_vectors.py), job `github-root-ca`
   du workflow `hygiene-specs.yml`), qui **extrait** les certificats réellement livrés et vérifie
   cryptographiquement l'auto-signature de chacun (`openssl verify -no-CAfile -no-CApath -CAfile
   <cert> <cert>`) — pas seulement le nom du sujet ni une empreinte recopiée à la main.
3. Elle ne dépend plus de la mémoire d'un ou d'une développeuse : un futur ajout de racine
   cross-signée, par erreur, **fait échouer la CI** avant d'atteindre un appareil réel.

## Alternatives considérées

- **Option A (rejetée)** — épingler le certificat **serveur** (certificate/public-key pinning) au
  lieu d'une ancre de confiance. Rejetée : un certificat serveur (leaf) est renouvelé bien plus
  souvent qu'une racine (mois contre décennies) — cela exigerait une mise à jour firmware à chaque
  renouvellement, un cercle vicieux pour **le canal même qui sert à se mettre à jour**.
- **Option B (rejetée)** — désactiver la validation de la chaîne de certificats (`setInsecure()`
  ou équivalent) pour contourner le problème. Rejetée sans discussion : désactiver la validation
  TLS sur le canal de mise à jour **du firmware lui-même** est une régression de sécurité majeure,
  sans rapport avec le principe de moindre privilège déjà appliqué ailleurs dans ce projet.
- **Option C (rejetée)** — compter sur la vigilance humaine et un commentaire à jour dans le code
  (c'était la situation après l'incident d'août). Rejetée par les faits : **elle a déjà échoué**,
  le commentaire disait la règle et n'a pas empêché la régression d'octobre.
- **Option D (retenue)** — maintenir le jeu minimal de racines auto-signées couvrant les chaînes
  réellement servies (ECC et RSA), et **vérifier automatiquement** par un contrôle qui lit les
  certificats réels et confirme cryptographiquement leur auto-signature, en CI, sur chaque push/PR.

## Conséquences

### Positives
- La règle ne peut plus régresser silencieusement : toute racine cross-signée ajoutée par erreur
  fait échouer la CI avant tout déploiement.
- Le contrôle vérifie aussi que les 3 racines d'origine ne disparaissent pas (dont ISRG Root X1,
  qui ancre le chemin de **téléchargement**, distinct du chemin de **vérification**) et que les
  empreintes annoncées en commentaire correspondent aux certificats réels.

### Négatives / dette assumée
- Le contrôle ne valide **pas** la chaîne TLS réellement servie par GitHub en conditions réelles
  (hors de portée d'un test natif) : si GitHub fait évoluer sa chaîne vers une **autre** autorité
  (pas une simple nouvelle variante croisée d'une autorité déjà couverte), le contrôle restera
  vert jusqu'au prochain échec constaté sur appareil réel. La détection reste donc réactive, pas
  prédictive.
- **Risque identifié, non vérifié à la date de cet ADR** : le domaine de téléchargement
  (`objects.githubusercontent.com`) sert aujourd'hui une chaîne de structure **identique** (un
  avant-dernier maillon croisé, « Root YR » croisée par ISRG Root X1) — le téléchargement du
  firmware est donc **probablement** déjà affecté par le même mécanisme, sans que cela ait pu être
  constaté puisque la recherche de mise à jour échouait avant d'atteindre cette étape. À vérifier
  après déploiement de ce correctif. Ajouter « ISRG Root X2 » ne suffirait pas : autre racine,
  sans rapport avec « Root YR ».

### Ce que ça verrouille
- Tout futur ajout de racine de confiance (GitHub ou un autre fournisseur, pour un futur canal de
  mise à jour ou d'intégration externe) doit être la variante **auto-signée**, identifiée par
  inspection explicite de la chaîne réellement servie (`openssl s_client -showcerts`), jamais
  supposée depuis le nom seul.
- Tout nouveau jeu de certificats embarqués doit passer par un contrôle du même type que
  `github_root_ca_vectors.py` (extraction + vérification cryptographique de l'auto-signature),
  pas une simple relecture humaine — le patron est désormais généralisable, pas un script
  jetable propre à GitHub.

## Références

- Code : [`src/github_root_ca.h`](../../src/github_root_ca.h) · [`src/ota_check_logic.h`](../../src/ota_check_logic.h)
  (`classifyOtaCheckFailure`) · [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp)
  (`handleCheckUpdate`) · [`tools/check_github_root_ca.sh`](../../tools/check_github_root_ca.sh) ·
  [`tools/github_root_ca_vectors.py`](../../tools/github_root_ca_vectors.py)
- Spec : [`specs/features/done/bug-165-recherche-de-mise-a-jour-en-ligne-echoue.md`](../../specs/features/done/bug-165-recherche-de-mise-a-jour-en-ligne-echoue.md)
- Doc externe : [`docs/subsystems/ota-manager.md`](../subsystems/ota-manager.md#règle--une-ancre-de-confiance-est-toujours-la-variante-auto-signée-bug-checkupdate-tls-core3-bug-165-adr-0111) ·
  [`docs/BUILD.md`](../BUILD.md#intégration-continue-github-actions)
