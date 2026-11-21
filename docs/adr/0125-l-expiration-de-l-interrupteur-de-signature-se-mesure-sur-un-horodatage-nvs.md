# ADR-0125 — L'expiration de l'interrupteur de signature se mesure sur un horodatage NVS, jamais sur `millis()`

- **Statut** : Accepté
- **Date** : 2026-11-16
- **Décideurs** : Nicolas + Claude (`architect`, `security-reviewer` P1 puis P2 — GO sous conditions C1-C12 —, `code-reviewer`)
- **Spec(s) liée(s)** : [`feature-151`](../../specs/features/done/feature-151-signature-de-l-image-ota.md) (sous-lot D3-b2 « les gardes inertes », B11 et condition 7 ; amende [ADR-0124](0124-le-mode-degrade-n-est-pas-un-mode-la-porte-de-sortie-du-serveur-web.md) sans le modifier)

## Contexte

D3-c voudra pouvoir **lever l'exigence de signature** pendant une fenêtre courte (récupération d'un appareil dont la chaîne de signature est cassée). La spec (condition 7) impose un interrupteur **borné** : expire au premier flash réussi **ou** après 60 min, relu à chaque début d'envoi, jamais armé automatiquement. D3-b2 livre ce mécanisme **inerte** : il décide et journalise, il ne refuse ni n'accepte rien (l'appareil reste en mode D1).

Il faut décider **comment mesurer les 60 min**. `millis()` est le réflexe, et il est faux ici : il repart de zéro à chaque redémarrage, donc un redémarrage **prolonge** la fenêtre (et un mot de 32 bits se replie à 49,7 j — c'est le défaut déjà rencontré dans ce projet). Or l'interrupteur doit survivre à un redémarrage **sans** allonger sa durée.

## Décision

1. **L'échéance est un horodatage epoch UTC écrit en NVS** (clé `sigsw`, U32, namespace `pool-ota`). L'armement est **dérivé** : clé présente **et** heure valide **et** `now < échéance` **et** `échéance − now ≤ kOtaSignatureSwitchWindowSec` (3600 s). ⛔ Jamais `millis()`.
2. **Fonction de décision unique et pure** (`decideOtaSignatureSwitch`, ADR-0017), **fail-closed** : tout ce qui n'est pas une échéance **positivement lue** rend « signature exigée » — pas de clé, NVS illisible, pas d'heure, expiré, échéance nulle ou **hors fenêtre** (borne haute : une horloge fausse vers le futur à l'armement ne doit pas produire un armement durable), verrou RAM posé.
3. **Une seule clé**, pas un couple `armed` + `expiry` : un état incohérent est ainsi **inexprimable**. Le miroir RAM est un **seul** `std::atomic<uint32_t>` pour la même raison.
4. **Jamais armé automatiquement** (un seul appelant de `Arm`, la route `CRITICAL`), **jamais dans la page de secours**, **consommé** au premier flash réussi, **décision capturée au début de session** et jamais relue pour accorder (C8).
5. **L'armement exige l'authentification active** : `403` si elle est coupée ou au premier démarrage — `checkAuth()` rend `true` auth coupée et ne distingue pas `CRITICAL` de `WRITE`.
6. Le témoin MQTT est **informatif**, jamais une preuve (voir Conséquences).

### Amendement à ADR-0124 (consigné ici ; ADR-0124 n'est pas modifié)

`src/ota_recovery_page.cpp`, fichier **généré et versionné** (ADR-0124 § 3), **dépend désormais de `version.h`** : la page de secours affiche le numéro de version. Règle ajoutée : ⛔ **le générateur n'émet jamais la VALEUR**, seulement le **jeton** `FIRMWARE_VERSION` dans une concaténation de littéraux (`R"…(…)…" FIRMWARE_VERSION R"…(…)…"`), résolu par le compilateur. Sinon le contrôle de fraîcheur (`check_recovery_page_generated.sh`, re-génère puis `diff`) rougirait à **chaque** bump de version, et le fichier généré contiendrait un nombre qui peut diverger du firmware. Conséquences conservées : `sizeof` exact, réponse zéro copie (`AsyncProgmemResponse`). Le générateur refuse toute version hors `^[0-9]+\.[0-9]+\.[0-9]+$`.

## Alternatives considérées

- **`millis()`** (rejetée) — un redémarrage prolonge la fenêtre ; repliement à 49,7 j ; l'échéance ne survit pas à la coupure.
- **Couple `armed:bool` + `expiry`** (rejetée) — état incohérent possible (armé sans échéance, échéance sans armé).
- **Trois atomiques pour le miroir RAM** (rejetée par la revue P1) — recrée l'incohérence écartée côté NVS.
- **Stockage via `config.cpp` / `poolctrl`** (rejetée) — mutex interdit depuis `async_tcp`.
- **`RTCManager::isTimeValid()` pour l'horloge** (rejetée) — prend le mutex I2C depuis `async_tcp` ; `time(nullptr) ≥ kMinValidEpoch` suffit.
- **Bouton dans l'interface / `switch` Home Assistant** (rejetée) — ce serait précisément le geste qu'un porteur de jeton volé cherche, ou une surface d'armement hors jeton.
- **Valeur de version écrite par le générateur** (rejetée) — voir amendement.

## Conséquences

### Positives
- La fenêtre est **bornée en temps réel**, redémarrage compris ; `/factory-reset` (`nvs_flash_erase()`) ramène à « exigée » sans une ligne de code.
- Décision testable en natif, l'appelant audité (faux `Preferences`, script CI).

### Négatives / dette assumée
- **Résiduel** : un `remove()` NVS en échec laisse une échéance armée jusqu'à ≤ 60 min (répondu `500` + CRITICAL, `rearms_on_reboot` / `nvs_residual_until`, `null`/`"unknown"` = inconnu).
- Dépend d'une **heure valide** ; sans elle l'armement répond `409`.
- 🔴 **Le témoin MQTT n'est PAS fiable comme preuve** : un firmware hostile lit les identifiants MQTT en NVS et peut republier `OFF` ; un courtier sans persistance perd le `retain` ; hors ligne la valeur reste périmée ; MQTT désactivé, le témoin n'existe pas. **Seule la TRANSITION historisée par un tiers survit** — d'où l'entité Home Assistant `binary_sensor` (le `recorder` de HA l'historise hors de portée de l'appareil).
- 🔴 **Cette feature ne ferme PAS le vecteur « jeton admin volé », et l'interrupteur le ROUVRE** : le jeton est statique et en clair, donc rejouable ; qui le détient peut **armer puis envoyer** — **deux requêtes au lieu d'une**. Le durcissement (`bug-112` avant D3-c, `CRITICAL` réellement distinct de `WRITE`, bouton physique après D5) reste à faire.
- Le lot est inerte : la valeur de sécurité n'existe qu'une fois D3-c branché.

### Ce que ça verrouille
- Toute évolution de la fenêtre garde un **horodatage persistant** ; ajouter un refus fondé sur `millis()` est exclu.
- `otaSignatureSwitchArm` n'a **qu'un** appelant ; `Peek` n'est lu que par `mqtt_manager.cpp` (outillé : `tools/check_ota_signature_switch.sh`).
- Le fichier généré de la page de secours ne contient jamais la valeur de version.

## Références

- Code : [`src/ota_signature_switch_logic.cpp`](../../src/ota_signature_switch_logic.cpp), [`src/ota_signature_switch.cpp`](../../src/ota_signature_switch.cpp), [`src/web_routes_ota_switch.cpp`](../../src/web_routes_ota_switch.cpp), [`tools/gen_recovery_page.js`](../../tools/gen_recovery_page.js)
- Spec : [`feature-151`](../../specs/features/done/feature-151-signature-de-l-image-ota.md)
- Doc : [`docs/subsystems/ota-manager.md`](../subsystems/ota-manager.md#interrupteur-de-signature-feature-151-d3-b2), [`docs/MQTT.md`](../MQTT.md#topics-et-entité-ajoutés-en-feature-151-d3-b2-interrupteur-de-signature-ota)
