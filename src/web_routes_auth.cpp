#include "web_routes_auth.h"
#include "web_helpers.h"
#include "auth.h"
#include "config.h"
#include "logger.h"
#include "constants.h"            // bug-185 : kAdminPasswordMinLength
#include "auth_bootstrap_logic.h"  // bug-185 : décisions pures
#include "ws_manager.h"  // bug-081 : révocation WS à la rotation de token
#include "auth_log_throttle_site.h"  // bug-183 : journal BORNÉ des échecs
#include "auth_fail_backoff_logic.h"  // bug-189 : ralentissement progressif des échecs
#include "ap_password_logic.h"        // bug-187 : règle des quatre familles
#include "auth_messages.h"            // bug-200 : phrases des refus (pures)
#include <ArduinoJson.h>
#include <WiFi.h>
#include <atomic>

// ---------------------------------------------------------------------------
// bug-183 : journal des échecs d'authentification des routes /auth/*
// ---------------------------------------------------------------------------
// ⛔ UN SEUL site borné pour les échecs des DEUX routes : la route est nommée dans
// le TEXTE de la ligne, pas dans la clé du site. Deux sites doubleraient le budget
// de journal de qui choisit la route (ou la nature) de ses essais. Clé fixe : le
// hachage ne distingue donc aucune route.
// Statiques PRIVÉES à l'unité ; seul drainAuthRoutesWarnLogs() est exporté.
namespace {
AuthLogThrottleSite gAuthFailSite;
// Échecs depuis la dernière connexion RÉUSSIE. Compte TOUS les échecs, journalisés
// ou occultés par le bornage : c'est ce que dit la ligne de réussite. Saturant.
std::atomic<uint16_t> gFailuresSinceSuccess{0};
// bug-189 : état des trois portes (IP, globale, exemption). Capacité FIXE, en RAM.
// Tous les handlers /auth/* tournent sur async_tcp : pas de verrou (voir le module pur).
AuthFailBackoff gAuthBackoff;
}  // namespace

// bug-189 : appelé à TOUT changement de mot de passe (AuthManager::setPassword,
// resetPasswordToDefault) : les adresses qui avaient prouvé l'ancien mot de passe
// perdent leur exemption.
void authFailBackoffClearExemptions() { authFailClearExemptions(gAuthBackoff); }

static uint32_t clientIpOf(AsyncWebServerRequest* req) {
  return (req != nullptr && req->client() != nullptr) ? static_cast<uint32_t>(req->client()->remoteIP()) : 0;
}

// Réponse 429 de PÉNALITÉ. ⛔ Construite SANS JsonDocument (un refus ne doit rien allouer
// de lourd). Distinguable du 429 de quota (« Too many requests », sans retryAfter) par le
// champ retryAfter ; le texte est fixe et n'apprend rien sur le compte (ni nature, ni IP).
static void sendAuthPenalty(AsyncWebServerRequest* req, uint32_t retryAfterSec) {
  char body[112];
  snprintf(body, sizeof(body), "{\"error\":\"Trop d'essais de connexion, réessayez dans %lu s\",\"retryAfter\":%lu}",
           static_cast<unsigned long>(retryAfterSec), static_cast<unsigned long>(retryAfterSec));
  char retry[12];
  snprintf(retry, sizeof(retry), "%lu", static_cast<unsigned long>(retryAfterSec));
  AsyncWebServerResponse* response = req->beginResponse(429, "application/json", body);
  response->addHeader("Retry-After", retry);
  req->send(response);
}

// Journalise (borné) un échec. `what` = « /route : nature », texte FIXE choisi par
// l'appelant. ⛔ Jamais le mot de passe, aucun fragment, aucune longueur, ni le nom
// d'utilisateur essayé (attaquant-contrôlé, de longueur libre : le recopier
// permettrait d'injecter du texte dans le journal). On dit le FAIT, pas la valeur.
// MUET => aucune allocation (ADR-0082) : IP lue brute, sortie AVANT toute String.
// `countFailure` : faux pour un REFUS de ralentissement (bug-189, condition 7) — un refus
// n'est pas un échec de mot de passe et ne doit pas gonfler « échecs depuis la dernière
// connexion réussie ».
static void logAuthFailure(AsyncWebServerRequest* req, const char* what, bool countFailure = true) {
  // Incrément saturant via le helper PUR déjà testé en natif (ADR-0017) plutôt
  // qu'une saturation réécrite à la main : une logique de moins hors couverture.
  uint16_t cur = gFailuresSinceSuccess.load(std::memory_order_relaxed);
  while (countFailure && cur != UINT16_MAX &&
         !gFailuresSinceSuccess.compare_exchange_weak(cur, authLogThrottleSatInc(cur),
                                                      std::memory_order_relaxed)) {
  }
  const bool hasClient = (req != nullptr && req->client() != nullptr);
  const uint32_t ip = hasClient ? static_cast<uint32_t>(req->client()->remoteIP()) : 0;
  const AuthLogSiteReport r = authLogSiteAttempt(gAuthFailSite, millis(), authLogThrottleHash("/auth"), ip,
                                                 kUnauthWarnThrottleMs, kUnauthWarnMaxShift, kUnauthWarnDecayMs);
  if (!r.emit) return;  // MUET : zéro allocation
  String msg = String("[Auth] ") + what + " depuis " +
               (hasClient ? req->client()->remoteIP().toString() : String("IP inconnue"));
  if (r.occulted > 0) {
    // ⚠️ Les occultés ne portent ni la nature ni la route annoncées ci-dessus, et
    // ⛔ PAS FORCÉMENT L'IP NOMMÉE. Sans le dire, on lit « et 12 autres essais DE
    // CETTE ADRESSE » alors qu'ils peuvent venir de douze adresses : c'est très
    // exactement la ligne sur laquelle on se trompe en cherchant qui a essayé
    // d'entrer. (Relevé par security-reviewer en passe P2 ; même défaut que celui
    // que auth.cpp/appendOcculted évitait déjà, lui, en affichant r.otherIps.)
    // ℹ️ r.otherRoutes n'est PAS affiché : la clé du site est fixe (« /auth »),
    //    donc il est toujours faux ici — l'afficher serait du code mort.
    msg += String(" ; et ") + String(r.occulted) + " " +
           authLogThrottlePlural(r.occulted, "autre échec non journalisé", "autres échecs non journalisés") +
           (r.otherIps ? " (route, nature et IP non précisées)" : " (route et nature non précisées)");
  }
  systemLogger.warning(msg);
}

// Connexion RÉUSSIE : INFO, avec le nombre d'échecs depuis la précédente. Sans elle
// le journal dit qu'on a essayé, jamais qu'on a réussi. Atteignable seulement avec
// le bon mot de passe, et sous checkRateLimit() : pas un émetteur anonyme.
static void logLoginSuccess(AsyncWebServerRequest* req) {
  const uint16_t fails = gFailuresSinceSuccess.exchange(0, std::memory_order_relaxed);
  String msg = String("[Auth] Connexion réussie depuis ") +
               ((req != nullptr && req->client() != nullptr) ? req->client()->remoteIP().toString()
                                                              : String("IP inconnue"));
  if (fails > 0) {
    // ⚠️ Le compte agrège les DEUX routes /auth/* (échecs d'identifiants de /auth/login
    // ET essais de jeton de /auth/change-password) : le libellé doit le dire, sinon on
    // lit « N mots de passe faux ». ⛔ Il est en RAM : un redémarrage le remet à zéro.
    msg += String(" (") + (fails == UINT16_MAX ? String("65535 ou plus") : String(fails)) +
           " " + authLogThrottlePlural(fails, "échec", "échecs") +
           " d'authentification sur /auth/* depuis le démarrage ou la dernière connexion réussie)";
  }
  // AUTH-LOG-EXEMPT(bug-183) : NON borné, délibérément. Atteignable seulement après
  // decideLogin() == kAllow — donc par qui détient déjà le bon mot de passe — et
  // derrière checkRateLimit() (120 req/min/IP) : ce n'est pas un émetteur anonyme,
  // et la garde check_unauth_log_guard.sh ne borne que le chemin NON authentifié.
  // ⚠️ Résidu assumé : qui détient le mot de passe peut noyer ses traces en ~27
  // connexions réussies (journal de 6 144 o conservés). Défaut déjà porté par
  // bug-112 et renvoyé à feature-172 ; ce lot y ajoute une voie, il ne la crée pas.
  systemLogger.info(msg);
}

// Drain (loop UNIQUEMENT), borné par le même recul que l'émission.
void drainAuthRoutesWarnLogs() {
  const AuthLogSiteReport r = authLogSiteDrain(gAuthFailSite, millis(), kUnauthWarnThrottleMs,
                                               kUnauthWarnMaxShift, kUnauthWarnDrainQuietMs);
  if (!r.emit) return;
  systemLogger.warning(String("[Auth] ") + String(r.occulted) + " " +
                       authLogThrottlePlural(r.occulted, "échec d'authentification non journalisé",
                                             "échecs d'authentification non journalisés") +
                       " (route et nature non précisées)");
}

void setupAuthRoutes(AsyncWebServer* server) {
  // Route: Statut d'authentification (PUBLIC - vérifier si premier démarrage)
  //
  // ⛔ CONTRAT DE SONDABILITÉ (bug-177) — NE PAS DURCIR CETTE ROUTE SANS LIRE CECI.
  // La page s'en sert pour savoir si l'appareil est REVENU après un redémarrage
  // OTA. Quatre propriétés la rendent utilisable pour ça, et chacune est
  // nécessaire :
  //   1. PUBLIQUE — ni REQUIRE_AUTH ni checkAuth : elle doit répondre alors que
  //      la page n'a pas encore pu se ré-authentifier ;
  //   2. HORS QUOTA — ⛔ ne PAS y ajouter checkRateLimit() : le sondage (une
  //      tentative toutes les 2 s pendant 90 s au plus) ne doit consommer aucun
  //      des kMaxRequestsPerMinute, sinon il s'auto-refuse ;
  //   3. SANS SYSTÈME DE FICHIERS — elle ne sert que trois booléens, donc elle
  //      répond même si LittleFS n'a pas pu être remonté après un envoi FS ;
  //   4. ENREGISTRÉE EN PREMIER (web_server.cpp) — donc hors de la garde 503 de
  //      famine mémoire : un appareil sous pression doit quand même dire « je suis là ».
  // ⚠️ Ces quatre propriétés étaient des ACCIDENTS jusqu'au 2026-11-15 : rien ne les
  // déclarait. Les casser ne produirait AUCUNE erreur — seulement une page qui
  // n'arrive plus à constater le retour de l'appareil, en silence. C'est la famille
  // de défauts qui a déjà coûté six semaines à ce projet (bug-169).
  // Contrat écrit aussi dans docs/API.md.
  server->on("/auth/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["firstBoot"] = authManager.isFirstBootDetected();
    // bug-185 — DEUX NOTIONS DISTINCTES, et leur confusion était le défaut :
    //   · firstBoot : « l'assistant doit encore être montré » ;
    //   · virgin    : « AUCUN mot de passe n'a jamais été choisi ».
    // ⛔ Seul `virgin` autorise le formulaire ANONYME de choix de mot de passe.
    // Sans cette clé, un appareil dont l'assistant a été interrompu APRÈS le
    // choix du mot de passe voyait les pages lui proposer un formulaire que
    // l'appareil REFUSE — la page de secours devenait une impasse, et la seule
    // issue était l'appui long, qui efface étalonnages, consignes et limites.
    // ⚠️ Ajout ADDITIF : aucune clé existante ne change, le contrat de
    // sondabilité de bug-177 (toujours répondre, sans auth ni quota) est intact.
    // A5 : l'état vierge est PUBLIC PAR CONSTRUCTION — le droit anonyme de
    // /auth/change-password le révèle à quiconque l'essaie, indépendamment de cette clé.
    doc["virgin"] = authManager.isFactoryPassword();
    doc["authEnabled"] = authManager.isEnabled();
    doc["forceWifiConfig"] = authCfg.forceWifiConfig;
    sendJsonResponse(req, doc);
  });

  // Route: Login (PUBLIC - génère un token de session)
  server->on("/auth/login", HTTP_POST, [](AsyncWebServerRequest *req) {}, nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!authManager.checkRateLimit(req)) {
        authManager.sendRateLimitExceeded(req);
        return;
      }

      // bug-189 : ralentissement des échecs. ⛔ PLACE IMPOSÉE : après checkRateLimit,
      // AVANT deserializeJson (la garde ne dépend pas du corps : pas de JsonDocument par
      // refus). ⛔ Jamais dans checkRateLimit/checkAuth/decideAdmission : auth.cpp
      // n'examine le jeton que si le quota passe, la garde y enfermerait le propriétaire.
      // Contrôle, comparaison et comptage : même invocation.
      const uint32_t clientIp = clientIpOf(req);
      {
        const AuthFailVerdict gate = authFailCheck(gAuthBackoff, clientIp, millis(), authFailIpPolicy(),
                                                   authFailGlobalPolicy());
        if (gate.refuse) {
          logAuthFailure(req, "/auth/login : essai refusé (ralentissement)", false);
          sendAuthPenalty(req, gate.retryAfterSec());
          return;
        }
      }

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, data, len);

      if (error) {
        sendErrorResponse(req, 400, "Invalid JSON");
        return;
      }

      String username = doc["username"] | "";
      String password = doc["password"] | "";

      // bug-185 : la décision est dans le module pur. Le mot de passe d'usine est
      // refusé (403) QUEL QUE SOIT isFirstBoot : sinon un tiers qui referme la
      // fenêtre rendrait admin/admin valide pour toujours.
      // ⛔ `isFirstBoot` n'est PAS passé, délibérément : un assistant interrompu
      // APRÈS le choix du mot de passe laisse isFirstBoot vrai sur un appareil
      // déjà possédé — le tester ici enfermerait son propriétaire dehors, et
      // l'OTA est le SEUL canal de déploiement. Voir auth_bootstrap_logic.h.
      // bug-183 : `admin` est PUBLIC (== reste), le mot de passe se compare à temps fixe.
      const bool userMatches = (username == "admin");
      const bool pwMatches = authManager.passwordMatches(password);
      switch (decideLogin(userMatches, pwMatches, authManager.isFactoryPassword())) {
        case LoginVerdict::kAllow:
          // Mot de passe PROUVÉ : libère le créneau et inscrit l'adresse à l'exemption.
          authFailRecordPasswordProof(gAuthBackoff, clientIp);
          break;
        case LoginVerdict::kDenyMustChangePassword:
          // A5 : 403 NON journalisé (bruit). Il est conservé, mais ce n'est pas la
          // publication de `virgin` qui le justifie : l'état vierge est public PAR
          // CONSTRUCTION (le droit anonyme de /auth/change-password le révèle).
          sendErrorResponse(req, 403, "Changement de mot de passe obligatoire au premier démarrage");
          return;
        case LoginVerdict::kDenyCredentials:
        default:
          // bug-189 : les DEUX natures (nom inconnu, mot de passe faux) sont comptées à
          // l'identique — sinon le délai serait un oracle d'énumération du compte.
          authFailRecordFailure(gAuthBackoff, clientIp, millis(), authFailIpPolicy(), authFailGlobalPolicy());
          // Le classifieur ne sert QU'au texte du journal : la réponse reste unique.
          switch (classifyLoginFailure(userMatches, pwMatches)) {
            case LoginLogKind::kUnknownUser:
              logAuthFailure(req, "/auth/login : nom d'utilisateur inconnu");
              break;
            case LoginLogKind::kBadPassword:
              logAuthFailure(req, "/auth/login : mot de passe incorrect");
              break;
            case LoginLogKind::kNone:
            default:
              break;
          }
          sendErrorResponse(req, 401, "Nom d'utilisateur ou mot de passe invalide");
          return;
      }

      // Authentification réussie - retourner le token API
      logLoginSuccess(req);
      JsonDocument response;
      response["success"] = true;
      response["token"] = authManager.getApiToken();
      response["username"] = "admin";
      sendJsonResponse(req, response);
    }
  );

  // Route: Changement de mot de passe (PUBLIC pour premier démarrage, sinon AUTH)
  server->on("/auth/change-password", HTTP_POST, [](AsyncWebServerRequest *req) {}, nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!authManager.checkRateLimit(req)) {
        authManager.sendRateLimitExceeded(req);
        return;
      }

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, data, len);

      if (error) {
        sendErrorResponse(req, 400, "Invalid JSON");
        return;
      }

      String currentPassword = doc["currentPassword"] | "";
      String newPassword = doc["newPassword"] | "";

      // bug-185 : décision dans le module pur (auth_bootstrap_logic.h). ⛔ Ordre
      // imposé là-bas : authentification AVANT longueur (401, pas 400, sinon fuite
      // de l'existence d'un compte). Droit anonyme = appareil vierge seulement.
      const bool isFirstBoot = authManager.isFirstBootDetected();
      // ⛔ LE DROIT ANONYME SUIT LE MOT DE PASSE, PAS L'ASSISTANT. Un appareil dont
      // l'assistant a été interrompu après le choix du mot de passe garde
      // isFirstBoot vrai alors qu'il est DÉJÀ possédé : lui accorder le droit
      // anonyme laisserait un inconnu écraser le mot de passe du propriétaire.
      const bool virginState = authManager.isFactoryPassword();
      const uint32_t clientIp = clientIpOf(req);
      // bug-189 : garde APRÈS l'analyse du corps, AVANT passwordMatches et checkTokenAuth.
      // ⚠️ Elle ne s'applique qu'à l'essai de MOT DE PASSE ACTUEL SANS JETON VALIDE.
      // ⛔ PIÈGE À NE PAS REFAIRE : l'interface envoie le jeton ET currentPassword à la
      // fois (data/app.js). Tester le seul mot de passe freinerait donc un propriétaire
      // muni d'un jeton valide (A2 violé sous attaque). Un jeton valide prouve que le
      // demandeur contrôle déjà l'appareil : le freiner ne protège rien. checkTokenAuth
      // est le prédicat SILENCIEUX (ni journal ni quota) : ne pas lui substituer checkAuth.
      // ⛔ `!virginState` : la prise de possession depuis la page de secours envoie
      // currentPassword "admin" sur un appareil vierge ; la freiner la laisserait affamer
      // par la porte globale. L'état vierge est public par construction : rien à protéger.
      if (!currentPassword.isEmpty() && !virginState && !authManager.checkTokenAuth(req)) {
        const AuthFailVerdict gate = authFailCheck(gAuthBackoff, clientIp, millis(), authFailIpPolicy(),
                                                   authFailGlobalPolicy());
        if (gate.refuse) {
          logAuthFailure(req, "/auth/change-password : essai refusé (ralentissement)", false);
          sendAuthPenalty(req, gate.retryAfterSec());
          return;
        }
      }
      PwdChangeInput in;
      in.virginState = virginState;
      in.hasCurrentPassword = !currentPassword.isEmpty();
      in.currentMatches = authManager.passwordMatches(currentPassword);  // bug-183 : temps fixe
      in.currentIsFactoryLiteral = (currentPassword == "admin");  // valeur PUBLIQUE : == reste
      // checkTokenAuth : ne répond pas lui-même (le 401 est émis ici, une seule fois).
      in.tokenValid = (!virginState && !in.hasCurrentPassword) ? authManager.checkTokenAuth(req) : false;
      in.newLenOk = (newPassword.length() >= kAdminPasswordMinLength);  // constante partagée avec /save-config (bug-185)
      // bug-187 : les quatre familles. ⚠️ Le plancher de longueur du mot de passe de
      // l'APPAREIL reste kAdminPasswordMinLength (8) et NON kApPwdMinLength (12) : il
      // est protégé par le ralentissement de bug-189, alors que la clé Wi-Fi s'attaque
      // HORS LIGNE après capture. Deux objets, deux menaces, deux planchers.
      // ⚠️ Le verdict est CONSERVÉ, pas réduit à un booléen : replier « trop long » et
      // « caractère interdit » sur « il manque une famille » afficherait un message FAUX
      // (refus juste, explication fausse — relevé en revue P2).
      const ApPwdVerdict verdictNouveau =
          apPwdCheck(newPassword.c_str(), newPassword.length(), kAdminPasswordMinLength,
                     kAdminPasswordMaxLength, /*allowNonAscii=*/true);
      in.newFamiliesOk = (verdictNouveau == ApPwdVerdict::kOk);
      in.newEqualsCurrent = in.hasCurrentPassword && (newPassword == currentPassword);

      switch (decidePasswordChange(in)) {
        case PwdChangeVerdict::kAllow:
          break;
        case PwdChangeVerdict::kDenyUnauth:
          // Essai de jeton, jusqu'ici invisible : même site borné que le mot de passe.
          logAuthFailure(req, "/auth/change-password : jeton absent ou invalide");
          sendErrorResponse(req, 401, pwdChangeMessage(PwdChangeVerdict::kDenyUnauth, verdictNouveau));
          return;
        case PwdChangeVerdict::kDenyBadCurrent:
          logAuthFailure(req, "/auth/change-password : mot de passe actuel incorrect");
          // Cohérent avec la garde : l'état vierge n'est ni freiné ni compté.
          if (!virginState) authFailRecordFailure(gAuthBackoff, clientIp, millis(), authFailIpPolicy(), authFailGlobalPolicy());
          sendErrorResponse(req, 401, pwdChangeMessage(PwdChangeVerdict::kDenyBadCurrent, verdictNouveau));
          return;
        case PwdChangeVerdict::kDenyTooShort:
          sendErrorResponse(req, 400, pwdChangeMessage(PwdChangeVerdict::kDenyTooShort, verdictNouveau));
          return;
        case PwdChangeVerdict::kDenyWeak:
          // bug-187 : message EN FRANÇAIS et EXACT — l'utilisateur doit savoir ce qui
          // manque VRAIMENT. ⛔ Ne jamais recopier le mot de passe reçu dans la réponse.
          sendErrorResponse(req, 400, pwdChangeMessage(PwdChangeVerdict::kDenyWeak, verdictNouveau));
          return;
        case PwdChangeVerdict::kDenySame:
        default:
          sendErrorResponse(req, 400, pwdChangeMessage(PwdChangeVerdict::kDenySame, verdictNouveau));
          return;
      }

      const AuthEffectPlan plan = planAfterPasswordChange(isFirstBoot);

      // ⛔ ORDRE D'EFFETS FIGÉ (bug-185) — non testable en natif (NVS/HTTP),
      // protégé par ce commentaire : 1 mot de passe, 2 forceWifiConfig, 3 jeton
      // (RAM ET authCfg ensemble), 4 UNE SEULE écriture NVS, 5 révocation WS,
      // 6 réponse avec le NOUVEAU jeton.
      // 1.
      authCfg.adminPassword = newPassword;
      authManager.setPassword(newPassword);  // vide aussi la table d'exemption (bug-189)
      // bug-189 : TOUT kAllow de /auth/change-password (mot de passe actuel, jeton valide
      // ou prise de possession anonyme d'un appareil vierge) a vidé la table (setPassword)
      // puis réinscrit l'auteur — sauf ip inconnue, ignorée par le module. Seulement ici :
      // pas sur n'importe quelle requête porteuse d'un jeton.
      authFailRecordPasswordProof(gAuthBackoff, clientIp);

      // 2. Si c'était un reset password, désactiver le flag forceWifiConfig
      if (authCfg.forceWifiConfig) {
        authCfg.forceWifiConfig = false;
      }

      // 3. ⛔ LES DEUX LIGNES ENSEMBLE : tourner le jeton en RAM sans le recopier
      // dans authCfg ferait RESSUSCITER l'ancien jeton de la NVS au redémarrage.
      if (plan.rotateToken) {
        authManager.regenerateApiToken();
        authCfg.apiToken = authManager.getApiToken();
      }

      // 4. UNE SEULE écriture : deux saveMqttConfig() ouvriraient une fenêtre de
      // redémarrage à l'état mi-ancien mi-neuf (drapeau persisté sans mot de passe
      // = appareil enfermé dehors). clearFirstBootFlag() sauvegarde déjà.
      if (plan.closeFirstBootWindow) {
        authManager.clearFirstBootFlag();
      } else {
        saveMqttConfig();
      }

      // 5. Les clients WS authentifiés avec l'ancien jeton sont révoqués.
      if (plan.revokeWebsockets) {
        wsManager.revokeAllAuthenticated();
      }

      // 6. Réponse : le NOUVEAU jeton (les clients stockent response.token ; la
      // page de secours OTA échoue sans jeton).
      JsonDocument response;
      response["success"] = true;
      // ⛔ `plan.returnRotatedToken` est CONSULTÉ, pas décoratif : un champ de plan
      // que la production n'applique pas ferait affirmer à ses tests une propriété
      // fausse (relevé par code-reviewer). Il vaut toujours vrai aujourd'hui — et
      // c'est le test de mutation qui le prouve, pas cette ligne.
      if (plan.returnRotatedToken) {
        response["token"] = authManager.getApiToken();
      }
      response["message"] = "Password changed successfully";
      sendJsonResponse(req, response);
    }
  );

  // Route: Régénération du token API (PROTÉGÉE - nécessite auth)
  server->on("/auth/regenerate-token", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    // Régénérer le token
    authManager.regenerateApiToken();
    authCfg.apiToken = authManager.getApiToken();
    saveMqttConfig();

    // bug-081, condition bloquante #3 (security-reviewer) : une rotation de
    // jeton ne révoquait rien côté WS — un client authentifié avec l'ANCIEN
    // token continuait de recevoir les pushes indéfiniment (keepalive). Les
    // clients WS déjà authentifiés doivent ré-authentifier avec le nouveau
    // token.
    wsManager.revokeAllAuthenticated();

    JsonDocument doc;
    doc["success"] = true;
    doc["token"] = authManager.getApiToken();
    doc["message"] = "API token regenerated";
    sendJsonResponse(req, doc);
  });

  // Route: Obtenir le token API actuel (PROTÉGÉE - nécessite auth)
  server->on("/auth/token", HTTP_GET, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    JsonDocument doc;
    doc["token"] = authManager.getApiToken();
    sendJsonResponse(req, doc);
  });

  // Route: Mot de passe AP WiFi (PROTÉGÉE - nécessite auth admin)
  // ⚠️ bug-187, condition A5 de la revue P1 : `Cache-Control: no-store`. Un secret
  // servi en JSON n'hérite d'aucun en-tête de cache — sans cette ligne il peut
  // rester dans le cache du navigateur bien après la fermeture de la page.
  // ⛔ Et il ne figure NI dans /get-config, NI dans /data, NI dans le WebSocket,
  // NI dans MQTT : cette route est le SEUL endroit où il sort de l'appareil, et il
  // n'en sort que sur un geste explicite de l'utilisateur.
  server->on("/auth/ap-password", HTTP_GET, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    JsonDocument doc;
    doc["ap_password"] = authCfg.apPassword;
    doc["ap_ssid"] = "PoolControllerAP";
    // bug-187 : vrai si la clé est encore celle d'avant ce lot (8 hexadécimaux,
    // 32 bits). ⛔ On ne régénère RIEN — l'étiquette doit rester vraie — mais
    // l'interface doit pouvoir conseiller à son propriétaire d'en choisir une autre.
    doc["ap_password_legacy"] =
        apPwdIsLegacyForm(authCfg.apPassword.c_str(), authCfg.apPassword.length());
    String body;
    serializeJson(doc, body);
    AsyncWebServerResponse* res = req->beginResponse(200, "application/json", body);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });

  // ===========================================================================
  // bug-187 — Route: l'utilisateur remplace le mot de passe du point d'accès
  // ===========================================================================
  // ⛔ PROTÉGÉE, sans aucun droit anonyme : contrairement à /auth/change-password,
  // il n'y a ici AUCUNE fenêtre de prise de possession. Un appareil vierge n'a pas
  // à voir son point d'accès reconfiguré par un inconnu — c'est précisément le
  // réseau par lequel l'installateur arrive.
  // ⚠️ L'ORIGINE N'EST JAMAIS TOUCHÉE : la réinitialisation d'usine ramènera
  // toujours le mot de passe imprimé sur l'étiquette. C'est l'invariant du lot.
  // ⚠️ Le handler de REQUÊTE s'exécute APRÈS le handler de corps. Si la requête n'a
  // AUCUN corps, le handler de corps n'est jamais appelé et personne ne répond : le
  // client reste pendu jusqu'à son propre délai. ⇒ On répond ici, et SEULEMENT dans ce
  // cas (contentLength nul), pour ne pas répondre deux fois.
  server->on("/auth/ap-password", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->contentLength() == 0) sendErrorResponse(req, 400, "Corps de requete absent");
  }, nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!authManager.checkAuth(req, RouteProtection::CRITICAL)) return;

      JsonDocument doc;
      if (deserializeJson(doc, data, len)) {
        sendErrorResponse(req, 400, "Invalid JSON");
        return;
      }
      String nouveau = doc["apPassword"] | "";

      // ⛔ Le contrôle est fait ICI, pas seulement dans la page : une page peut être
      // contournée, et une phrase trop courte ferait refuser softAP() — ou pire, une
      // phrase vide créerait un réseau OUVERT.
      // ⛔ `allowNonAscii = false` : WPA2 ne transporte que de l'ASCII imprimable.
      const ApPwdVerdict verdictAp = apPwdCheck(nouveau.c_str(), nouveau.length(), kApPwdMinLength,
                                                kApPwdMaxLength, /*allowNonAscii=*/false);
      if (verdictAp != ApPwdVerdict::kOk) {
        // ⚠️ Cas très concret : un accent collé depuis un gestionnaire. WPA2 ne
        // transporte que de l'ASCII imprimable. Phrases : auth_messages.h.
        sendErrorResponse(req, 400, apPwdMessage(verdictAp));
        return;
      }

      if (!authManager.setApPassword(nouveau)) {
        // L'écriture n'a pas été confirmée par relecture : l'ancien reste en service.
        sendErrorResponse(req, 500, "Enregistrement refuse par la memoire de l'appareil");
        return;
      }

      JsonDocument out;
      out["success"] = true;
      // ⚠️ Le changement ne prend effet qu'au prochain démarrage du point d'accès :
      // l'appareil ne coupe pas un réseau de secours en service pour appliquer un
      // réglage — ce serait couper la branche sur laquelle l'utilisateur est assis.
      out["message"] = "Mot de passe du point d'acces enregistre";
      sendJsonResponse(req, out);
    });

  // Route: Marquer le wizard comme complété (désactive le flag firstBoot)
  // bug-185 : n'est plus PUBLIQUE. Filet idempotent : exige le jeton (donc un mot
  // de passe déjà choisi, la clôture normale passant par /auth/change-password) et
  // refuse (409) si le mot de passe est encore celui d'usine.
  server->on("/auth/complete-wizard", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    // ⛔ `tokenValid` est codé à `true` À DESSEIN : le REQUIRE_AUTH ci-dessus a DÉJÀ
    // répondu 401 et rendu la main si le jeton manquait. Rappeler checkAuth ici
    // consommerait un SECOND quota et enverrait une SECONDE réponse sur la même
    // requête. ⚠️ Conséquence assumée : la branche kDenyUnauth du module pur est
    // donc inatteignable depuis cette route — ses tests protègent le module, pas
    // ce site. La vraie garde est la ligne au-dessus.
    const WizardCloseVerdict v = decideWizardClose(authManager.isFirstBootDetected(), true,
                                                   authManager.isFactoryPassword());
    if (v == WizardCloseVerdict::kDenyFactoryPassword) {
      sendErrorResponse(req, 409, "Choisissez d'abord un mot de passe");
      return;
    }
    if (v == WizardCloseVerdict::kClose) {
      authManager.clearFirstBootFlag();
      // AUTH-LOG-EXEMPT(borné par nature : n'écrit qu'une fois, gardé par isFirstBootDetected() que clearFirstBootFlag() rend faux)
      systemLogger.info("Premier démarrage finalisé - Configuration wizard complétée");
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Wizard completed successfully";
    sendJsonResponse(req, doc);
  });
}
