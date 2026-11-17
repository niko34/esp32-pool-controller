# ADR-0130 — L'appareil n'accepte qu'une seule preuve d'identité : le jeton `X-Auth-Token` (HTTP Basic retiré)

- **Statut** : Accepté
- **Date** : 2026-11-17
- **Décideurs** : `security-reviewer` (relevé et direction recommandée), `architect` — ⚠️ décision **technique**, prise dans le lot ; l'utilisateur a demandé le traitement de `bug-184`, pas le choix entre retrait et durcissement.
- **Spec(s) liée(s)** : [bug-184](../../specs/features/done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md) · [bug-185](../../specs/features/todo/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md) · [ADR-0127](0127-le-bornage-du-journal-d-authentification-est-global-par-emetteur.md)

## Contexte

`checkAuth()` acceptait deux preuves : le jeton `X-Auth-Token` et HTTP Basic (`curl -u admin:motdepasse`). Pour Basic, la comparaison est faite par la bibliothèque ESPAsyncWebServer, qui compare la chaîne Base64 reçue **sans tenir compte de la casse** (`.pio/libdeps/esp32dev_core3/ESPAsyncWebServer/src/WebAuthentication.cpp:24`). Mesuré sur l'appareil : un en-tête à la casse inversée a répondu `200`. Un mot de passe qui n'est pas le bon était donc accepté.

## Décision

**Il ne reste qu'une preuve d'identité : `X-Auth-Token`.** `AuthManager::checkBasicAuth()` est supprimée, ainsi que ses appels dans les trois prédicats de corps (calibration, `/save-config`, `/update`). Un mot de passe s'échange contre un jeton par `POST /auth/login` (comparaison faite par le firmware). `checkAuth()` délègue la décision à un module pur, `auth_admission_logic.h` (ADR-0017). `ota_update.sh` suit : login, puis jeton seul, sans rien passer sur la ligne de commande de `curl`.

## Alternatives considérées

- **Garder Basic et recomparer nous-mêmes le mot de passe** (rejetée) : exige de relire/décoder l'en-tête à la main (allocations sur un chemin critique) pour garder un confort, et laisse deux chemins à garder cohérents dans trois prédicats.
- **Patcher la bibliothèque tierce** (rejetée) : divergence à rejouer à chaque mise à jour de dépendance.
- **Garder Basic sur les seules routes `WRITE`** (rejetée) : `checkAuth()` ne distingue pas les niveaux dans le choix de méthode ; le trou resterait.

## Conséquences

### Positives
- Une seule voie à raisonner, à tester en natif et à garder en CI (la règle 5 de la garde de journalisation passe de six à cinq fonctions).
- Le trou « mot de passe à la casse inversée accepté » est fermé.
- Exemption du premier démarrage resserrée à l'URL exacte `/save-config` ; `/tune-ph-offset` n'est plus ouvert au premier démarrage.

### Négatives
- **Rupture** : `curl -u admin:motdepasse` est refusé. Tout script tiers doit passer par `POST /auth/login`.
- Les docs de l'API (≈ 90 exemples) passent à `-H "X-Auth-Token: $TOKEN"`.

### Ce que ça verrouille
- Ne pas réintroduire de voie Basic ou de comparaison d'identifiants déléguée à la bibliothèque.
- ⚠️ **Non traité** (bug-185) : la fenêtre de premier démarrage ne se referme que sur `/auth/complete-wizard`, et le jeton n'est pas régénéré au changement de mot de passe. Le mot de passe reste stocké en clair en NVS.

## Références

- [`src/auth.cpp`](../../src/auth.cpp), [`src/auth_admission_logic.h`](../../src/auth_admission_logic.h), `test/test_native_auth_admission/`
- [`docs/subsystems/auth.md`](../subsystems/auth.md), [`ota_update.sh`](../../ota_update.sh)
