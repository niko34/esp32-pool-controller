# Bug — un échec d'écriture OTA est indéchiffrable ET noie le journal

- **Type** : bug (format minimal)
- **Statut** : **livré** — tests verts, revue OK (2026-08-15, v2.20.1)
- **Priorité** : haute
- **Version cible** : 2.20.1
- **Créée** : 2026-08-15 (diagnostic on-target après deux échecs de mise à jour
  filesystem signalés par l'utilisateur)
- **Domaine** : Web — routes HTTP OTA, Journal, UI de mise à jour manuelle

## Ce qui s'est réellement passé (établi on-target)

L'utilisateur a tenté deux fois la mise à jour du filesystem depuis
« Mise à jour manuelle ». Les deux ont échoué. Le journal ne contenait que
**200 lignes `ERROR "Erreur écriture OTA"` identiques**, suivies de
`Erreur finalisation OTA: Aborted`. Rien d'autre : plus aucun contexte.

Reproduction avec `update_type=filesystem` envoyé explicitement : **succès en
2,4 s** (`Progression OTA: 31/63/95 %`, `Mise à jour OTA réussie`). Le chemin
OTA filesystem est donc sain. L'appareil est reparti en 2.20.0 avec
`fs_total_bytes = 327680`.

Ce qui a échoué est l'envoi d'une image **filesystem déclarée comme firmware** :
le sélecteur « Type » de la carte « Mise à jour manuelle » vaut **`firmware` par
défaut** (`data/index.html:1727`), et il revient à cette valeur à chaque
rechargement de page — donc juste après un flash de firmware réussi, qui
redémarre l'appareil et recharge l'UI.

Avec `cmd = U_FLASH`, `UpdateClass::_writeBuffer()` vérifie l'octet magique
`ESP_IMAGE_HEADER_MAGIC` du tout premier tampon de 4 Ko ; une image LittleFS ne
le porte pas → `_abort(UPDATE_ERROR_MAGIC_BYTE)` dès le premier vidage.

## Les trois défauts à corriger

1. **La cause est écrasée.** `handleOtaUpdate` (`src/web_routes_ota.cpp:228-231`)
   journalise `"Erreur écriture OTA"` **sans** `Update.errorString()`, puis
   appelle `Update.abort()`. Or `abort()` fait `_abort(UPDATE_ERROR_ABORT)`, qui
   **remplace** le code d'erreur réel. Le message de finalisation ne peut donc
   plus dire que « Aborted ». La vraie cause (`MAGIC_BYTE`, `ERASE`, `WRITE`,
   `SPACE`…) est perdue à jamais, y compris pour l'utilisateur qui ne voit que
   « FAIL ».

2. **Le journal est noyé.** Après `Update.abort()`, `Update.write()` retourne 0
   à chaque appel (garde `hasError()`), donc **chaque chunk restant** re-journalise
   la même erreur. L'anneau ne fait que `kMaxLogEntries = 200` entrées
   (`src/constants.h:103`) : une seule tentative ratée **efface tout
   l'historique**, y compris les lignes qui désignaient le coupable
   (`Type de mise à jour: …`, `Début mise à jour OTA: …`). Un incident OTA
   détruit ainsi les preuves de tout autre incident en cours d'analyse.

3. **Le piège d'interface reste ouvert.** Rien ne rapproche le nom du fichier du
   type déclaré. La détection par nom existe mais n'est qu'un **repli** (utilisée
   seulement si `update_type` est absent) et ne reconnaît que `.littlefs.bin`,
   `.spiffs.bin`, `.fs.bin` — or le fichier produit par `build_fs.sh` s'appelle
   `littlefs.bin`, qui ne correspond à **aucun** de ces suffixes.

## Correctif attendu

1. `src/web_routes_ota.cpp` — sur échec d'écriture : lire `Update.errorString()`
   **avant** `Update.abort()` et le journaliser. Ajouter un message explicite
   quand la cause est l'octet magique (« l'image reçue n'est pas un firmware —
   type de mise à jour incorrect ? »).
2. Journaliser **une seule fois** par session d'upload : les chunks suivants
   doivent rester muets. L'anneau de 200 entrées ne doit plus jamais être vidé
   par une seule tentative ratée.
3. Même traitement au second site d'écriture (`src/web_routes_ota.cpp:872`,
   chemin téléchargement par URL), qui journalise aussi sans la cause.
4. `data/` — déduire le type du nom de fichier à la sélection et refuser (ou
   avertir explicitement) une incohérence manifeste avant l'envoi.
5. La réponse HTTP doit porter la cause, pas seulement `FAIL`.

## Critères d'acceptation

- [~] Une image filesystem envoyée en type « firmware » produit **une seule**
      ligne d'erreur, nommant la cause réelle et suggérant le type
      *(implémenté et couvert par les tests natifs ; **non vérifié on-target**)*
- [~] L'historique du journal antérieur à la tentative est **préservé**
      *(implémenté ; **non vérifié on-target**)*
- [~] Le message affiché dans l'UI nomme la cause
      *(implémenté ; **non vérifié on-target**)*
- [ ] Le chemin nominal (bon type) reste inchangé — **vérifié on-target**
      *(NON VÉRIFIÉ : l'appareil de production tourne en 2.20.0. Ce critère
      exige un passage matériel — voir le plan de vérification on-target de
      `test-engineer` ci-dessous. La clôture logicielle ne vaut PAS validation
      de ce critère.)*
- [x] `pio run` bi-core verts, `pio test -e native` sans régression
      *(565/565 ; esp32dev 83,8 % flash, esp32dev_core3 93,5 % ;
      `./build_fs.sh` 327680 o)*

---

## Suivi / itérations

### Itération 1 — v2.20.1 (2026-08-15)

**Statut** : livré — tests verts, revue OK (`code-reviewer` : Approved après correctifs).

**Fichiers modifiés**

| Fichier | Nature |
|---|---|
| `src/ota_type_logic.h` | **nouveau** — logique pure : `inferOtaKindFromFilename`, `checkOtaFirstByte` (ADR-0017) |
| `src/web_routes_ota.cpp` | pré-contrôle magique, `failOtaUpload`, `remountUploadFsIfNeeded`, verdict HTTP, ordre dosage/montage |
| `data/index.html` | `#update_type_notice` + attributs ARIA de la barre de progression |
| `data/app.js` | `detectUpdateType`, `parseUpdateFailure`, notice, confirmation, affichage de la cause |
| `data/app.css` | style de la notice |
| `ota_update.sh` | test de succès ancré (200 **et** corps exactement `OK`), corps d'erreur affiché |
| `test/test_native_ota_type/` | **nouvelle suite** — 29 cas |
| `src/version.h` | 2.20.1 |

**Décisions d'implémentation**

1. **Pré-contrôle AVANT tout effet de bord** : à `index == 0`, avant
   `setOtaInProgress(true)`, avant `LittleFS.end()`, avant `Update.begin()`. Un
   refus de type ne coûte donc rien : ni inhibition de dosage, ni démontage FS,
   ni redémarrage.
2. **Asymétrie de refus assumée** : refus dur uniquement pour
   « firmware déclaré + premier octet ≠ 0xE9 » (contrôle **prouvablement
   équivalent** à `UpdateClass::_writeBuffer()` → ensemble des images acceptées
   INCHANGÉ) ; simple `WARN` dans le sens inverse (LittleFS n'a pas de magique
   fiable à l'offset 0 → faux positifs garantis). Réserve bloquante consignée
   dans le header : l'équivalence tombe si `Update.setupCrypt()` est un jour
   appelé. Principe écrit dans `docs/subsystems/ota-manager.md` pour survivre au
   prochain refactor.
3. **`errorString()` capturé avant `abort()`** sur les deux sites d'écriture :
   `abort()` fait `_abort(UPDATE_ERROR_ABORT)` et remplace le code réel.
4. **Une seule ligne de journal par session** (`g_uploadErrorLogged` + chemin
   d'échec devenu terminal) : l'anneau de 200 entrées n'est plus vidé.
5. **Ordre imposé par le gate `pool-chemistry`** : `setOtaInProgress(false)`
   TOUJOURS avant `LittleFS.begin(false)` — la tâche AsyncTCP n'est pas abonnée
   au watchdog, un montage bloquant y laisserait les pompes inhibées sans
   redémarrage possible. Le chemin `/download-update` a été corrigé dans ce sens.
6. **Remontage best-effort, jamais de formatage** (`begin(false)`), une seule
   tentative par session, `critical` si le remontage réussit après échec
   (contenu potentiellement incohérent).
7. **Contrat `/update`** : le code HTTP porte le verdict (400 / 500 / 409 / 200).
   Corps de succès `OK` **inchangé** (le front et `ota_update.sh` en dépendent).
8. **UI** : type déduit, jamais verrouillé, **non persisté** (le persister
   recréerait le piège dans l'autre sens) ; confirmation franchissable en un clic.

**Décision ADR — aucun ADR requis.** Les 4 critères ont été confrontés
explicitement (analyse `architect`, partagée par `doc-writer`) :

1. *Contrainte imposée ailleurs* — non : rien hors `web_routes_ota.cpp` /
   `ota_type_logic.h` n'est contraint. L'ordre dosage-avant-montage est une
   règle locale de handler, documentée dans le subsystem.
2. *Invisible dans le code* — non : le pré-contrôle, l'asymétrie et sa réserve
   sont commentés au point d'application (`src/ota_type_logic.h`), pas
   reconstituables seulement par archéologie.
3. *Alternatives crédibles à figer* — non : l'alternative au remontage FS
   (« ne rien remonter, exiger un redémarrage ») est **réversible en trois
   lignes**, sans coût de migration ni impact sur un contrat externe.
4. *Verrouillage long terme* — non : pas de partition, pas de protocole, pas
   d'API publique nouvelle. Le seul changement de contrat (`200 FAIL` → codes
   HTTP réels) est une **correction** d'un comportement erroné, pas un choix
   entre options équivalentes.

Le correctif reste un **bug fix** au sens du contre-exemple explicite de la
procédure ADR. Décision consignée ici plutôt que dans un ADR vide.

**Vérifications** : `pio run` SUCCESS, `pio run -e esp32dev_core3` SUCCESS
(flash 93,5 %), `pio test -e native` **565/565**, `./build_fs.sh` SUCCESS
(327 680 o).

**Documentation livrée** : `docs/API.md` (contrat `/update` réécrit),
`docs/subsystems/ota-manager.md` (4 nouvelles sections),
`docs/features/page-settings.md` (carte « Mise à jour manuelle »),
`docs/UPDATE_GUIDE.md` (procédure + « Lire un échec »), `CHANGELOG.md` [2.20.1].

**Reste ouvert (hors périmètre)** : le repli « détection par nom de fichier »
est dupliqué entre `src/ota_type_logic.h` (`kFsSuffixes`) et `data/app.js`
(`detectUpdateType`) — les deux règles doivent évoluer ensemble, rappel écrit
des deux côtés.
