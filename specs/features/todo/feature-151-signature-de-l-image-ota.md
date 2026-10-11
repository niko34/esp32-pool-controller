## Métadonnées
- **ID** : feature-151
- **Slug** : signature-de-l-image-ota
- **Type** : feature (sécurité)
- **Statut** : todo
- **Priorité** : haute — c'est le seul mécanisme qui satisfait réellement la règle de sécurité n° 6 de `CLAUDE.md`
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-10-10
- **Spec(s) liée(s)** : [`feature-118`](../doing/feature-118-authenticite-de-l-image-ota.md) (empreinte SHA-256, lot A livré — journalise l'intégrité du **transfert**, ne livre PAS l'authenticité d'origine), [`bug-117`](../done/bug-117-upload-ota-sans-verification-d-authentification.md) (garde d'authentification de l'upload)

## Domaines touchés
- [x] OTA (`src/web_routes_ota.cpp`, `src/ota_manager.*`), `ota_update.sh`
- [x] Frontend (`data/app.js`) — page d'upload — **voir impasse ci-dessous, à trancher en premier**
- [ ] Régulation — [ ] Capteurs — [ ] MQTT

## Agents requis
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] web-ui-developer
- [x] `security-reviewer` — **deux passages obligatoires, verdict contraignant**, même
      protocole que `feature-118` : P1 sur le design (schéma de clé, format de signature,
      emplacement de vérification, plancher de version) **avant** tout code ; P2 sur le
      diff, après implémentation.
- [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] `pool-chemistry` — sans objet directement, mais l'**impact** est total, comme pour
      `feature-118` : un firmware arbitraire contrôle les pompes doseuses.

## Description
`feature-118` a établi qu'une empreinte SHA-256 **fournie par l'appelant** ne prouve que
l'intégrité du transfert, jamais l'authenticité de l'origine — la règle de sécurité n° 6 de
`CLAUDE.md` (« toujours vérifier la signature/CRC avant flash ») reste donc non satisfaite.
Cette spec livre le mécanisme qui la satisfait réellement : une **signature à clé publique**
vérifiée par le firmware avant toute activation d'image, assortie d'un **plancher de version**
qui interdit le retour en arrière vers une image plus ancienne (donc potentiellement
vulnérable) même si elle était signée légitimement en son temps.

## Contexte
Voir [ADR-0096](../../docs/adr/0096-empreinte-sha256-obligatoire-sur-post-update.md) pour
l'arbitrage complet des trois issues envisagées par `feature-118` (empreinte + JS embarqué,
retenue pour ce lot-là ; exemption de la page web, écartée ; signature, reportée ici). Voir
aussi [ADR-0081](../../docs/adr/0081-recensement-du-tas-et-non-livraison-de-la-lecture-de-contenu.md),
dont le régime de sécurité R2 (recensement du tas, compilé inconditionnellement) reste
conditionné à l'absence d'authenticité OTA **jusqu'à la livraison de cette spec** — pas
jusqu'à celle de `feature-118` lot A/B, qui ne change pas la nature de la preuve.

## Comportement attendu
- Le firmware embarque une **clé publique** (format à trancher par `architect` +
  `security-reviewer` P1 — ed25519 ou RSA, taille, emplacement en flash) utilisée pour
  vérifier une signature accompagnant chaque image.
- La vérification a lieu **avant `Update.end()`** (jamais après l'activation), sur **les deux**
  routes qui écrivent une partition applicative ou filesystem : `POST /update` **et**
  `POST /download-update`. Une image non signée, ou signée par une clé inconnue, est
  **refusée sans effet de bord** — même exigence de place de la garde que `feature-118`
  (condition 1 de son audit `security-reviewer`) : la vérification doit précéder toute
  prise de propriété de la session d'upload et tout `Update.begin()`.
- Un **plancher de version anti-retour en arrière** empêche de flasher une image dont le
  numéro de version (`src/version.h`) est **inférieur** à une version plancher persistée
  (NVS), y compris si cette image porte une signature valide — une ancienne version
  légitimement signée mais depuis reconnue vulnérable ne doit pas redevenir installable.
  Mécanisme de relèvement du plancher à définir (champ de configuration ? relevé
  automatiquement à chaque flash réussi ? jamais abaissé sans geste explicite).
- `ota_update.sh` doit produire ou accepter un fichier de signature (`.sig` ou embarqué en
  en-tête du `.bin`, à trancher) et le transmettre à la route.

### Cas limites
- Image firmware valide, signée, mais version inférieure au plancher → refusée, cause
  explicite (« version antérieure au plancher de sécurité X.Y.Z »), pas confondue avec un
  échec de signature.
- Signature présente mais clé de signature différente de celle embarquée → refusée, même
  famille de message que « sha256 non conforme » côté `feature-118` (ne pas dupliquer un
  vocabulaire différent pour un même type de refus).
- Panne du mécanisme de signature côté outillage (`ota_update.sh` ne trouve pas d'outil de
  signature) → échec **explicite à l'écran/console**, jamais un envoi sans signature qui
  se ferait accepter silencieusement.

## ⚠️ Impasse connue à trancher en premier — la page web ne peut pas signer
Exactement le point dur déjà rencontré par `feature-118` (la page web ne peut pas calculer
nativement certaines primitives cryptographiques dans de bonnes conditions), mais **plus
sérieux ici** : signer exige la **clé privée**, qui ne doit **jamais** vivre dans une page
servie en HTTP clair sur une IP de LAN — la embarquer dans `data/app.js` reviendrait à la
publier à quiconque ouvre la page, ce qui annule tout l'intérêt de la signature. Deux
options, aucune gratuite, à arbitrer **avant tout code** (passage `security-reviewer` P1) :

1. **La page d'upload téléverse un `.bin` + un fichier `.sig` séparé**, signé **en amont**
   sur un poste de confiance (celui qui construit la release), jamais par le navigateur.
   Coût : un geste manuel de plus pour l'utilisateur (récupérer les deux fichiers), mais
   aucune clé privée exposée.
2. **Exemption de la page web** pour la signature (elle resterait sur le mécanisme
   `feature-118`, empreinte seule) — **ramène exactement au dilemme déjà arbitré par
   `feature-118`/[ADR-0096](../../docs/adr/0096-empreinte-sha256-obligatoire-sur-post-update.md)** :
   le firmware ne peut **structurellement pas** distinguer la page officielle d'un `curl`
   qui imiterait ses champs — une exemption serait universellement invocable. **Ne pas
   redécouvrir ce point en cours d'implémentation : il est connu dès l'écriture de cette
   spec.**

## `esp_secure_boot` v2 — à évaluer séparément, pas dans cette spec
Noté ici pour ne pas le perdre, **hors périmètre** de cette spec : `esp_secure_boot` v2
apporterait une vérification de signature au niveau du bootloader, mais il est
**irréversible** une fois activé, exige un **accès USB** et la consommation d'**eFuses** —
donc **incompatible en l'état** avec la contrainte dominante du projet, « l'OTA est le seul
canal de déploiement » (pas de port série en environnement de développement, conteneur
LXC). Une spec séparée devra, le cas échéant, documenter comment lever cette incompatibilité
(ou l'accepter pour un sous-ensemble de la flotte) avant toute activation.

## Contraintes techniques
- ⚠️ **L'OTA est le seul canal de déploiement** (pas de port série, conteneur LXC). Toute
  exigence nouvelle qui casserait `ota_update.sh` ou la page web rendrait l'appareil non
  déployable, **sans repli**. Même contrainte dominante que `feature-118`.
- Taille de clé publique et de signature à budgéter sur la partition `spiffs` (marge connue
  à la date de cette spec : 324 181 o après `feature-118` lot A) et sur la flash applicative.
- Le déploiement du premier correctif de cette spec doit être suivi **immédiatement** d'un
  OTA de vérification complet (firmware et filesystem, script et page), pour découvrir un
  blocage à la seconde et non à la prochaine livraison — même précaution que `feature-118`.

## Fichiers potentiellement concernés
- `src/web_routes_ota.cpp` — vérification de signature sur les deux routes d'écriture
- `src/ota_integrity_logic.h`/`.cpp` — ou nouveau module dédié à la signature (à trancher
  par `architect` : extension du module existant vs module séparé)
- `src/constants.h` — clé publique embarquée, constantes de plancher de version
- `ota_update.sh` — production/transmission du fichier de signature
- `data/app.js` — flux d'upload adapté selon l'option retenue pour l'impasse ci-dessus
- `docs/API.md`, `docs/subsystems/ota-manager.md`, `docs/UPDATE_GUIDE.md` — documentation
  du nouveau contrat

## Nouvelles signatures / API
- À définir par `architect` : nouveau champ POST (`signature=`?) sur `/update` et
  `/download-update`, ou en-tête dédié
- Constantes : clé publique embarquée, plancher de version minimal

## Sécurité chimique
- Dosage impliqué : non directement — **impact total par ricochet** : un firmware
  arbitraire contrôle les pompes doseuses. Même raisonnement que `feature-118`.
- Risque identifié : une signature mal vérifiée (ex. comparaison non constante en temps,
  ou vérifiée après activation) donnerait une **fausse assurance** plus dangereuse que
  l'absence de mécanisme — passage `security-reviewer` P1 **non négociable** avant code.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contraint durablement le format d'image et le processus
  de release), 3 (alternatives crédibles : `.sig` séparé vs exemption page web vs
  `esp_secure_boot`), 4 (verrouille le contrat OTA pour le reste de la vie du projet)
- **Sujet pressenti** : choix du schéma de signature et de la réponse à l'impasse page web

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `POST /update`, `POST /download-update` — contrat enrichi d'un paramètre de signature

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/ota-manager.md` — nouvelle section signature, succédant à la section
  lot A de `feature-118`

#### Docs transverses
- `docs/API.md` — nouveau paramètre, nouveaux codes de refus
- `docs/UPDATE_GUIDE.md` — procédure de récupération du fichier de signature
- `CHANGELOG.md` — entrée dédiée, section Firmware

#### Docs immuables
- `docs/adr/NNNN-<slug>.md` — décision de schéma de signature et de réponse à l'impasse
  page web (ADR attendu ci-dessus)

## Critères d'acceptation
- [ ] Une image non signée, ou signée par une clé inconnue, est **refusée** avant tout
      `Update.end()`, sur `/update` **et** `/download-update`
- [ ] Une image signée mais de version inférieure au plancher est **refusée**, avec une
      cause distincte d'un échec de signature
- [ ] L'impasse « la page web ne peut pas signer » est tranchée **explicitement** (option 1
      ou 2 ci-dessus) et documentée, pas redécouverte en cours d'implémentation
- [ ] `ota_update.sh` fonctionne — prouvé par un déploiement réel, firmware et filesystem
- [ ] La documentation dit ce que la signature prouve (authenticité d'origine) par
      opposition à ce que l'empreinte de `feature-118` prouvait seule (intégrité du
      transfert) — ne pas laisser les deux mécanismes se confondre dans le texte
- [ ] ADR créé pour le schéma retenu

## Plan de test manuel
1. Image correctement signée, version supérieure au plancher → flash accepté
2. Image non signée → refusée avant tout effet de bord (pompes non inhibées, FS non démonté)
3. Image signée par une clé différente → refusée, message distinct de « sha256 non conforme »
4. Image signée valide mais version inférieure au plancher → refusée, message dédié
5. `ota_update.sh` de bout en bout sur un appareil réel, firmware et filesystem
6. Page web d'upload, selon l'option retenue pour l'impasse, sur ordinateur et téléphone

## Notes pour agent
- Ne pas répéter l'erreur relevée sur `feature-118` (condition 1 de son audit
  `security-reviewer`) : la place de la vérification compte autant que sa présence — vérifier
  qu'elle précède bien toute prise de propriété de session et tout `Update.begin()`, pas
  seulement qu'elle existe quelque part dans le handler.
- `esp_secure_boot` v2 est **hors périmètre** de cette spec (voir section dédiée) — ne pas
  le mélanger avec la signature applicative demandée ici.

## Hors périmètre
- `esp_secure_boot` v2 (bootloader) — spec séparée si jugé utile un jour
- Révocation de clé en cours de vie (rotation de la clé publique embarquée) — à évaluer
  seulement si un besoin concret apparaît
