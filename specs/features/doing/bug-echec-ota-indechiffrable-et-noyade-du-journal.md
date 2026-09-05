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

- [ ] Une image filesystem envoyée en type « firmware » produit **une seule**
      ligne d'erreur, nommant la cause réelle et suggérant le type
- [ ] L'historique du journal antérieur à la tentative est **préservé**
- [ ] Le message affiché dans l'UI nomme la cause
- [ ] Le chemin nominal (bon type) reste inchangé — vérifié on-target
- [ ] `pio run` bi-core verts, `pio test -e native` sans régression
