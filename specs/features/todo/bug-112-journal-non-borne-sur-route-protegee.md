# bug-112 — Le refus d'authentification journalise sans limite : la fenêtre forensique est effaçable sans identifiants

## Métadonnées
| | |
|---|---|
| Type | bug (sécurité / observabilité) |
| Statut | todo |
| Priorité | **Haute** — vise directement le seul critère encore ouvert de `bug-080` |
| Version cible | à définir |
| Relevé par | `code-reviewer`, 2026-09-07, pendant l'audit de `bug-save-config-applique-avant-authentification` |
| Specs liées | `bug-080` (le critère menacé), `feature-109` (la fenêtre qu'il détruit), `bug-113` (même chemin non authentifié) |

## Domaines touchés
- [x] Web — routes HTTP / Authentification (`src/auth.cpp`, `sendAuthRequired`)
- [x] Observabilité (`src/logger.cpp`)
- [ ] Régulation — [ ] Capteurs — [ ] MQTT — [ ] Frontend — [ ] OTA

## Agents requis
- [x] architect · [x] embedded-developer · [x] code-reviewer · [x] doc-writer
- [ ] `pool-chemistry` — **sans objet** : aucun chemin de dosage, aucune décision, aucun seuil chimique.

## Description
Toute route protégée journalise son refus d'authentification **sans limite de débit**.
Un client non authentifié sur le réseau local peut donc **purger le journal
persistant à volonté**, et avec lui la fenêtre forensique dont dépend le
diagnostic de `bug-080`.

## Contexte — les chiffres, mesurés
- Le journal conserve **24 Ko sur 32** à la rotation (`src/logger.cpp:357`).
- Une requête non authentifiée coûte **~230 o persistés** (`sendAuthRequired` ≈ 84 o,
  plus les émetteurs de la route concernée).
- Les 24 Ko conservés tombent donc en **~105 requêtes**, soit **~70 s au seul quota
  autorisé** (120 req/min), et quelques secondes au débit TCP réel.
- Au-delà du quota, le limiteur est lui-même un **amplificateur** : `src/auth.cpp:171`
  **et** `:243` journalisent → jusqu'à **3 lignes par requête**.

**Pourquoi ça compte précisément** : `feature-109` a livré un cycle entier pour
diviser par 67 le débit du journal et porter la fenêtre forensique à **~2 h 30**.
C'est elle qui doit porter la ligne « Cinétique heap avant reset » d'un redémarrage
inopiné — la preuve du critère « 7 jours sans redémarrage » de `bug-080`, dont une
capture automatique quotidienne dépend (`/root/pool-captures/`). Un émetteur non
authentifié et non borné rend cette preuve effaçable par un tiers.

## Comportement actuel
`checkRateLimit()` est appelé dans le handler de **requête** (`src/auth.cpp:197`),
donc **après** un éventuel handler de corps. Le quota borne les requêtes *traitées*,
**jamais** les lignes déjà écrites.

## Comportement attendu
Le refus d'authentification reste **visible** — c'est une information de sécurité,
la supprimer serait pire — mais **borné** : throttle par route et/ou par IP, avec
compteur d'occultés reversé dans la ligne suivante, sur le patron déjà livré en
v2.46.1 pour les routes de calibration (`kCalibrationAuthWarnThrottleMs`,
`src/web_routes_calibration.cpp`).

**Invariant impératif** : le chemin muet ne doit **allouer aucune mémoire**. C'est le
point non négociable — ce chemin est atteignable sans authentification pendant une
famine mémoire active (`bug-080` : 21 échecs d'allocation reproduits le 07/09 en trois
chargements de page, tampons WiFi de 1 622 o).

## Contraintes techniques
- Le correctif touche un chemin commun à **toutes** les routes protégées : le risque
  de régression est la perte de visibilité d'une attaque réelle, pas un plantage.
- Ne pas confondre avec le throttle déjà en place : v2.46.1 borne les trois routes de
  calibration. Ici il s'agit de l'émetteur **générique**, qui **préexiste**.

## Critères d'acceptation
- [ ] 500 requêtes non authentifiées consécutives ne consomment pas plus de N lignes
      de journal (N à fixer par l'architect, chiffré)
- [ ] Le chemin muet n'alloue pas (vérifié par lecture, et par l'absence de churn)
- [ ] Un refus isolé reste journalisé — la visibilité d'une tentative unique n'est pas perdue
- [ ] Le compteur d'occultés est reversé, borné, et immunisé au repliement de `millis()`
- [ ] `pio run -e esp32dev_core3` et `pio test -e native` sans régression

## Plan de test manuel
1. Relever la taille du journal, puis envoyer 200 requêtes non authentifiées.
2. Vérifier que le journal n'a pas été purgé et que les lignes antérieures survivent.
3. Vérifier qu'une tentative isolée, une heure plus tard, est bien journalisée.

## Hors périmètre
- Le tamponnage JSON non authentifié : c'est `bug-113`.
- Le double-`send` préexistant (authentifié au-dessus du quota → 200 puis 429).
