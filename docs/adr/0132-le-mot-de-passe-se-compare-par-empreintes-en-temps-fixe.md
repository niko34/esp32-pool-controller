# ADR-0132 — Le mot de passe se compare par empreintes SHA-256 en temps fixe ; le jeton, directement

- **Statut** : Accepté
- **Date** : 2026-11-17
- **Décideurs** : Nicolas + Claude (`security-reviewer` passe P1, `code-reviewer`)
- **Spec(s) liée(s)** : [bug-183](../../specs/features/done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) · [ADR-0127](0127-le-bornage-du-journal-d-authentification-est-global-par-emetteur.md) · [ADR-0131](0131-l-etat-de-premier-demarrage-se-deduit-du-mot-de-passe.md)

## Contexte

`/auth/login` et `/auth/change-password` comparaient le mot de passe candidat au mot de passe stocké par `==` (via `getPassword()`). Une comparaison de `String` s'arrête au premier octet qui diffère : son temps trahit la longueur du préfixe correct. Le jeton d'API disposait déjà d'une comparaison à temps constant ; le mot de passe, non.

Les deux secrets n'ont pas la même nature : la **longueur du jeton est publique** (fixe, connue), celle du **mot de passe est secrète**. Une comparaison « à largeur fixe » sur le mot de passe obligerait à choisir une largeur, donc à révéler ou à tronquer.

## Décision

1. **Deux primitives distinctes, délibérément.**
   - **Jeton** : comparaison directe à largeur fixe (longueur publique).
   - **Mot de passe** : `AuthManager::passwordMatches(const String& candidate) const` calcule le SHA-256 des **deux** côtés (`OtaStreamHasher`, contexte sur la pile), ce qui ramène deux longueurs inconnues à deux empreintes de 32 octets, puis les compare avec `fixedWidthEquals(a, b, n)` ([`secret_compare.h`](../../src/secret_compare.h)) : OU cumulé des XOR, **aucune sortie anticipée**. `getPassword()` est supprimée : plus aucun appelant ne peut recomparer par `==`.
2. **Le stockage en clair est assumé.** `adminPassword` reste en clair en RAM et en NVS non chiffrée. Les deux appels à `mbedtls_platform_zeroize` n'effacent que **les deux empreintes de la pile** ; ils **ne protègent pas** le mot de passe. Le but est étroit : ne pas laisser d'empreinte du secret dans une pile réutilisée par la requête suivante. Le chiffrement de la NVS est un chantier distinct, **non ouvert**.
3. **Limite de la garde de source.** `tools/check_secret_compare_guard.sh` (job `secret-compare-guard`) lit le texte : il ne peut pas exclure qu'un compilateur transforme la boucle. Seul le binaire le prouve. Mesure sur l'ELF livré (`firmware.elf`, SHA256 `5c1f16b693bdde2fe28fa0fd3536165949ff10b9a384b76ea32329f8ad463c30`) : boucle matérielle Xtensa `loop` de **32 tours fixes**, corps de 8 instructions (`l8ui`/`l8ui`/`xor`/`or`/`extui`), **zéro branchement** ; résultat par `nsau`+`srli`, sans branchement ; les deux `mbedtls_platform_zeroize` sont présents, non éliminés. ⚠️ Vérité de **cette** compilation : à refaire si la chaîne d'outils change.

## Alternatives considérées

- **`==` conservé** (rejetée) — canal temporel sur le préfixe.
- **Comparaison directe à largeur fixe du mot de passe** (rejetée) — il faudrait fixer une largeur : tronquer ou révéler la longueur.
- **Une seule primitive pour jeton et mot de passe** (rejetée) — fait payer deux SHA-256 au jeton, dont la longueur n'a rien de secret.
- **`mbedtls_ct_memcmp`** (non retenue) — dépendance à un symbole interne de la bibliothèque ; la garde de source et le désassemblage portent sur du code du dépôt.

## Conséquences

### Positives
- Le temps de vérification ne dépend plus de la ressemblance avec le bon mot de passe ni de sa longueur ; testé en natif (`test_native_secret_compare`).
- Plus d'accès public en lecture au mot de passe depuis le manager.

### Négatives / dette assumée
- ⚠️ Mot de passe toujours **en clair** en NVS et en RAM ; cet ADR n'y change rien.
- Deux SHA-256 par tentative (coût négligeable devant le quota de tentatives).
- La garantie de temps fixe est **mesurée sur un binaire**, pas démontrée pour tout compilateur.

### Ce que ça verrouille
- Aucune comparaison de secret par `==` / `!=` ; toute nouvelle comparaison passe par `fixedWidthEquals` (garde CI).
- Tout futur secret de longueur secrète suit la voie « empreintes puis largeur fixe ».

## Références

- Code : [`src/secret_compare.h`](../../src/secret_compare.h), [`src/auth.cpp`](../../src/auth.cpp) (`passwordMatches`), [`tools/check_secret_compare_guard.sh`](../../tools/check_secret_compare_guard.sh)
- Doc : [`docs/subsystems/auth.md`](../subsystems/auth.md)
