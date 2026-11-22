# ADR-0138 — L'intégrité d'une valeur persistée est déléguée à NVS, jamais à l'utilisateur

- **Statut** : Accepté
- **Date** : 2026-11-22
- **Décideurs** : Nicolas + Claude (`pool-chemistry`, `ux-ui-designer`, `architect`)
- **Spec(s) liée(s)** : [feature-170](../../specs/features/done/feature-170-retirer-la-confirmation-de-correction-ph.md) (C34) ; retire un mécanisme créé par [feature-168](../../specs/features/done/feature-168-correction-ph-sans-borne.md) (C12) ; voir aussi [ADR-0114](0114-correction-mesure-ph-sans-borne-de-reglage.md)

> Règle de forme, posée par ce lot : dans tout document, une condition de gate s'écrit **`feature-170 (C34)`**, jamais `C34` seul. Voir [ADR-0139](0139-le-firmware-notifie-un-evenement-l-ecran-rend-un-etat.md), section « Conventions ».

## Contexte

Depuis feature-168, la correction de mesure pH n'est plus bornée. Pour couvrir le risque d'une valeur NVS corrompue mais restée « dans l'échelle » (par exemple +9,30), feature-168 (C12) avait ajouté un mécanisme de **confirmation** : au démarrage, si `|ph_offset| > 0,50`, un drapeau `phOffsetBootUnconfirmed` levait un bandeau « Correction à revoir » et une alerte, que l'utilisateur devait acquitter (« Garder la correction »). Une péremption à 30 jours (cause `expiration`) s'y ajoutait.

L'utilisateur a contesté le mécanisme le 2026-11-14 : *« Si on a enregistré une valeur à 1.2, en quoi une 2ᵉ confirmation apporte plus de preuve que la donnée n'a pas été altérée ? »*. Vérification faite, il avait raison sur trois points : le drapeau n'avait aucune clé NVS (relevé à chaque démarrage, même après confirmation) ; le seuil 0,50 était hérité d'une ancienne borne de réglage, pas justifié ; et l'amplitude est un mauvais détecteur d'altération (une mémoire altérée donne 0,30 aussi facilement que 9,30).

La question restante était : **la valeur est-elle réellement protégée contre l'altération ?** Elle a été tranchée par lecture de la source ESP-IDF v5.5, pas par présomption.

## Décision

**L'intégrité d'une valeur persistée est déléguée à NVS.** Le firmware ne demande jamais à l'utilisateur de la « confirmer », et ne maintient aucune somme de contrôle maison sur un réglage stocké en NVS.

Fait mesuré (ESP-IDF v5.5) :

- NVS calcule **un CRC32 par entrée** (`Item::crc32`), vérifié par `Item::checkHeaderConsistency()` — `nvs_types.cpp:46-52` : `if (crc32 != calculateCrc32()) return false;`.
- Cette vérification est appelée **dans `Page::findItem()`** (`nvs_page.cpp:954`), donc **à chaque recherche de clé**, pas seulement au montage.
- Les données de longueur variable portent en plus un `dataCrc32`, vérifié à chaque lecture (`nvs_page.cpp:280`).
- Une entrée dont le CRC ne correspond plus est marquée ERASED, la clé devient introuvable, et `prefs.getFloat("ph_offset", défaut)` rend **le défaut**.

**Le renversement qui justifie le retrait.** La corruption NVS ne produit **pas** une correction fausse et grande : elle produit la **disparition** de la correction. Le mécanisme retiré ne regardait que les valeurs **grandes** ; il était donc **aveugle au seul mode de défaillance atteignable** (`|0,00| > 0,50` est faux : le drapeau ne se levait pas), et bavard sur un mode qui n'existe pas. Il protégeait de l'inverse de ce qui arrive.

**La règle.** Une confirmation humaine n'est pas un contrôle d'intégrité : elle établit une **intention**, pas un **état de la mémoire**.

**Ce qui reste, au titre de l'information et non du contrôle** : le firmware constate au démarrage la correction appliquée (journal seul, `warning` si elle dépasse `kPhOffsetUnusualAbs`, sinon `info`), et affiche en permanence sa date de pose. `kPhOffsetUnusualAbs` (0,50, dérivé de `kPhSafetyMargin`) devient un seuil de **conséquence** : à la consigne basse 7,20, une correction qui disparaîtrait ferait cesser l'acidification dès qu'elle dépasse 0,50. Ce n'est **pas** un détecteur d'intégrité.

## Alternatives considérées

- **Conserver la confirmation** (rejetée) — elle ne répond pas à la question posée (la valeur a-t-elle été altérée ?), a un faux positif permanent (drapeau sans clé NVS : l'utilisateur l'a vu un mois sans le comprendre) et un faux négatif sur le seul défaut atteignable.
- **Somme de contrôle maison sur `ph_offset`** (rejetée) — NVS le fait déjà, par entrée, à chaque recherche. Une seconde somme ne protégerait ni de l'écriture d'une valeur fausse par un bug, ni de la perte de l'entrée.
- **Marqueur « correction retirée volontairement »** (différée, hors périmètre) — c'est la seule façon de distinguer une perte d'une suppression volontaire. Voir limites ci-dessous.
- **Retrait pur et simple, sans signal de démarrage** (rejetée) — le journal de démarrage ne coûte rien, n'interrompt personne et laisse une trace.

## Conséquences

### Positives
- Plus de question sans réponse posée à l'utilisateur ; plus d'alerte qui use un canal sans rien dire de fondé.
- Une seule cause de caducité subsiste, `calibration`, celle qui répond à un **événement**.
- Aucune migration : le drapeau n'avait aucune clé NVS.

### Négatives / dette assumée
Deux angles morts, **assumés et nommés** :
1. Une valeur **correctement stockée mais fausse** (écrite par un bug) : le CRC la valide.
2. Une correction **perdue** (entrée corrompue puis effacée → 0,00), **indistinguable d'une suppression volontaire**. Conséquence chiffrée sur un appareil à +1,20 : acide arrêté sur le champ (lecture 6,10 < 6,70), et inhibition ORP repoussée à 8,80 réels. Aucune somme de contrôle maison ne l'attraperait.

### Ce que ça verrouille
- Ne pas réintroduire de « confirmation » comme preuve d'intégrité d'une valeur NVS.
- Ne pas réutiliser `kPhOffsetUnusualAbs` comme détecteur d'altération.
- Le jour où l'on veut détecter une perte, il faut un marqueur explicite en NVS, pas un seuil.

## Références

- Code : [`src/constants.h`](../../src/constants.h) (`kPhOffsetUnusualAbs`), [`src/config.cpp`](../../src/config.cpp) (relecture au démarrage), [`src/ph_offset.h`](../../src/ph_offset.h)
- Spec : [`specs/features/doing/feature-170-retirer-la-confirmation-de-correction-ph.md`](../../specs/features/done/feature-170-retirer-la-confirmation-de-correction-ph.md)
- Doc : [`docs/features/page-ph.md`](../features/page-ph.md), [`docs/subsystems/sensors.md`](../subsystems/sensors.md)
