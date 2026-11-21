# ADR-0126 — La version demandée ne vient jamais d'une sous-chaîne d'URL

- **Statut** : Accepté
- **Date** : 2026-11-16
- **Décideurs** : Nicolas + Claude (`architect`, `security-reviewer`, `code-reviewer`)
- **Spec(s) liée(s)** : [`feature-151`](../../specs/features/done/feature-151-signature-de-l-image-ota.md) (D3-b2, liaison genre + version)

## Contexte

D3-b2 ajoute une **liaison genre + version** : comparer la version **déclarée par le manifeste signé** à celle que la requête est **censée** porter. Dans ce lot elle ne fait que **journaliser** un « désaccord » (aucun refus). Il faut décider d'où vient la « version demandée ».

La spec **nomme déjà** ce défaut pour le genre : « `/download-update` choisit la partition par une sous-chaîne d'URL ». Une URL de release GitHub contient la version, tentante à extraire ; la faire reviendrait à reproduire pour la version la faute que le projet documente pour le genre.

## Décision

La version demandée vient, **dans cet ordre** :
1. du **tag mémorisé par `/check-update`** (`g_sigSource[kind].tag`) ;
2. à défaut, du paramètre **facultatif** `expected_version` de `/download-update` ;
3. sinon : **aucune référence**, silence.

⛔ **Jamais** d'une sous-chaîne d'URL. `expected_version` est **journalisé re-sérialisé depuis les entiers parsés** (jamais brut), **jamais** passé à `storeSigSource`, **jamais prioritaire** sur le tag. Parse strict (`v2.89.0` ou `2.89.0`, trois entiers de 1 à 6 chiffres). Le paramètre est **facultatif** : un onglet resté ouvert exécutant du JS périmé ne le connaît pas et doit continuer à fonctionner.

Aucune décision d'acceptation ni de refus n'en dépend (`otaSelectVersionReference` / `otaCompareRequestedVersion`, module pur `ota_version_request_logic`).

## Alternatives considérées

- **Sous-chaîne d'URL** (rejetée) — la faute déjà nommée pour le genre ; l'URL est une entrée du client.
- **Paramètre obligatoire** (rejetée) — casserait l'envoi d'un onglet au JS périmé (épisode du 2026-11-15).
- **Tag mémorisé seul** (partiellement retenue) — insuffisant sans `/check-update` préalable ; d'où le repli facultatif.
- **Référence indépendante de GitHub** (reportée) — la bonne réponse avant toute promotion en refus, hors de ce lot.

## Conséquences

### Positives
- Pas de seconde analyse d'URL ; journal borné et non injectable (entiers re-sérialisés) ; module pur testé par mutation.

### Négatives / dette assumée
- ⚠️ Le tag vient de **GitHub**, la chaîne dont la feature se défie : ce **n'est pas un contrôle d'authenticité**.
- Il est **périmé** après un envoi direct (`ota_update.sh`) : **faux « désaccord »** possible.

### Ce que ça verrouille
- Avant de **promouvoir** le « désaccord » en refus (D3-c ou après), il faut une **référence de version indépendante de GitHub** (durcissement à ouvrir). Un refus fondé sur le tag mémorisé serait contournable par celui qui contrôle la réponse GitHub.
- Aucun futur code ne tire de version d'une URL.

## Références

- Code : [`src/ota_version_request_logic.h`](../../src/ota_version_request_logic.h), `logOtaVersionAgreement` dans [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp)
- Spec : [`feature-151`](../../specs/features/done/feature-151-signature-de-l-image-ota.md)
- Doc : [`docs/API.md`](../API.md), [`docs/subsystems/ota-manager.md`](../subsystems/ota-manager.md)
