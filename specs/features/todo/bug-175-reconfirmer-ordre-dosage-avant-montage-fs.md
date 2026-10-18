## Métadonnées
- **ID** : bug-175
- **Slug** : reconfirmer-ordre-dosage-avant-montage-fs
- **Type** : bug — revue, pas de code attendu en sortie (sauf si `pool-chemistry` renverse l'ordre)
- **Statut** : todo
- **Priorité** : normale — ⚠️ aucun symptôme observé, c'est la **prémisse** d'une décision de
  sécurité déjà en production qui s'est révélée fausse, pas son effet
- **Version cible** : N/A — revue, pas de livraison de code attendue a priori
- **Dernière mise à jour** : 2026-10-18
- **Spec(s) liée(s)** : `doing/bug-173-la-reponse-github-est-lue-au-fil-de-l-eau-et-tronquee.md`
  (⚠️ reste en `doing/` : son critère n°1 — test sur l'appareil réel — n'est pas encore coché)
  (c'est son instruction qui a trouvé la prémisse fausse, en corrigeant un commentaire, pas la
  décision elle-même) · `docs/subsystems/ota-manager.md#ordre-impose-dosage-avant-montage-v2201`
  (gate `pool-chemistry` v2.20.1, section « ORDRE IMPOSÉ »)

## Domaines touchés
- [ ] Capteurs · [x] **Régulation (pump_controller)** → passage obligatoire par `pool-chemistry`
- [ ] Filtration / Éclairage · [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant · [ ] UART
- [x] **OTA**
- [ ] Authentification · [ ] Historique / persistance · [ ] Frontend

## Agents requis
- [x] spec-reader
- [ ] architect — pas d'implémentation attendue a priori
- [x] **pool-chemistry — OBLIGATOIRE, c'est l'objet même de cette spec**
- [ ] ux-ui-designer
- [ ] embedded-developer — **seulement si** le gate renverse l'ordre
- [ ] web-ui-developer
- [ ] test-engineer — **seulement si** du code change
- [x] code-reviewer — si du code change
- [x] doc-writer

## Description
Faire **reconfirmer par `pool-chemistry`** l'ordre imposé en v2.20.1 — « `setOtaInProgress(false)`
AVANT tout `LittleFS.begin()` » sur les chemins terminaux de `/update` et `/download-update` — dont
la **justification écrite** vient de se révéler **fausse** (découvert et corrigé le 2026-10-18,
pendant l'instruction de `bug-173`). La **conclusion** (l'ordre lui-même) tient très probablement
toujours pour une autre raison, mais c'est au gate qui a pris la décision de le dire, pas à qui
corrige un commentaire.

## Contexte
`docs/subsystems/ota-manager.md` affirmait, depuis la v2.20.1 : « ces handlers s'exécutent sur la
tâche AsyncTCP, qui **n'est pas** abonnée au watchdog » — c'était le **motif donné** pour imposer
l'ordre (réarmer le dosage avant de risquer un montage FS long ou un panic sur partition
corrompue : si le watchdog ne peut pas sauver la tâche, le pire cas devient une inhibition du
dosage **permanente**, sans reboot).

Pendant l'instruction de `bug-173`, une vérification dans la bibliothèque (pas une relecture du
commentaire) a établi le contraire : `CONFIG_ASYNC_TCP_USE_WDT` vaut **1** par défaut
(`AsyncTCP.h:41-42`), le `#undef` d'`AsyncTCP.cpp:18` ne s'applique qu'à la plateforme LibreTiny
(hors ce projet), et `platformio.ini` ne surcharge rien. **La tâche AsyncTCP EST abonnée au
watchdog.** Un montage long ou un panic provoque donc un **redémarrage**, pas un blocage
indéfini.

⚠️ **Deux revues indépendantes se sont contredites sur ce point factuel** pendant l'instruction de
`bug-173` — l'une d'elles s'appuyait sur le commentaire erroné. Le commentaire a été corrigé **sans
changer l'ordre** (il reste défendable : réarmer le dosage en premier garantit l'état sûr sans
**dépendre** du redémarrage pour y arriver — mais c'est une **nouvelle** justification, pas celle
d'origine). ⇒ **Le gate qui avait tranché en v2.20.1 doit se prononcer sur la base du fait
correct**, pas sur l'ancien.

## Comportement attendu
1. `pool-chemistry` relit la section « ORDRE IMPOSÉ » de `docs/subsystems/ota-manager.md` (déjà
   mise à jour avec le fait corrigé) et rend un verdict explicite : **l'ordre reste-t-il justifié**
   maintenant que la tâche AsyncTCP est effectivement surveillée par le watchdog ?
2. Si **oui** (ordre confirmé, nouvelle justification acceptée) : consigner le verdict dans
   `docs/subsystems/ota-manager.md` (remplacer la mention « à reconfirmer » par la confirmation
   datée) et dans cette spec, puis la clore sans code.
3. Si **non** (le gate identifie un angle mort que la nouvelle justification ne couvre pas) :
   rouvrir l'implémentation (`architect` + `embedded-developer` + `test-engineer` +
   `code-reviewer`), avec la même rigueur que pour toute modification de `pump_controller`.

### Cas limites
- Aucun cas limite technique : cette spec est une **revue de décision**, pas une implémentation.

## Comportement actuel
L'ordre « réarmer le dosage avant de remonter le filesystem » est en production depuis la v2.20.1
et reste inchangé par `bug-173`. Seule sa justification écrite a été corrigée.

## Contraintes techniques
- Aucune modification de code tant que `pool-chemistry` n'a pas statué.
- Toute règle de sécurité absolue de `CLAUDE.md` (watchdog jamais désactivé, etc.) reste opposable
  pendant la relecture.

## Fichiers potentiellement concernés
- `docs/subsystems/ota-manager.md` — section « ORDRE IMPOSÉ », à compléter du verdict
- `src/web_routes_ota.cpp` — **seulement si** le gate renverse l'ordre

## Livrables — check-list
- [ ] A1 — Verdict explicite et daté de `pool-chemistry`, consigné dans
      `docs/subsystems/ota-manager.md`
- [ ] A2 — Si renversement : implémentation du nouvel ordre + tests + revue

## Sécurité chimique
- Dosage impliqué : oui — `PumpController.setOtaInProgress()` conditionne l'inhibition des pompes
  doseuses pendant une OTA.
- Limites journalières / horaires concernées : aucune directement, c'est l'**inhibition globale**
  pendant l'OTA qui est en jeu.
- Filtration requise avant action : sans objet (l'OTA inhibe le dosage, ne l'autorise pas).
- Calibration capteur modifiée : non.
- Algorithme PID touché : non.
- Risque identifié : si l'ordre s'avérait insuffisant, un montage FS bloquant pourrait laisser le
  dosage inhibé plus longtemps que nécessaire (pas un risque de **sur-dosage**, un risque de
  régulation **absente** pendant la panne — déjà le cas aujourd'hui pendant toute OTA).

## ADR attendu ?
- **Probable** : non, si le gate confirme l'ordre existant (pas de nouvelle décision, juste une
  justification corrigée). **Oui** si le gate renverse l'ordre — ce serait alors une décision
  structurante sur un chemin de sécurité déjà verrouillé par une note de `docs/subsystems/ota-manager.md`.
- **Critère(s) candidat(s)** : 2 (décision invisible dans le code) — **si** renversement
- **Sujet pressenti** : aucun à ce stade

## Éléments visibles (pour doc-writer)
### Documentation à produire / mettre à jour
- `docs/subsystems/ota-manager.md` — verdict de `pool-chemistry` consigné dans la section « ORDRE
  IMPOSÉ »
- `CHANGELOG.md` — seulement si l'ordre change

## Critères d'acceptation
- [ ] `pool-chemistry` a rendu un verdict explicite, daté, consigné dans
      `docs/subsystems/ota-manager.md`
- [ ] Si l'ordre est confirmé sans changement : aucune régression, spec close sans code
- [ ] Si l'ordre est renversé : implémentation testée, revue de code faite, CHANGELOG mis à jour

## Plan de test manuel
- Sans objet tant qu'aucun code ne change. Si renversement : reprendre le plan de test de
  `docs/subsystems/ota-manager.md` pour les chemins terminaux `/update` et `/download-update`.

## Notes pour agent
- Cette spec n'exige **pas** de compilation ni de modification de code par défaut — c'est une
  demande de confirmation de décision. Ne pas sur-outiller.
- Le fait corrigé (AsyncTCP abonné au watchdog) est déjà écrit dans
  `docs/subsystems/ota-manager.md`, section « ORDRE IMPOSÉ » — partir de là, ne pas re-vérifier
  dans la bibliothèque sauf doute.

## Hors périmètre
- Le reste de `bug-173` (buffering PSRAM de `/check-update`) — déjà traité, clos séparément.
- `bug-174` (échéance d'inactivité de `/download-update`) — spec jumelle distincte.
