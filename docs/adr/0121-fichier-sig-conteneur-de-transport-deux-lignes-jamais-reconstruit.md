# ADR-0121 — Le fichier `.sig` OTA est un conteneur de TRANSPORT à deux lignes, jamais reconstruit côté appareil ni côté page

- **Statut** : Accepté
- **Date** : 2026-10-21
- **Décideurs** : Nicolas + Claude (architect, security-reviewer P1 et P2, ux-ui-designer)
- **Spec(s) liée(s)** : [`feature-151`](../../specs/features/doing/feature-151-signature-de-l-image-ota.md) (lot D1-bis ; complète [ADR-0120](0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md), qui avait explicitement reporté cette décision)

## Contexte

[ADR-0120](0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md) a tranché le schéma
cryptographique de la signature OTA (RSA-3072 sur un manifeste d'une ligne) mais a
**explicitement reporté** la réponse à l'impasse « la page web ne peut pas signer » : elle ne
transmettait encore ni `manifest` ni `signature`, ces deux champs POST n'étant produits que par
`ota_update.sh`.

`feature-118` avait déjà établi pourquoi la page ne peut pas **signer** (la clé privée ne doit
jamais vivre dans une page servie en HTTP clair sur une IP de LAN). Ce lot ajoute un deuxième
constat, plus sérieux : la page ne peut pas non plus **construire** le manifeste, parce que le
descripteur d'application du binaire ESP32 porte un hash git, jamais la version SemVer — établi
par `security-reviewer` en P1. La page ne peut donc, structurellement, que **transporter** un
manifeste et une signature produits ailleurs (poste de confiance qui signe, ou `ota_update.sh`).

Deux options s'offraient pour ce transport :

1. Un format **brut** de 384 octets (la signature RSA seule), obligeant l'appareil à
   **reconstruire** le manifeste signé depuis des éléments fournis séparément (tag de version
   saisi par l'opérateur ou lu ailleurs, empreinte déjà calculée côté page).
2. Un fichier `.sig` **texte, à deux lignes** : la ligne signée en clair, suivie de la signature
   en base64 — l'appareil n'a plus qu'à **lire** ce que l'opérateur a transmis, jamais à le
   reconstituer.

Une contrainte supplémentaire a été **découverte en mesurant**, pas anticipée : les assets GitHub
ne renvoient aucun en-tête `Access-Control-Allow-Origin` — le navigateur ne peut donc pas lire un
`.sig` à une URL distante. La mise à jour en ligne (`/check-update`) devra relayer le **contenu**
des deux lignes, jamais l'URL, dans une requête HTTPS supplémentaire au sein de la session TLS
déjà ouverte.

## Décision

### Format `.sig` : deux lignes texte, aucun changement cryptographique

```
ligne 1 : pool-ota/1 <firmware|filesystem> <MAJ.MIN.PATCH> sha256:<64 hex minuscules>
ligne 2 : <signature RSA-3072 en base64, exactement 512 caractères, sans padding>
```

La ligne 1 est **exactement** le manifeste d'[ADR-0120](0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md),
sans saut de ligne final dans ce qui est signé. La signature de la ligne 2 porte toujours sur les
octets **canoniques re-sérialisés par l'appareil**, jamais sur le fichier `.sig` reçu tel quel.
Ce fichier est un **pur conteneur de transport** : `src/` n'en lit jamais un seul octet
directement — l'appareil continue de recevoir `manifest` et `signature` comme deux champs POST
distincts, exactement comme en D1 ([ADR-0120](0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md)).
C'est la page (ou `ota_update.sh`) qui lit les deux lignes et les répartit dans ces deux champs.

### La page TRANSPORTE, elle ne CONSTRUIT ni ne VÉRIFIE l'authenticité

Qu'il s'agisse du script ou de la page, **il n'existe qu'une seule voie pour obtenir le
manifeste** : le lire tel qu'il a été produit en amont (poste de confiance signataire). Ni la
page ni `/download-update` ne le reconstruisent depuis un tag et une empreinte séparés. Un second
site de construction aurait dupliqué exactement le risque que le manifeste signé est censé
fermer : deux endroits où la même information (genre, version, empreinte) pourrait diverger sans
que la signature ne le révèle.

La comparaison d'empreinte que la page effectue entre le `.bin` sélectionné et l'empreinte
déclarée par le `.sig` (déjà calculable côté page depuis `feature-118`) établit uniquement la
**liaison** — ce manifeste décrit bien ces octets — jamais l'**authenticité** — ce manifeste vient
du détenteur de la clé. Cette dernière reste du ressort exclusif de l'appareil, qui la
**journalise seulement** en D1/D2 ([ADR-0120](0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md)).
Aucun texte de l'interface ne doit laisser croire que la page a vérifié une signature.

### Verdict de liaison dédié, jamais confondu avec un échec d'authenticité

Prendre le `.bin` d'une version et le `.sig` d'une autre produit une signature **authentique**
sur un manifeste **cohérent** : ce n'est pas l'authenticité qui échoue, c'est la **liaison**
(`kUnbound`, lot B1 de D1). Le libellé dédié retenu — « signature authentique, mais pas pour
cette image » — évite d'orienter l'opérateur vers un diagnostic de clé quand le problème réel est
un mélange de fichiers.

### Comparaison d'empreinte bloquante côté page, sans dérogation — seule garde de ce type

C'est la seule vérification de ce lot qui **refuse** quelque chose côté page, et elle ne souffre
aucune exception (pas de « Envoyer quand même »), contrairement au garde-fou de type hérité de
`feature-118`/v2.20.1. Raison structurelle : sur `/update`, la liaison manifeste ↔ image n'échoue
qu'**après** `Update.begin()` — en mode système de fichiers, la partition est déjà effacée à ce
stade, et un envoi incohérent coûterait l'accès à l'interface elle-même jusqu'au prochain envoi
réussi. Un « Envoyer quand même » ici n'offrirait pas un geste de secours : il offrirait de se
couper soi-même l'accès à l'appareil.

### L'heuristique de nom de fichier ne l'emporte jamais sur un constat d'octets

`detectUpdateType()` (v2.20.1) reste un repli utile en l'absence de `.sig`, mais une divergence
nom ↔ genre déclaré par la signature ne bloque pas l'envoi quand la comparaison d'empreinte a
déjà **prouvé** la liaison (`bindingState === "match"`) : un nom de fichier est altérable par un
téléchargement ou un renommage, une empreinte est constatée sur les octets eux-mêmes. Faire
gagner l'heuristique contre le constat reviendrait à refuser un envoi que rien ne rend suspect.

## Alternatives considérées

- **Format brut de 384 octets (signature seule), reconstruction du manifeste côté appareil**
  (écartée) — aurait créé un **second site de construction** du manifeste (le premier étant le
  poste signataire), exactement le défaut structurel que le manifeste signé est censé fermer :
  deux endroits où version/genre/empreinte pourraient diverger sans que la signature ne le
  révèle. `/download-update` aurait eu besoin de la même reconstruction, dupliquant le risque.
- **Blocage souple avec dérogation (« Envoyer quand même ») sur la comparaison d'empreinte**
  (écartée) — valable pour le garde-fou de type existant (faux positifs possibles, aucune perte
  d'accès), mais ici une dérogation coûterait l'accès à l'interface en mode système de fichiers :
  un simple avertissement franchissable transformerait un filet de sécurité en illusion.
- **Faire gagner l'heuristique de nom sur la comparaison d'empreinte prouvée** (écartée après
  correction de revue) — implémentée un temps par erreur dans ce lot, puis retirée : bloquait un
  envoi déjà prouvé cohérent sur la seule foi d'un nom de fichier altérable.
- **Qualifier la comparaison d'empreinte de « preuve » d'authenticité** (écartée après revue
  `security-reviewer` P2) — une empreinte ne prouve jamais l'origine, seulement la liaison ; le
  texte doit rester littéralement exact sur ce point, y compris dans les messages d'interface.

## Conséquences

### Positives
- La page web participe enfin à la campagne terrain D2 de `feature-151` (elle en était
  jusqu'ici exclue, puisqu'aucun appelant côté page n'envoyait `manifest`/`signature`).
- Aucun changement cryptographique ni aucune ligne nouvelle dans `src/` : `manifest` et
  `signature` restent de simples champs POST, condition 10 du verdict P1 préservée.
- Le geste réel de l'opérateur (télécharger deux fichiers d'une même release, les sélectionner)
  est simple et sans outillage supplémentaire côté navigateur.

### Négatives / dette assumée
- Un geste manuel de plus pour l'opérateur (récupérer et apparier deux fichiers plutôt qu'un
  seul) — assumé : c'est le prix déjà identifié par `feature-151` de l'option « `.sig` séparé »
  retenue en [ADR-0120](0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md).
- La mise à jour en ligne (`/check-update`) devra, pour D3, relayer le contenu du `.sig` en clair
  dans une requête HTTPS supplémentaire (CORS interdit de lire l'URL GitHub directement) — coût
  reporté à ce lot ultérieur, documenté ici pour ne pas être redécouvert en cours
  d'implémentation.
- Le sélecteur de type manuel a disparu : un `.bin` au nom non reconnu, envoyé sans `.sig`, part
  désormais toujours en firmware par défaut — l'opérateur doit renommer le fichier ou fournir son
  `.sig` pour choisir explicitement.

### Ce que ça verrouille
- Le fichier `.sig` à deux lignes devient le **seul** format que le processus de release
  (`.claude/commands/release.md`), l'outillage de développement (`ota_update.sh --sign-only`) et
  la vérification d'artefacts (`tools/check_release_artefacts.sh` bloc S2) doivent produire et
  accepter — toute évolution future de ce format touche ces trois points ensemble.
- La page web (et, demain, `/check-update`) ne doit **jamais** reconstruire un manifeste depuis
  des éléments séparés : elle ne fait que lire et transmettre ce qui a été signé en amont. Un
  futur lot qui introduirait un second site de construction romprait cet invariant.
- La comparaison d'empreinte entre `.bin` et `.sig` reste la **seule** garde bloquante sans
  dérogation côté page ; toute nouvelle garde côté page qui voudrait un comportement aussi strict
  devra justifier une perte d'accès équivalente, pas simplement « par prudence ».

## Références

- Code : `data/app.js` (`parseSigFile`, `parseOtaManifestLine`, `otaManualSendBlocked`),
  [`src/ota_manifest_binding_logic.h`](../../src/ota_manifest_binding_logic.h)/`.cpp` (verdict de
  liaison `kSkipped`/`kBound`/`kUnbound`, lot B1), `ota_update.sh` (`sign_only_file`,
  `--sign-only`), `.claude/commands/release.md` (étape 2ter), `tools/check_release_artefacts.sh`
  (bloc S2)
- Tests : `test/sig_parse_vectors.js` + `tools/check_sig_parse_vectors.sh` (job CI
  `sig-parse-vectors`), `test/test_native_ota_manifest_binding/`
- Spec : [`specs/features/doing/feature-151-signature-de-l-image-ota.md`](../../specs/features/doing/feature-151-signature-de-l-image-ota.md)
  (section D1-bis)
- Doc externe : [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md#et-la-page-web-dupload--tranché-en-v2860-feature-151-d1-bis),
  [`docs/API.md`](../API.md#fichier-sig-feature-151-d1-bis),
  [`docs/features/page-settings.md`](../features/page-settings.md),
  [`docs/DESIGN_SYSTEM.md` §5.4](../DESIGN_SYSTEM.md#54-btn-et-variantes)
- ADR liés : [ADR-0120](0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md) (schéma
  cryptographique et format du manifeste, complété — pas remplacé — par celui-ci), [ADR-0096](0096-empreinte-sha256-obligatoire-sur-post-update.md)
  (calcul d'empreinte côté page, réutilisé ici pour la comparaison de liaison)
