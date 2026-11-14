## Métadonnées
- **ID** : feature-170
- **Slug** : retirer-la-confirmation-de-correction-ph
- **Type** : feature (retrait — ⚠️ **revient sur une condition bloquante d'un gate chimie**)
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-11-14
- **Spec(s) liée(s)** : `done/feature-168-correction-ph-sans-borne.md` (**condition C12**, qui a
  créé ce mécanisme) · `done/feature-156-…` (bandeau de péremption) · ADR-0098 (une constante
  déclare sa source)

## Domaines touchés
- [x] **Capteurs (pH)** — la correction de mesure
- [x] **Régulation** → ⛔ **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage · [ ] MQTT · [ ] UART · [ ] OTA · [ ] Authentification
- [x] **Web — routes HTTP / WebSocket** (`ph_offset_stale`, `ph_offset_stale_cause`)
- [x] **Frontend (`data/`)** — bandeau et boutons
- [x] **Historique / persistance** (NVS)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT.** ⚠️ Cette spec **retire** une condition
      qu'il avait posée (C12 de feature-168). ⛔ Il ne s'agit pas de la contourner : il s'agit
      de la lui **re-soumettre avec des arguments neufs**, qu'il n'avait pas au moment du gate.
- [x] ux-ui-designer — un bandeau et deux boutons disparaissent d'un écran
- [x] embedded-developer · [x] web-ui-developer
- [x] test-engineer · [x] code-reviewer · [x] doc-writer

## Description
Retirer le mécanisme de **confirmation** de la correction de mesure pH : le drapeau
`phOffsetBootUnconfirmed`, la cause de péremption `boot`, le bandeau et son bouton **« Garder la
correction »**.

## Contexte — 🔴 DÉCISION UTILISATEUR DU 2026-11-14

> *« Si on a enregistré une valeur à 1.2, en quoi une 2ᵉ confirmation apporte plus de preuve que
> la donnée n'a pas été altérée ? »*
> *« 0.5 comme seuil est totalement arbitraire. Pourquoi pas 0.2 ou 1.0 ? »*
> *« Ce mécanisme doit être supprimé. Si on veut garantir que la valeur n'est pas altérée, il
> faut trouver un mécanisme de stockage sûr. »*

⭐ **Les trois arguments sont justes, et vérifiés dans le code** :

1. **Une confirmation humaine ne prouve RIEN sur la mémoire.** Elle établit une **intention**, à
   l'instant du clic. ⛔ Ce n'est pas la question posée, qui est : *cette valeur a-t-elle été
   altérée ?* Un clic n'y répond pas, et le clic suivant pas davantage.
2. **Le seuil 0,50 est HÉRITÉ, pas calculé.** `constants.h` le dit mot pour mot :
   *« kPhOffsetUnusualAbs — l'ANCIENNE borne ±0,50 (feature-067/pool-chemistry), CONSERVÉE »*.
   ⚠️ C'était une borne de **RÉGLAGE** (jusqu'où corriger), recyclée en seuil de **DÉTECTION**
   (qu'est-ce qui est suspect) ⛔ **sans être rejustifiée pour ce second usage**. ADR-0098 exige
   qu'une constante déclare sa source ; celle-ci déclare un **héritage**, pas une raison.
3. ⛔ **L'amplitude est le mauvais instrument.** Une mémoire altérée produit **0,30** aussi
   facilement que **9,30**. Un seuil attrape les **grandes** valeurs, ⛔ pas les **fausses**.
   ⇒ Le mécanisme surveille autre chose que ce qu'il prétend surveiller.

🔴 **Et un quatrième défaut, mesuré dans le code** (`config.cpp:798`) : le drapeau est relevé
**à CHAQUE démarrage** dès que `|offset| > 0,50`, ⛔ **même après confirmation**. ⇒ Confirmer ne
règle rien : la question revient au redémarrage suivant.
⚠️ **Conséquence observée** : l'utilisateur, seul concerné parce que sa correction est
légitimement grande (+1,20, cohérente avec un zéro d'électrode à 128 mV), a vu ce message
pendant **un mois sans jamais comprendre ce qu'on lui demandait**. ⭐ Un avertissement que
personne ne comprend n'avertit personne.

## Ce que C12 voulait protéger — ⛔ à ne pas perdre de vue
L'écrêtage retiré par feature-168, une valeur NVS corrompue **dans** l'échelle (ex. +9,30) est
désormais **appliquée**, et elle décale la mesure sur laquelle l'appareil **dose de l'acide**.
⚠️ **Le besoin est réel.** C'est la **réponse** qui était mauvaise.

## Comportement attendu
1. ⛔ **Supprimer** : `phOffsetBootUnconfirmed`, la cause `boot`, le bandeau pour cette cause,
   le bouton « Garder la correction » s'il ne sert plus qu'à ça.
2. **Conserver** le log `critical` + l'alerte MQTT à la relecture d'une correction inhabituelle :
   ⭐ **ça n'interrompt personne et ça laisse une trace**. ⛔ Mais ce n'est plus une **question**.
3. ⚠️ **Arbitrer le sort des deux autres causes** (`calibration`, `expiration` à 30 jours) :
   elles répondent à une question **différente** — la correction est-elle **périmée** —
   ⛔ pas « la mémoire est-elle saine ». ⇒ `pool-chemistry` dit si elles restent.
4. 🔴 **La vraie réponse au besoin de C12 est un STOCKAGE SÛR**, ⛔ pas un humain :
   - **À vérifier d'abord** : NVS calcule-t-il un **CRC par entrée** ? ⚠️ Vérification
     **commencée et NON terminée** le 2026-11-14 — ⛔ ne pas la présumer. Si oui, une entrée
     corrompue est **déjà** rejetée à la lecture, et le besoin est largement couvert en amont.
   - Sinon : écrire une **somme de contrôle** (ou un double) à côté de la valeur, vérifiée au
     démarrage. ⇒ Détection **sans seuil, sans humain, sans amplitude**.
   - ⛔ Ce qu'aucun des deux ne détectera jamais : une valeur **correctement stockée mais
     fausse** (écrite par un bug). ⚠️ **Le dire**, plutôt que laisser croire le contraire.

### Cas limites
- ⚠️ Un appareil **déjà** en `phOffsetBootUnconfirmed = true` doit repartir proprement : le
  champ NVS disparaît ou est ignoré, ⛔ sans bloquer le démarrage.
- La parité `/get-config` ↔ WebSocket doit rester **exacte** sur les champs retirés
  (⛔ 4 récidives documentées de ce piège — comparer les **expressions**, pas les clés).

## Comportement actuel
À chaque démarrage, si `|ph_offset| > 0,50` : log `critical`, alerte MQTT, bandeau ambre
« Correction à revoir » sur la page pH, deux boutons. Confirmer éteint le drapeau **jusqu'au
redémarrage suivant seulement**.

## Contraintes techniques
- ⛔ **Ne PAS toucher** aux filets bruts absolus `kPhRawFloorAbsolute` (5,50) /
  `kPhRawCeilingAbsolute` (8,50) : ils ont une **justification chimique propre** (chlore gazeux
  sous ~5,0 ; 9 % de HOCl à 8,50) et ne sont **pas** concernés.
- ⛔ Ne pas réintroduire de borne sur la correction : feature-168 l'a retirée **à la demande
  explicite de l'utilisateur**.
- Français dans toute l'UI. Vanilla JS. Offline first.

## Fichiers potentiellement concernés
- `src/config.cpp` — `phOffsetStaleNow()`, `phOffsetStaleCause()`, relecture NVS (~798)
- `src/constants.h` — `kPhOffsetUnusualAbs`, `kPhOffsetConfirmIntervalDays`
- `src/web_routes_config.cpp` · `src/ws_manager.cpp` — champs servis (⚠️ **parité**)
- `src/main.cpp` — alerte MQTT différée
- `data/index.html` · `data/app.js` — bandeau, textes, boutons
- `docs/features/page-ph.md` · `docs/MQTT.md` · `CHANGELOG.md`

## Livrables — check-list
- [ ] A0 — 🔴 **Verdict `pool-chemistry`** sur le retrait, AVANT toute ligne de code
- [ ] A1 — retrait du drapeau, de la cause `boot` et des champs servis associés
- [ ] A2 — conservation du log + alerte MQTT **en information**
- [ ] A3 — sort des causes `calibration` / `expiration`, tranché par le gate
- [ ] A4 — **vérifier** la protection d'intégrité de NVS ; si insuffisante, somme de contrôle
- [ ] B1 — retrait du bandeau et des boutons (passage `ux-ui-designer`)
- [ ] D1 — vecteurs : un appareil porteur de l'ancien champ NVS démarre proprement
- [ ] C1 — docs + CHANGELOG

## Sécurité chimique
- Dosage impliqué : **oui, indirectement** — la correction décale la mesure qui pilote
  l'acidification.
- Risque identifié : ⚠️ **une correction fausse reste appliquée sans être signalée comme
  question.** ⭐ Mais elle l'est **déjà** : le mécanisme actuel ⛔ ne bloque rien, il demande —
  et personne ne répondait. ⇒ **Le retrait ne retire aucune protection effective** ; il retire
  une **question sans réponse**.

---

# Verdict `pool-chemistry` — 2ᵉ passe (2026-11-14) : **GO** pour le retrait de `expiration` aussi

## Décision utilisateur qui a provoqué cette 2ᵉ passe
> *« Je n'ai encore rien compris à tes demandes. On supprime ce message ou pas ? »*

⚠️ **C'est le défaut que ce lot corrige, en train de se rejouer** : présenter trois options sur
un mécanisme jamais compris, ⛔ ce n'est pas consulter, c'est reporter la charge. ⇒ Les **deux**
motifs partent : `boot` **et** `expiration`.

## Pourquoi `expiration` tombe (argument du gate, ⛔ pas du confort)
⭐ **La validité d'une correction ne se dégrade pas avec le TEMPS, mais avec des ÉVÉNEMENTS** :
intervention sur la pompe de filtration, modification du circuit électrique, étalonnage,
vieillissement de la jonction de référence. ⛔ Aucun n'est corrélé à un calendrier de 30 jours.
⇒ Un déclencheur par **date** ne mesure rien : il répète une consigne déjà écrite à l'écran, à un
moment choisi au hasard par rapport à ce qui compte.

🔴 **Et il a un coût chimique réel** : il **use le seul canal** qui porte encore une information
fondée sur un fait. ⚠️ Une alerte dont on a appris qu'elle ne veut rien dire **ne se lit plus le
jour où elle veut dire quelque chose** — et ce jour-là, c'est `calibration`.

⇒ Partent avec : `isPhOffsetExpired()`, `kPhOffsetConfirmIntervalDays`, le champ servi
`ph_offset_confirm_interval_days`. `isPhOffsetStale()` se réduit à
`isPhOffsetSupersededByCalibration()`. ⛔ **Plus aucune migration** (l'ancienne C4 disparaît).

## C11 (remplace C4) — la DATE, en permanence, sans bandeau
`ph_offset_epoch` est **déjà servi** par `/get-config` ET la trame WS — ⚠️ **et lu NULLE PART
dans `data/`** (zéro occurrence). ⭐ Un champ servi et **décoratif** : le travers que ce projet
connaît sous le nom de « parachute anti-OOM en PSRAM ».
⇒ Afficher : **« Correction +1,20 — posée le 16/10/2026 »**.
⭐ Un **fait**, visible en continu, qui ⛔ ne s'use pas, ⛔ n'interrompt personne, et laisse
l'utilisateur juger de l'ancienneté sans qu'un seuil décide à sa place.
⇒ Principe du projet : *les plafonds protègent, le reste informe*. Ici, rien à protéger.
⚠️ Patron existant : `ph_auto_manual_epoch` (`epoch == 0` ⇒ « date inconnue », ⛔ jamais 1970).

## Le bandeau `calibration`, resté seul — ⚠️ plus lisible, pas automatiquement compris
L'utilisateur a passé **un mois** à apprendre que la boîte ambre est du bruit. ⛔ Sa prochaine
apparition risque d'être lue comme « le truc qui était déjà là ».
- **C12** — ancrer le texte sur l'**événement** et ses **DEUX dates** (`ph_offset_epoch` et
  `ph_cal_epoch`, tous deux **déjà servis** et inutilisés côté client) :
  *« Vous avez étalonné la sonde le 14/11/2026. La correction en vigueur (+1,20) a été posée le
  16/10/2026, donc AVANT : elle compense un état de la sonde qui n'existe plus. »*
  ⭐ Deux dates et un lien de causalité ⇒ ⛔ impossible à confondre avec un rappel périodique.
- **C13** — nommer l'**acte** sur les boutons : **« Je l'ai vérifiée — garder »** / **« Remettre
  à zéro »**. ⛔ « Garder la correction » ne dit pas ce que le clic fait.
- **C14** — le bandeau doit **se voir comme neuf** : il hérite de la boîte que l'utilisateur a
  appris à ignorer. ⛔ Ne pas inventer de 3ᵉ couleur (daltonisme) — le chapeau
  `.callout__eyebrow` existe et porte le sens par le **mot**. Captures avant/après à 390 px.

## Conditions de la 1ʳᵉ passe
⛔ **C4 ANNULÉE** (remplacée par C11). **C5 MAINTENUE** (le seuil sert encore à l'avertissement
C8 et au log d'écriture C7 ⇒ sa justification reste due). **C6 ajustée** : le log de démarrage en
`warning` devient le **seul** signal périodique — ⛔ jamais une alerte MQTT.
**C1, C2, C3, C7, C8, C9, C10 inchangées.** ⚠️ **C2 reste valable** : `ph_offset_stale` et
`ph_offset_stale_cause` **survivent** pour `calibration` — ⛔ ne pas les supprimer sous prétexte
qu'il ne reste qu'une cause.

## Test exigé en plus
Un vecteur prouvant qu'**aucune** cause ne se lève sur une correction de **+1,20 vieille de 400
jours**. **Mutation** : réintroduire un test d'âge ⇒ ce vecteur doit **mourir**.
