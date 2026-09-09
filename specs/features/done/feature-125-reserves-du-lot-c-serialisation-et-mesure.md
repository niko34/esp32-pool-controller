## Métadonnées
- **ID** : feature-125
- **Slug** : reserves-du-lot-c-serialisation-et-mesure
- **Type** : **RÉSERVE** — ⛔ **NE PAS OUVRIR** sans qu'une condition de réouverture ci-dessous soit constatée
- **Statut** : todo (réserve)
- **Priorité** : **aucune** — le problème qu'elles visaient est résolu depuis le 09/09/2026
- **Version cible** : N/A
- **Dernière mise à jour** : 2026-09-09
- **Spec(s) liée(s)** : `bug-080` lot C (résolu par S7), `feature-124` (S7, le correctif qui les a rendues inutiles), `feature-123` (gelée)

## Pourquoi cette spec existe

Deux chantiers ont été **conçus, chiffrés, puis rendus inutiles** par le succès de
`feature-124` (S7). Ils étaient consignés à la **ligne 1935 d'une spec de 1984
lignes** — donc introuvables au moment précis où l'on en aurait besoin.

Cette spec n'est **pas du travail à faire**. Elle existe pour qu'une condition de
réouverture, si elle se produit, tombe sur un document qu'on trouve en listant
`todo/`.

## Le problème qu'elles visaient — RÉSOLU

Une rafale de ~6 requêtes parallèles fragmentait le tas interne 8 bits. Le plus
gros bloc libre tombait à **1 076 o**, sous les **1 622 o** du tampon TX WiFi :
d'où les échecs d'allocation, les refus 503 (« mémoire insuffisante ») et les
déconnexions MQTT.

**Résolu le 09/09/2026** par S7 (`feature-124`, seuil PSRAM 4 095 → 1 024) :
plancher passé à **8 692 o** sous une rafale **deux fois plus forte**, zéro refus,
zéro déconnexion MQTT, un seul échec d'allocation contre 25.

## Domaines touchés (si réouverture)
- [x] Web — routes HTTP, service des fichiers statiques
- [x] Politique d'allocation / diagnostic mémoire
- [ ] tout le reste

## Agents requis (si réouverture)
- [x] spec-reader · [x] architect · [x] pool-chemistry · [x] web-ui-developer
- [x] test-engineer · [x] code-reviewer · [x] doc-writer

---

## 🅢2 — SÉRIALISER LE SERVICE DES FICHIERS STATIQUES

**Ce que c'est** : un gestionnaire placé devant `serveStatic` qui n'autorise que
**2 ou 3** fichiers servis à la fois et met les autres en file d'attente. Le
navigateur en demande jusqu'à **6 en parallèle**, et l'appareil les accepte tous.

**Pourquoi ça marcherait** : moins de connexions simultanées ⇒ moins de structures
lwIP vivantes en même temps ⇒ moins de fragmentation.

**Coût** : 1 à 2 s de latence supplémentaire au chargement de page sur un réseau
local, et du vrai code — une file d'attente, avec son risque classique de délais
d'expiration mal gérés.

**Pourquoi c'est redondant aujourd'hui** — les deux scénarios attaquent le même
produit, par ses deux facteurs :

```
fragmentation  ≈  nombre de connexions  ×  coût interne par connexion
                          ↑                          ↑
                     ce que S2 réduit         ce que S7 a réduit
```

S7 a réduit le **coût par connexion** en déportant les structures lwIP en PSRAM.
Réduire aussi le **nombre** serait ceinture et bretelles, payé en latence pour une
marge devenue inutile.

### ⚠️ Conditions de réouverture — la marge tient à 24 OCTETS

Un échec d'allocation subsiste après S7 : demande de **1 612 o** refusée avec un
plus gros bloc à **1 588 o**. La marge est positive, mais **étroite**.

Rouvrir si **l'une** de ces trois conditions est constatée :

1. **Plus de connexions simultanées** — plusieurs navigateurs ouverts, clients
   WebSocket supplémentaires, application mobile en parallèle de l'interface web.
2. **Un firmware plus bavard** — ajout d'entités Home Assistant, de champs WS, de
   routes : tout ce qui augmente le trafic par chargement de page.
3. **Un réseau plus lent** — les connexions restent ouvertes plus longtemps, donc
   le nombre de structures vivantes **à un instant donné** augmente, même à
   nombre de requêtes égal.

**Comment le constater** : `hf.i8.mb` (plancher fin du plus gros bloc) redescend
sous **1 622 o**, ou le compteur `cumul` d'échecs en `caps=0x080C` repart. Les deux
se lisent dans `GET /get-system-info?hf=1` et dans le journal.

---

## 🅢 MESURE DU FRAGMENTEUR

**Ce que c'était** : instrumenter les **tailles** d'allocation interne pendant une
rafale, pour **nommer** ce qui remplit le tas. On savait ce qui n'était pas
coupable — les tampons stdio de 4 096 o, prouvés partir en PSRAM par le témoin
d'A7 — mais le vrai consommateur restait anonyme.

**Pourquoi c'est répondu** : la réponse est arrivée par le résultat de S7. Déplacer
la classe **[1 025 ; 4 095]** vers la PSRAM produit un **facteur 8** sur le
plancher. Le fragmenteur était donc **dans cette classe** — et la seule chose qui
y grandit avec le nombre de requêtes simultanées, ce sont les **structures lwIP,
une par connexion TCP** (`lwipopts.h` : `mem_clib_malloc = malloc`,
`MEM_LIBC_MALLOC=1`, `MEMP_MEM_MALLOC=1`, car
`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` n'est pas activé). La mesure ne
confirmerait que ce que le résultat démontre.

### Condition de réouverture

**Si le plancher redescend sans explication.** Elle nommerait alors le **nouveau**
consommateur — celui qu'on ne connaît pas encore, par définition.

---

## ⚠️ RÉSERVE À SURVEILLER — un effet de S7 qui n'a PAS été mesuré

S7 a déporté les structures TCP en **PSRAM**, qui est plus lente (bus SPI, cache,
et contournement MEMW actif).

**Le traitement réseau est donc légèrement ralenti, et cela n'a pas été mesuré.**
L'utilisateur n'a rien perçu pendant le test de reproduction du 09/09, mais une
absence de perception ne vaut pas mesure.

**À instruire si** : le service web paraît mou, un chargement de page s'allonge
sensiblement, ou une latence apparaît sur les commandes MQTT / WebSocket. Ce
serait le **prix** de S7, et il faudrait alors arbitrer entre ce prix et un retour
à un seuil plus haut — auquel cas S2 redeviendrait la voie à privilégier, puisque
lui ne déporte rien.

## Notes

⛔ **Ne pas convertir cette spec en travail** sans avoir constaté une condition.
Le problème visé est résolu, mesuré, et documenté dans `feature-124`. Ouvrir S2
« pour faire propre » coûterait de la latence de chargement à tous les usages pour
protéger une marge dont on n'a pas besoin.

À l'inverse, **si une condition se produit, ne pas repartir de zéro** : tout le
raisonnement est ici, et le chiffrage de S7 (avant/après, 1 076 → 8 692) fournit
la référence.
