#include "auth.h"
#include "logger.h"
#include "config.h"
#include <esp_random.h>
#include "auth_log_throttle_site.h"
#include "log_text_sanitize_logic.h"  // bug-179
#include "auth_admission_logic.h"
#include "auth_bootstrap_logic.h"
#include "web_routes_auth.h"  // bug-189 : authFailBackoffClearExemptions
#include "secret_compare.h"   // bug-183 : fixedWidthEquals (pur)
#include "ap_password_logic.h" // bug-187 : fabrication et contrôle des mots de passe (pur)
#include <bootloader_random.h>  // bug-187 : vraie entropie AVANT le démarrage de la radio
#include "ota_integrity.h"    // bug-183 : OtaStreamHasher (porte déjà le garde mbedtls 2/3)
#include <mbedtls/platform_util.h>

// ---------------------------------------------------------------------------
// Bornage du journal des refus d'authentification (bug-112)
// ---------------------------------------------------------------------------
// DEUX sites DISTINCTS, jamais fusionnés : 401 (sendAuthRequired) et 429
// (checkRateLimit). Le throttle ne borne QUE le journal : le 401, le 429 et le
// `return false` partent toujours, ce helper est `void` et ne décide de rien.
// La coquille atomique (auth_log_throttle_site.h) porte les champs partagés avec
// la loop (drain) ; le module pur ne voit que des valeurs.
namespace {
AuthLogThrottleSite gUnauthSite;  // 401 : UNE clé GLOBALE pour les 86 routes (ADR-0127)
AuthLogThrottleSite gRateSite;    // 429

// Complément « et N refus non journalisés (autres routes/IP) », accord français géré.
void appendOcculted(String& msg, const AuthLogSiteReport& r) {
  msg += String(" ; et ") + String(r.occulted) + " " +
         authLogThrottlePlural(r.occulted, "refus non journalisé", "refus non journalisés");
  if (r.otherRoutes) msg += " (autres routes)";
  if (r.otherIps) msg += " (autres IP)";
}

// kind 0 : « Accès non autorisé » (401) ; kind 1 : « Rate limit dépassé » (429).
// INVARIANT (ADR-0082) : la sortie muette est la PREMIÈRE décision, avant toute
// String (IP, URL). Le hachage lit url().c_str() (référence, pas de copie) et
// l'IPv4 est lue brute.
void logUnauthWarn(AuthLogThrottleSite& site, AsyncWebServerRequest* req, uint8_t kind, uint16_t reqCount) {
  const bool hasClient = (req != nullptr && req->client() != nullptr);
  const uint32_t ip = hasClient ? static_cast<uint32_t>(req->client()->remoteIP()) : 0;
  const uint32_t urlHash = (req != nullptr) ? authLogThrottleHash(req->url().c_str()) : 0;

  const AuthLogSiteReport r = authLogSiteAttempt(site, millis(), urlHash, ip, kUnauthWarnThrottleMs,
                                                 kUnauthWarnMaxShift, kUnauthWarnDecayMs);
  if (!r.emit) return;  // MUET : zéro allocation

  String msg;
  const String clientIp = hasClient ? req->client()->remoteIP().toString() : String("IP inconnue");
  if (kind == 0) {
    // bug-179 : l'URL est du texte externe (déjà décodée pourcent par la
    // bibliothèque) -> assainie sur la pile, APRÈS la sortie muette. L'IP n'en est pas.
    char safeUrl[kLogExternalTextBufBytes];
    if (req != nullptr) {
      const String& u = req->url();
      logSanitizeExternalText(u.c_str(), u.length(), safeUrl, sizeof(safeUrl));
    } else {
      safeUrl[0] = '?'; safeUrl[1] = '\0';
    }
    msg = "Accès non autorisé depuis " + clientIp + " vers " + safeUrl;
  } else {
    msg = "Rate limit dépassé pour " + clientIp + " (" + String(reqCount) + " req/min)";
  }
  if (r.occulted > 0) appendOcculted(msg, r);
  systemLogger.warning(msg);
}

// Drain d'un site (loop) : borné par le MÊME recul que l'émission (coquille).
void drainUnauthSite(AuthLogThrottleSite& site, const char* label, uint32_t now) {
  const AuthLogSiteReport r = authLogSiteDrain(site, now, kUnauthWarnThrottleMs, kUnauthWarnMaxShift,
                                               kUnauthWarnDrainQuietMs);
  if (!r.emit) return;
  String msg = String(label) + " :";
  appendOcculted(msg, r);
  systemLogger.warning(msg);  // WARNING seulement : ERROR/CRITICAL flushent en synchrone
}
}  // namespace

void AuthManager::drainUnauthWarnLogs() {
  const uint32_t now = millis();
  drainUnauthSite(gUnauthSite, "Accès non autorisé", now);
  drainUnauthSite(gRateSite, "Rate limit dépassé", now);
}

AuthManager authManager;

// SÉCURITÉ: Comparaison à temps constant (feature-028).
// L'opérateur == sur String court-circuite au premier octet différent, ce qui
// rend le temps de réponse dépendant du préfixe correct du token fourni
// (attaque temporelle sur le contrôle d'accès central). Ici, on compare
// TOUS les octets sans court-circuit en accumulant les différences par OR.
// La vérification de longueur en amont est acceptable : la longueur du token
// n'est pas secrète (format fixe, 32 caractères hexadécimaux).
static bool constantTimeEquals(const String& a, const String& b) {
  if (a.length() != b.length()) {
    return false;
  }
  uint8_t diff = 0;
  for (unsigned int i = 0; i < a.length(); i++) {
    diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
  }
  return diff == 0;
}

bool AuthManager::secureTokenEquals(const String& candidate) const {
  return constantTimeEquals(candidate, apiToken);
}

// bug-183 : comparaison du mot de passe sans court-circuit. SHA-256 des DEUX côtés
// (contextes et empreintes sur la pile), puis OR des XOR sur 32 octets : le temps
// ne dépend plus de la relation entre candidat et secret. Il dépend encore de la
// longueur de chacun, mais c'est un décalage constant, identique à chaque requête.
// ⛔ Effacement par mbedtls_platform_zeroize : un memset nu en fin de portée est
// supprimé par le compilateur.
//
// ⚠️ CE QUE CET EFFACEMENT NE PROTÈGE PAS — à lire avant de s'en croire couvert.
// Il n'efface que les DEUX EMPREINTES de cette pile. Le mot de passe lui-même reste
// en clair ailleurs, et ce lot n'y change rien : membre `adminPassword` (tas), copie
// dans `authCfg.adminPassword`, NVS non chiffrée, et le corps HTTP reçu. ⇒ Un
// attaquant qui lit la mémoire ou démonte la flash l'obtient malgré cet appel.
// Le but est étroit et assumé : ne pas laisser traîner d'empreinte du secret dans
// une pile réutilisée par la requête suivante. Le chiffrement de la NVS est un
// chantier distinct, non ouvert.
bool AuthManager::passwordMatches(const String& candidate) const {
  uint8_t dCandidate[32];
  uint8_t dSecret[32];
  {
    OtaStreamHasher h;  // le destructeur libère (et efface) le contexte
    h.begin();
    h.update(reinterpret_cast<const uint8_t*>(candidate.c_str()), candidate.length());
    h.finish(dCandidate);
  }
  {
    OtaStreamHasher h;
    h.begin();
    h.update(reinterpret_cast<const uint8_t*>(adminPassword.c_str()), adminPassword.length());
    h.finish(dSecret);
  }
  const bool same = fixedWidthEquals(dCandidate, dSecret, sizeof(dCandidate));
  mbedtls_platform_zeroize(dCandidate, sizeof(dCandidate));
  mbedtls_platform_zeroize(dSecret, sizeof(dSecret));
  return same;
}

AuthManager::AuthManager() {}

void AuthManager::begin() {
  // Générer un token API par défaut si vide
  // ⛔ MÊME SOURCE D'ENTROPIE QUE LA CLÉ WI-FI (relevé en revue de code). Le jeton
  // d'API est la preuve d'admission de TOUTES les routes protégées : le tirer hors de
  // la fenêtre de vraie entropie, juste au-dessus d'une clé Wi-Fi qu'on y encadre
  // soigneusement, serait une incohérence dans une seule et même fonction.
  // ⛔ Sûr ICI seulement : la radio n'est pas démarrée, et aucun code de ce dépôt
  // n'utilise l'ADC (vérifié : aucun analogRead, aucun adc1_*).
  bootloader_random_enable();
  if (apiToken.isEmpty()) {
    apiToken = generateRandomToken();
    // SÉCURITÉ: Ne jamais logger le token complet
    String maskedToken = apiToken.length() > 8 ? (apiToken.substring(0, 8) + "...") : "***";
    systemLogger.info("API Token généré: " + maskedToken);
  }

  // ===========================================================================
  // bug-187 — MISE EN SERVICE : fabrication du mot de passe du point d'accès.
  // ===========================================================================
  // ⛔ N'ARRIVE QU'UNE FOIS dans la vie de l'appareil. Ce mot de passe part sur
  // l'ÉTIQUETTE du boîtier, et il est conservé À PART (apPasswordOrigin) pour que
  // la réinitialisation d'usine puisse toujours le ramener — c'est ce qui rend
  // l'étiquette vraie à vie, même après que l'utilisateur a choisi le sien.
  //
  // ⚠️ Journalisé EN CLAIR, délibérément : à cet instant aucune étiquette n'existe
  // encore et aucun écran n'est joignable. Le câble série est le seul chemin.
  // Décision écrite dans la fiche bug-187, § « le sort de la journalisation ».
  if (authCfg.apPassword.isEmpty()) {
    char fabrique[kApPwdGenLength + 1];
    // ⛔ SOURCE D'ENTROPIE (condition A1 de la revue de sécurité). `esp_random()` n'est
    // garanti VRAIMENT aléatoire que si la radio ou l'ADC alimente le générateur
    // matériel — or begin() s'exécute bien AVANT setupWiFi(). Sans cet encadrement,
    // les 73,5 bits annoncés ne seraient qu'une affirmation : la graine viendrait du
    // bootloader, donc d'un état bien moins imprévisible.
    // ⚠️ Et cette valeur est DÉFINITIVE : c'est l'origine, celle de l'étiquette, celle
    // que la réinitialisation d'usine ramènera toujours. Un tirage faible ici ne se
    // rattrape jamais.
    // ⛔ Sûr ICI et nulle part ailleurs : la radio n'est pas démarrée (conflit interdit)
    // et aucun code de ce dépôt n'utilise l'ADC (vérifié : aucun analogRead, aucun adc1_*).
    const bool ok = apPwdGenerate([]() { return static_cast<uint8_t>(esp_random() & 0xFF); },
                                  fabrique, sizeof(fabrique));
    if (ok) {
      authCfg.apPassword = fabrique;
      authCfg.apPasswordOrigin = fabrique;
      saveMqttConfig();
      // ⛔ RELIRE AVANT DE DIRE « notez-le » (condition G3 de la revue P2).
      // saveMqttConfig() est `void` et peut échouer en SILENCE. Sans cette relecture,
      // l'installateur recopierait sur l'étiquette une valeur que le démarrage suivant
      // ne retrouverait pas — il en fabriquerait une autre, et l'étiquette serait fausse
      // DÈS LE PREMIER JOUR, sans aucun signal. C'est l'invariant même du lot.
      if (reloadApPasswordFromNvs() == authCfg.apPassword) {
        systemLogger.info("================================================");
        systemLogger.info("=== MOT DE PASSE AP WIFI: " + authCfg.apPassword + " ===");
        systemLogger.info("=== Notez-le sur une etiquette !            ===");
        systemLogger.info("================================================");
      } else {
        systemLogger.critical("[AP] Le mot de passe du point d'acces n'a PAS ete enregistre "
                              "en memoire persistante : NE PAS l'ecrire sur l'etiquette, "
                              "il changera au prochain demarrage. Redemarrer l'appareil.");
      }
    } else {
      // ⛔ LE PIRE CAS, ET IL EST SILENCIEUX SANS CETTE LIGNE : laisser le mot de
      // passe VIDE ferait démarrer le point d'accès SANS CHIFFREMENT (AP.cpp ne
      // pose WPA2 que si la phrase est non vide), et /wifi/connect est ouvert sans
      // jeton en mode AP. On préfère un point d'accès qui ne démarre pas.
      systemLogger.critical("[AP] Fabrication du mot de passe du point d'acces IMPOSSIBLE "
                            "(source d'alea defaillante) : le point d'acces NE DEMARRERA PAS. "
                            "Redemarrer l'appareil.");
    }
  } else if (authCfg.apPasswordOrigin.isEmpty()) {
    // bug-187 — MIGRATION, une seule fois : un appareil mis en service avant ce lot
    // n'a pas d'origine enregistrée. ⭐ Son mot de passe COURANT est forcément celui
    // de l'étiquette, puisque RIEN ne permettait de le changer avant aujourd'hui.
    // ⇒ On l'adopte comme origine. L'étiquette reste donc vraie, sans rien régénérer.
    authCfg.apPasswordOrigin = authCfg.apPassword;
    saveMqttConfig();
    systemLogger.info("[AP] Mot de passe d'origine du point d'acces enregistre "
                      "(reprise de la valeur en service, bug-187)");
  }
  // ⛔ FERMER LA FENÊTRE. Laisser la source matérielle active consommerait l'ADC et
  // entrerait en conflit avec le démarrage de la radio, quelques lignes plus loin dans
  // setup(). Les deux tirages sensibles — jeton d'API et clé Wi-Fi — sont derrière.
  bootloader_random_disable();

  // Détecter premier démarrage (bug-185) : l'état se déduit du MOT DE PASSE, pas
  // du seul drapeau wizardCompleted (qu'un inconnu pouvait poser). Migration
  // auto-réparatrice : wizardCompleted vrai + mot de passe d'usine => l'assistant
  // se rouvre, au lieu de laisser admin/admin à vie.
  isFirstBoot = computeFirstBoot(authCfg.wizardCompleted, isFactoryPassword());
  if (isFirstBoot) {
    if (adminPassword.isEmpty()) {
      adminPassword = "admin";
    }
    systemLogger.warning("SÉCURITÉ: Premier démarrage détecté - Configuration initiale requise !");
  }

  // bug-185 — ROTATION UNIQUE À LA MISE À JOUR. Corriger le code ne retire pas
  // les jetons DÉJÀ pris : avant ce lot, n'importe qui sur le réseau pouvait en
  // obtenir un pendant la fenêtre de premier démarrage, et rien ne l'invalidait
  // jamais — pas même un changement de mot de passe. ⇒ Sans ce passage, le
  // correctif ne protégerait que les appareils NEUFS.
  // ⚠️ Visible : toutes les sessions ouvertes retombent sur l'écran de connexion,
  // UNE fois.
  // ⛔ `wasPristine` se lit sur `authCfg.apiToken` (l'état NVS), ⛔ JAMAIS sur le
  // membre `apiToken` : celui-ci vient peut-être d'être généré quelques lignes
  // plus haut, donc il n'est JAMAIS vide ici — le tester rendait la garde morte
  // et faisait journaliser « jeton renouvelé par la mise à jour » sur un appareil
  // SORTI DU CARTON, où il n'y a eu ni mise à jour ni session à reconnecter.
  // (Relevé par code-reviewer : le commentaire d'origine expliquait vouloir
  // éviter ce mensonge, sans voir qu'il le produisait.)
  if (authCfg.authEpoch < kAuthEpoch) {
    const bool wasPristine = authCfg.apiToken.isEmpty();
    regenerateApiToken();          // pose apiToken (RAM)
    authCfg.apiToken = apiToken;   // ⛔ ET la NVS : sans cette ligne l'ANCIEN jeton
    authCfg.authEpoch = kAuthEpoch;//   ressusciterait au redémarrage suivant.
    saveMqttConfig();
    if (!wasPristine) {
      systemLogger.warning("SÉCURITÉ: jeton d'API renouvelé par la mise à jour — reconnexion requise (bug-185)");
    }
  }

  if (authEnabled) {
    systemLogger.info("Authentification activée (API Token X-Auth-Token uniquement)");
  } else {
    systemLogger.warning("Authentification désactivée - Mode ouvert !");
  }
}

String AuthManager::generateRandomToken() {
  // Générer un token de 32 caractères hexadécimaux
  String token = "";
  for (int i = 0; i < 16; i++) {
    uint8_t randomByte = esp_random() & 0xFF;
    char hex[3];
    sprintf(hex, "%02x", randomByte);
    token += hex;
  }
  return token;
}

// Invariant de concurrence (bug-189) : appelée UNIQUEMENT depuis la tâche async_tcp
// (handlers HTTP) ou setup() ; l'état de ralentissement qu'elle vide n'a donc pas de verrou.
void AuthManager::setPassword(const String& pwd) {
  adminPassword = pwd;
  authFailBackoffClearExemptions();  // bug-189 : les preuves de l'ancien mot de passe tombent
  systemLogger.info("Mot de passe administrateur modifié");
  // Note: isFirstBoot n'est plus désactivé ici, il sera désactivé uniquement
  // quand le wizard est complété via completeFirstBoot()
}

String AuthManager::getApPassword() const {
  return authCfg.apPassword;
}

// bug-187 — le mot de passe de l'ÉTIQUETTE. ⛔ Ne sert QU'à la réinitialisation
// d'usine : il n'est exposé par aucune route, sans quoi afficher « l'ancien » à
// côté du nouveau annulerait tout l'intérêt d'en avoir choisi un autre.
String AuthManager::getApPasswordOrigin() const {
  return authCfg.apPasswordOrigin;
}

// bug-187 — l'utilisateur remplace le mot de passe du point d'accès par le sien.
// ⛔ NE TOUCHE JAMAIS À L'ORIGINE : c'est l'invariant de tout le lot. L'appelant a
// déjà validé la valeur (apPwdCheck) ; cette fonction refuse quand même une valeur
// trop courte, parce qu'une phrase de moins de 8 caractères fait refuser softAP()
// et qu'une phrase VIDE crée un réseau OUVERT.
// ⚠️ Écriture NVS puis RELECTURE (condition G3 de la revue P1) : si la NVS refuse
// l'écriture en silence, la RAM et la NVS divergeraient et l'utilisateur recopierait
// une valeur fausse qui disparaîtrait au redémarrage suivant.
bool AuthManager::setApPassword(const String& pwd) {
  if (pwd.length() < kApPwdMinLength || pwd.length() > kApPwdMaxLength) return false;

  // ⛔ MUTEX OBLIGATOIRE (relevé en revue de code). Avant ce lot, `apPassword` n'était
  // écrit qu'au démarrage, serveur arrêté. Il l'est désormais depuis la tâche HTTP,
  // alors que la boucle principale le LIT pour démarrer le point d'accès de secours
  // (`main.cpp`, repli après perte du Wi-Fi). Réaffecter un `String` libère son ancien
  // tampon : sans verrou, le lecteur peut copier depuis une zone libérée — phrase WPA2
  // corrompue, donc réseau de secours injoignable, précisément quand il sert.
  // ⚠️ Mutex RÉCURSIF : les prises imbriquées de saveMqttConfig() et de
  // reloadApPasswordFromNvs() restent valides depuis cette même tâche.
  if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(kConfigMutexTimeoutMs)) != pdTRUE) {
    systemLogger.error("[AP] Changement du mot de passe du point d'acces abandonne : "
                       "configuration occupee");
    return false;
  }

  const String ancien = authCfg.apPassword;
  authCfg.apPassword = pwd;
  saveMqttConfig();
  bool ok = (reloadApPasswordFromNvs() == pwd);
  if (!ok) {
    authCfg.apPassword = ancien;  // la NVS n'a pas pris : on ne ment pas à l'utilisateur
    saveMqttConfig();
    // ⚠️ RELIRE LA RESTAURATION AUSSI. Si la première écriture a pris mais que la
    // relecture a échoué, puis que la restauration échoue à son tour, la NVS garderait
    // le NOUVEAU mot de passe pendant que l'interface annonce que l'ancien reste en
    // service — exactement le mensonge que ce lot combat.
    if (reloadApPasswordFromNvs() == ancien) {
      systemLogger.error("[AP] Enregistrement du mot de passe du point d'acces REFUSE "
                         "par la memoire persistante : l'ancien reste en service");
    } else {
      systemLogger.critical("[AP] Etat INCERTAIN du mot de passe du point d'acces : ni le "
                            "nouveau ni l'ancien n'ont pu etre confirmes en memoire. "
                            "Verifier dans Parametres avant de se fier a l'etiquette.");
    }
  } else {
    systemLogger.info("[AP] Mot de passe du point d'acces modifie par l'utilisateur "
                      "(l'etiquette reste valable apres reinitialisation)");
  }
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
  return ok;
}

void AuthManager::setApiToken(const String& token) {
  apiToken = token;
  systemLogger.info("API Token modifié");
}

void AuthManager::regenerateApiToken() {
  apiToken = generateRandomToken();
  // SÉCURITÉ: Ne jamais logger le token complet
  String maskedToken = apiToken.length() > 8 ? (apiToken.substring(0, 8) + "...") : "***";
  systemLogger.info("Nouveau API Token généré: " + maskedToken);
}

bool AuthManager::checkTokenAuth(AsyncWebServerRequest* req) {
  // SÉCURITÉ: Vérifier uniquement le header X-Auth-Token
  // On n'accepte PAS le query parameter ?token=... car :
  // - Il apparaît dans les logs serveur
  // - Il reste dans l'historique du navigateur
  // - Il peut être transmis via le header Referer
  // - Il est visible dans l'URL (shoulder surfing)
  if (req->hasHeader("X-Auth-Token")) {
    const AsyncWebHeader* header = req->getHeader("X-Auth-Token");

    // SÉCURITÉ: comparaison à temps constant (pas de court-circuit dépendant du contenu)
    if (constantTimeEquals(header->value(), apiToken)) {
      return true;
    }
  }
  // (bug-112) debug « aucun header » supprimé : une entrée par requête dans
  // l'anneau évincerait des WARN non persistés ; le 401 qui suit l'implique.

  return false;
}

bool AuthManager::checkRateLimit(AsyncWebServerRequest* req) {
  // Nettoyer les entrées expirées toutes les 100 requêtes
  static uint16_t requestCounter = 0;
  if (++requestCounter >= 100) {
    cleanupRateLimitMap();
    requestCounter = 0;
  }

  // Obtenir l'IP du client
  // Garde contre un client nul (préexistant : absente ici avant bug-112)
  if (req->client() == nullptr) {
    return true;
  }
  String clientIP = req->client()->remoteIP().toString();
  unsigned long now = millis();

  // Vérifier ou créer l'entrée de rate limiting
  auto it = rateLimitMap.find(clientIP);
  if (it == rateLimitMap.end()) {
    // Première requête de cette IP
    rateLimitMap[clientIP] = {now, 1};
    return true;
  }

  RateLimitEntry& entry = it->second;

  // Vérifier si la fenêtre a expiré
  if (now - entry.firstRequestTime >= RATE_LIMIT_WINDOW_MS) {
    // Nouvelle fenêtre
    entry.firstRequestTime = now;
    entry.requestCount = 1;
    return true;
  }

  // Incrémenter le compteur
  entry.requestCount++;

  // Vérifier la limite
  if (entry.requestCount > MAX_REQUESTS_PER_MINUTE) {
    logUnauthWarn(gRateSite, req, 1, entry.requestCount);  // journal borné ; le refus, lui, part toujours
    return false;
  }

  return true;
}

void AuthManager::cleanupRateLimitMap() {
  unsigned long now = millis();
  // MEM-EXEMPT(ADR-0082) : purge périodique du rate-limiting, hors chemin de journalisation, vidé des entrées expirées toutes les 100 requêtes. ⚠️ AUCUN plafond dur sur la taille de la map : « borné au nombre d'IP distinctes » est une tautologie, pas une garantie. Acceptable sur un LAN domestique, à reprendre en même temps que auth.h [feature-131]
  std::vector<String> toRemove;

  for (auto& pair : rateLimitMap) {
    if (now - pair.second.firstRequestTime >= RATE_LIMIT_WINDOW_MS) {
      toRemove.push_back(pair.first);
    }
  }

  for (const String& ip : toRemove) {
    rateLimitMap.erase(ip);
  }

  if (!toRemove.empty()) {
    systemLogger.debug("Rate limit: " + String(toRemove.size()) + " entrées nettoyées");
  }
}

bool AuthManager::checkAuth(AsyncWebServerRequest* req, RouteProtection level) {
  // Collecte des faits ; la décision est dans le module pur (bug-184).
  AuthAdmissionInput in{};
  in.authEnabled = authEnabled;
  in.publicRoute = (level == RouteProtection::NONE);
  // Le quota n'est consommé que si la décision l'atteint (auth active, route
  // protégée). bug-185 : plus AUCUNE exemption anonyme ici (celle du premier
  // démarrage sur /save-config est supprimée) ; seule la route publique
  // /auth/change-password porte le droit anonyme d'un appareil vierge.
  in.rateLimitOk = (!in.authEnabled || in.publicRoute) ? true : checkRateLimit(req);
  // ⚠️ Ces gardes REFLÈTENT l'« ORDRE IMPOSÉ » déclaré en tête de
  // auth_admission_logic.h : elles n'évaluent un fait que si la décision peut
  // encore l'atteindre, pour ne pas payer un calcul inutile. ⛔ Si cet ordre
  // changeait dans le header, elles fourniraient SILENCIEUSEMENT false — les
  // deux doivent bouger ensemble.
  in.tokenValid = (in.authEnabled && !in.publicRoute && in.rateLimitOk) ? checkTokenAuth(req) : false;

  switch (decideAdmission(in)) {
    case AuthVerdict::kAllow:
      return true;
    case AuthVerdict::kDenyRateLimited:
      sendRateLimitExceeded(req);  // le journal borné a déjà été écrit par checkRateLimit()
      return false;
    case AuthVerdict::kDenyUnauth:
    default:
      sendAuthRequired(req);  // journal borné (gUnauthSite) + 401
      return false;
  }
}

void AuthManager::sendAuthRequired(AsyncWebServerRequest* req) {
  // Enregistrer la tentative (journal BORNÉ, bug-112 — la réponse 401 ci-dessous
  // part TOUJOURS, quel que soit le verdict du throttle)
  logUnauthWarn(gUnauthSite, req, 0, 0);

  // Envoyer la réponse 401 sans challenge Basic Auth (évite la pop-up navigateur)
  AsyncWebServerResponse* response = req->beginResponse(401, "application/json", "{\"error\":\"Authentication required\"}");
  req->send(response);
}

void AuthManager::sendRateLimitExceeded(AsyncWebServerRequest* req) {
  // Pas de WARN ici (bug-112) : doublon strict de celui de checkRateLimit(), déjà
  // appelé et faux chez tous les appelants.
  AsyncWebServerResponse* response = req->beginResponse(429, "application/json", "{\"error\":\"Too many requests\"}");
  response->addHeader("Retry-After", "60");
  req->send(response);
}

void AuthManager::clearFirstBootFlag() {
  isFirstBoot = false;
  authCfg.wizardCompleted = true;

  // Sauvegarder immédiatement dans la NVS pour persister au redémarrage
  saveMqttConfig();
}

bool AuthManager::isFactoryPassword() const {
  return isFactoryPasswordLiteral(adminPassword.c_str());
}

void AuthManager::resetPasswordToDefault() {
  // Réinitialiser le mot de passe à "admin"
  adminPassword = "admin";
  authFailBackoffClearExemptions();  // bug-189
  isFirstBoot = true;

  systemLogger.critical("SÉCURITÉ: Mot de passe réinitialisé à 'admin' via bouton physique !");
  systemLogger.warning("Changement de mot de passe obligatoire au prochain login");
}
