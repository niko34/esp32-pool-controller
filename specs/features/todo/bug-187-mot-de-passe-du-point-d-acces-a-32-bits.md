# bug-187 — Le mot de passe du point d'accès ne fait que 32 bits, dans un alphabet connu

## Métadonnées
| | |
|---|---|
| Type | bug — 🔴 **sécurité, provisionnement Wi-Fi** |
| Statut | todo |
| Priorité | normale — ⚠️ **touche la livraison client**, pas l'appareil de l'utilisateur en service |
| Version cible | à définir |
| Relevé par | `security-reviewer` (P2 de [`bug-181`](../done/bug-181-get-config-expose-la-configuration-sans-authentification.md)), 2026-11-17 — ⭐ **vérifié dans le code** le même jour |
| Décision | l'utilisateur a demandé le 2026-11-17 de **garder le sujet en attente**, plutôt que de l'abandonner |

## Domaines touchés
- [x] **Authentification** (`src/auth.cpp`) · [x] Provisionnement Wi-Fi (`src/main.cpp`)
- [ ] Capteurs — [ ] Régulation — [ ] Filtration — [ ] MQTT — [ ] Frontend

## Agents requis
- [x] **`security-reviewer`** — ⛔ OBLIGATOIRE (c'est l'objet) · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer

## Le fait, mesuré dans le code

`src/auth.cpp:108-110` :
```cpp
String apPwd = generateRandomToken().substring(0, 8);
apPwd.toUpperCase();
```
`generateRandomToken()` (`:162-172`) produit **32 caractères hexadécimaux** depuis `esp_random()`
— la source d'aléa est bonne. ⛔ **Mais on n'en garde que 8**, soit **4 octets = 32 bits**.
`toUpperCase()` ne change rien à l'entropie (l'hexadécimal est insensible à la casse).

⇒ **La longueur et l'alphabet sont publics** (ils sont dans ce dépôt) : un attaquant n'a aucune
incertitude à lever. L'espace à parcourir est **exactement** 16⁸ = 2³², hors ligne, après capture
d'une connexion au point d'accès. C'est à portée d'une carte graphique du commerce.

## ⚠️ Ce qui relativise — à ne pas perdre de vue

- Le point d'accès n'est **allumé qu'à l'installation**, ou en repli quand l'appareil perd le
  Wi-Fi. Un appareil en service n'expose rien.
- Depuis `bug-185`, celui qui entre ⛔ **ne peut plus prendre l'appareil** : le droit anonyme se
  limite à choisir un mot de passe **sur un appareil vierge**.
- Ce qu'il peut faire : `/wifi/connect` et `/wifi/disconnect` sont ouverts **sans jeton** quand
  `wifiConfigAllowed()` ⇒ **sortir l'appareil du réseau du client et le rattacher au sien**.

## Comportement attendu
Un mot de passe de point d'accès dont l'espace de recherche met la capture hors de portée.
⚠️ **Contrainte produit** : il doit rester **recopiable à la main** par un installateur — c'est
pourquoi il est aujourd'hui court et en majuscules. ⛔ Arbitrer longueur **contre** lisibilité,
ne pas rallonger sans regarder l'étiquette.

## Lié
- ⚠️ `src/auth.cpp:112-115` journalise ce mot de passe **en clair** à chaque génération, et
  `main.cpp:854` **à chaque démarrage** en mode AP. Volontaire (« seul moyen de le récupérer sans
  écran »), mais à réexaminer dans le même lot.
- [`bug-186`](bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md) — tout essai
  demande un appareil en état vierge.

## Critères d'acceptation
- [ ] L'espace de recherche est porté hors de portée d'une attaque hors ligne, **chiffré dans la fiche**
- [ ] Le mot de passe reste recopiable à la main (dire combien de caractères, et pourquoi)
- [ ] Le sort de la journalisation en clair est **tranché par écrit**
- [ ] Un appareil déjà installé **ne change pas** de mot de passe de point d'accès sans raison
