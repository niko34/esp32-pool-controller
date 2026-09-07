# feature-118 — Authenticité de l'image OTA : de l'empreinte facultative à la signature

## Métadonnées
| | |
|---|---|
| Type | feature (sécurité) |
| Statut | todo |
| Priorité | Haute — mais **après** `bug-117`, qui ferme le trou pratique |
| Version cible | à définir |
| Origine | 4ᵉ critère d'acceptation de `bug-117`, délibérément **non traité** dans son correctif |
| Specs liées | `bug-117` (la garde d'authentification), `feature-026` (vérification d'intégrité OTA) |

## Domaines touchés
- [x] OTA (`src/web_routes_ota.cpp`), `ota_update.sh`
- [x] Frontend (`data/app.js`) — page d'upload
- [ ] Régulation — [ ] Capteurs — [ ] MQTT

## Agents requis
- [x] architect · [x] embedded-developer · [x] web-ui-developer · [x] code-reviewer · [x] doc-writer
- [ ] `pool-chemistry` — sans objet directement, mais l'**impact** est total : un firmware
      arbitraire contrôle les pompes.

## Description
`POST /update` accepte une image **sans empreinte** : la comparaison SHA-256 est
conditionnée à `g_uploadHasExpected`, donc au bon vouloir de l'appelant. Un upload
authentifié mais non accompagné d'empreinte est flashé sans qu'aucune vérification de
contenu n'ait eu lieu.

Incohérence à corriger : **`/download-update` rend l'empreinte obligatoire** (fail-closed).
Deux routes qui écrivent la même partition n'ont pas la même exigence.

## Ce que `CLAUDE.md` demande, et ce qui est réellement fait
Règle de sécurité absolue n° 6 : « OTA : toujours vérifier la signature/CRC avant flash ».
L'implémentation vérifie un condensat **que l'appelant fournit lui-même**. Cela prouve
l'**intégrité du transport** — l'image reçue est celle envoyée — mais **rien** sur son
**authenticité** : qui l'a produite.

**Formulation à ne pas diluer** (verdict `code-reviewer`, 07/09) : rendre l'empreinte
obligatoire est la bonne **étape intermédiaire**, mais il ne faut **pas l'appeler
sécurité**. Le vrai correctif est une **signature** — clé publique embarquée, ou
`esp_secure_boot`.

## Le point dur, trouvé en revue : le navigateur ne peut pas calculer l'empreinte
`ota_update.sh` a le `.bin` en local et peut calculer le SHA-256 trivialement.
**La page web d'upload, non** : `crypto.subtle` n'est disponible que dans un
**contexte sécurisé** (HTTPS ou `localhost`). L'appareil sert du **HTTP en clair sur une
IP de réseau local** — `crypto.subtle` y est donc **indisponible**.

Rendre l'empreinte obligatoire **casserait la page d'upload** (`data/app.js:7715`), à
moins d'embarquer une implémentation SHA-256 en JavaScript — ce qui pèse sur une
partition contrainte (charge utile actuelle ~190 Ko sur 512 Ko, mais la marge est un
sujet à part entière dans ce projet).

**Trois issues à arbitrer, aucune gratuite :**
1. Empreinte obligatoire + SHA-256 en JS embarqué dans `data/` (coût : poids FS).
2. Empreinte obligatoire **sauf** pour la page web, qui reste dispensée (coût : le
   fail-closed a un trou nommé, ce qui est au moins honnête et documenté).
3. Signature à clé publique, et l'empreinte devient sans objet (coût : le plus élevé,
   la valeur la plus haute — et c'est la seule qui réponde vraiment à la règle n° 6).

## Comportement attendu
À trancher par l'`architect` sur les trois issues, avec le coût de chacune chiffré —
poids FS, complexité, et ce que chacune **prouve réellement**.

## Contraintes techniques
- ⚠️ **L'OTA est le seul canal de déploiement** (pas de port série, conteneur LXC). Toute
  exigence nouvelle qui casserait `ota_update.sh` ou la page web rendrait l'appareil non
  déployable, **sans repli**. C'est la contrainte dominante, avant toute considération de
  pureté cryptographique.
- Le déploiement de tout correctif de cette spec doit être suivi **immédiatement** d'un
  OTA de vérification, pour découvrir un blocage à la seconde et non à la prochaine
  livraison.

## Critères d'acceptation
- [ ] Une image sans preuve d'origine acceptable est **refusée** avant tout `Update.begin()`
- [ ] `ota_update.sh` fonctionne — **prouvé par un déploiement réel**
- [ ] La page web d'upload fonctionne, ou son exemption est **explicite et documentée**
- [ ] La documentation dit ce que le mécanisme prouve **et ce qu'il ne prouve pas** :
      transport contre authenticité. Ne pas laisser croire à une signature ce qui n'en est pas une.
- [ ] ADR : le choix entre les trois issues verrouille un futur, il en relève

## Hors périmètre
- La garde d'authentification de l'upload : c'est `bug-117`, livrée en v2.46.2.
