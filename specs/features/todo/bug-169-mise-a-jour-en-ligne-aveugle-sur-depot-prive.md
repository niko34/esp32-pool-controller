# bug-169 — La mise à jour en ligne ne voit aucune release (dépôt privé + noms d'assets)

## Métadonnées
- **ID** : bug-169
- **Slug** : mise-a-jour-en-ligne-aveugle-sur-depot-prive
- **Type** : bug — ⚠️ **PÉRIMÈTRE RÉDUIT le 2026-10-18** : il ne reste que « **qui le dit
  mal** ». La fonction, elle, **marche**.
- **Statut** : todo — ⚠️ **la cause BLOQUANTE est levée**, voir « Ce qui est résolu » 
- **Priorité** : normale — ⚠️ **plus un blocage, un défaut d'HONNÊTETÉ de l'affichage** ;
  ⚠️ mais la fonction **existe dans l'interface** et répond « vous êtes à jour »
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-10-16
- **Spec(s) liée(s)** : `done/bug-165-…` (la **session TLS**, réparée — ⚠️ défaut **distinct**) ·
  `todo/feature-151-signature-de-l-image-ota.md` · ADR-0036 (PSRAM requise pour la mise à jour
  en ligne) · ADR-0112 (mbedtls en PSRAM)

## Domaines touchés
- [ ] Capteurs · [ ] Régulation · [ ] Filtration / Éclairage
- [x] **Web — routes HTTP** — `/check-update`, `/download-update`
- [ ] MQTT / Home Assistant · [ ] UART
- [x] **OTA**
- [ ] Authentification · [ ] Historique
- [x] **Frontend (`data/`)** — le message affiché

## Agents requis
- [x] spec-reader
- [x] architect — ⚠️ il y a un **arbitrage** (voir « Options »), pas une seule voie
- [ ] pool-chemistry — aucun chemin de dosage
- [x] **security-reviewer — OBLIGATOIRE** : touche la **chaîne OTA**, et l'option B
      ferait **résider un jeton GitHub dans l'appareil**
- [ ] ux-ui-designer — sauf si le message affiché est retravaillé
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## ✅ CE QUI EST RÉSOLU — 2026-10-18

⭐ **L'obstacle bloquant a disparu : l'utilisateur a rendu le dépôt PUBLIC.**
⇒ L'API GitHub répond **200** sans authentification (mesuré), et `/check-update` **aboutit** sur
l'appareil réel en **2.85.0** : les deux URL, les deux empreintes, le nom de la release.

⚠️ **Et ce bug n'était que le PREMIER de trois obstacles**, découverts l'un après l'autre —
⛔ aucun n'était visible tant que le précédent n'était pas levé :
1. **Dépôt privé** ⇒ API **404** — *ce bug*, levé.
2. **Noms d'assets** : l'appareil cherche `firmware.bin`/`littlefs.bin`, la procédure publiait
   `firmware-<version>.bin` et ne joignait plus le second — **bug-170**, corrigé.
3. **Réponse lue au fil de l'eau** ⇒ `IncompleteInput` — **bug-173**, corrigé.

⇒ ⚠️ **Les options A/B/C ci-dessous sont donc SANS OBJET** : l'utilisateur a tranché pour A
(dépôt public). ⛔ Ne pas les réinstruire.

## 🔴 CE QUI RESTE — et c'est le seul périmètre de cette spec

⛔ **L'appareil dit encore « vous êtes à jour » dans des cas où il n'a RIEN comparé.**
⚠️ Le scénario qui mord maintenant est le **3ᵉ** de la liste ci-dessous : une release trouvée
**sans asset utilisable** (c'est **exactement** ce qui s'est produit pendant six semaines avec
bug-170) serait **encore** annoncée comme « à jour ».
⇒ ⚠️ **Un défaut d'affichage qui a MASQUÉ un défaut réel pendant six semaines** n'est pas
cosmétique.

## Description

Depuis l'interface, « Rechercher une mise à jour » répond **« Vous êtes à jour »**
alors qu'une release plus récente existe. ⚠️ Le message est **faux** : l'appareil
n'a pas comparé des versions, il n'a **rien trouvé du tout**.

## Constat (2026-10-16, mesuré sur l'appareil en 2.81.0 avec la v2.82.0 publiée)

```
GET /check-update →
{"current_version":"2.81.0","latest_version":"2.81.0",
 "update_available":false,"no_release":true,
 "message":"Aucune release disponible sur GitHub"}
```

⚠️ **Attention au piège de diagnostic** : la route répond **HTTP 200**. Un contrôle
qui s'arrête au code HTTP conclut que tout va bien. ⛔ **C'est l'erreur que j'ai
commise** en clôturant bug-165 : j'ai écrit que la mise à jour en ligne
« refonctionne » sur la foi d'un 200, alors que ce 200 portait *« aucune release »*.
⇒ **Le critère est le CORPS de la réponse, jamais le code.**

## DEUX causes indépendantes — ⛔ corriger l'une ne suffit pas

### Cause 1 — le dépôt est PRIVÉ, l'appareil n'envoie aucun jeton (BLOQUANTE)

`src/web_routes_ota.cpp:970` interroge
`https://api.github.com/repos/niko34/esp32-pool-controller/releases/latest`
et `:978` ne pose **que** `User-Agent` — ⛔ aucun `Authorization`.

Mesuré :
```
curl sans authentification  → HTTP 404
gh api (authentifié)        → v2.82.0
```
⚠️ GitHub répond **404** (et non 403) sur un dépôt privé, pour ne pas révéler son
existence. ⇒ Le code interprète ce 404 comme *« aucune release »* (`:1006-1012`) —
interprétation **correcte pour un dépôt public**, **trompeuse ici**.

⚠️ Et même résolu, **le téléchargement** (`browser_download_url`) exige lui aussi
une authentification sur un dépôt privé.

### Cause 2 — les noms d'assets ne correspondent plus (BLOQUANTE AUSSI)

`src/web_routes_ota.cpp:1108-1112` cherche les noms **exacts** `firmware.bin` et
`littlefs.bin`. Or la procédure de release les nomme `firmware-<version>.bin`
depuis la v2.42.0, et ⛔ **`littlefs.bin` n'était plus joint du tout**.

| Release | Assets | Mise à jour en ligne |
|---|---|---|
| v2.16.0, v2.18.7 | `firmware.bin`, `littlefs.bin` | ✅ trouvables |
| v2.42.0 → v2.82.0 | `firmware-<version>.bin`, `.elf` | ❌ |

⭐ **Cela explique le souvenir de l'utilisateur** : *« Au début du projet, la mise à
jour via github fonctionnait. »* Elle fonctionnait — elle a été cassée par un
renommage, **six semaines sans que personne ne le voie**.

✅ **Déjà traité, hors de cette spec** : la v2.82.0 a reçu les deux binaires aux noms
exacts, et `.claude/commands/release.md` les **impose** désormais, avec un contrôle
`gh release view ... --json assets` et la raison écrite.
⇒ ⛔ Il reste la **cause 1**, qui est la vraie.

## Options pour la cause 1 — ⚠️ à arbitrer, pas à présumer

| | Option | Ce qu'on gagne | Ce qu'on paie |
|---|---|---|---|
| **A** | **Rendre le dépôt public** | ⛔ Zéro code. Fonctionne immédiatement. | Le code devient public. ⚠️ **Décision de l'utilisateur, pas technique.** |
| **B** | **Jeton GitHub à portée réduite dans l'appareil** | Dépôt reste privé | ⚠️ Un **secret** réside dans l'appareil, expire, et se lit via un accès physique. ⛔ `security-reviewer` obligatoire. |
| **C** | **Publier les binaires ailleurs** (releases publiques d'un dépôt d'artefacts séparé) | Code privé, binaires publics | Deux dépôts à tenir ; ⛔ contredit « on versionne les sources, les binaires vont en release ». |

⚠️ **Position de l'utilisateur au 2026-10-16** : *« on passera le dépôt en public une
autre fois »* ⇒ **option A, différée**. ⛔ Ce n'est pas un refus des autres options,
c'est un « pas maintenant ».

## Comportement attendu (quelle que soit l'option retenue)

1. ⛔ **Ne plus jamais afficher « vous êtes à jour » quand rien n'a été comparé.**
   ⚠️ Un 404 sur un dépôt **privé** et un 404 « aucune release » sur un dépôt
   **public** sont deux situations différentes — ⛔ elles ne peuvent pas partager
   le même message.
2. Le message distingue : *aucune release* · *dépôt inaccessible (authentification)*
   · *release trouvée mais **sans asset utilisable*** · *à jour*.
3. ⚠️ Le cas 3 mérite son propre message : c'est **exactement** la cause 2, et elle
   serait **restée invisible** derrière « aucune release ».

## Éléments visibles
- [x] **Interface** — le message de recherche de mise à jour
- [x] **Log** — distinguer les causes
- [ ] Endpoint HTTP (⚠️ la **forme** de la réponse change, pas la route)

## Critères d'acceptation
- [ ] Sur un dépôt **privé** sans jeton : message **explicite**, ⛔ jamais « à jour »
- [ ] Release trouvée **sans** `firmware.bin` : message dédié, ⛔ pas « à jour »
- [ ] Release **plus récente** avec les bons assets : proposée
- [ ] ⚠️ **Le test porte sur le CORPS de la réponse**, ⛔ jamais sur le code HTTP
      (c'est l'erreur de diagnostic qui a masqué ce défaut)
- [ ] `pio run` 0 warning · natifs sans régression

## Hors périmètre
- La session TLS elle-même — réparée par bug-165 (mbedtls en PSRAM, ADR-0112)
- La signature de l'image OTA — c'est feature-151
- ⚠️ Le **renommage** des assets — **déjà corrigé** dans `.claude/commands/release.md`
