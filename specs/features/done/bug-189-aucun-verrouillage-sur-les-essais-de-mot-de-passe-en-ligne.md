# bug-189 — Rien ne ralentit un essai de mot de passe : 172 800 tentatives par jour et par adresse

## Métadonnées
| | |
|---|---|
| Type | bug — 🔴 **sécurité, chemin d'authentification** |
| Statut | todo |
| Priorité | **HAUTE** — ⭐ c'est le risque CHIFFRÉ réel de la porte d'entrée, celui que [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) a rendu visible sans le fermer |
| Version cible | à définir |
| Relevé par | `security-reviewer` **et** `code-reviewer`, indépendamment, en passe P2 de [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md), 2026-11-17 — ⭐ chiffres vérifiés dans `src/constants.h:238` |
| Specs liées | [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) (trace des échecs) · [`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md) · [`feature-172`](../todo/feature-172-retention-protegee-des-traces-de-securite.md) |

## Domaines touchés
- [x] **Authentification** (`src/auth.cpp`, `src/web_routes_auth.cpp`)
- [ ] Capteurs — [ ] Régulation — [ ] Filtration — [ ] MQTT — [ ] OTA
- [x] **Frontend** — TRANCHÉ : le compte à rebours s'affiche (`data/login.html`, page de secours OTA), donc [`ux-ui-designer`](../../../CLAUDE.md) est passé sur l'ÉCRAN ENTIER de connexion

## Agents requis
- [x] **`security-reviewer`** — ⛔ OBLIGATOIRE, **passe P1 sur le design exigée** : le choix du mécanisme de ralentissement n'est pas lisible dans un diff
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [x] **`ux-ui-designer`** — mandat sur l'écran de connexion entier (contraste AA mesuré, 390 px, mouvement réduit)
- [ ] `pool-chemistry` — sans objet

## Le fait, mesuré dans le code

`src/constants.h:238-239` :
```cpp
constexpr uint16_t kMaxRequestsPerMinute = 120;      // Limite globale par IP
constexpr unsigned long kRateLimitWindowMs = 60000;
```

⇒ **120 × 60 × 24 = 172 800 essais par jour et par adresse IP**, et ⛔ **aucun verrouillage
de compte** : un échec ne coûte rien de plus que le suivant. Le quota est un plafond de
DÉBIT, pas une sanction.

⚠️ **Et il est par IP** : la borne se **multiplie** par le nombre d'adresses que l'attaquant
prend sur le réseau. Sur un Wi-Fi domestique, en prendre plusieurs est trivial.

### Ce que ça donne, chiffré

| Mot de passe | Temps depuis UNE seule adresse |
|---|---|
| Dictionnaire d'un million de mots | **~6 jours** |
| 8 minuscules aléatoires (~37 bits) | des siècles |

⇒ **Le risque porte sur les mots de passe FAIBLES**, pas sur les bons. C'est ce qui rend
ce lot utile sans être urgent au point de tout arrêter : il protège l'utilisateur qui a
choisi un mot de passe court, c'est-à-dire la majorité.

### ⛔ La comparaison qui remet les choses en place

[`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) a fermé un canal temporel qui aurait demandé **~10⁶ à 10⁷ requêtes** pour livrer
quoi que ce soit. **Celui-ci demande six jours contre un mot de passe faible.** Les deux
passes de revue l'ont écrit dans les mêmes termes : ⛔ ne pas inverser les priorités.

## Comportement attendu

Un essai de mot de passe raté doit **coûter de plus en plus cher**, sans jamais enfermer
le propriétaire dehors.

### ⛔ CONTRAINTE RÉDHIBITOIRE — ne jamais verrouiller le propriétaire

**L'OTA est le SEUL canal de déploiement : il n'y a pas de port série.** Un appareil dont
le propriétaire serait durablement exclu serait **perdu**, sans aucun moyen de reprise.

⇒ Tout mécanisme proposé doit satisfaire les trois règles suivantes, ⛔ sans exception :
1. **Un jeton déjà émis continue de passer.** Le ralentissement frappe les ÉCHECS, jamais
   une session valide en cours.
2. **Le ralentissement s'efface tout seul**, par simple écoulement du temps. ⛔ Aucun état
   qui exige une action humaine pour être levé.
3. **Il ne survit pas à un redémarrage** de façon qui pourrait enfermer dehors — ou, s'il
   est persistant, il porte une borne de durée **dure**, écrite dans `constants.h`.

⚠️ Un « verrouillage de compte après N échecs » au sens classique est donc **à écarter**
tel quel : c'est précisément la forme qui enferme dehors. Viser un **ralentissement
progressif** (délai croissant avant la réponse, ou quota d'échecs dégressif), ⛔ pas un
refus définitif.

### Piste à instruire, pas à imposer

Un recul exponentiel sur les **échecs**, calqué sur celui que le projet a déjà pour le
journal (`kUnauthWarnMaxShift` / `kUnauthWarnDecayMs`, [ADR-0127](../../../docs/adr/0127-le-bornage-du-journal-d-authentification-est-global-par-emetteur.md)) : la mécanique est
écrite, testée en natif et comprise. ⚠️ **À instruire par `architect` puis P1**, pas à
retenir d'avance.

⛔ **Attention au handler asynchrone** : AsyncWebServer n'autorise **aucun handler bloquant
> 50 ms**. Un « délai avant réponse » ne peut PAS être un `delay()`. C'est le point dur de
la conception, et c'est pour lui que la passe P1 est exigée.

### Robustesse du mot de passe choisi

À examiner dans le même lot, puisque c'est le même risque : exiger (ou au moins **mesurer
et dire**) la robustesse du mot de passe au moment où l'utilisateur le choisit. ⚠️ Un
ralentissement qui protège un mot de passe « 1234 » ne fait que repousser l'échéance.
⇒ Arbitrage produit à poser : **refuser** un mot de passe faible, ou **l'afficher comme
faible** et laisser choisir. ⛔ Ne pas trancher ici.

## Comportement actuel

- `checkRateLimit()` plafonne à 120 req/min/IP, **toutes routes confondues**.
- Un échec d'authentification est désormais **journalisé** et borné (bug-183) — on le
  **voit**, mais rien ne le **freine**.
- Le compteur `gFailuresSinceSuccess` existe (bug-183) mais ne sert **qu'au texte** de la
  ligne de connexion réussie : ⛔ il ne décide de rien.

## Contraintes techniques

- ⛔ **Le serveur reste en HTTP clair.** Ni TLS, ni proxy, ni VPN (décision produit du
  2026-11-17).
- ⛔ **Aucune solution ne doit exiger d'équipement ou de configuration chez le client.**
- ⛔ Aucun handler bloquant > 50 ms (AsyncWebServer).
- ADR-0082 : pas d'allocation sur le chemin de journalisation.
- ADR-0017 : la décision de ralentissement est de la **logique pure** ⇒ module testable en
  natif, et **validé par mutation**.

## ⭐ CONCEPTION ARBITRÉE — 2026-11-18

### ⛔ LE CHIFFRE ANNONCÉ PAR LA CONCEPTION ÉTAIT FAUX, et c'est ce qui a changé le choix

Le plan annonçait « attente maximale du propriétaire : 10 s ». **C'est faux sous attaque
active**, relevé par `security-reviewer` en P1 (G1) et **vérifié par simulation** :

```
porte globale, plafond 10 s, attaquant à 2 req/s, propriétaire qui retente toutes les 15 s
sur 10 minutes :  tentatives acceptées = 0  ·  refusées = 40
```

**Mécanisme du défaut** : la porte libère un créneau toutes les 10 s ; l'attaquant, qui
frappe plus vite, le consomme **systématiquement avant** le propriétaire. Ce n'est pas une
attente, c'est une **famine**. ⚠️ Et le jeton vit en `sessionStorage` (`data/app.js:100`) :
un nouvel onglet repasse par `/auth/login`, donc le propriétaire perd l'accès à l'interface
**et** à la page de secours OTA tant que l'attaque dure.

⇒ ⭐ **Un chiffre juste en l'absence d'adversaire, faux en présence d'adversaire.** C'est
exactement ce qu'une passe de sécurité doit attraper, et c'est pour ça qu'elle passe avant
le code.

### Décision de l'utilisateur, 2026-11-18

⭐ **Option C (hybride globale + par IP) PLUS une table d'EXCEPTION**, retenue sur les
chiffres ci-dessous. L'exception entre **dans ce lot**, ⛔ elle n'est pas remise à plus tard.

| | Protection réelle | Propriétaire pendant l'attaque |
|---|---|---|
| Par adresse seule | ⛔ ×2,4 à 50 adresses — **rate le critère** | jamais gêné |
| Globale simple | ✅ ×1 000 | ⛔ **famine totale** (mesurée ci-dessus) |
| ⭐ **Globale + exception** | ✅ ×1 000 | ✅ passe **si déjà connecté depuis le dernier redémarrage** |

**L'exception** : l'appareil retient, dans une table à **capacité fixe**, les quelques
adresses ayant **réussi** une connexion, et ne leur applique pas la porte globale.

⛔ **SA LIMITE, À ÉCRIRE PARTOUT ET À NE JAMAIS TAIRE** : la table est en RAM. ⇒ **Après un
redémarrage, ou depuis un appareil jamais utilisé, le propriétaire retombe dans le cas de
famine.** ⚠️ Et c'est précisément le scénario de la page de secours OTA (interface cassée ⇒
redémarrage ⇒ table vide). L'exception **réduit** G1, elle ne le **supprime pas**.

### Arithmétique vérifiée indépendamment (2026-11-18)

Les onze chiffres du tableau de l'architect ont été **recalculés** : tous justes.
⚠️ Mais le « facteur 120 » de l'option par adresse est un **mirage** : il vaut 120 contre
**une** adresse et **2,4** contre cinquante. Un chiffre exact peut induire en erreur ; c'est
le **scénario** qui compte, pas le nombre.

### Verdict P1 — GO SOUS CONDITIONS, 8 conditions

Détail dans le rapport ; les deux structurantes :
1. ⛔ **Supprimer `nextAllowedMs`** (échéance absolue) : au repliement de `millis()`, elle
   peut bloquer l'accès **jusqu'à 49,7 jours**. Verdict calculé sur l'écart **non signé**
   `now - lastFailMs`, décalage **borné avant** le `<<`. ⇒ L'attente devient au plus le
   plafond **par construction**.
2. ⛔ **Place de la garde** : `/auth/login` après `checkRateLimit` et **avant**
   `deserializeJson` (elle ne dépend pas du corps, et évite un `JsonDocument` par refus) ;
   `/auth/change-password` après l'analyse, **avant** `passwordMatches` et **avant**
   `checkTokenAuth`. ⛔ **Jamais** dans `checkRateLimit`, `checkAuth` ni `decideAdmission`
   (`src/auth.cpp:338` : un refus de quota empêche l'examen du jeton ⇒ enfermement dehors).

### ⛔ CE QUE CE LOT NE FERMERA PAS — à ne pas laisser croire durci

- **8 640 essais/jour en attaque distribuée** : une liste de 10 000 mots tombe en ~1,2 jour.
  ⇒ Le lot protège contre un dictionnaire, ⛔ **pas** contre un mot de passe vraiment faible.
- Un **redémarrage** rend 3 essais gratuits (la franchise), et **vide la table d'exception**.
- Le mot de passe circule **en clair** sur le Wi-Fi (décision produit assumée).
- ⚠️ La **robustesse du mot de passe choisi** n'est dans **aucun** livrable de cette fiche —
  à arbitrer séparément (spec de suite), ⛔ ne pas la traiter en silence ici.

## ⭐ DEUX ERREURS DE LA REVUE, ATTRAPÉES PAR LA MESURE (2026-11-18)

⚠️ Les deux passes P1 ont été décisives — et **deux de leurs affirmations étaient fausses**.
Les garder sans vérifier aurait coûté, dans un cas une protection, dans l'autre un réglage
inutile. ⇒ **Un verdict de revue se vérifie comme le reste.**

### ⛔ ERREUR 1 — condition 10 « mot de passe uniquement » : trop étroite

`embedded-developer` l'a appliquée **à la lettre** et a **signalé le problème au lieu de
trancher seul** — c'est le bon comportement, et c'est la condition qui était mauvaise.

Enchaînement **entièrement routinier** que la lettre produisait :
1. le propriétaire est connecté, son adresse est exemptée ;
2. il change son mot de passe **depuis l'interface**, donc **par jeton** (le cas NORMAL) ;
3. la table est vidée, ⛔ **et il n'y est pas réinscrit** ;
4. son jeton est rotaté ⇒ il doit se reconnecter ;
5. attaque en cours à cet instant ⇒ **famine**.

⇒ ⭐ **L'action la plus recommandable qui soit — changer son mot de passe — lui retirait sa
protection, précisément au pire moment.**

**Règle corrigée, unique** : ⭐ **tout `kAllow` de `/auth/change-password` vide la table puis
RÉINSCRIT l'auteur, sauf `ip == 0`.** Les trois chemins sont couverts sans distinction :
mot de passe actuel, **jeton valide** (« un jeton de 128 bits prouve davantage qu'un mot de
passe, et qui l'a volé contrôle déjà l'appareil »), et **changement anonyme sur appareil
vierge** (l'auteur détient le nouveau mot de passe : l'exemption ne lui donne rien de plus).
⛔ Limite : seulement sur un `kAllow` de **cette** route, jamais sur une requête à jeton
quelconque. ⇒ Table vidée aussi à la **réinitialisation par le bouton physique**.

### ⛔ ERREUR 2 — condition 12 « les débits annoncés sont faux » : RETIRÉE

La revue affirmait qu'une stratégie « rafale puis silence » donnerait **~16 100 essais/jour**
au lieu de 8 640, ramenant le gain à 10,7× — à la limite du critère. Elle précisait
elle-même « calculé à la main, pas simulé » et « je n'ai pas vu le code ».

**Simulé sur la sémantique EXACTE du module livré, horizon 7 jours :**

| Stratégie de l'attaquant | Essais/jour |
|---|---|
| Attaque simple (frapper dès que permis) | **8 641** |
| ⭐ La stratégie de la revue (14 en rafale + 60 s de silence) | **6 368** |
| 14 en rafale + 300 s de silence | 2 814 |
| 14 en rafale + 1 200 s de silence | 1 004 |
| 1 000 en rafale + 300 s de silence | 8 402 |

⚠️ **Correction de ma propre première mesure** (relevée par `test-engineer`) : j'avais annoncé
8 401 et 7 723 pour « silence 300 s / 1 200 s » sans préciser que ma simulation utilisait des
rafales de **1 000**, pas de 14. Avec la rafale de 14 de la revue, c'est **2 814** et
**1 004**. ⇒ La conclusion n'en est pas affaiblie, elle en est **renforcée** : plus l'attaquant
se tait, plus il perd. Balayage complet `{5, 14, 30, 100, 400, 1000} × {10, 60, 300, 1 200 s}` :
⛔ **aucune combinaison ne bat l'attaque simple.**

⇒ **Le maximum est l'attaque simple, à 8 641/jour** — très exactement les 8 640 annoncés.
**Facteur 20× confirmé.** Porte par IP : **1 441/jour** contre 1 440 annoncés.

**Cause de l'erreur** : la revue supposait un pardon **TOTAL**. Le code fait
`forgiven = elapsed / decayMs` ⇒ pardon **INCRÉMENTAL**, un échec effacé par période. Après
14 échecs et 60 s de silence il reste **13** échecs effectifs et **4 s** d'attente ; effacer
14 échecs demanderait **14 minutes** de silence. ⇒ Se taire ne rend jamais plus d'un essai
par période, alors que frapper au plafond en rend un toutes les 10 s : **l'optimum est de ne
jamais se taire.**

⇒ ⛔ **Aucune constante changée, aucun pardon allongé.** Balayage de 60 s à 1 800 s :
**aucun effet** sur le débit maximal. La revue a retiré sa condition après relecture du code.

---

## ⭐ INTERFACE — décisions, et un défaut d'enchaînement trouvé au passage

### ⛔ Le défaut que personne n'aurait vu avant la mise en service

`showError()` masquait **toute** alerte au bout de **5 secondes**. Or le blocage peut durer
**60 secondes**. ⇒ L'utilisateur se serait retrouvé devant un **bouton inerte sans aucune
explication à l'écran** pendant 55 s. ⇒ Le `setTimeout` est **supprimé** : une alerte 401 ou
« injoignable » vit jusqu'au prochain envoi ou à la première frappe, et l'alerte 429 vit
**exactement** le temps du compte à rebours.

### Forme retenue
Alerte **statique** pour la cause (« Trop d'essais. Patientez un instant. ») + **compte à
rebours dans le libellé du bouton désactivé** (« Réessayer dans 0:42 »). Une seule source du
chiffre, placée sur l'objet qui est bloqué. ⛔ Écartés : le chiffre figé dans l'alerte (faux
dès la 2ᵉ seconde) et le compte à rebours en `aria-live` (un lecteur d'écran lirait **chaque
seconde**). ⚠️ Le message est **factuel et sans reproche** : le ralentissement peut frapper le
propriétaire alors qu'il n'a rien fait, parce qu'un tiers attaque au même moment.

### ⚠️ Contraste — mesuré, et un piège évité
Texte blanc sur le bouton : **3,92:1**, sous le seuil AA de 4,5. Corrigé en `#4a68c8` →
**5,12:1** (le second arrêt du dégradé, `#687ad8`, était **encore pire** ⇒ `#5470d0`, 4,56:1).
⛔ **Un remplacement global aurait été FAUX** : `#5c7cdb` apparaît **15 fois dans 3 fichiers**
et sert à **trois choses** — fonds de bouton (texte blanc ⇒ concerné), bordures de focus et
liserés de mise au point (critère 3:1, non textuels ⇒ **non concernés**), barre de progression.
⇒ **3 occurrences changées**, sur les 3 écrans, pour ne pas rendre l'interface **moins**
cohérente qu'avant.

### 🔴 RÉSIDU ÉCRIT, NON TRAITÉ DANS CE LOT
⚠️ **Le sous-titre de l'assistant reste à 3,92:1** (`data/wizard.html`, 14 px, poids 400,
blanc sur le dégradé d'en-tête) — ⛔ **sous le seuil**, et c'est le **premier écran qu'un
client voit à la livraison**. La pastille d'étape (`wizard.html:148`, chiffre blanc) est dans
le même cas. ⚠️ L'en-tête de `wifi.html` (24 px) **passe**, lui, au titre du « grand texte ».
⇒ Composant différent, écrans que bug-189 ne touche pas autrement ⇒ **spec de suite**, pas
d'élargissement silencieux.

### Autre nettoyage écarté
⚠️ `showFirstBootForm()` est **morte** (vérifié : une seule occurrence, sa définition ; la
page redirige vers `/wizard.html`). ⛔ **Non supprimée** : hors périmètre, et elle touche le
chemin de premier démarrage que [`bug-185`](../done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md) vient de stabiliser. ⇒ Spec de suite.

## 🔴 LE DÉFAUT LE PLUS INSTRUCTIF DU LOT — un test vert qui ne prouvait rien (2026-11-18)

Trouvé par `code-reviewer` en revue du diff réel, **vérifié par moi dans `data/app.js`**.

### Le fait

`data/app.js:15236-15242` — la vraie page envoie **les DEUX ensemble** :
```js
headers: { ..., "X-Auth-Token": token },
body: JSON.stringify({ currentPassword, newPassword })
```
Or la garde était `!currentPassword.isEmpty() && !virginState` ⇒ ⛔ **elle s'appliquait malgré un
jeton valide.** Le seul chemin « jeton » épargné était une forme de requête qu'**aucun client du
dépôt ne produit**.

Et le test `test_A2_..._le_jeton_valide_passe` modélisait `tokenChange()` = **jeton SEUL**.

| | Ce qui était testé | Ce que la page envoie |
|---|---|---|
| Forme | jeton seul | jeton **+** `currentPassword` |
| Résultat | épargné ✅ | **freiné** ⛔ |

⇒ **Le test était vert, le commentaire du code affirmait la protection, et sur le vrai chemin elle
n'existait pas.** Conséquence réelle : propriétaire **non exempté** (après redémarrage, ou évincé
des 4 entrées) + attaque en cours ⇒ **429 sur son changement de mot de passe, jeton valide en
main**.

### ⛔ Et la garde CI GELAIT le défaut

`tools/check_auth_fail_guard.sh` (règle B) exigeait **littéralement** la condition fautive. ⚠️ Elle
aurait **empêché** quiconque de la corriger sans s'en apercevoir. ⇒ Mise à jour en même temps, et
re-vérifiée par mutation : retirer `!virginState` → rc=1, retirer `!checkTokenAuth(req)` → rc=1.

### Correction

```cpp
if (!currentPassword.isEmpty() && !virginState && !authManager.checkTokenAuth(req)) {
```
⛔ `checkTokenAuth()` est le prédicat **SILENCIEUX** : `checkAuth()`, `sendAuthRequired()` et
`checkRateLimit()` journalisent, et les employer ici aurait cassé `check_unauth_log_guard.sh`
(ADR-0082).

### ⭐ LA LEÇON, et elle dépasse ce lot

**Un test qui ne reproduit pas ce que le vrai client envoie ne prouve pas que ça marche : il
prouve que le modèle est cohérent avec lui-même.**

⚠️ Les deux revues avaient écrit, **chacune de son côté**, « le modèle de test n'est pas le handler
réel ». ⛔ Ce n'était pas une clause de style : c'était **le** défaut. ⇒ Tout modèle de handler
doit désormais citer, **en commentaire, le client qui produit cette forme** — c'est fait dans
`test_native_auth_fail_backoff` (`uiChange` / `passwordOnly` / `tokenOnly`, chacun annoté).

⇒ Audit des autres modèles mené dans la foulée : `loginModel` et la prise de possession sont
conformes aux clients réels. ⭐ **Trouvé au passage** : les tests des 400 employaient
`passwordOnly` au lieu de la forme de l'interface — corrigé.

---

## ⭐ VERDICTS FINAUX — 2026-11-18

| Passe | Verdict | Reste |
|---|---|---|
| `security-reviewer` **P1** (design) | GO SOUS CONDITIONS, 11 conditions | toutes levées |
| `security-reviewer` **P1-bis** (exemption) | GO SOUS CONDITIONS | levées |
| `security-reviewer` **P2** (diff) | **GO SOUS CONDITIONS** (A, B, C) | ✅ **B levée** : D3 fait le 2026-11-18 |
| `code-reviewer` | Request changes (1 écart, 2 livrables) | écart corrigé, C1 fait, ✅ D3 fait |

### Mesures finales (reconstruction COMPLÈTE, caches effacés)
```
pio run           SUCCESS 57 s · 0 warning dans src/ (2 warnings viennent d'une bibliothèque tierce)
                  Flash 1 958 637 o (52,4 %) · RAM 98 536 o (30,1 %)
pio test -e native   93 suites · 2 668 cas · 0 régression
build_fs.sh       SUCCESS
7 gardes d'hygiène vertes · CI à 32 jobs (auth-fail-guard branché, vérifié)
```

### Correction d'une affirmation de cette fiche
⚠️ « Table vidée à la réinitialisation par le bouton physique » : `resetPasswordToDefault()`
**n'a aucun appelant** (relevé par `code-reviewer`). ⇒ C'est le **redémarrage** qui vide la table,
puisqu'elle est en RAM. Le résultat est le même, la justification était fausse.

### Défaut de documentation corrigé au passage
⚠️ `docs/subsystems/auth.md` annonçait **30 requêtes/minute** là où le code en dit **120**, et
renvoyait à `constants.h:37` (numéro périmé). ⛔ Le fichier se **contredisait lui-même** depuis
l'ajout de la section bug-189. Corrigé, avec rappel d'[ADR-0073](../../../docs/adr/0073-chiffre-consigne-porte-sa-methode.md) : ⛔ ne pas recopier un numéro de ligne.


---

## ⭐ D3 — ESSAI RÉEL SUR L'APPAREIL — 2026-11-18, v2.94.0

Déployé par OTA (programme + système de fichiers, **signés**, signatures relues sans
re-signature grâce à `--use-sig`). `/get-system-info` renvoie `2.94.0` : c'est bien le binaire
mesuré ici. Attaquant = le conteneur LXC, `192.168.1.55`.

### (c) Le débit d'essais s'effondre

133 tentatives anonymes, une par seconde pendant 150 s :

| | |
|---|---|
| Mot de passe **réellement comparé** | **11** |
| Refusées par le ralentissement (429 + `retryAfter`) | 122 |
| Refusées par le quota `kMaxRequestsPerMinute` | 0 |

Délai entre deux essais acceptés, relevé : **1 s, 2 s, 4 s, 8 s, 16 s, 32 s, puis 60 s**
(plafond `kAuthFailIpCapMs`). ⇒ **Régime établi : 1 essai/minute contre 120/minute**
auparavant — facteur **120**. Sur la fenêtre complète de 150 s : 11 au lieu de 300, facteur 27.

⚠️ `Retry-After` **décroît** d'une seconde à chaque refus (59, 58, 57…) : l'en-tête annonce le
temps **restant**, pas la pénalité. C'est voulu, et c'est ce que lit le compte à rebours de
l'écran de connexion.

### (a) et (b) — portes FERMÉES, le propriétaire entre quand même

Séquence exécutée d'un seul tenant, la fermeture étant établie par le refus qui la précède
immédiatement :

```
essais anonymes 1-4   -> 401
essai anonyme 5       -> 429 {"retryAfter":1}      PORTE FERMÉE
page protégée + jeton -> 200
changement de mot de passe + jeton + mot de passe actuel correct -> 200
```

⭐ Le **200** a bien été obtenu **porte fermée**, avec un jeton émis **avant** le ralentissement.
Le mot de passe a été changé pour une valeur temporaire puis **remis à sa valeur d'origine**
(200 également), et la connexion avec le mot de passe d'origine revérifiée : **200**.

Essai complémentaire, porte fermée, avec un mot de passe actuel **volontairement faux** :
réponse `401 Current password incorrect` — ⛔ **pas** un 429. La demande a donc été
**instruite** : le porteur du jeton n'est pas freiné, c'est bien la garde qui l'épargne et non
un hasard de calendrier.

### Le journal

4 lignes pour 133 tentatives :

```
[Auth] Connexion réussie depuis 192.168.1.55
[Auth] /auth/login : mot de passe incorrect depuis 192.168.1.55
[Auth] /auth/login : essai refusé (ralentissement) … ; et 56 autres échecs non journalisés
[Auth] /auth/login : mot de passe incorrect … ; et 75 autres échecs non journalisés
```

2 émises + 56 + 75 = **133** : le compte est exact, et le recul de bug-183 a tenu — le journal
de 6 144 o n'a pas débordé.

⚠️ **Relevé de méthode** ([ADR-0073](../../../docs/adr/0073-chiffre-consigne-porte-sa-methode.md)) :
`/get-logs` est **paginé** (~4 Ko). Un appel unique ne rend que la PREMIÈRE page — ici les seules
lignes de démarrage. Il faut boucler sur `?since=<dernier timestamp>` tant que `truncated` est
vrai, sous peine de conclure à tort que rien n'a été journalisé.

### Temps de traitement

6 requêtes chronométrées bout en bout (réseau compris) : **16 à 25 ms**, 401 comme 429.
Le critère « aucun handler ne bloque > 50 ms » est tenu avec de la marge, SHA-256 compris.

### ⛔ CE QUE CET ESSAI NE PROUVE PAS

L'adresse qui attaquait **était exemptée** : elle s'était connectée avec succès avant la
campagne. Ses échecs ont donc alimenté **sa propre porte**, conformément à
[ADR-0133](../../../docs/adr/0133-le-ralentissement-des-echecs-de-mot-de-passe-est-global-avec-exemption.md).

⇒ Deux conséquences, l'une rassurante, l'autre à écrire noir sur blanc :

1. ✅ **Aucune autre adresse n'a été gênée** pendant les 133 échecs — la famine que P1 avait
   mesurée (0 accepté / 40 refusés) ne s'est pas produite. C'est exactement l'effet voulu par
   la table d'exemption.
2. ⛔ **La porte GLOBALE et la table des 8 créneaux n'ont donc PAS été éprouvées sur
   matériel** : il aurait fallu une seconde adresse, ou un redémarrage suivi d'une attaque
   sans connexion préalable. Elles restent couvertes par les 38 cas natifs et par la revue,
   ⛔ pas par un relevé terrain. **La politique mesurée ici est celle par IP**
   (franchise 3, base 1 s, plafond 60 s) ; un attaquant non exempté subit
   `max(porte de créneau, porte globale)`, donc **au moins** cette sévérité.

## Livrables — check-list

- [x] A1 — Mécanisme de ralentissement progressif des ÉCHECS sur `/auth/login` **et**
      `/auth/change-password`, en **logique pure** (module testable en natif)
- [x] A2 — ⛔ Garantie, prouvée par test **sur la FORME RÉELLE du client** (jeton ET `currentPassword` ensemble, `data/app.js:15236`), qu'un **jeton valide passe toujours**, quel que
      soit l'état du ralentissement
- [x] A3 — Effacement automatique par écoulement du temps, borne de durée dure dans
      `constants.h`
- [x] A4 — Le ralentissement apparaît dans le journal (borné, site existant de bug-183)
- [x] B1 — ⚠️ Si l'utilisateur voit un message : passage `ux-ui-designer` **sur l'écran
      entier** de connexion, pas sur l'ajout
- [x] D1 — Tests natifs : montée, décroissance, jeton valide épargné, bornes
- [x] D2 — **Validation par mutation** de chaque test neuf
- [x] D3 — Essai réel **fait le 2026-11-18 sur l'appareil en v2.94.0** : 133 tentatives,
      **11 évaluées** ; régime établi 1 essai/minute contre 120 — section « D3 » ci-dessus
- [x] C1 — `docs/subsystems/auth.md`, `docs/API.md`, `CHANGELOG.md`, ADR si P1 l'exige

## Critères d'acceptation

- [x] Après N échecs consécutifs, le débit d'essais possibles est **divisé par au moins un
      ordre de grandeur**, chiffres à l'appui — **facteur 120** en régime établi (1/min contre
      120/min), **27** sur la fenêtre de 150 s
- [x] ⛔ Un jeton valide émis **avant** le ralentissement continue de fonctionner pendant
      celui-ci — testé **sur matériel, porte fermée** : page protégée 200, changement de mot de
      passe 200
- [x] Le propriétaire qui connaît son mot de passe **reprend la main sans intervention**,
      après un délai borné et annoncé — 11 réouvertures relevées, délai **plafonné à 60 s** et
      annoncé par `Retry-After`
- [x] Aucun handler ne bloque > 50 ms — **16 à 25 ms** mesurés bout en bout, réseau compris
- [x] `pio run` sans nouveau warning ; tests natifs verts

## Notes pour agent

- ⚠️ **Ne pas confondre avec le quota existant.** `kMaxRequestsPerMinute` protège l'appareil
  de la **charge**. Ce lot protège le **mot de passe**. Les deux coexistent.
- ⛔ **Le pire résultat possible de ce lot serait un appareil inaccessible à son
  propriétaire.** Relire la contrainte rédhibitoire ci-dessus avant chaque décision.
- ⚠️ L'attaquant peut changer d'IP. Un mécanisme **uniquement** par IP sera contourné :
  dire explicitement ce que la conception retenue couvre et ce qu'elle ne couvre pas,
  plutôt que de laisser croire à une fermeture complète.

## Hors périmètre

- Le mot de passe en clair sur le Wi-Fi — décision produit assumée, ⛔ pas de spec.
- Le chiffrement de la NVS → [`feature-192`](../todo/feature-192-chiffrement-de-la-nvs-pour-l-appareil-vole.md).
- La rétention des traces → [`feature-172`](../todo/feature-172-retention-protegee-des-traces-de-securite.md).
