# UX — Message d'indisponibilité de la mise à jour en ligne (sans PSRAM)

- **Type** : amélioration UX (format minimal `specs/modele.md`)
- **Statut** : done — implémenté, validation on-target restante
- **Version cible** : 2.18.7
- **Créée** : 2026-08-13 (retour utilisateur immédiat après flash de la 2.18.6)
- **Domaine** : `src/web_routes_ota.cpp` (texte du 503), `data/app.js`, `data/index.html`
  — aucune modification de la logique de garde ni du contrat HTTP
- **Specs liées** : bug-garde-tls-trop-permissive-sans-psram (2.18.6, done/), ADR-0036

## Demande utilisateur (verbatim reformulé)

Le refus de la 2.18.6 s'affichait : « **Erreur vérification mise à jour :** Mise à
jour en ligne indisponible : ce module n'a pas de PSRAM (connexion sécurisée
GitHub impossible). Utilisez la mise à jour par upload de fichier. »

Trois reproches, tous fondés :
1. **Ce n'est pas une erreur** — c'est une caractéristique du matériel ;
2. « upload de fichier » n'est pas le vocabulaire de l'interface, qui dit
   « **Mise à jour manuelle** » (titre de carte, index.html) ;
3. message trop technique (TLS, PSRAM sans contexte d'action).

Message proposé par l'utilisateur et retenu **tel quel** :
> La mise à jour en ligne n'est pas disponible. Votre matériel ne dispose pas
> d'une mémoire PSRAM (module ESP32-DevKitC-VE nécessaire). Utilisez la mise à
> jour manuelle.

## Correctif livré

1. **Firmware** : texte du 503 `psram_required` remplacé dans le helper partagé
   (donc sur `/check-update` ET `/download-update`). Statut 503 et
   `"code":"psram_required"` INCHANGÉS (contrat machine) ; les logs WARN restent
   techniques (destinés au diagnostic, pas à l'utilisateur).
2. **Frontend** : `data.code` désormais propagé sur l'objet `Error` (il était
   perdu) ; pour `psram_required` → message affiché **tel quel, sans préfixe**,
   toast de type `info` (et non `error`/`warning` : rien n'est corrigeable,
   l'orange suggérerait à tort une action). Même traitement sur le bouton
   d'installation.
3. **Ergonomie — indisponibilité permanente rendue visible AVANT le clic** :
   `applyOnlineUpdateAvailability(psramSize)` (appelée par `loadSystemInfo()`,
   `psram_size` était déjà exposé par `/get-system-info`) neutralise le bouton
   « Vérifier les mises à jour » et affiche un callout permanent dans la carte
   « Mise à jour en ligne (GitHub) » renvoyant vers « Mise à jour manuelle ».
   Garde-fous : neutralisation SEULEMENT si `psram_size === 0` (champ absent →
   aucun changement, compatibilité ascendante) ; le toast reste le filet si une
   requête part malgré tout ; `checkBtn.disabled` respecté dans les `finally`.

## Critères d'acceptation

- [x] Message exact de l'utilisateur, vocabulaire aligné sur l'interface
- [x] Présentation non alarmante (pas de préfixe « Erreur », toast `info`)
- [x] Indisponibilité visible sans cliquer (bouton neutralisé + callout)
- [x] Contrat HTTP inchangé (503, garde et parachute intacts) — `psram_required`
      a toutefois été **scindé en trois codes** par le correctif de revue ci-dessous
- [x] `node --check`, `./build_fs.sh`, builds core 2 (82,9 %) et core 3 (92,7 %) verts
- [ ] **On-target** : après flash 2.18.7 sur WROOM — callout visible, bouton
      grisé, aucun message alarmant ; sur module PSRAM (WROVER) — carte et bouton
      nominaux (à vérifier au swap)

## Suivi — correctif de revue (2026-08-13, même version 2.18.7)

Statut : **livré — tests verts (466 natifs), revue OK, doc alignée**

La revue a relevé que `psram_required` couvrait deux situations différentes
(PSRAM **absente** = permanent, PSRAM **saturée** = temporaire) et que le
frontend devinait le caractère du refus dans le **texte** du message. Correctif :

1. **Firmware** (`refuseIfTlsGateClosed`) : trois codes machine — `psram_required`
   (PSRAM absente, `ESP.getFreePsram() == 0`), **nouveau** `psram_low` (PSRAM
   saturée, message « momentanément indisponible … Réessayez »), **nouveau champ**
   `"code":"tls_low_memory"` sur le refus heap (texte inchangé). Discrimination
   faite dans la **route** par test de nullité : `oomTlsGateReason()`, les seuils,
   l'ordre de la garde dans `handleDownloadUpdate` et le parachute OOM intacts.
2. **Frontend** : lecture du champ `code` (constantes `OTA_CODE_*`), plus aucun
   test sur le texte. Seul `psram_required` neutralise le bouton et affiche le
   callout ; `psram_low` / `tls_low_memory` → toast `info` sans neutralisation puis
   `loadSystemInfo()` (source de vérité). Callout `#online_update_unavailable` :
   `role="status"` + `aria-live="polite"`.

Fichiers : `src/web_routes_ota.cpp`, `data/app.js`, `data/index.html`,
`docs/API.md`, `docs/UPDATE_GUIDE.md`, `docs/features/page-settings.md`,
`docs/features/README.md`, `docs/subsystems/oom-guard.md`,
`docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md` (2ᵉ note additive),
`CHANGELOG.md`. Builds core 2 82,9 % / core 3 92,7 %, `./build_fs.sh` OK.

## Validation on-target — 2026-08-13, v2.18.7 en production (WROOM sans PSRAM)

Appareil : version 2.18.7, `sketch_size` = 1 700 976 o — **identique à l'octet
près au binaire publié** dans la release v2.18.7 (donc bien le firmware livré,
pas un build local divergent). PSRAM rapportée : 0 o. Uptime 6 h 34 sans incident.

`GET /check-update` :
- **HTTP 503 en 35 ms** (contre ~900 ms en v2.18.5, où la tentative TLS partait
  quand même) — aucune session TLS n'est ouverte ;
- corps exact : `{"error":"La mise à jour en ligne n'est pas disponible. Votre
  matériel ne dispose pas d'une mémoire PSRAM (module ESP32-DevKitC-VE
  nécessaire). Utilisez la mise à jour manuelle.","code":"psram_required"}` ;
- journal : `WARN /check-update refusé (PSRAM libre absente/insuffisante) :
  PSRAM libre=0 o (min 1048576), heap libre=77148 o (min 65536), plus gros
  bloc=34804 o (min 20480)` — les trois mesures et leurs seuils sont tracés.

**Le résultat décisif** : AUCUNE ligne « Allocation heap échouée » ne suit le
refus, alors que la même requête en v2.18.5 en produisait quatre (dont une
allocation de 100 o refusée). La fenêtre de famine — celle du crash `bad_alloc`
du 11/08 — est bien fermée. Aucun « Réserve OOM consommée » : le parachute n'a
pas eu à intervenir.

Reste ouvert (matériel indisponible) : branche PSRAM présente — seuils
`kOtaTlsMinPsramBytes` / heap à re-mesurer au montage du DevKitC-VE.
