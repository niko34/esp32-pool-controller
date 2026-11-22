# feature-170 — Retirer la confirmation de correction pH

## Métadonnées
- **ID** : feature-170
- **Slug** : retirer-la-confirmation-de-correction-ph
- **Type** : feature (retrait — ⚠️ **revient sur une condition bloquante d'un gate chimie**)
- **Statut** : **doing**
- **Priorité** : normale
- **Version cible** : 2.99.0
- **Dernière mise à jour** : 2026-11-22
- **Spec(s) liée(s)** : `done/feature-168-correction-ph-sans-borne.md` (**sa condition C12**, qui a
  créé ce mécanisme) · `done/feature-156-…` (bandeau de péremption) · `todo/bug-201-…`
  (**BLOQUÉ par cette fiche** ; porte la dette de visibilité de `feature-170 (C28)`) ·
  ADR-0098 (une constante déclare sa source) · ADR-0100 (un verdict servi n'est jamais recalculé
  côté client) · **ADR-0138** (à créer, `feature-170 (C34)`)

## Domaines touchés
- [x] **Capteurs (pH)** — la correction de mesure
- [x] **Régulation** → ⛔ **passage obligatoire par `pool-chemistry`** (fait : 3 passes, voir plus bas)
- [ ] Filtration / Éclairage · [ ] UART · [ ] OTA · [ ] Authentification
- [x] **MQTT** — ⚠️ bilan du lot : **deux émetteurs retirés, un ajouté** (`feature-170 (C32)`)
- [x] **Web — routes HTTP / WebSocket** (`ph_offset_stale`, `ph_offset_stale_cause`,
      `ph_offset_confirm_interval_days`)
- [x] **Frontend (`data/`)** — bandeau, textes, boutons, ligne de date
- [x] **Historique / persistance** (NVS) — ⭐ **rien à migrer**, voir `feature-170 (C27)`

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT.** ⚠️ Cette spec **retire** une condition
      qu'il avait posée (`feature-168 (C12)`). ⛔ Il ne s'agit pas de la contourner : il s'agit
      de la lui **re-soumettre avec des arguments neufs**. ✅ **Fait** — 3ᵉ passe du 2026-11-22,
      **GO SOUS CONDITIONS**, conditions `C20`→`C34`.
- [x] ux-ui-designer — un bandeau et deux boutons disparaissent d'un écran, une ligne apparaît
- [ ] embedded-developer · [ ] web-ui-developer
- [ ] test-engineer · [ ] code-reviewer · [x] doc-writer

---

## ⛔ RÈGLE DE NUMÉROTATION DES CONDITIONS — posée le 2026-11-22, applicable à TOUT le projet

**Ce qui l'a provoquée.** La 2ᵉ passe de cette fiche avait posé des conditions `C11`→`C14`
pendant que `feature-168` occupait déjà `C1`→`C17` **sur les mêmes fichiers**. Le cas le plus
dangereux : un commentaire neuf disant « C12 » (le texte du bandeau) se serait trouvé à trois
lignes d'un `feature-168 (C12)` existant qui désigne **le mécanisme que ce lot détruit**.
⚠️ Pire : la fiche mélangeait **deux séries dans une seule phrase** (« C5 MAINTENUE (le seuil
sert encore à l'avertissement C8 et au log d'écriture C7) », où C5 était de feature-170 et C7/C8
de feature-168). ⇒ La liste « C1, C2, C3, C7, C8, C9, C10 inchangées » n'était pas *perdue*,
elle était **indécidable — y compris pour son auteur**.

> **Règle 1 (générale).** ⛔ **Aucun `Cxx` nu hors de la fiche qui le définit.** Toute référence
> à une condition de gate — commentaire de code, spec, message de commit, doc, rapport d'agent —
> porte son identifiant de fiche : **`feature-170 (C29)`**, jamais `C29`. Le `Cxx` nu n'est
> légal **que** dans le corps de la fiche qui l'énonce.
>
> **Règle 2 (spécifique).** **feature-170 numérote ses conditions à partir de `C20`.**
> **bug-201**, quand il redémarrera, **à partir de `C40`** (ses `C0`→`C4` collisionnent elles
> aussi avec `feature-168 (C1)`→`(C4)`).

⭐ **Le renumérotage ne coûte rien ici** : **aucune** condition de feature-170 n'est implémentée
(vérifié — `feature-170` n'apparaît dans **aucun** fichier de `src/`, `data/` ou `test/`). Il ne
touche que cette fiche, qui devait de toute façon être réécrite.

⚠️ **Pourquoi la qualification seule ne suffisait pas** (argument du gate) : elle protège le
lecteur attentif, pas la **recherche**. Un `grep "C12"` sur `src/` et `data/` rendait le drapeau
détruit **et** le texte qui le remplace. Une règle qui dépend de la discipline de celui qui écrit
le prochain commentaire n'est pas une garde, c'est un vœu. Un numéro unique rend le `grep` décisif.

⇒ **Suivi créé** : un contrôle d'hygiène `tools/check_condition_refs.sh` (modèle
`check_fopen_guard.sh`) refusant un `Cxx` nu dans un commentaire de `src/`/`data/` non précédé de
`feature-NNN`/`bug-NNN` sur la même ligne. **Hors périmètre de ce lot** — voir « Suivis ».

---

## Description
Retirer le mécanisme de **confirmation** de la correction de mesure pH : le drapeau
`phOffsetBootUnconfirmed`, la cause de péremption `boot`, la cause `expiration` et ses deux
alertes, le bandeau correspondant et son bouton **« Garder la correction »**.

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
2. **Le seuil 0,50 est HÉRITÉ, pas calculé.** `constants.h:2054-2060` le dit mot pour mot :
   *« kPhOffsetUnusualAbs — l'ANCIENNE borne ±0,50 (feature-067/pool-chemistry), CONSERVÉE »*.
   ⚠️ C'était une borne de **RÉGLAGE** (jusqu'où corriger), recyclée en seuil de **DÉTECTION**
   ⛔ sans être rejustifiée. ADR-0098 exige qu'une constante déclare sa source ; celle-ci
   déclarait un **héritage**, pas une raison. ⇒ **La justification due est fournie par
   `feature-170 (C23)`, et elle change la nature du seuil.**
3. ⛔ **L'amplitude est le mauvais instrument.** Une mémoire altérée produit **0,30** aussi
   facilement que **9,30**. Un seuil attrape les **grandes** valeurs, ⛔ pas les **fausses**.

🔴 **Quatrième défaut, mesuré dans le code et RE-VÉRIFIÉ le 2026-11-22** : le drapeau est relevé
**à CHAQUE démarrage** dès que `|offset| > 0,50` (`src/config.cpp:829-830`), ⛔ **même après
confirmation** — car il n'a **aucune clé NVS** (inventaire exhaustif : `config.h:229`,
`config.cpp:196`, `:254`, `:266`, `:830`, `main.cpp:516` — **cinq sites, zéro
`prefs.putBool`/`getBool`**). ⇒ Confirmer n'éteint rien au-delà du redémarrage suivant.
⚠️ **Conséquence observée** : l'utilisateur, seul concerné parce que sa correction est
légitimement grande (+1,20, cohérente avec un zéro d'électrode à 128 mV), a vu ce message
pendant **un mois sans jamais comprendre ce qu'on lui demandait**. ⭐ Un avertissement que
personne ne comprend n'avertit personne.

## 🔴 Ce que la 3ᵉ passe a établi — le mécanisme surveille **l'inverse** du défaut atteignable

**A4 est tranchée, par lecture de la source ESP-IDF v5.5, ⛔ pas par présomption.** NVS calcule
**un CRC32 par entrée** (`Item::crc32`), vérifié par `Item::checkHeaderConsistency()`
(`nvs_types.cpp:46-52` : `if (crc32 != calculateCrc32()) return false;`), appelée **dans
`Page::findItem()`** (`nvs_page.cpp:954`) — donc **à chaque recherche de clé**, pas seulement au
montage. Les données de longueur variable ont en plus un `dataCrc32` vérifié à chaque lecture
(`nvs_page.cpp:280`). Une entrée dont le CRC ne correspond plus est marquée ERASED, la clé
devient introuvable, et `prefs.getFloat("ph_offset", défaut)` rend **le défaut**.

⇒ ⭐ **La corruption NVS ne produit pas une correction fausse et grande. Elle produit la
DISPARITION de la correction.**

Sur cet appareil (eau réelle 7,30, brut 6,10, correction +1,20), une disparition donne :
- acide **arrêté sur le champ** (6,10 < 6,70) — visible ;
- ⛔ inhibition ORP repoussée à **8,80 réels** ⇒ **le chlore continue de se doser** pendant que la
  fraction de HOCl s'effondre (63,5 % à 7,30 → 12 % à 8,40, pKa 7,5) ;
- et comme l'hypochlorite est basique, la boucle ORP **accélère la dérive qu'elle subit**.

⭐ **Or `|0,00| > 0,50` est faux : le drapeau `boot` ne se lève PAS.** Le mécanisme retiré était
**aveugle au seul mode de défaillance atteignable**, et bavard sur un mode qui n'existe pas.
⇒ Ce n'est plus « il ne protège pas effectivement » : **il protège de l'inverse de ce qui arrive.**
Le retrait est **justifié**, pas seulement toléré.

⛔ **Deux angles morts assumés, à écrire plutôt qu'à taire** (`feature-170 (C34)`) :
une valeur **correctement stockée mais fausse** (écrite par un bug) ; et une correction
**perdue**, indistinguable d'une suppression volontaire. Aucune somme de contrôle maison ne les
attraperait — c'est pourquoi il n'en est pas exigé.

## Comportement actuel
À chaque démarrage, si `|ph_offset| > 0,50` : log `critical` (`config.cpp:832-838`), alerte MQTT
différée (`main.cpp:513-525`), bandeau ambre « Correction à revoir » sur la carte
`#ph-card-tune`, deux boutons. Confirmer éteint le drapeau **jusqu'au redémarrage suivant
seulement**. En parallèle, une alerte périodique `warning` + MQTT sur la cause `expiration`
(`main.cpp:1916-1938`).

⚠️ **Et le bandeau n'est pas atteignable en usage courant** : `#ph-card-tune`
(`data/index.html:1365`) est marquée `.flow-maintenance` et `hidden`, règle CSS
`#view-ph.is-flow-maintenance > :not(.flow-maintenance)` (`data/app.css:4708`) ⇒ visible
**uniquement** en « mode réglage », qui gèle le dosage. **C'est bug-201.** ⚠️ Le commentaire
`data/index.html:1355-1356` dit « Toujours visible » : il est faux.

## Comportement attendu
1. ⛔ **Supprimer** : `phOffsetBootUnconfirmed`, la cause `boot`, la cause `expiration`,
   `isPhOffsetExpired()`, `kPhOffsetConfirmIntervalDays`, le champ servi
   `ph_offset_confirm_interval_days`, les deux bandeaux correspondants, le bouton
   « Garder la correction » dans sa formulation actuelle.
2. **Conserver** le signal de démarrage en **information** (`feature-170 (C24)`) : ⭐ ça
   n'interrompt personne et ça laisse une trace. ⛔ Mais ce n'est plus une **question**.
3. **Conserver `calibration`** — c'est la seule cause qui répond à un **événement** — et lui
   donner enfin un signal hors écran (`feature-170 (C32)`).
4. **Afficher la date de pose en permanence** (`feature-170 (C28)`) : un **fait**, qui ⛔ ne
   s'use pas et ⛔ n'interrompt personne.

### Cas limites
- ⚠️ Un appareil **déjà** en `phOffsetBootUnconfirmed = true` doit repartir proprement.
  ⭐ **Vérifié : rien à migrer** — le drapeau n'a aucune clé NVS (`feature-170 (C27)`).
- La parité `/get-config` ↔ WebSocket doit rester **exacte** sur les champs retirés **et**
  conservés (⛔ 4 récidives documentées — comparer les **expressions**, pas les clés).
- Un firmware **antérieur** peut encore servir `ph_offset_stale_cause = "boot"` ou
  `"expiration"` : le client ne doit **jamais** afficher le texte `calibration` dans ce cas
  (`feature-170 (C33)`).

## Contraintes techniques
- ⛔ **Ne PAS toucher** aux filets bruts absolus `kPhRawFloorAbsolute` (5,50) /
  `kPhRawCeilingAbsolute` (8,50) : justification chimique propre, non concernés.
- ⛔ Ne pas réintroduire de borne sur la correction (feature-168, demande explicite de
  l'utilisateur).
- ⛔ Le mot « sonde » ne doit mettre en cause la sonde dans **aucun** texte dérivé.
- Français dans toute l'UI, sans jargon de code. Vanilla JS. Offline first. 390 px d'abord.

## Fichiers concernés — inventaire VÉRIFIÉ le 2026-11-22
| Fichier | Sites |
|---|---|
| `src/config.h` | `:229` membre `phOffsetBootUnconfirmed` · `:740` commentaire des causes |
| `src/config.cpp` | `:196` extinction · `:254`, `:266` lecture · `:829-838` pose + log · `:228-241` `markPhCalibrationStamped()` (**ajout** `feature-170 (C32)`) |
| `src/ph_offset.h` | `:105-118` `isPhOffsetExpired()` · `:135-143` `isPhOffsetStale()` · `:55-63` mise en garde « pas une suspicion de sonde » (à conserver) |
| `src/constants.h` | `:2054-2060` `kPhOffsetUnusualAbs` · `:2066-2070` `kPhOffsetConfirmIntervalDays` · `:506` commentaire de budget |
| `src/main.cpp` | `:33` `#include "ph_offset.h"` · `:513-525` alerte différée · **`:1916-1938` bloc périodique NON LISTÉ dans les versions précédentes** |
| `src/web_routes_config.cpp` | `:171` décompte de taille · `:311-312` accesseur (⚠️ parité) · `:353` champ servi |
| `src/ws_manager.cpp` | `:1545`, `:1568` décomptes · `:1695-1696` accesseur (⚠️ parité) |
| `data/app.js` | `:2427-2452` repli + garde des seuils · `:2593-2626` `renderPhOffsetStaleBanner` · `:2628-2655` `bindPhOffsetStaleBanner` · `:2657-2672` `renderPhTuneRemoveRow` (masquage mutuel) · `:2721-2722` commentaire jumeau |
| `data/index.html` | `:1355-1356` commentaire faux · `:1371-1392` bandeau et boutons · `:1407` point d'insertion de la date |
| `test/` | `test_native_ph_offset/test_ph_offset.cpp:573-691` (bloc D4bis) · `test_native_ws_config_parity/test_ws_config_parity.cpp:76` (exception à retirer) |
| `docs/` | `MQTT.md` (émetteur de `ph_offset_stale` changé) · `API.md` · `features/page-ph.md` · `subsystems/sensors.md` · `adr/0138-*` (**à créer**) · `CHANGELOG.md` |

---

# Historique du gate `pool-chemistry`

| Passe | Date | Verdict |
|---|---|---|
| 1ʳᵉ | 2026-11-14 | GO SOUS CONDITIONS — ⛔ **texte jamais écrit dans le dépôt, série déclarée NULLE EN BLOC** |
| 2ᵉ | 2026-11-14 | GO pour le retrait d'`expiration` **aussi** |
| 3ᵉ | 2026-11-22 | **GO SOUS CONDITIONS** — conditions `C20`→`C34`, vecteurs `V1`→`V8` |

⚠️ **Pourquoi la 1ʳᵉ série est nulle et non restaurée.** `git log --all` sur cette fiche ne rend
**qu'un seul commit** (`3d9140d`), dont le contenu est **identique** à la version reçue : la
fiche est **née** avec la 2ᵉ passe dedans. ⭐ Et le gate l'a dit lui-même : **il ne garde aucune
trace entre deux passages** — il ne « se souvient » pas de sa 1ʳᵉ passe, il ne pouvait que la
**réémettre sous sa responsabilité d'aujourd'hui**. ⛔ Ce qui suit n'est donc **pas** une
restitution, et ne doit jamais être présenté comme telle.

**Ce qui a provoqué la 2ᵉ passe** — décision utilisateur :
> *« Je n'ai encore rien compris à tes demandes. On supprime ce message ou pas ? »*

⚠️ **C'est le défaut que ce lot corrige, en train de se rejouer** : présenter trois options sur
un mécanisme jamais compris, ⛔ ce n'est pas consulter, c'est reporter la charge.

**Pourquoi `expiration` tombe** (argument du gate, ⛔ pas du confort) : ⭐ la validité d'une
correction ne se dégrade pas avec le **TEMPS**, mais avec des **ÉVÉNEMENTS** (intervention sur la
pompe, modification du circuit électrique, étalonnage, vieillissement de la jonction). ⛔ Aucun
n'est corrélé à un calendrier de 30 jours. 🔴 **Et il a un coût chimique réel** : il **use le
seul canal** qui porte encore une information fondée sur un fait. ⚠️ Une alerte dont on a appris
qu'elle ne veut rien dire **ne se lit plus le jour où elle veut dire quelque chose** — et ce
jour-là, c'est `calibration`.
⭐ **Fait aggravant trouvé en 3ᵉ passe** : le latch est un `static bool` **réinitialisé à chaque
démarrage** ⇒ l'alerte n'arrivait **pas tous les 30 jours, mais à CHAQUE redémarrage**, donc à
chaque mise à jour OTA — **et sous la même clé MQTT `ph_offset_stale` que celle de `boot`**.

---

# Conditions bloquantes — série `C20` → `C34` (autoportante)

> ⛔ **Aucune condition de cette fiche n'existe sous forme de renvoi.** C'est le renvoi
> (« C7 inchangée ») qui a produit la perte de la 1ʳᵉ série.

- [ ] **C20 — Aucune garde de dosage n'est touchée.** Les filets bruts fixes 5,50 / 8,50
      (`constants.h:2027-2028`), le plancher et le plafond corrigés 6,70 / 7,60, l'inhibition ORP
      à 7,60 et le gate propre de `POST /tune-ph-offset` (mesure fraîche, 10 min de filtration,
      ≥ 2 points d'étalonnage) sortent de ce lot **identiques**. Ce retrait n'enlève qu'une
      **question**, ⛔ jamais une barrière — et un vecteur le prouve avant/après.
- [ ] **C21 — `ph_offset_stale` et `ph_offset_stale_cause` SURVIVENT.** *(seule condition
      textuellement attestée dans la fiche d'origine)* Ils restent servis **par appel à
      l'accesseur unique** des deux côtés (`web_routes_config.cpp:311-312`,
      `ws_manager.cpp:1695-1696`). ⛔ Ne pas les supprimer sous prétexte qu'il ne reste qu'une
      cause. ⛔ Ne pas remplacer l'appel par une expression recopiée (**5ᵉ forme du piège de
      parité**).
- [ ] **C22 — Aucun verdict recalculé côté navigateur (ADR-0100).** Le client **lit la clé
      servie** ; ⛔ il ne compare jamais `ph_offset` à un seuil pour décider lui-même qu'une
      correction est caduque.
- [ ] **C23 — `kPhOffsetUnusualAbs` : la justification due est fournie, et elle CHANGE SA
      NATURE.** `constexpr float kPhOffsetUnusualAbs = kPhSafetyMargin;` — **dérivée**
      (ADR-0098), **valeur inchangée à 0,50**.
      ⭐ **Ce n'est pas un seuil d'amplitude, c'est un seuil de CONSÉQUENCE** : à la consigne
      basse `kPhTargetMin` (7,20), une correction `o` qui **disparaît** fait lire `7,20 − o` ;
      l'appareil cesse d'acidifier dès que `7,20 − o < kPhFloorCorrected` (6,70), soit
      **`o > kPhSafetyMargin = 0,50`**. ⇒ Au-delà, et seulement au-delà, la correction est
      devenue **une pièce du dosage**.
      Réécrire les commentaires `constants.h:2054-2060` **et** `data/app.js:2721-2722` :
      ⛔ supprimer « l'ANCIENNE borne … CONSERVÉE » (l'héritage mis en cause par l'utilisateur)
      et ⛔ supprimer « SIGNAL qu'un défaut matériel est possible » (désignation d'un coupable
      non identifié — travers du 25/08/2026, `src/ph_offset.h:55-63`).
      ⛔ **NE PAS toucher au texte visible de `feature-168 (C8)`** (`data/app.js:2963-3015`) :
      il est juste et ne blâme rien. ⚠️ En particulier sa dernière phrase — *« Revérifiez par un
      test manuel après tout étalonnage, toute intervention sur la pompe de filtration, et au
      moins une fois par mois »* — **doit SURVIVRE** : en retirant `expiration`, elle devient le
      **seul** porteur du conseil périodique. ⛔ Ne pas la supprimer « par cohérence ».
      ⛔ **Ne PAS renommer** `kPhOffsetUnusualAbs` malgré son nom devenu inexact : la clé servie
      `ph_offset_unusual_abs` est lue par le client et par le harnais de parité. Corriger le
      **commentaire**, pas le nom.

      ⚠️ **PRÉCISION DU GATE (2026-11-22) — la dérivation est celle du CHEMIN ACIDE.**
      `7,20 − o < 6,70 ⇒ o > 0,50` vaut pour l'acidification. ⛔ Le symétrique côté
      alcalinisation n'est **pas** 0,50 mais `kPhHighMargin = 0,10` (`constants.h:1971`,
      asymétrie assumée et documentée). ⇒ Les textes dérivés **nomment l'acidification** — comme
      le faisait déjà celui de `feature-168 (C7)`, qui cite `kPhRawFloorAbsolute`. ⛔ **Ne pas
      UNIVERSALISER** la justification : la variante `ph_plus` se traitera le jour où ce produit
      est déclaré, et elle vivra dans `feature-168 (C8)`, qui porte déjà deux variantes.
      ⛔ **Aucune seconde variante de texte dans ce lot.**

      🔴 **`feature-168 (C7)` EST RÉÉCRITE PAR CETTE CONDITION** (arbitrage Q13 du 2026-11-22).
      ⚠️ **Motif dirimant** : c'est `C23` **elle-même** qui a créé la contradiction. Le texte
      **visible** de `feature-168 (C7)` (`src/config.cpp`, log `critical` + alerte MQTT
      `ph_offset_unusual`, déclenché quand l'utilisateur **pose** une correction au-delà du
      seuil) disait encore :
      > « Correction de mesure **inhabituelle** : +1,00 pH (au-delà de 0,50, **ancienne borne de
      > l'appareil**). … »

      ⛔ C'est **mot pour mot la formule que l'utilisateur a contestée** (*« 0.5 comme seuil est
      totalement arbitraire »*). Lui répondre « ancienne borne » **après** ce lot, c'est lui
      **confirmer l'arbitraire qu'on vient de lever**. ⭐ *Un lot qui redéfinit une constante et
      laisse debout le texte visible qui énonce l'ancienne définition n'a pas fini son travail :
      il a déplacé le défaut du code vers l'utilisateur.*
      ⇒ **Même déclencheur, justification nouvelle.** Texte imposé mot pour mot :
      > Correction +1,00 : le dosage en dépend. Au-delà de 0,50, la retirer arrêterait l'acidification sur le champ. Rien ne la limite : elle est appliquée telle quelle. Le refus d'acidifier sur la mesure brute reste fixé à 5,50, quelle que soit la correction.

      - **Tête MESURÉE : 39 octets** pour `+1,00`, **40** pour `+10,50` (⚠️ cette fiche a dit
        40/41 — corrigé par mesure le 2026-11-22) ⇒ ⭐ Home Assistant reçoit **la valeur ET le
        verdict entiers**, avec **6 octets de marge**. ⚠️ **Défaut préexistant corrigé au passage** :
        l'ancien texte perdait le verdict dans la troncature.
      - ⛔ Disparaissent : « inhabituelle », « ancienne borne de l'appareil ». ⛔ Aucun « sonde ».
      - **Survivent intactes** les deux affirmations de `feature-168 (C7)` qui restent vraies :
        *rien ne la limite, elle est appliquée telle quelle* et *le refus d'acidifier sur la
        mesure brute reste fixé à 5,50*.
      - Les deux seuils (`0,50`, `5,50`) restent **interpolés** depuis les constantes, virgule
        décimale — ⛔ jamais en dur.
      - **Niveau inchangé** : `critical` **+** `publishAlert`. ⚠️ Le doublon interdit par
        `C32`/F1 ⛔ **ne s'applique pas ici** : ce sont **deux niveaux différents**, usage établi.
      - ⚠️ **Deux canaux, deux formes** (ADR-0139) : le **journal** garde le préfixe `[pH]` de
        toutes les lignes pH du projet ; l'**alerte MQTT** s'en passe — le type
        `ph_offset_unusual` nomme déjà la chose, et ces 5 octets se prendraient sur le verdict.
- [ ] **C24 — Le journal de démarrage, et lui seul.** ⛔ Aucune alerte MQTT. ⛔ Aucune date
      (`loadMqttConfig()` s'exécute **avant** `applyTimeConfig()`, `main.cpp:527` — l'horloge
      n'est pas synchronisée, toute date y serait fausse ou 1970 ; la date vit dans `C28`).
      ⚠️ La méthode est `systemLogger.warning(...)`, **pas** `warn`.
      Textes imposés — un **constat** suivi de sa **conséquence**, ⛔ aucune question, ⛔ les mots
      « confirmez », « corrigez-la », « mémoire altérée » et « sonde » bannis :
      - `|offset| > kPhOffsetUnusualAbs` → **`warning`** :
        > `[pH] Correction de mesure appliquée au démarrage : +1,20 pH. Elle dépasse 0,50 : si elle venait à disparaître, la mesure corrigée tomberait sous 6,70 à la consigne et l'appareil cesserait d'ajouter de l'acide.`
      - `0 < |offset| <= kPhOffsetUnusualAbs` → **`info`**, une ligne :
        > `[pH] Correction de mesure appliquée au démarrage : +0,30 pH.`
      - `offset == 0` → **rien**.
      (Seuils **interpolés** depuis les constantes, virgule décimale, comme le code voisin.)
- [ ] **C25 — Retrait COMPLET du drapeau et de la cause `boot`.** Les cinq sites
      (`config.h:229`, `config.cpp:196`, `:254`, `:266`, `:830`) **et** le bloc d'alerte différée
      `main.cpp:513-525`. ⛔ **Pas de drapeau laissé « inerte »** : le projet traîne déjà
      `screen_enabled`, on n'en ajoute pas un second.
- [ ] **C26 — Retrait COMPLET d'`expiration`, troisième site compris.** `isPhOffsetExpired()`,
      `kPhOffsetConfirmIntervalDays`, le champ servi, **et le bloc périodique
      `main.cpp:1916-1938`** que les versions précédentes de cette fiche ne listaient pas (sans
      quoi la compilation casse), avec l'`#include` `main.cpp:33` et le commentaire `:1920-1923`
      — ⚠️ **vérifié faux** : il affirme que `boot` **et `calibration`** ont « CHACUNE déjà leur
      propre alerte dédiée », alors que `markPhCalibrationStamped()` n'émet **aucun**
      `publishAlert` (voir `C32`).
- [ ] **C27 — Aucune migration, et c'est VÉRIFIÉ, pas supposé.** Le drapeau n'a **aucune clé
      NVS** ; `ph_offset_ep` / `ph_offset_sq` / `ph_cal_ep` / `ph_cal_sq` restent lus et écrits à
      l'identique.
- [ ] **C28 — La date, affichée en permanence, sans bandeau.** ⭐ Principe du projet : *les
      plafonds protègent, le reste informe* — ici, rien à protéger.
      Libellés exacts — ⚠️ **amendés le 2026-11-22 (arbitrage G2) : l'unité « pH » est AJOUTÉE**,
      par cohérence avec l'étiquette absorbée et avec les textes de `feature-168 (C8)` :
      - correction en vigueur, date connue → **« Correction +1,20 pH — posée le 16/10/2026 »**
      - `ph_offset_epoch === 0` → **« Correction +1,20 pH — date de pose inconnue »**
        (⛔ jamais 1970 ; patron `ph_auto_manual_epoch`)
      - `ph_offset === 0` ou non fini → **rien** (⛔ jamais « Correction +0,00 »)
      - ⛔ rien avant le premier `/get-config`.
      **Où** : dans `#ph-card-tune`, en tête de `card__body`, **avant** `#ph_tune_stakes`
      (`data/index.html:1407`). ⛔ **Ce lot ne touche NI `hidden`, NI `.flow-maintenance`, NI
      `MAINTENANCE_PAGES.ph.cardIds`.**
      🔴 **FUSION IMPOSÉE (arbitrage G1 du 2026-11-22) — cette ligne en ABSORBE une autre.**
      L'écran porte **déjà** le même fait : `data/index.html:1480-1482`,
      `#ph_tune_remove_label` → « Correction actuelle : **—** pH », aujourd'hui **masqué** par le
      masquage mutuel de la bannière. ⇒ Une fois la bannière éteinte, il se **dévoilerait** à
      quelques pixels de cette ligne. **Deux formulations du même fait sur un écran de 390 px** —
      exactement le défaut que `C28` devait éviter.
      ⇒ **Supprimer `#ph_tune_remove_label` et `#ph_tune_remove_value`**, retargeter
      l'`aria-describedby` de `#ph_tune_remove_btn` (`:1493`) sur la nouvelle ligne +
      `#ph_tune_remove_hint`. ⚠️ Vérifié : le marqueur « texte imposé mot pour mot (feature-157) »
      de `:1489-1491` couvre **le libellé du bouton**, ⛔ pas cette étiquette.

      ⛔ **DEUX PIÈGES DE LA FUSION, à ne pas rater :**
      1. 🔴 **`renderPhTuneRemoveRow()` SORT sur l'élément supprimé.** `data/app.js:2658-2662` :
         `const label = $("#ph_tune_remove_label"); … if (!label || !btn || !hint) return;`
         ⇒ Supprimer l'élément **sans reprendre cette garde** fait sortir la fonction avant tout,
         et **« Supprimer la correction » ne s'afficherait PLUS JAMAIS**. Retirer `label` de la
         garde, et supprimer `valueEl` (`:2671`) avec.
      2. ⚠️ **La fusion corrige une incohérence de décimales — ⛔ ne pas la recréer.** L'étiquette
         rendait `_fmtPh(offset, true)` → « +1,2 » (**1** décimale, `app.js:2671`) pendant que la
         bannière rend `_fmtPhOffset2` → « +1,20 ». La ligne fusionnée porte **`_fmtPhOffset2`,
         2 décimales signées** — seule convention de la correction sur cette carte.

      🔴 **Dette de visibilité, ÉCRITE et non tue** : tant que **bug-201** n'est pas livré,
      `C28` **n'est pas visible en usage courant**. ⇒ feature-170 ne délivre **pas encore** la
      promesse « un fait visible en continu » ; le seul signal permanent dans l'intervalle est le
      journal de démarrage (`C24`).
      ⚠️ **Motif chimique du refus de franchir cette frontière** (gate, mot pour mot) : rendre la
      carte permanente exposerait « Annuler la correction » et « Supprimer la correction » **en
      un clic, sans aucun garde-fou**, sur un appareil où ce clic déclenche le scénario chiffré
      plus haut. **bug-201 paie cette friction dans le même lot** ; les séparer livrerait
      l'exposition **sans** la contrepartie.
- [ ] **C29 — Le texte du bandeau `calibration` : QUATRE rendus imposés mot pour mot.**
      ⛔ La formule *« elle compense un état de la sonde qui n'existe plus »* est **BANNIE** (gate
      bug-201 du 2026-11-21 : la correction ne corrige pas un état de la **sonde** mais un
      décalage de **MESURE** — courants de fuite de la pompe de filtration).
      ⚠️ **Preuve à l'écran que la formule actuelle est fausse** : le bandeau dit « …un état de
      la sonde qui n'existe plus » et l'encadré **juste en dessous** dit « L'écart vient de
      l'installation, **pas de la sonde** ». Deux paragraphes qui se touchent se contredisent.
      ⚠️ Le verdict d'ordre vient des **séquences** (`phOffsetSeq`/`phCalSeq`,
      `ph_offset.h:130-133`) — ⛔ **jamais des `epoch`**, qui ne servent qu'à l'affichage : une
      date manquante ⛔ ne masque jamais la bannière et ⛔ ne change jamais le verdict.
      Fragments interpolés (`toLocaleDateString('fr-FR')`, ⛔ pas `toLocaleString` : l'heure
      n'apporte rien entre deux événements distants de semaines, et on est à 390 px) :
      date connue → `le 16/10/2026` · `epoch === 0` →
      `à une date que l'appareil n'a pas pu enregistrer (son horloge n'était pas à l'heure)`.

      **(a) Les deux dates connues**
      > L'étalonnage a changé le 14/11/2026. La correction en vigueur (+1,20) a été posée le 16/10/2026, donc avant : ce changement a déplacé la mesure sur laquelle elle avait été calculée. Refaites un test manuel et accordez la mesure dessus, ou remettez la correction à zéro.

      **(b) `ph_cal_epoch === 0`, date de pose connue**
      > L'étalonnage a changé à une date que l'appareil n'a pas pu enregistrer (son horloge n'était pas à l'heure). La correction en vigueur (+1,20) a été posée le 16/10/2026, donc avant : ce changement a déplacé la mesure sur laquelle elle avait été calculée. Refaites un test manuel et accordez la mesure dessus, ou remettez la correction à zéro.

      **(c) `ph_offset_epoch === 0`, date d'étalonnage connue**
      > L'étalonnage a changé le 14/11/2026. La correction en vigueur (+1,20) a été posée avant, à une date que l'appareil n'a pas pu enregistrer (son horloge n'était pas à l'heure) : ce changement a déplacé la mesure sur laquelle elle avait été calculée. Refaites un test manuel et accordez la mesure dessus, ou remettez la correction à zéro.

      **(d) Les deux à 0** — ⛔ rendu **DÉDIÉ** : ne pas enchaîner deux fois le fragment
      (« donc avant » sans aucune date ne veut rien dire)
      > L'étalonnage a changé après la pose de la correction en vigueur (+1,20) — l'appareil n'a pu enregistrer aucune des deux dates, son horloge n'étant pas à l'heure. Ce changement a déplacé la mesure sur laquelle elle avait été calculée. Refaites un test manuel et accordez la mesure dessus, ou remettez la correction à zéro.

      **DÉCOUPE TYPOGRAPHIQUE IMPOSÉE (arbitrage G3 du 2026-11-22) — ⛔ aucun mot ne bouge.**
      Chaque rendu fait 230 à 300 caractères, soit ~9 lignes à 390 px.
      ⛔ **AUCUN repliable (`<details>`)**, ni ici ni sur le texte neutre de `C33c` : *replier la
      raison ou l'acte, c'est les supprimer pour qui ne lit pas — et ne pas lire est précisément
      le problème qu'on corrige.*
      ⇒ **Deux paragraphes** : `P1` = le **fait** (phrases 1 et 2), `P2` = l'**acte**
      (« Refaites un test manuel et accordez la mesure dessus, ou remettez la correction à
      zéro. »), 8 px entre eux, `P2` juste au-dessus des boutons. Rendu (d) : coupure au même
      endroit, avant « Refaites ». Texte neutre de `C33c` : `P1` = « L'appareil signale cette
      correction comme à revérifier. »
      ⚠️ Le JS construit **deux constantes** `fait` / `acte`, ⛔ jamais une chaîne découpée après coup.
      ⛔ Les deux `<p>` restent **dans le même conteneur `aria-live`** (`#ph_tune_stale_banner`,
      `role="status"`, `index.html:1376`) — ⛔ pas deux régions annoncées séparément.
      ⛔ `P2` ne doit **jamais** pouvoir s'afficher **sans** `P1` (pas de troncature, pas de
      hauteur maximale) : le fait justifie l'acte ; **l'acte seul est un ordre sans raison**.

      Règles attachées : valeur formatée par `_fmtPhOffset2` (2 décimales signées,
      `data/app.js:2572`) ⛔ jamais `_fmtPh` · `try/catch` autour de la conversion de date, repli
      sur le fragment « date inconnue » (patron `renderPhAutoManualBanner`, `app.js:9253-9258`)
      · ⛔ le mot « sonde » n'apparaît dans **aucun** des quatre.
- [ ] **C30 — Nommer l'ACTE sur les boutons.** **« Je l'ai vérifiée — garder »** /
      **« Remettre à zéro »**. ⛔ « Garder la correction » ne dit pas ce que le clic fait.
      **Mesuré le 2026-11-22** : 25 et 15 caractères ; le libellé long **tient sur une ligne dès
      320 px**. ⚠️ Les boutons sont empilés par une **règle CSS** à ≤ 768 px
      (`.detail-actions`, `app.css:3633`), ⛔ pas par manque de place — rien à raccourcir.
      **Ordre et poids imposés** : « Je l'ai vérifiée — garder » **en premier**, `btn--secondary`
      (⛔ pas `btn--primary` : il ne doit pas **inviter** à valider) ; **« Remettre à zéro » en
      SECOND**, `btn--danger action-destructive`.
      ⚠️ **Motif, chiffré par le gate** : *annuler coûte **un clic sans aucun contrôle** ;
      remettre exige un test au réactif, dix minutes de filtration et une sonde étalonnée.*
      ⇒ Un geste destructif ⛔ ne doit pas être le premier sous le pouce.
      ⛔ Aucune confirmation (verdict du gate) : « Je l'ai vérifiée » **est** la déclaration.
      ⚠️ Les **identifiants** `#ph_tune_stale_keep_btn` / `#ph_tune_stale_cancel_btn` ne changent
      pas ; le toast de `app.js:2648` doit rester cohérent avec le nouveau libellé.
      ⛔ **Rappel de périmètre** : tant que la carte reste derrière le flux maintenance, ce bouton
      ne porte **aucune friction** — c'est **bug-201 (`C40`…)** qui doit la poser, ⛔ pas ce lot.
- [ ] **C31 — Le bandeau doit se voir comme NEUF.** Il hérite de la boîte que l'utilisateur a
      appris, en un mois, à ignorer. ⛔ Ne pas inventer de 3ᵉ couleur (daltonisme) — le chapeau
      `.callout__eyebrow` existe et porte le sens par le **mot**. Captures avant/après à 390 px.
      **Forme retenue par l'étude d'ergonomie du 2026-11-22, approuvée par le gate** :
      - chapeau `.callout__eyebrow callout__eyebrow--danger` portant le mot **« Étalonnage
        modifié »** — il nomme l'**événement déclencheur**, ce que l'ancien bandeau ne faisait
        pas (il se lisait donc comme un état permanent). ⚠️ La variante `--danger` **existe
        déjà** (`data/app.css:4607`), employée par « ⚠ Impasse possible » : ⛔ **aucune couleur
        nouvelle**, la boîte reste `callout--warn`.
      - titre **« Correction à vérifier »** — ⛔ « Correction à revoir » est la formule que
        l'utilisateur a appris à ignorer en un mois. ⭐ Le titre et le bouton de `C30` nomment
        désormais **le même acte**.
      - ⚠️ **Contraste à MESURER** (≥ 4,5:1) du chapeau rouge sur le fond ambre, **et en mode
        sombre**. S'il manque, retirer `--danger` : **le mot suffit**.
      - ⛔ Pas d'`aria-live` ajouté : le bandeau est re-rendu à chaque `loadConfig()` et serait
        ré-annoncé à chaque fois.
- [ ] **C32 — La cause survivante reçoit enfin un signal HORS ÉCRAN.** 🔴 Inventaire complet des
      `publishAlert("ph…")` : `ph_injection_aborted`, `ph_low_warning`, `ph_guard`,
      `ph_guard_scheduled`, `ph_integral_saturated`, `ph_daily_limit_streak`,
      `ph_offset_unusual`, `ph_offset_stale` (×2, **les deux retirés par ce lot**), `ph_limit`,
      `ph_impasse`, `ph_abnormal` — ⛔ **rien** n'est émis par `markPhCalibrationStamped()`
      (`config.cpp:228-241`, qui ne fait qu'un `systemLogger.info`).
      ⇒ Sans cette condition, le lot laisserait **la seule cause qui protège réellement** sans
      aucun canal, enfermée dans une carte que bug-201 démontre inatteignable.
      **Bilan MQTT du lot : deux émetteurs retirés, un ajouté** (clé `ph_offset_stale`
      **libérée** par `C25` et `C26`). ⛔ Aucun topic neuf, aucune entrée d'auto-découverte.

      🔴 **C32 AMENDÉE le 2026-11-22 sur TROIS points, chacun appuyé sur un fait MESURÉ dans le
      code** (arbitrages Q10/Q11/Q12 — la rédaction d'origine était fautive sur les trois).

      **F1 — `publishAlert()` journalise DÉJÀ.** `src/mqtt_manager.cpp:929` :
      `systemLogger.warning("Alerte: " + alertType + " - " + message);` — ⭐ et il journalise
      **`message` ENTIER**, ⛔ pas le `msg` tronqué (`:907`, `:926`). ⇒ Exiger un
      `systemLogger.warning` **en plus** produirait **deux lignes identiques au même niveau**.
      ⚠️ `ph_offset_unusual` (`config.cpp:216-217`) n'est pas un contre-exemple : `critical` +
      alerte = deux **niveaux différents**.
      ⇒ **UN SEUL APPEL : `mqttManager.publishAlert("ph_offset_stale", …)`.** ⛔ Pas de
      `systemLogger.warning` adjacent. Perte du préfixe `[pH]` **assumée** — le type nomme déjà
      la chose.
      ⇒ **À écrire en doc** (`docs/subsystems/mqtt-manager.md` ou `logger.md`) : *une alerte
      publiée produit sa propre ligne de journal au format `Alerte: <type> - <message>` — ⛔ ne
      jamais la doubler d'un log de même niveau.*

      **F2 — Home Assistant ne verra jamais plus de ~43 CARACTÈRES.** `kMaxPayloadLen = 128`
      (`mqtt_manager.h:407`), budget 127 (`mqtt_manager.cpp:906`) ; l'enveloppe
      `{"type":"ph_offset_stale","message":"…","timestamp":<millis>}` pèse **~62 o** et la queue
      `"... (voir journal)"` **19 o** ⇒ **~46 OCTETS utiles**, soit ~43 caractères français
      (⚠️ `à` et `é` pèsent **2 octets**).
      ⇒ ⛔ **Le firmware NE MIROITE PAS les quatre rendus de `C29`.** Argument décisif, ⛔ et il
      n'est pas budgétaire : ⭐ **le navigateur rend un ÉTAT, le firmware notifie un ÉVÉNEMENT.**
      Les quatre variantes existent parce que le bandeau est lu *des semaines après* et doit
      **situer deux événements l'un par rapport à l'autre**. L'alerte part **à l'instant même** :
      *« vient de changer »* est **plus exact** qu'une date, et la variance disparaît par
      construction. ⚠️ Le cas (d) **est** atteignable côté firmware (pas de NTP + pile RTC morte
      ⇒ `phCalEpoch = 0`, garde `config.cpp:233`) : il n'est pas « impossible », il est **sans
      objet**, le firmware n'ayant aucune date à citer.
      **Alternative écartée** : miroiter (a)/(b)/(c)/(d) en C++ — deux chemins de formatage de
      date qu'⛔ **aucun test ne peut mettre d'accord** (`toLocaleDateString('fr-FR')` vs
      `strftime`), ~800 o de littéraux dupliqués en flash, **pour un résultat invisible sur MQTT**.

      **TEXTE UNIQUE IMPOSÉ** (⚠️ tête raccourcie par le gate : ma première proposition faisait
      **46 octets PILE** sur un budget de ~46 — zéro marge, et `+10,50` aurait coupé **dans
      « vérifier »**, c'est-à-dire dans le **verdict**) :
      > Correction de mesure +1,20 à vérifier : l'étalonnage vient de changer, après sa pose — il a déplacé la mesure sur laquelle elle avait été calculée. Refaites un test manuel et accordez la mesure dessus, ou remettez la correction à zéro.

      Tête = **41 octets** ⇒ ~5 octets de marge, `+10,50` passe encore (« à vérifier » finit à
      l'octet **39/40**, mesuré). 🔴 **La MARGE doit être assertée, pas seulement le texte** : à
      +6 octets de tête, « à vérifier » finit **PILE** à l'octet 46 et passerait une assertion
      naïve. ⇒ `V6a` exige **au moins 4 octets de marge** (ajout du testeur, ⛔ pas dans la
      rédaction d'origine). ⭐ **Le verdict ET la
      valeur survivent à la troncature** (front-chargement volontaire). « pH » est porté par le
      type `ph_offset_stale` ; ⛔ c'est **« de mesure »** qui distingue la correction de
      l'étalonnage, pas « pH ». Phrase d'acte **mot pour mot** celle de `C29`. « à vérifier »
      accordé au titre de `C31`. ⛔ Aucune occurrence de « sonde ».

      **F3 — la rédaction d'origine faisait lire `mqttCfg` HORS MUTEX.** Émettre « après avoir
      rendu `configMutex` » tout en testant `mqttCfg.phOffset` **à cet endroit** lit la config
      sans le verrou qui la protège (interdit par CLAUDE.md).
      ⇒ **Exigé** : prendre **TROIS instantanés DANS la section critique** — `phOffset`,
      `phOffsetSeq`, `phCalSeq` — **puis** rendre le mutex, **puis** décider et émettre.
      ⛔ **Et la décision n'est PAS `offset != 0`** : c'est
      `isPhOffsetSupersededByCalibration(offsetSnap, offsetSeqSnap, calSeqSnap)`
      (`ph_offset.h:130-133`). ⚠️ Même si l'équivalence est vraie par construction après
      l'estampillage, écrire `offset != 0` recréerait **le verdict en deux expressions** que ce
      fichier déclare avoir éliminé (`:121-129`).
      ⇒ ⛔ **AUCUNE fonction nouvelle dans `ph_offset.h`**, **et** ⛔ aucun raccourci qui en
      tienne lieu. Front montant garanti par construction (`calSeq = max+1 > offsetSeq`).
- [ ] **C33 — Le client et l'ORDRE D'ENVOI.**
      **(a)** Retirer `confirmIntervalDays` **du repli `PH_OFFSET_THRESHOLDS_FALLBACK`
      (`app.js:2432`) ET de la garde (`:2445`, `:2447`) dans le MÊME lot.** 🔴 Sans quoi un
      retrait côté firmware ferait **basculer les six seuils en bloc** sur des copies client —
      une violation d'ADR-0100 **déclenchée par un retrait**.
      **(b)** Retirer l'exception `test/test_native_ws_config_parity/test_ws_config_parity.cpp:76`
      (sinon exception orpheline) et le décompte de taille `web_routes_config.cpp:171`.
      **(c)** ⛔ **Cause héritée** : un firmware antérieur (ou un retour arrière) peut encore
      envoyer `"boot"` / `"expiration"`. Le `else` actuel (`app.js:2616-2621`) afficherait alors
      le texte `calibration` — **une affirmation fausse** (« L'étalonnage a changé le … »
      alors qu'il ne s'est rien passé). Exigé : `"calibration"` **et `undefined`** →
      texte `C29` (compatibilité ascendante préservée) ; `"boot"` / `"expiration"` → **texte
      neutre imposé** :
      > « L'appareil signale cette correction comme à revérifier. Refaites un test manuel et accordez la mesure dessus, ou remettez la correction à zéro. »

      ⛔ Même découpe en deux paragraphes que `C29`, et ⛔ **aucun repliable**.

      **(d)** **Ordre d'envoi OTA : FIRMWARE D'ABORD, puis le système de fichiers.** Motif
      vérifié : les cinq seuils du repli client (`app.js:2427-2431` : 0,50 / 1,20 / −0,90 /
      5,50 / 8,50) sont **numériquement identiques** aux constantes — la fenêtre « firmware neuf
      / client ancien » est donc **sans effet visible**, alors que l'ordre inverse ferait
      afficher un texte faux (point c). ⚠️ **Corollaire de `C23`** : la valeur de
      `kPhOffsetUnusualAbs` ⛔ **ne doit pas changer dans ce lot**, sans quoi cette garantie tombe.
- [x] **C34 — A4 close PAR ÉCRIT, et la fiche devient autoportante.**
      1. **ADR-0138** (prochain numéro libre — revérifier au moment d'écrire) :
         *« L'intégrité d'une valeur persistée est déléguée à NVS, jamais à l'utilisateur »*.
         Contenu minimal : CRC32 par entrée vérifié à **chaque `findItem()`** ; conséquence =
         une entrée corrompue **disparaît** et rend le défaut ; **les deux angles morts
         assumés** ; et la règle : ⛔ *une confirmation humaine n'est pas un contrôle
         d'intégrité — elle établit une intention, pas un état de la mémoire*.
      2. `src/constants.h`, bloc `kPhOffsetUnusualAbs` : « ⛔ ce seuil n'est PAS un détecteur
         d'intégrité (ADR-0138) » — c'est la phrase qui empêche la réinvention.
      3. `src/config.cpp`, à la place du bloc `:816-839` : deux lignes disant que la relecture ne
         **vérifie** plus rien (NVS s'en charge) et ne fait plus que **constater** (`C24`).
      4. Cette fiche : série `C20`→`C34` complète, ⛔ **aucune condition sous forme de renvoi**.
      5. **ADR-0139** (prochain numéro libre après 0138 — revérifier au moment d'écrire) :
         *« Le firmware notifie un ÉVÉNEMENT, l'écran rend un ÉTAT — ils ne partagent que
         l'acte »*. Contenu : l'alternative miroir écartée ; **le budget de 46 octets MESURÉ**
         comme contrainte de rédaction ; la sentinelle de parité `.cpp` ↔ `.js` (**première du
         genre dans ce projet**) ; et la règle : ⭐ *sur un canal borné, le verdict et la valeur
         se placent EN TÊTE — l'ordre des mots est une contrainte, pas un style.*
      6. **ADR-0017 — AMENDEMENT** (⛔ pas un ADR neuf ; idiome du projet, cf. l'amendement
         d'ADR-0099 du 2026-10-11) : deux règles de frontière —
         ⭐ *on extrait ce qui VARIE, pas la condition d'appel* ;
         ⭐ *un vecteur natif ne prouve jamais un SITE D'APPEL — ça, c'est le travail d'une
         sentinelle.*

---

# Vecteurs exigés — `V1` → `V8`

- [ ] **V1** — correction +1,20, `offsetEpoch = now − 400 j`, `offsetSeq > calSeq` ⇒ **aucune
      cause** ne se lève. **Mutation : réintroduire un test d'âge ⇒ V1 meurt.**
- [ ] **V2** — `offset = 1,20`, `offsetSeq = 5`, `calSeq = 7` ⇒ `isPhOffsetSupersededByCalibration`
      **true**, cause servie = **`"calibration"`**. *Mutation : `return false` ⇒ V2 meurt.*
- [ ] **V3** — `offset = 1,20`, `offsetSeq = 7`, `calSeq = 5` (correction postérieure) ⇒ **false**.
      *Mutation : inverser la comparaison ⇒ V3 meurt.*
- [ ] **V4** — `offset = 0,00`, `offsetSeq = 1`, `calSeq = 5` ⇒ **false** (une correction nulle
      n'est jamais caduque). *Mutation : retirer la garde `offset != 0` ⇒ V4 meurt.*
- [ ] **V5** — `offsetSeq == calSeq == 0` (premier démarrage) ⇒ **false**.
      *Mutation : `<` → `<=` ⇒ V5 meurt.*
- [ ] **V6a** (`C32`, **natif**) — ⚠️ **V6 a été SCINDÉE le 2026-11-22**, pour une raison qui
      vaut au-delà de ce lot : ⭐ **un vecteur natif ne prouve JAMAIS un site d'appel.**
      `V6a` prouve le **TEXTE** produit par le module pur neuf `src/ph_stale_message.h`
      (patron `src/auth_messages.h` de bug-200 et `src/ota_refusal_messages.h` de feature-151 :
      header-only, `snprintf` dans un buffer fourni, ⛔ zéro `String`, zéro Arduino).
      Suite `test/test_native_ph_stale_message/`. Assertions : format `+1,20` / `-0,30` (⚠️ **le firmware écrit un `-` ASCII** — `%+.2f` — là où l'écran
      écrit un `−` U+2212 via `_fmtPhOffset2` : **deux conventions assumées**, chacune
      verrouillée par son propre vecteur)
      (⚠️ 2 décimales **signées**, virgule décimale), ⛔ absence du mot « sonde »,
      non-dépassement sur buffer court, **et — exigence du gate — la MARGE** : pour `+1,20`
      **et** `+10,50`, les **46 premiers octets** contiennent « à vérifier » **en entier**.
      *Mutations qui la tuent* : `%+.2f` → `%.2f` · oublier `replace('.', ',')` ·
      **rallonger la tête d'un mot** · réintroduire « sonde ».
      ⚠️ Sans l'assertion de marge, **la marge se perdra à la première retouche**.
- [ ] **V6b** (`C32`, **sentinelle** `tools/check_ph_stale_alert_emitted.sh`) — prouve le **SITE
      D'APPEL**, que `V6a` ne peut pas atteindre. ⛔ Sans elle, `C32` reposerait sur
      « quelqu'un appelle bien ceci, au bon endroit » — **c'est-à-dire sur rien**.
      Elle vérifie **TROIS** choses : (1) l'appel `publishAlert("ph_offset_stale"` existe dans le
      corps de `markPhCalibrationStamped()` ; (2) il est **APRÈS** le
      `xSemaphoreGiveRecursive(configMutex)` ; (3) ⛔ **aucune lecture de `mqttCfg` ne subsiste
      après lui** (c'est `C32`/F3 rendue opposable).
      *Mutations qui la tuent* : supprimer l'émission · la remonter avant la libération du mutex ·
      réintroduire une lecture de `mqttCfg` après.
- [ ] **V6c** (sentinelle `tools/check_ph_stale_text_parity.sh`) — 🔴 **aucun outil du projet
      n'attrape aujourd'hui une divergence de TEXTE entre un `.cpp` et un `.js`** (vérifié : les
      harnais ADR-0099 extraient du **JS vivant** évalué sous Node, ils ne lisent pas de C++ ; les
      marqueurs `PARITE-DATA` comparent deux `.cpp` et des **jeux de clés**, pas des littéraux).
      ⭐ Avec le texte unique de `C32`, il ne reste **qu'UNE** phrase partagée — l'**acte** — et
      `C29` impose déjà au JS d'en faire une constante : **un seul point d'ancrage de chaque
      côté**. La sentinelle extrait la phrase entre ancres
      `// >>> PH-STALE-ACTE:DEBUT` / `FIN` dans `data/app.js` **et** dans
      `src/ph_stale_message.h`, normalise la syntaxe de chaîne (guillemets, `+` JS, concaténation
      implicite C, retours à la ligne) et exige l'**égalité OCTET POUR OCTET**.
      ⛔ **Exigence du gate** : **la disparition d'une ancre doit FAIRE ÉCHOUER la sentinelle**,
      jamais la faire passer — *une sentinelle qui ne trouve plus ce qu'elle garde et rend 0 est
      pire que pas de sentinelle : c'est une garantie fausse.*
      ⚠️ Patron `check_fopen_guard.sh` (⚠️ `grep -H` obligatoire, même piège).
      *Mutation : altérer un caractère de l'acte côté `.js` ⇒ rouge.*
- [ ] **CI** — ⚠️ **deux jobs** dans `.github/workflows/hygiene-specs.yml` pour `V6b` et `V6c` :
      *une sentinelle que personne ne lance ne protège de rien* (précédent
      `check_ph_tune_vectors.sh`).
- [ ] **V7** (`C33a`) — `_applyPhOffsetThresholds` avec une config **sans**
      `ph_offset_confirm_interval_days` et les cinq autres champs présents ⇒ les **cinq seuils
      servis sont adoptés** (⛔ pas de repli en bloc).
      *Mutation : laisser `isFinite(ci) && ci > 0` dans la garde ⇒ V7 meurt.*
- [ ] **V7bis** (`C23`, texte réécrit de `feature-168 (C7)`) — les **46 premiers octets** du
      message contiennent « le dosage en dépend » **en entier**, pour `+1,00` **et** `+10,50`.
      *Mutation : rallonger la tête d'un mot ⇒ il meurt.* Même forme que `V6a`.
- [ ] **V8** (`C33c`) — `ph_offset_stale_cause = "boot"` reçu d'un firmware antérieur ⇒ **le
      texte neutre** s'affiche, ⛔ **jamais** le texte `calibration`.

**Tests de sécurité recommandés, au-delà de V1→V8**
- **Banc de régression du dosage** — ⚠️ **ÉNONCÉ CORRIGÉ le 2026-11-22 : la version précédente
  de cette ligne était FAUSSE**, et elle venait du gate. Elle disait « correction +1,20, brut
  6,10 ⇒ acide **refusé** ». ⛔ C'est l'inverse : `applyPhOffset(6,10 ; +1,20) = 7,30`, et
  `phGuardBlocksBelow(7,30 ; 6,70)` est **faux** — l'acide n'est **pas** refusé, et il ne doit
  pas l'être. ⇒ **Les deux faces sont verrouillées par les vecteurs** :
  - **avec** la correction : mesure corrigée **7,30** ⇒ acide **autorisé**, ORP **non inhibé**
    (7,30 < 7,60) ;
  - **sans** elle (le scénario de disparition de `C23`) : lecture **6,10** ⇒ acide **refusé**
    (6,10 < 6,70) — ⭐ **et c'est exactement le danger que ce lot documente**.
  **Inchangé avant/après le lot** : `ph_guard_logic.h` n'est pas touché, et seule la *définition*
  de `kPhOffsetUnusualAbs` change, **à valeur égale**. C'est l'invariant de `C20`.
- **Filets bruts** 5,50 / 8,50 **inchangés** quelle que soit la correction — un test qui meurt si
  quelqu'un les refait dériver de l'offset.
- **Parité** : `/get-config` et la trame WS rendent le **même** `ph_offset_stale` /
  `ph_offset_stale_cause` **par appel à l'accesseur** — comparer les **expressions**, pas les
  clés (**5ᵉ forme du piège**).

⚠️ **Attendu, et légitime** : la suppression d'`isPhOffsetExpired()` **retire des cas** de
`test/test_native_ph_offset/test_ph_offset.cpp` (bloc D4bis `:634-691`, assertions `:573-622`).
C'est une **réduction**, ⛔ pas une régression — et ⛔ **ne pas conserver la fonction pour
« sauver » ses tests**.

---

## 🎨 Ce que l'étude d'ergonomie RETIRE de l'écran — règle d'accumulation

**Compté sur la capture 2 (390 px), bannière affichée : 7 blocs.** Sans retrait, après ce lot et
bannière éteinte : **8** — le lot ajoute la ligne de date et **dévoile** trois éléments
(étiquette, bouton « Supprimer », phrase d'explication). ⚠️ **Trois d'entre eux parlent de la
même correction.** C'est le travers de la page Produits qui recommence.

- **D1 — OBLIGATOIRE : `#ph_tune_remove_label`** (+ `#ph_tune_remove_value`) — absorbé par
  `C28`. Voir les **deux pièges** de la fusion dans `C28`.
- **D2 — AUTORISÉ par le gate : `#ph_tune_calib_note`** (`data/index.html:1467-1474`,
  « Cela n'étalonne pas la sonde : pour cela, utilisez la carte "Calibration de la sonde pH"
  ci-dessus. »). ⚠️ **Vérifié : aucun marqueur de gate** ; feature-156 n'en avait corrigé que la
  direction. Motifs : la carte d'étalonnage est **juste au-dessus**, et le refus bloquant
  « sonde pas calibrée » dit la même chose **au moment utile**.
  🔴 **Réserve BLOQUANTE du gate** : ce texte n'est retirable **que parce qu'un refus au moment
  utile le remplace**. ⇒ Le gate de `POST /tune-ph-offset` (`src/ph_tune_logic.h:83-113` :
  mesure fraîche, 10 min de filtration, **≥ 2 points d'étalonnage**) et son message visible
  restent **INTACTS** dans ce lot, et **une capture le prouve** (carte en état « sonde non
  étalonnée »). ⚠️ Si ce refus venait à être allégé un jour, **cette note redevient nécessaire**.
- ⛔ **PAS retiré** : la phrase `#ph_tune_remove_hint` (« Sans correction, l'appareil régule sur
  la mesure de la sonde telle quelle ») — elle dit la **conséquence** d'un geste destructif, et
  elle porte l'`aria-describedby`.
- ⛔ **PAS retiré** : `#ph_tune_stakes`. ⭐ Son 1er paragraphe (« L'écart vient de l'installation,
  **pas de la sonde** ») **cesse d'être contredit** une fois `C29` appliquée.

**Bilan visé : 6 blocs** bannière éteinte, **7** avec bannière.

**Verdict d'ergonomie sur l'écran livré (capture `5-apres-sans-bandeau.png`, l'état PERMANENT
après ce lot)** : ⭐ *« Oui. C'est un écran sobre … Je ne vois rien à rouvrir. »* Deux remarques
sans conséquence pour ce lot :
- ⚠️ La ligne de fait est **le signal le plus faible de l'écran** — ce qui **confirme** que le
  vrai enjeu de visibilité est **bug-201**, pas ce lot.
- « Supprimer la correction » est désormais **toujours visible dans ce mode** : c'est l'effet de
  bord annoncé (dette n°3). Garde-fou maintenu par le gate : geste **réversible vers le défaut
  sûr**, phrase de conséquence sous le bouton.

**Réservé à bug-201** (⛔ pas à ce lot) : penser **ensemble** la ligne de date, le chapeau et la
pastille « Calibré · critère hors norme » de la capture 1 — qui parle de l'**étalonnage**, pas de
la correction ; réévaluer la carte entière une fois permanente ; et un signal visible **hors**
du mode réglage.

## 🔴 Q14 (2026-11-22) — `C29`, `C31` et le corps de `C32` REMPLACÉS : le texte affirmait un événement non prouvé

**Trouvé par la revue de code, après Approve.** ⛔ Ce n'était **pas** un défaut d'implémentation :
le code était fidèle aux textes imposés — **les textes étaient faux**.

**Le fait, vérifié dans `src/sensors.cpp`** : `markPhCalibrationStamped()` — le déclencheur de
`C32` — est appelée depuis **TROIS** sites : `:859` `CalibratePhMid`, `:882` `CalibratePhLow`,
et 🔴 `:920` **`ClearPhCal`**, l'**EFFACEMENT** de l'étalonnage. ⚠️ Le commentaire du code le dit
(`:854`, `:877`) : c'est **délibéré**, posé sur contre-visite du gate pour feature-156.

⇒ Sur un effacement, *« un étalonnage vient d'avoir lieu »* (MQTT), *« Vous avez refait
l'étalonnage »* (les quatre rendus de `C29`) et le chapeau *« Étalonnage refait »* (`C31`)
**mentaient tous les trois**. ⭐ La **conclusion chimique** restait juste — la correction est bien
caduque — ⛔ c'est l'**assertion d'événement** qui ne l'était pas.

**Arbitrage du gate : (a) un libellé vrai dans les trois cas.** Les deux autres voies sont
refusées, et les motifs méritent d'être gardés :
- ⛔ **(c) « dette assumée » refusée** : *l'effacement d'étalonnage est un geste de **dépannage**,
  donc exactement le moment où l'utilisateur lit, où il cherche un coupable, et où ce projet a
  déjà fait condamner une **sonde saine** (25/08/2026). Lui écrire « Étalonnage refait » après
  qu'il vient d'en effacer un, c'est lui apprendre que l'appareil raconte n'importe quoi — au
  moment précis où on a besoin qu'il le croie.*
- ⛔ **(b) « distinguer les deux cas » refusée** : l'information existe, mais **ce qu'elle
  apporterait est nul une fois le texte correctement formulé** — conclusion et geste sont
  identiques dans les trois cas. Payer un champ servi, une parité et une fenêtre de version pour
  une nuance qui **ne change aucune action** serait un mauvais échange.

⭐ **LA RÈGLE, inscrite dans ADR-0139** :
> **N'affirme que l'événement que tu peux prouver.** Quand plusieurs chemins mènent au même état,
> le texte nomme **l'ÉTAT**, ⛔ jamais le chemin. Ici l'état est : *l'étalonnage n'est plus celui
> sur lequel la correction a été calculée* — vrai pour `CalibratePhMid`, `CalibratePhLow` **et**
> `ClearPhCal`.

⚠️ **Troisième occurrence du même motif dans ce seul lot** (`C33c`, puis l'écart n°1 du chapeau,
puis ceci). Les deux premières fois l'information **manquait** ; la troisième, elle **existait**.

**Ce qui change, et ce qui ne change pas :**
- `C29` : « Vous avez refait l'étalonnage » → **« L'étalonnage a changé »** ; « l'étalonnage a
  déplacé » → **« ce changement a déplacé »**. ⛔ **La phrase d'ACTE est inchangée** ⇒ la
  sentinelle de parité `.cpp` ↔ `.js` reste valide telle quelle.
- `C31` : chapeau **« Étalonnage modifié »** (ex-« Étalonnage refait »). Titre, boîte et variante
  `--danger` (contraste mesuré **6,14:1**) **inchangés**.
- `C32` : **seul le CORPS change**. ⭐ **La tête reste « Correction de mesure +1,20 à vérifier : »,
  41 octets** ⇒ les assertions de marge déjà mesurées et jouées en mutation **restent valides**,
  ⛔ rien à remesurer.
- ⚠️ **bug-201 `(C42)` est amendée à l'identique** — c'est le même gate qui avait imposé
  l'ancienne formule et qui la corrige. Les deux fiches portent désormais la **même** phrase.

**Verrouillage (remplace le `V9` proposé par le gate, et le dépasse)** : les mots **« refait »**,
**« vient d'avoir lieu »** et **« Vous avez »** sont assertés **absents du texte PRODUIT** —
5 valeurs d'offset dans la suite native, **16 combinaisons cause × dates** dans le harnais JS.
⛔ **Pas un contrôle sur la SOURCE** : les commentaires citent volontairement ces mots **pour les
interdire**, un contrôle de source s'y tromperait. *Mutations jouées : réintroduire la formule
côté firmware ⇒ la suite native meurt ; côté navigateur ⇒ 3 vecteurs meurent.*

## ⚖️ Trois écarts relevés par la passe interface — tranchés le 2026-11-22

1. 🔴 **Le chapeau « Étalonnage refait » serait FAUX sur une cause héritée.** Si un firmware
   antérieur renvoie `"boot"` ou `"expiration"`, **aucun étalonnage n'a eu lieu** — le chapeau
   affirmerait un événement inexistant, exactement le défaut que `C33c` corrige dans le corps du
   texte. ⇒ **ACCEPTÉ** : le chapeau est **masqué** (`#ph_tune_stale_eyebrow`, `hidden`) dès que
   la cause n'est pas `calibration` / `undefined`. ⚠️ `C31` est amendée en ce sens : *le chapeau
   nomme un ÉVÉNEMENT — il ne s'affiche que lorsque cet événement est établi.*
2. 🔴 **La ligne de date reste-t-elle affichée quand le bandeau est visible ?** ⇒ **NON —
   DÉCISION RENVERSÉE le 2026-11-22, sur CAPTURE.**
   ⚠️ L'étude d'ergonomie avait écrit « je tolère cette redondance » **sans l'avoir vue rendue**.
   La capture `4-apres-bandeau.png` (390 px) montre **la valeur `+1,20` ET la date `16/10/2026`
   DEUX FOIS en 2 cm, dans deux styles différents**. ⭐ *« J'ai écrit "tolérée" sans l'avoir vu
   rendu : je retire cette tolérance. »*
   ⚠️ Le « clignotement » redouté est un **faux coût** : la ligne ne change d'état qu'**au clic
   de l'utilisateur** sur « Je l'ai vérifiée — garder » — le bandeau part, la ligne prend sa
   place, le fait **reste** à l'écran. C'est une transition **causée**, pas un scintillement.

   **Règle exacte à implémenter :**
   - Condition **partagée, écrite une seule fois** :
     `bandeauAvecFait = staleBannerShown && cause ∈ {"calibration", undefined}`.
     La ligne est visible si `offset ≠ 0` **et** `!bandeauAvecFait`.
   - ⛔ **NE PAS masquer la ligne** quand le bandeau porte le **texte neutre** de `C33c` (cause
     héritée `boot` / `expiration`) : ⚠️ ce texte ⛔ **ne cite ni la valeur ni la date** — masquer
     la ligne **ferait perdre le fait**.
   - Le bouton « Supprimer la correction » reste masqué quand le bandeau est affiché ; son
     `aria-describedby` pointe vers `#ph_tune_applied`, **masqué avec lui** ⇒ ⛔ aucun renvoi
     vers un élément caché depuis un bouton actif.
   - ⛔ Aucune animation. ⛔ Aucun `aria-live` sur la ligne.
3. **Le toast de « garder » disait « vérifiez-la par un test fiable »** — il **contredisait** le
   nouveau libellé « Je l'ai vérifiée ». ⇒ **ACCEPTÉ**, remplacé par « Correction conservée,
   marquée comme vérifiée ». ⚠️ La condition mineure du gate de feature-156 (« "conservée" seul
   laisserait croire que le sujet est clos ») reste satisfaite : le toast dit **ce que le clic a
   fait**, pas que le sujet est clos.

## Livrables — check-list
- [x] **A0** — 🔴 **Verdict `pool-chemistry`** AVANT toute ligne de code — *3ᵉ passe, 2026-11-22,
      GO SOUS CONDITIONS*
- [ ] **A1** — retrait du drapeau, des causes `boot`/`expiration` et des champs servis associés
      (`C25`, `C26`)
- [ ] **A2** — le journal de démarrage **en information** (`C24`)
- [ ] **A3** — `calibration` conservée **et dotée d'un canal** (`C21`, `C32`)
- [ ] **A4** — ✅ **protection d'intégrité NVS VÉRIFIÉE** (`nvs_types.cpp:46-52` appelée depuis
      `nvs_page.cpp:954` dans `Page::findItem` ; `dataCrc32` vérifié `nvs_page.cpp:280`).
      ⇒ **Aucune somme de contrôle maison** ; à consigner dans ADR-0138 avec les **deux angles
      morts** (`C34`)
- [ ] **B1** — retrait du bandeau et des boutons, ligne de date ajoutée (`C28`→`C31`, passage
      `ux-ui-designer` **obligatoire**)
- [ ] **D1** — ⚠️ **réécrit** : la formulation d'origine (« un appareil porteur de l'ancien champ
      NVS ») reposait sur une **prémisse fausse** — ce champ n'existe pas (`C27`). Formulation
      correcte : *un appareil dont la NVS porte `ph_offset = 1,20` démarre, journalise le
      `warning` de `C24`, et sert `ph_offset_stale = false`.*
- [ ] **D2** — vecteurs `V1`→`V8`, **validés par mutation**
- [x] **C1** — docs + CHANGELOG + ADR-0138

## Sécurité chimique
- Dosage impliqué : **oui, indirectement** — la correction décale la mesure qui pilote
  l'acidification.
- Risque identifié : ⚠️ une correction fausse reste appliquée sans être signalée comme question.
  ⭐ Mais elle l'est **déjà** : le mécanisme actuel ⛔ ne bloque rien, il demande — et personne ne
  répondait. ⇒ **Le retrait ne retire aucune protection effective.**
- 🔴 **Et il retire une protection qui regardait dans la mauvaise direction** : le seul mode de
  défaillance atteignable est la **disparition** de la correction, sur laquelle ce mécanisme
  **reste muet** (voir « Ce que la 3ᵉ passe a établi »).

## Dettes assumées — ⛔ écrites, pas tues
1. **`C28` n'est pas visible en usage courant** tant que bug-201 n'est pas livré.
2. **La perte silencieuse de l'entrée NVS** (correction → 0,00) reste **indétectable** sans un
   marqueur explicite « correction retirée par l'utilisateur » qui la distinguerait d'une
   suppression volontaire. **Hors périmètre** — à consigner dans ADR-0138 comme limite connue,
   ⛔ plutôt que découverte dans six mois.
3. ⚠️ **Effet de bord visible à vérifier à l'écran** : pour cet appareil, `ph_offset_stale` passe
   à `false` ⇒ la bannière disparaît ⇒ `renderPhTuneRemoveRow()` (`app.js:2657-2672`, masquage
   mutuel) **dévoile « Supprimer la correction »**. Chimiquement équivalent (un clic, même route,
   même effet), mais c'est un **changement visible** : capture à 390 px attendue — et c'est ce
   bouton-là que bug-201 devra ferrer.

## Recommandations non bloquantes du gate
- ⭐ **Supprimer `isPhOffsetStale()` entièrement** (`ph_offset.h:135-143`) au lieu de la réduire à
  une enveloppe d'une ligne : `phOffsetStaleNow()` appellerait directement
  `isPhOffsetSupersededByCalibration()`. Une enveloppe au corps identique rouvre le piège
  « même notion, deux expressions » décrit `ph_offset.h:121-133`. **Un seul nom, une seule
  expression.**

## Hors périmètre — suivis à créer
- `tools/check_condition_refs.sh` : refuser un `Cxx` nu dans un commentaire de `src/`/`data/`
  non précédé de `feature-NNN`/`bug-NNN` (voir « Règle de numérotation »).
- Le commentaire **faux** `data/index.html:1355-1356` (« Toujours visible ») : corrigé **par
  bug-201**, qui traite la cause.
