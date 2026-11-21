# ADR-0136 — Assainir le texte externe à la source, jamais dans le journal

- **Statut** : Accepté
- **Date** : 2026-11-21
- **Décideurs** : Nicolas + Claude (`architect`, `code-reviewer`)
- **Spec(s) liée(s)** : [bug-179](../../specs/features/done/bug-179-journal-rendu-en-innerhtml-sous-csp-permissive.md)

## Contexte

Mesuré sur l'appareil le 2026-11-21 : un `curl` anonyme sur `/get-config/<b>X</b>` rend 401 (correspondance de préfixe) et `auth.cpp` journalisait l'URL brute. Le journal étant rendu en HTML chez l'administrateur, n'importe qui sur le réseau local pouvait déposer du balisage, sous une CSP qui garde `'unsafe-inline'`. Même famille d'entrées : charges MQTT, SSID, nom de fichier d'une mise à jour. Il faut décider **où** nettoyer.

## Décision

Tout texte choisi par un tiers est assaini **par l'appelant, avant** l'appel à `systemLogger`, via `logSanitizeExternalText()` (`src/log_text_sanitize_logic.h`) : liste blanche positive, le reste en `%XX`, `%` en `%25` (réversible), borne `kLogExternalTextMax = 96` et suffixe `~<longueur>o`, tampon de pile, zéro allocation. Le journal et le logger ne nettoient rien. Le rendu de l'interface est aussi en DOM (défense indépendante).

## Alternatives considérées

- **Nettoyer à l'enregistrement dans le logger** (rejetée) : `Logger::log` capture les **pointeurs de l'appelant avant le mutex** (`src/logger.cpp:111-114`) et les transmet tels quels au callback WebSocket. L'arène aurait été propre, la **trame WebSocket** non : 5e récidive du piège de parité entre deux chemins qui devaient dire la même chose.
- **Échapper en HTML à l'affichage seulement** (rejetée) : laisse le texte vérolé dans le fichier téléchargé et tout outil tiers.
- **Retirer `'unsafe-inline'` de la CSP** (rejetée dans ce lot) : 4 scripts en ligne et 3 `onclick=` en dépendent ; risque de rendre l'appareil inatteignable. Fiche de suivi.

## Conséquences

### Positives
- Journal, WebSocket, `/get-logs` et `/download-logs` portent la même forme sûre.
- Réversible : la valeur d'enquête est conservée.

### Négatives / dette assumée
- Lisibilité : `Livebox-Café` s'écrit `Livebox-Caf%C3%A9` dans le journal.
- Garde partielle : la règle 6 ne voit que les valeurs écrites dans l'appel ; la 6b ne couvre que `auth.cpp`. Un nouveau site oublié n'est pas protégé.
- Restent non assainies les valeurs journalisées exigeant des identifiants (fiche de suivi).

### Ce que ça verrouille
- Tout nouveau `systemLogger.*` recevant du texte externe passe par le helper (identifiant préfixé `safe`).
- Dépend d'[ADR-0082](0082-arene-circulaire-du-journal-en-ram-interne-statique.md) (le journal n'alloue pas) et le renforce : l'assainissement se fait sur la pile de l'appelant.

## Références

- Code : `src/log_text_sanitize_logic.h`, `src/auth.cpp`, `src/mqtt_manager.cpp`, `src/logger.cpp:111`
- Gardes : `tools/check_unauth_log_guard.sh` (règles 6, 6b), `tools/check_no_html_interpolation.sh`
- Doc : [logger.md](../subsystems/logger.md), [auth.md](../subsystems/auth.md)
