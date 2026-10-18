## Métadonnées
- **ID** : bug-174
- **Slug** : download-update-sans-echeance-dinactivite
- **Type** : bug — durcissement, pas de défaut observé en production à ce jour
- **Statut** : todo
- **Priorité** : normale — ⚠️ conséquence mesurée mais pas (encore) déclenchée en production
- **Version cible** : à planifier (aucune version cible fixée à l'ouverture de cette spec)
- **Dernière mise à jour** : 2026-10-18
- **Spec(s) liée(s)** : `doing/bug-173-la-reponse-github-est-lue-au-fil-de-l-eau-et-tronquee.md`
  (⚠️ reste en `doing/` : son critère n°1 — test sur l'appareil réel — n'est pas encore coché)
  (c'est son instruction qui a fait remonter cette lacune, sans la corriger — hors périmètre
  explicite de ce lot-là) · [ADR-0119](../../docs/adr/0119-reponse-check-update-bufferisee-en-psram-bornee.md)

## Domaines touchés
- [ ] Capteurs · [ ] Régulation · [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket (hors périmètre direct : la route existe déjà)
- [ ] MQTT / Home Assistant · [ ] UART
- [x] **OTA**
- [ ] Authentification · [ ] Historique / persistance · [ ] Frontend

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — ⚠️ **pas automatique ici**, mais si l'échéance d'inactivité touche l'ordre
      `setOtaInProgress`/montage FS, le gate doit être consulté (voir `bug-175`, spec jumelle)
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
`POST /download-update` télécharge le `.bin` depuis GitHub par une boucle de lecture qui n'a
**aucune échéance d'inactivité** — seul le watchdog ESP32 (30 s, `kWatchdogTimeoutSec`) finit par
intervenir, en `panic`, pas en abandon propre. Borner cette boucle par une échéance
**d'inactivité** (pas une durée totale) et lui faire produire un refus HTTP propre au lieu d'un
redémarrage par panic.

## Contexte
Pendant l'instruction de `bug-173` (parse de `/check-update` tronqué en silence), la revue de
sécurité a vérifié que `/download-update` **ne portait pas** le même défaut : sa boucle de lecture
est pilotée par le `content-length` annoncé et l'empreinte SHA-256 est vérifiée avant toute
activation de l'image (voir `docs/subsystems/ota-manager.md`). Mais cette même revue a identifié
une lacune **distincte**, non corrigée dans ce lot (hors périmètre explicite de `bug-173`) :
**rien ne borne le temps d'INACTIVITÉ** de cette boucle. Si le pair cesse d'envoyer des données
sans fermer la connexion (coupure réseau asymétrique, proxy qui ferme silencieusement dans un
sens), la boucle attend indéfiniment — jusqu'à ce que le watchdog de la tâche AsyncTCP (30 s,
`kWatchdogTimeoutSec`, voir la note corrigée dans `docs/subsystems/ota-manager.md` sur
l'abonnement réel de cette tâche au TWDT) déclenche un **panic**, pas un abandon propre.

⚠️ **Conséquence mesurée, pas seulement théorique** : sur la **voie filesystem**, au moment où ce
blocage se produirait, la partition `spiffs` a **déjà été effacée** (le flash écrase au fil de
l'eau, avant la fin du transfert). Un panic watchdog à cet instant redémarre l'appareil sur un
**filesystem vide** — l'interface web est **indisponible** jusqu'à ce qu'un nouvel OTA filesystem
soit effectué (par upload manuel, l'API HTTP n'étant plus servie). C'est un pire cas nettement
plus sévère qu'un simple échec de téléchargement.

## Comportement attendu
1. La boucle de lecture du `.bin` est bornée par une échéance d'**inactivité** — **pas** une durée
   totale : un téléchargement de ~1,5 Mo (image firmware) sur un lien WiFi lent peut légitimement
   dépasser 15 s au total, ce n'est pas une anomalie. L'échéance se réarme à chaque octet reçu.
2. Dépassement de l'échéance ⇒ **abandon propre** : `Update.abort()`, fermeture de la connexion,
   réponse HTTP d'erreur explicite — **jamais** laisser le watchdog trancher par un panic.
3. **Voie filesystem** : si l'abandon survient après que la partition a été effacée, le message
   d'erreur et le journal doivent le dire explicitement (l'utilisateur doit savoir qu'un nouvel
   OTA filesystem est nécessaire, pas simplement « réessayez »).
4. `PumpController.setOtaInProgress(false)` et le remontage best-effort de LittleFS (existants,
   v2.20.1) s'appliquent sur ce nouveau chemin terminal, comme sur tous les autres.
5. Aucune régression sur le cas nominal (téléchargement lent mais continu) : seule l'**inactivité**
   déclenche l'abandon, pas la lenteur en elle-même.

### Cas limites
- Téléchargement qui démarre normalement puis se fige après quelques Ko (le cas réel visé).
- Téléchargement complet mais dont le dernier chunk n'arrive jamais (EOF jamais signalé).
- Échéance d'inactivité qui expire **exactement** au dernier octet (frontière) — ne doit pas
  rejeter un transfert en réalité terminé.

## Comportement actuel
Boucle pilotée uniquement par `content-length` et l'état de connexion, sans minuterie
d'inactivité. Seul filet : le watchdog FreeRTOS de la tâche AsyncTCP (30 s), qui panique plutôt
que d'abandonner proprement.

## Contraintes techniques
- ESP32, offline first
- Logique de décision (continuer / abandonner) à extraire en module **pur testable en natif**
  (ADR-0017) — le projet vient de refaire exactement cet exercice pour `/check-update`
  (`src/ota_response_buffer_logic.h`, bug-173) : s'en inspirer, ne pas dupliquer une structure
  différente sans raison.
- `esp_task_wdt_reset()` déjà appelé périodiquement sur ce chemin — vérifier son nourrissage reste
  correct avec la nouvelle structure de boucle.
- Watchdog **jamais désactivé** (règle de sécurité absolue, `CLAUDE.md`).

## Fichiers potentiellement concernés
- `src/web_routes_ota.cpp` — boucle de téléchargement de `handleDownloadUpdate`
- `src/constants.h` — nouvelle constante nommée (échéance d'inactivité, distincte de
  `kOtaCheckResponseReadTimeoutMs` qui est une échéance **totale** pour une réponse **beaucoup**
  plus petite)
- `src/ota_download_logic.h` [nouveau, éventuel] — logique pure de décision, sur le modèle de
  `ota_response_buffer_logic.h`
- `docs/subsystems/ota-manager.md` — section flow `/download-update`

## Livrables — check-list
- [ ] A1 — Échéance d'inactivité nommée dans `constants.h`, avec sa source documentée (ADR-0098)
- [ ] A2 — Boucle de lecture réarmant l'échéance à chaque octet reçu, logique de décision pure
      testée en natif
- [ ] A3 — Abandon propre : `Update.abort()`, fermeture connexion, réponse HTTP explicite — plus
      aucun panic watchdog déclenchable par ce chemin
- [ ] A4 — Message distinct si l'abandon survient après effacement de la partition FS
- [ ] D1 — Tests natifs + validation par mutation (rétablir l'absence d'échéance doit faire
      échouer un test)
- [ ] C1 — `docs/subsystems/ota-manager.md` mis à jour

## Sécurité chimique
- Dosage impliqué : non directement — mais `setOtaInProgress`/remontage FS touchent l'ordre déjà
  gardé par `pool-chemistry` (v2.20.1). Si cette spec introduit un nouveau chemin terminal,
  vérifier qu'il respecte le même ordre (réarmement dosage **avant** tentative de remontage).
- Risque identifié : aucun nouveau — c'est une amélioration de robustesse d'un chemin déjà gardé.

## ADR attendu ?
- **Probable** : non, a priori — c'est une application du patron déjà posé par `bug-173`/ADR-0119
  (boucle bornée, logique pure, abandon propre nommé), pas un nouveau trade-off. À reconfirmer par
  `architect` si l'implémentation révèle une alternative crédible non triviale.
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- `POST /download-update` — nouveau cas de refus (timeout d'inactivité)

### Documentation à produire / mettre à jour
- `docs/subsystems/ota-manager.md` — section flow `/download-update`
- `docs/API.md` — nouveau cas d'erreur si le contrat de réponse change
- `CHANGELOG.md` — entrée à ajouter sous la version cible

## Critères d'acceptation
- [ ] Un téléchargement qui se fige après le démarrage est abandonné proprement, sans panic
      watchdog, dans un délai borné et nommé
- [ ] Un téléchargement lent mais continu (jamais d'inactivité) aboutit normalement, quelle que
      soit sa durée totale
- [ ] Voie filesystem : message explicite si l'abandon survient après effacement de la partition
- [ ] Mutation : rétablir l'absence d'échéance d'inactivité doit tuer un test
- [ ] `pio run` 0 warning · natifs sans régression

## Plan de test manuel
1. Simuler une coupure réseau en cours de téléchargement (ex. couper le WiFi du routeur
   intermédiaire sans fermer la connexion TCP côté appareil) et vérifier l'abandon propre.
2. Télécharger un firmware complet sur un lien WiFi volontairement limité en débit (QoS) et
   vérifier qu'il aboutit malgré une durée totale longue.
3. Relire le journal : un seul message d'erreur par tentative avortée (pas de spam, même
   principe que `v2.20.1`).

## Notes pour agent
- S'appuyer sur le patron de `bug-173` (logique pure testée nativement, échéance nommée dans
  `constants.h` avec sa source documentée) plutôt que de réinventer une structure différente.
- Ne pas confondre cette échéance avec `kOtaCheckResponseReadTimeoutMs` (bug-173) : celle-ci est
  **totale** sur une réponse JSON de quelques dizaines de Ko ; celle à introduire ici est
  **d'inactivité** sur un binaire potentiellement de plusieurs Mo.

## Hors périmètre
- Le parse de `/check-update` — déjà traité par `bug-173`.
- La signature de l'image OTA — `feature-151`.
