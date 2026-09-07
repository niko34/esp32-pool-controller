# bug-117 — Le handler d'upload OTA ne vérifie aucune authentification

## Métadonnées
| | |
|---|---|
| Type | bug de **sécurité — critique** |
| Statut | todo |
| Priorité | **CRITIQUE** — à traiter avant tout autre bug de cette file |
| Version cible | à définir (correctif prioritaire) |
| Relevé par | `doc-writer`, 2026-09-07, en refusant de reprendre le verdict « sain » de l'audit ; confirmé par lecture directe |
| Specs liées | `bug-save-config-applique-avant-authentification` (**même topologie**, corrigée en v2.46.1), `feature-026` (intégrité OTA) |

## Domaines touchés
- [x] OTA (`src/web_routes_ota.cpp`, `handleOtaUpdate()`)
- [x] Web — Authentification
- [ ] Régulation — [ ] Capteurs — [ ] MQTT — [ ] Frontend

## Agents requis
- [x] architect · [x] embedded-developer · [x] code-reviewer · [x] doc-writer
- [ ] `pool-chemistry` — sans objet (aucun chemin de dosage), mais l'**impact** est total :
      un firmware arbitraire contrôle les pompes.

## Description
`handleOtaUpdate()` (`src/web_routes_ota.cpp:234`) est le **handler d'upload** de
`POST /update`. Il **ne contient aucune vérification d'authentification** — vérifié par
recherche exhaustive sur les ~286 lignes de la fonction : ni `authManager`, ni
`checkAuth`, ni `REQUIRE_AUTH`, ni `checkTokenAuth`, ni `checkBasicAuth`.

`REQUIRE_AUTH(req, RouteProtection::CRITICAL)` n'existe que dans le **lambda de
réponse** (`:1198`), exécuté **après** le handler d'upload. Or ce handler exécute
`Update.begin()` (`~:400`) puis `Update.end(true)` (`~:497`).

**C'est exactement la topologie du défaut corrigé en v2.46.1 sur les trois routes de
calibration** — authentification présente mais évaluée trop tard — avec un effet
incomparablement plus lourd : l'écriture d'une partition applicative.

## Pourquoi l'empreinte SHA-256 ne rattrape pas
La comparaison est conditionnée à `g_uploadHasExpected` : elle n'a lieu **que si le
client a fourni** un paramètre `sha256`. Sans ce paramètre, la finalisation s'exécute.

Et même fournie, une empreinte **transmise par l'appelant** vérifie l'intégrité du
**transport**, jamais l'**authenticité** de l'image. `CLAUDE.md` (règle de sécurité
absolue n° 6) demande de « vérifier la signature/CRC avant flash » : l'implémentation
actuelle vérifie un condensat que l'appelant choisit lui-même.

## Pourquoi le défaut a survécu à un audit conduit exprès
L'audit du 07/09 avait classé cette route **saine**, en lisant le pré-contrôle de
l'octet magique (`:260-315`, « refus AVANT `setOtaInProgress`, `LittleFS.end()` et
`Update.begin()` »). Ce pré-contrôle refuse une image **invalide** — c'est un contrôle
de **validité**, pas d'**autorisation**. Les deux ont été confondus.

**Leçon à retenir, et c'est la troisième fois aujourd'hui** : la présence visible d'un
garde-fou fait passer le relecteur. `REQUIRE_AUTH` présent trois lignes plus bas, une
sentinelle de parité déclarant légitime le bug qu'elle devait signaler, un pré-contrôle
de validité pris pour une autorisation. Chercher la **place** de la garde dans le cycle
de vie, jamais sa seule présence.

## Comportement attendu
Vérification d'authentification en **première instruction** du handler d'upload, avant
tout effet de bord — sur le patron de `calibrationBodyAuthorized()` (v2.46.1) et de
`saveConfigBodyAuthorized()` (v2.25.0), avec le niveau `CRITICAL`.

`REQUIRE_AUTH` reste dans le lambda de réponse : il produit le 401 propre. La garde
n'est pas redondante — elle empêche l'effet de bord.

## Contraintes techniques
- ⚠️ **Ne pas casser l'OTA légitime.** C'est le seul canal de déploiement de cet
  appareil (conteneur LXC sans port série depuis le 05/09 : OTA uniquement). Un
  correctif trop strict rendrait l'appareil non déployable — donc non corrigeable.
- Vérifier le chemin `/download-update` (mise à jour depuis GitHub), qui pilote aussi
  `Update` : même examen, même exigence.
- Le prédicat doit accepter tout ce que `REQUIRE_AUTH(CRITICAL)` accepte, sans le rate
  limiting (une seule réponse, un seul décompte).

## Critères d'acceptation
- [ ] Un upload non authentifié n'atteint **ni** `Update.begin()`, **ni** aucune
      écriture de partition
- [ ] L'OTA authentifié fonctionne — **vérifié par un déploiement réel**, pas par
      raisonnement
- [ ] `/download-update` audité, résultat consigné
- [ ] Décision tracée sur l'empreinte : rendue **obligatoire**, ou remplacée par une
      vraie signature, ou explicitement assumée comme contrôle d'intégrité seul (→ ADR)
- [ ] `pio run` et `pio test -e native` sans régression

## Plan de test manuel
À écrire **après** le correctif, et à exécuter **après** déploiement : tenter un upload
non authentifié et vérifier qu'aucune écriture de partition n'a lieu, puis prouver la
non-régression par un déploiement OTA authentifié complet.

## Hors périmètre
- Le journal non borné (`bug-112`), le tamponnage JSON (`bug-113`).
- La refonte du mécanisme d'intégrité en signature cryptographique : à instruire
  séparément si la décision du 4ᵉ critère va dans ce sens.
