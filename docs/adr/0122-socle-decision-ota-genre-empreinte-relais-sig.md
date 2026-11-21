# ADR-0122 — Le socle de la décision OTA : genre, empreinte et relais du `.sig`

- **Statut** : Accepté
- **Date** : 2026-11-15
- **Décideurs** : Nicolas + Claude (architect, security-reviewer P2, code-reviewer)
- **Spec(s) liée(s)** : [`feature-151`](../../specs/features/done/feature-151-signature-de-l-image-ota.md) (lot D3-a ; [ADR-0120](0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md) tranche le schéma cryptographique, [ADR-0121](0121-fichier-sig-conteneur-de-transport-deux-lignes-jamais-reconstruit.md) le format de transport ; celui-ci tranche la **conception d'exécution** côté appareil)

## Contexte

Avant d'ajouter un refus (D3-c), le chemin d'**acceptation** doit être prouvé sur le terrain. Le lot D3-a pose ce socle : quel genre fait autorité, d'où vient l'empreinte attendue, comment le `.sig` arrive à l'appareil. ⛔ **Il ne refuse rien** : la vérification journalise seulement, et une release sans signature s'installe comme avant (l'OTA est le seul canal de déploiement, sans repli USB).

## Décision

### 1. Le genre signé est l'autorité — et c'est un no-op aujourd'hui

Quand le verdict est `kValid`, le genre du manifeste choisit la partition, résolu **une seule fois** avant les cinq dépendants. Corollaire **mesuré** : `combineOtaAuthenticityVerdict` rend déjà `kKindMismatch` si genre signé ≠ genre appelant, donc `kValid` implique l'égalité et la réaffectation **ne change rien**.

🔴 **La condition 2 (autorité du genre) est donc en réalité implémentée par le REFUS `kKindMismatch` de D3-c**, pas par ce lot. Elle ne doit pas être relue comme levée : tout y reste à faire. Ce que D3-a apporte : un drapeau de désaccord journalisé même hors `kValid`, et un site unique de résolution pour le jour où le verdict ne portera plus le genre.

### 2. Une seule source d'empreinte, avec une tolérance nommée

Avec une signature valide, l'empreinte attendue vient du **manifeste**. Une divergence manifeste ↔ paramètre (`digest`/`sha256`) ne peut venir que d'un défaut d'outillage (un `.sig` d'une autre version joint à la release, déjà vu) ou d'une attaque. Pour tenir « aucun refus », un bloc unique par route l'absorbe en `CRITICAL` « divergence manifeste ↔ paramètre — TOLÉRÉ EN D3-a, D3-c refusera ». **Critère de suppression mécanique** : `grep -c "TOLÉRANCE D3-a" src/web_routes_ota.cpp` rend **4** en fin de D3-a et **0** en fin de D3-c.

### 3. Une ACCEPTATION neuve, non refusée — vecteur ouvert

🔴 Une image dont l'empreinte égale celle du **manifeste signé** mais **pas** le paramètre de l'appelant était **refusée** avant ; elle est **installée** maintenant. Bornée par la signature RSA-3072, mais le **plancher de version est INERTE à 0.0.0** : la borne haute est réelle, la basse ne l'est pas. **Vecteur** : un asset remplacé ferait installer **une autre image signée** que celle demandée (par exemple un retour à une version antérieure légitimement signée). ⇒ **À traiter en D3-b** : lier l'image au genre **et** à la version demandés, pas seulement à un manifeste authentique. Une image corrompue (aucune empreinte ne correspond) reste refusée.

### 4. Relais serveur du `.sig`, non bloquant

Cause : les assets GitHub n'émettent pas d'`Access-Control-Allow-Origin` (**mesuré**), la page ne peut pas les lire. L'appareil les lit donc dans `/check-update`. Conséquence : l'hôte des assets (`objects.githubusercontent.com`) diffère de celui de l'API, donc **session TLS séparée**, aucune reprise possible, après `heap_caps_free(rawBuffer)` (le creux mémoire, pas le pic). **Invariant** : le relais est **non bloquant** — les 7 cas d'échec (URL absente, hôte refusé, HTTP ≠ 200, corps > 1 Ko, délai, hors grammaire, garde TLS fermée) donnent champs vides + `200`, genres indépendants. `kOtaCheckJsonBudgetBytes` reste inchangé pour ne pas durcir la garde TLS de bug-165/bug-173.

### 5. Délais TLS bornés

Le watchdog mesure le temps **depuis le dernier acquittement**, pas la durée totale : la borne qui compte est la **plus longue attente sans acquitter**. Pire GET : un `.sig` via redirection (2 connexions) = `2×(3+5)+3` = **19 s** < 30 s, verrouillé par `static_assert` (`constants.h`). Constantes : 3000 ms / **5 s** (API en secondes) / 3000 ms, posées aussi sur le GET API préexistant. ⚠️ La **résolution DNS n'est pas bornable** avec cette API.

### 6. Rappels imposés par les audits

- Le vecteur **« jeton admin volé » reste OUVERT** : un attaquant authentifié contrôle manifeste, signature et paramètres.
- Mot de passe et jeton sont **en clair en NVS, sans sel**.

## Alternatives considérées

- **Relais dans la session TLS de l'API** (rejetée) — impossible : hôte différent.
- **Lecture du `.sig` par la page** (rejetée) — CORS absent.
- **Refuser dès D3-a toute divergence manifeste ↔ paramètre** (rejetée) — viole l'invariant « aucun refus » avant preuve terrain.
- **Borner le cumul des sessions sous 30 s** (rejetée) — mauvaise borne (~49 s), le watchdog mesure l'attente sans acquitter.

## Conséquences

### Positives
- Le chemin d'acceptation exerce, en production, le chemin que D3-c refusera.
- Le défaut préexistant de handshake non borné (redémarrage par watchdog) est corrigé.

### Négatives / dette assumée
- « Vérifier » est plus lent de quelques secondes.
- Acceptation neuve (§3) tant que D3-b n'est pas livré.
- La matrice de grammaire `.sig` C++ est recopiée de `test/sig_parse_vectors.js` : à mécaniser avant D3-c.

### Ce que ça verrouille
- D3-c doit retirer les 4 marqueurs `TOLÉRANCE D3-a` et implémenter le refus `kKindMismatch`.
- D3-b doit lier l'image à genre + version demandés.

## Références

- Code : `src/ota_kind_authority_logic.*`, `src/ota_digest_source_logic.h`, `src/ota_sig_relay_logic.*`, `src/web_routes_ota.cpp`, `src/constants.h` (`kOtaSigFetchMaxBytes`, `kOtaTls*`)
- Spec : [`feature-151`](../../specs/features/done/feature-151-signature-de-l-image-ota.md) (sections D3-a)
- Doc : [`docs/subsystems/ota-manager.md`](../subsystems/ota-manager.md)
