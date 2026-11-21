# ADR-0128 — L'appareil refuse une image OTA non authentique ; la décision est séparée de la page par le type

- **Statut** : Accepté
- **Date** : 2026-11-16
- **Décideurs** : Nicolas + Claude (`architect`, `security-reviewer`, `code-reviewer`)
- **Spec(s) liée(s)** : [`feature-151`](../../specs/features/done/feature-151-signature-de-l-image-ota.md) (sous-lot D3-c « le refus » ; lève l'inertie posée par D1 à D3-b2, voir [ADR-0122](0122-socle-decision-ota-genre-empreinte-relais-sig.md) et [ADR-0125](0125-l-expiration-de-l-interrupteur-de-signature-se-mesure-sur-un-horodatage-nvs.md))

## Contexte

Jusqu'à D3-b2, l'appareil vérifiait la signature d'une image OTA, journalisait le verdict, et **n'appliquait aucun refus** : le chemin d'acceptation devait être prouvé sur le terrain avant que le chemin de refus n'existe. Cette preuve (F0, B12 marche 3) est faite pour les portes de sortie ; D3-c active le refus. Il fallait trancher : quoi refuser, quoi ne jamais refuser, comment empêcher qu'une page ancienne (qui n'envoie pas la signature) ne devienne une raison de tolérer, et par où sortir quand le refus gêne.

## Décision

1. **L'appareil refuse** (`POST /update` et `POST /download-update`) tout verdict d'authenticité autre que `valide`, **avant toute écriture** et avant les trois `setOtaInProgress(true)`, sauf autorisation temporaire de mise à jour non signée activée. Table : absente 403, malformée 400, invalide 403, vérification impossible 503, genre discordant 415, plancher 403 (inatteignable). Les trois sites tardifs de liaison image↔manifeste restent `400` / `422 integrity_mismatch` (formats figés).
2. ⛔ **Ce qui n'est JAMAIS refusé** : le **désaccord de version** (la référence vient du tag GitHub mémorisé par `/check-update` et **périme après un envoi direct** : un « désaccord » peut être un faux positif) et le **plancher de version** (**aucun, nulle part** — décision de l'utilisateur ; le retour arrière manuel reste possible).
3. ⭐ **Séparation typée de la décision et de la phrase** ([`ota_refusal_messages.h`](../../src/ota_refusal_messages.h), module pur, ADR-0017) : `otaRefusalDecide(verdict)` ne reçoit **aucun** paramètre de page ; `otaRefusalPhrase(verdict, pageAnnouncedVersion, site)` reçoit le marqueur POST `ui` (présence seule) et ne change que la phrase. Le marqueur de page est **incapable par construction** d'atteindre la décision : l'option « le firmware tolère une page ancienne » est fermée par le type, pas par une surveillance qu'un futur lot pourrait oublier.
4. **Quatre portes de sortie**, chacune pour un scénario :
   - **page de secours** (`/recovery`) : interface effacée ou refus tardif (prouvée B12 marche 3) ;
   - **autorisation temporaire** (60 min, consommée au premier flash) : clé perdue ou défaut du vérificateur (prouvée F0) ;
   - **`POST /update` signé** : sans plancher, toute image signée passe, y compris plus ancienne ;
   - **`/factory-reset`** : dernier recours.
5. **Refus tardif accepté** : si l'image est authentique mais diverge de son manifeste après écriture, le filesystem peut être détruit : l'interface est perdue, la régulation continue, la sortie est la page de secours. Phrases 7 et 8 (jamais « Rien n'a été modifié », faux dès que l'écriture a commencé).
6. **Vocabulaire** : « interrupteur » devient « autorisation temporaire de mise à jour non signée » ; « armé/désarmé » devient « activée/retirée » ; entité Home Assistant « Mise à jour non signée autorisée ». ⛔ `unique_id`, topic, routes et champs JSON inchangés (interfaces publiques).

## Alternatives considérées

- **Tolérer une page ancienne** (absence de `.sig` acceptée si le marqueur `ui` est absent) (rejetée) — rouvre l'envoi non signé à tout client qui omet le marqueur ; un attaquant l'omettrait.
- **Passer le marqueur de page à la décision et le surveiller par test** (rejetée) — une garde de test peut être contournée ou oubliée ; le type ne le peut pas.
- **Refuser sur désaccord de version** (rejetée) — la référence GitHub périme après un envoi direct (faux refus), et le désaccord n'est pas une preuve d'inauthenticité.
- **Plancher de version actif** (rejetée par l'utilisateur) — interdirait le retour arrière manuel, seule réparation d'une régression.
- **409 pour le genre discordant** (rejetée) — 409 est déjà pris par « envoi concurrent » ; 415 est retenu.
- **Même format de réponse pour les deux routes** (rejetée) — `/update` répond `text/plain` `FAIL: …` depuis toujours (appelants : `ota_update.sh`, page) ; `/download-update` répond en JSON avec `code`. Dissymétrie assumée plutôt que rupture de contrat.

## Conséquences

### Positives
- L'appareil satisfait enfin la règle de sécurité n° 6 de `CLAUDE.md` pour l'authenticité (clé publique), au-delà de l'intégrité.
- Le refus précoce n'a aucun effet de bord (pompes non gelées, filesystem monté, partition intacte).
- Les phrases de refus disent quoi faire ; l'interface les affiche sans les remplacer.

### Négatives / dette assumée
- 🔴 **Le vecteur « jeton administrateur volé » RESTE OUVERT** : un porteur du jeton active l'autorisation temporaire puis envoie ; l'autorisation le rouvre **en deux requêtes** au lieu d'une. Le refus ne le ferme pas.
- Un refus tardif peut détruire l'interface (récupérable par la page de secours).
- ⚠️ **[ADR-0022](0022-verification-integrite-ota.md) déclare encore `ArduinoOTA` actif** alors que `otaManager` n'a **aucun appelant** dans `src/`. S'il était rebranché, il **contournerait totalement** le refus (flash sans passer par les routes HTTP). ADR-0022 est daté et immuable : le constat est consigné ici, et [`tools/check_ota_manager_isolated.sh`](../../tools/check_ota_manager_isolated.sh) (job CI `ota-manager-isole`) **l'interdit désormais mécaniquement** (aucune référence à `otaManager`, `ota_manager.h` ni `ArduinoOTA` hors `ota_manager.*`).
- **Campagne de terrain NON faite** : douze essais (G0 à F11) restent à mener, dont l'**expiration naturelle à 60 min**, jamais observée sur l'appareil.

### Ce que ça verrouille
- Le contrat HTTP des refus (codes, `code` JSON de `/download-update`, phrases) : l'interface lit la phrase, un script lira `code`.
- L'impossibilité de faire dépendre la décision de refus d'une donnée de page.
- Tout futur plancher ou refus sur la version exigera un nouvel ADR qui supersède celui-ci.

## Références

- Code : [`src/ota_refusal_messages.h`](../../src/ota_refusal_messages.h) / `.cpp`, [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp), [`src/web_routes_ota_switch.cpp`](../../src/web_routes_ota_switch.cpp), [`tools/check_ota_manager_isolated.sh`](../../tools/check_ota_manager_isolated.sh)
- Spec : [`specs/features/done/feature-151-signature-de-l-image-ota.md`](../../specs/features/done/feature-151-signature-de-l-image-ota.md)
- Doc : [`docs/API.md`](../API.md#refus-d-une-image-non-authentique-feature-151-d3-c), [`docs/subsystems/ota-manager.md`](../subsystems/ota-manager.md), [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md#appareil-refuse-une-mise-à-jour-non-signée)
