# ADR-0139 — Le firmware notifie un ÉVÉNEMENT, l'écran rend un ÉTAT — ils ne partagent que l'acte

- **Statut** : Accepté
- **Date** : 2026-11-22
- **Décideurs** : Nicolas + Claude (`pool-chemistry`, `architect`, `test-engineer`)
- **Spec(s) liée(s)** : [feature-170](../../specs/features/done/feature-170-retirer-la-confirmation-de-correction-ph.md) (C32, V6a/V6b/V6c) ; prolonge [ADR-0100](0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md) et [ADR-0099](0099-tester-par-extraction-de-texte-jamais-par-duplication.md) dans leur esprit

## Contexte

La cause de caducité `calibration` n'avait aucun signal hors écran : `markPhCalibrationStamped()` ne faisait qu'un `systemLogger.info`. Feature-170 lui en donne un : une alerte MQTT `ph_offset_stale` à l'étalonnage. Le bandeau de l'écran, lui, rend **quatre variantes datées** (date d'étalonnage connue ou non, date de pose connue ou non).

Faut-il que le firmware reproduise ces quatre rendus ? Le canal MQTT est **borné** : `publishAlert()` tronque à **~46 octets utiles** (`kMaxPayloadLen = 128` dans `mqtt_manager.h`, enveloppe JSON ~62 o, queue `"... (voir journal)"` 19 o). En français accentué, cela fait ~43 **caractères** (`à` et `é` pèsent 2 octets). Le texte **intégral** part néanmoins au journal : `mqtt_manager.cpp:929` journalise `message`, pas le `msg` tronqué.

## Décision

1. **Le firmware notifie un événement, l'écran rend un état.** L'écran est lu des semaines après et doit situer deux événements l'un par rapport à l'autre (d'où les dates). L'alerte part à l'instant même : « vient de changer » est plus exact qu'une date. Le cas où le firmware n'a aucune date à citer (pas de NTP, RTC morte) est **sans objet** pour lui, non impossible.
2. **Un seul texte côté firmware**, produit par un module pur (`src/ph_stale_message.h`, ADR-0017) : *« Correction de mesure +1,20 à vérifier : l'étalonnage vient de changer, après sa pose — il a déplacé la mesure sur laquelle elle avait été calculée. Refaites un test manuel et accordez la mesure dessus, ou remettez la correction à zéro. »*

   🔴 **2bis. N'AFFIRME QUE L'ÉVÉNEMENT QUE TU PEUX PROUVER.** Quand **plusieurs chemins mènent au même état**, le texte nomme **l'ÉTAT**, ⛔ jamais le chemin.
   **Ce qui a établi la règle** : `markPhCalibrationStamped()` est appelée depuis **trois** sites de `src/sensors.cpp` — `CalibratePhMid`, `CalibratePhLow` **et `ClearPhCal`**, l'**effacement** de l'étalonnage (délibéré, posé sur contre-visite d'un gate). Le premier texte écrit disait « un étalonnage **vient d'avoir lieu** » et le chapeau de l'écran « Étalonnage **refait** » : ⛔ **faux sur le troisième chemin**, et faux au moment précis d'un **dépannage** — donc là où l'utilisateur lit, cherche un coupable, et où ce projet a déjà fait condamner une **sonde saine** (incident du 25/08/2026).
   ⇒ L'état, lui, est vrai dans les trois cas : *l'étalonnage n'est plus celui sur lequel la correction a été calculée*. D'où « L'étalonnage **a changé** » et le chapeau « Étalonnage **modifié** ».
   ⚠️ C'est la **troisième occurrence du même motif dans un seul lot** (les deux premières : ne pas affirmer un étalonnage sur une cause héritée d'un firmware antérieur ; masquer le chapeau quand l'événement n'est pas établi). Les deux premières fois, l'information **manquait** ; la troisième, elle **existait** — et le texte l'ignorait quand même.
   **Verrouillage** : les mots « refait », « vient d'avoir lieu » et « Vous avez » sont **bannis** et assertés absents du texte **PRODUIT** — 5 valeurs d'offset côté natif, 16 combinaisons cause × dates côté navigateur. ⛔ Pas un contrôle sur la **source** : les commentaires citent volontairement ces mots pour les interdire.
3. **Ils ne partagent que l'acte** : la phrase « Refaites un test manuel… à zéro. » est identique octet pour octet des deux côtés.
4. **Règle de rédaction pour tout canal borné** : *le verdict et la valeur se placent EN TÊTE — l'ordre des mots est une contrainte, pas un style.* La tête de ce texte fait 41 octets : « à vérifier » finit à l'octet 39/40, y compris pour `+10,50`. La **marge** (au moins 4 octets) est assertée par un vecteur, pas seulement le texte.
5. **Sentinelle de parité de texte `.cpp` ↔ `.js`** (`tools/check_ph_stale_text_parity.sh`), la **première du genre** dans ce projet : elle extrait la phrase entre les ancres `// >>> PH-STALE-ACTE:DEBUT` / `FIN` dans `data/app.js` **et** `src/ph_stale_message.h`, normalise la syntaxe de chaîne, exige l'égalité octet pour octet. Les harnais ADR-0099 extraient du JS vivant évalué sous Node et ne savent pas lire du C++ ; les marqueurs `PARITE-DATA` comparent deux `.cpp` et des jeux de clés, pas des littéraux.
6. ⛔ **Exigence** : une sentinelle qui ne trouve plus son ancre doit **ÉCHOUER**, jamais passer. Une garantie fausse est pire que pas de garantie.

## Alternatives considérées

- **Miroiter en C++ les quatre rendus datés** (rejetée) — deux chemins de formatage de date (`toLocaleDateString('fr-FR')` côté navigateur, `strftime` côté firmware) qu'aucun test ne peut mettre d'accord ; ~800 o de littéraux dupliqués en flash ; **pour un résultat invisible sur MQTT** (tronqué à 46 octets).
- **Ne partager aucune phrase** (rejetée) — l'acte est le geste demandé à l'utilisateur ; qu'il diverge entre l'écran et Home Assistant serait un défaut de sécurité d'usage.
- **Tout partager via un fichier généré** (non retenue) — un pipeline de génération pour une seule phrase.

## Conséquences

### Positives
- Home Assistant reçoit le verdict **et** la valeur entiers, même tronqué.
- Une seule phrase à garder synchrone, et un contrôle qui échoue si on la déplace.

### Négatives / dette assumée
- Le texte MQTT est moins précis que le bandeau (pas de date).
- La sentinelle repose sur des ancres textuelles : elle suppose que quelqu'un les maintienne (d'où l'exigence d'échec si elles disparaissent).

### Ce que ça verrouille
- Ne pas rallonger la tête du texte d'alerte sans recompter la marge.
- Toute nouvelle phrase partagée `.cpp` ↔ `.js` reçoit ses ancres et sa sentinelle.
- Une alerte publiée produit **sa propre** ligne de journal (`Alerte: <type> - <message>`) : ne jamais la doubler d'un log de même niveau (voir [mqtt-manager.md](../subsystems/mqtt-manager.md)).

### Conventions posées par le même lot
- **Aucun `Cxx` nu hors de la fiche qui le définit.** Toute référence à une condition de gate (code, spec, commit, doc) porte l'identifiant de fiche : `feature-170 (C29)`, `feature-168 (C8)`. Motif : feature-168 occupe `C1`→`C17` sur les mêmes fichiers, et `feature-168 (C12)` désigne précisément le mécanisme que feature-170 détruit — un `grep C12` rendait le drapeau détruit **et** le texte qui le remplace. feature-170 numérote à partir de `C20`, bug-201 à partir de `C40`. Contrôle automatique : suivi `tools/check_condition_refs.sh` (non livré).

## Références

- Code : [`src/ph_stale_message.h`](../../src/ph_stale_message.h), [`src/config.cpp`](../../src/config.cpp) (`markPhCalibrationStamped`), [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) (`publishAlert`), [`data/app.js`](../../data/app.js) (`renderPhOffsetStaleBanner`)
- Sentinelles : `tools/check_ph_stale_text_parity.sh`, `tools/check_ph_stale_alert_emitted.sh`, `tools/check_ph_unusual_alert_head.sh`, `tools/check_ph_stale_banner_vectors.sh`
- Tests : `test/test_native_ph_stale_message/`, `test/ph_stale_banner_vectors.js`
- Spec : [`specs/features/doing/feature-170-retirer-la-confirmation-de-correction-ph.md`](../../specs/features/done/feature-170-retirer-la-confirmation-de-correction-ph.md)
