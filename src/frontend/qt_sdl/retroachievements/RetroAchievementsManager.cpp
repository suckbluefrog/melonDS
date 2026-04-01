#include "RetroAchievementsManager.h"

#include <string>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>

extern "C"
{
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_error.h"
}

#include "Config.h"
#include "EmuInstance.h"
#include "EmuThread.h"
#include "NDS.h"
#include "NDSCart/CartCommon.h"
#include "Platform.h"
#include "version.h"

using namespace melonDS;
using namespace melonDS::Platform;

namespace
{

constexpr unsigned int kColorInfo = 0;
constexpr unsigned int kColorSuccess = 0xA0FFA0;
constexpr unsigned int kColorWarn = 0xFFE080;
constexpr unsigned int kColorError = 0xFFA0A0;

uint32_t translateAddress(uint32_t consoleId, uint32_t address, bool& ok)
{
    ok = false;

    const rc_memory_regions_t* regions = rc_console_memory_regions(consoleId);
    if (!regions)
        return 0;

    for (uint32_t i = 0; i < regions->num_regions; i++)
    {
        const rc_memory_region_t& region = regions->region[i];
        if (address < region.start_address || address > region.end_address)
            continue;
        if (region.type == RC_MEMORY_TYPE_UNUSED)
            return 0;

        ok = true;
        return region.real_address + (address - region.start_address);
    }

    return 0;
}

}

namespace RetroAchievements
{

RetroAchievementsManager::RetroAchievementsManager(EmuInstance* emuInstance)
    : emuInstance(emuInstance), client(nullptr), gameLoaded(false)
{
    client = rc_client_create(&RetroAchievementsManager::ReadMemory, &RetroAchievementsManager::ServerCall);
    rc_client_set_userdata(client, this);
    rc_client_set_event_handler(client, &RetroAchievementsManager::EventHandler);
    rc_client_set_allow_background_memory_reads(client, 0);
    applyConfig();
}

RetroAchievementsManager::~RetroAchievementsManager()
{
    if (!client)
        return;

    rc_client_unload_game(client);
    rc_client_destroy(client);
}

bool RetroAchievementsManager::isEnabled() const
{
    return emuInstance->getGlobalConfig().GetBool("RA.Enabled");
}

void RetroAchievementsManager::setEnabled(bool enabled)
{
    emuInstance->getGlobalConfig().SetBool("RA.Enabled", enabled);
    Config::Save();

    if (enabled)
        onGameChanged();
    else
        onGameStopped();
}

bool RetroAchievementsManager::isHardcoreEnabled() const
{
    return emuInstance->getGlobalConfig().GetBool("RA.Hardcore");
}

void RetroAchievementsManager::setHardcoreEnabled(bool enabled)
{
    emuInstance->getGlobalConfig().SetBool("RA.Hardcore", enabled);
    Config::Save();
    applyConfig();
}

bool RetroAchievementsManager::isLoggedIn() const
{
    return rc_client_get_user_info(client) != nullptr;
}

bool RetroAchievementsManager::loginWithPassword(const std::string& username, const std::string& password, std::string& error)
{
    if (username.empty() || password.empty())
    {
        error = "Username and password are required.";
        return false;
    }

    SyncResult result;
    applyConfig();
    rc_client_begin_login_with_password(client, username.c_str(), password.c_str(), &RetroAchievementsManager::SyncCallback, &result);
    if (!result.called || result.result != RC_OK)
    {
        error = result.error.empty() ? "Login failed." : result.error;
        return false;
    }

    const rc_client_user_t* user = rc_client_get_user_info(client);
    if (!user || !user->username || !user->token)
    {
        error = "Login succeeded, but no API token was returned.";
        return false;
    }

    saveCredentials(user->username, user->token);
    return true;
}

bool RetroAchievementsManager::loginWithToken(const std::string& username, const std::string& token, std::string& error)
{
    if (username.empty() || token.empty())
    {
        error = "Username and API token are required.";
        return false;
    }

    SyncResult result;
    applyConfig();
    rc_client_begin_login_with_token(client, username.c_str(), token.c_str(), &RetroAchievementsManager::SyncCallback, &result);
    if (!result.called || result.result != RC_OK)
    {
        error = result.error.empty() ? "Login failed." : result.error;
        return false;
    }

    const rc_client_user_t* user = rc_client_get_user_info(client);
    if (user && user->username && user->token)
        saveCredentials(user->username, user->token);
    else
        saveCredentials(username, token);
    return true;
}

void RetroAchievementsManager::logout()
{
    rc_client_logout(client);
    clearCredentials();
    onGameStopped();
}

void RetroAchievementsManager::onGameChanged()
{
    if (!isEnabled())
    {
        onGameStopped();
        return;
    }

    melonDS::NDS* nds = emuInstance->getNDS();
    if (!nds || !nds->GetNDSCart())
    {
        onGameStopped();
        return;
    }

    std::string error;
    if (!ensureLoggedIn(error))
    {
        if (!error.empty())
            emuInstance->osdAddMessage(kColorWarn, "RetroAchievements: %s", error.c_str());
        return;
    }

    if (!loadCurrentGame(error) && !error.empty())
        emuInstance->osdAddMessage(kColorError, "RetroAchievements: %s", error.c_str());
}

void RetroAchievementsManager::onGameStopped()
{
    rc_client_unload_game(client);
    gameLoaded = false;
}

void RetroAchievementsManager::reset()
{
    if (gameLoaded)
        rc_client_reset(client);
}

void RetroAchievementsManager::frameUpdate()
{
    if (gameLoaded && rc_client_is_processing_required(client))
        rc_client_do_frame(client);
}

void RetroAchievementsManager::idle()
{
    if (gameLoaded)
        rc_client_idle(client);
}

bool RetroAchievementsManager::doSavestate(Savestate* savestate)
{
    savestate->Section("RCHV");

    u8 hasRuntime = gameLoaded ? 1 : 0;
    savestate->Var8(&hasRuntime);

    if (!hasRuntime)
        return !savestate->Error;

    if (savestate->Saving)
    {
        u32 stateSize = (u32)rc_client_progress_size(client);
        std::string buffer(stateSize, '\0');
        int result = rc_client_serialize_progress_sized(client, reinterpret_cast<uint8_t*>(buffer.data()), buffer.size());
        if (result != RC_OK)
            return false;

        savestate->Var32(&stateSize);
        savestate->VarArray(buffer.data(), stateSize);
        return !savestate->Error;
    }

    if (savestate->Error)
        return false;

    u32 stateSize = 0;
    savestate->Var32(&stateSize);
    std::string buffer(stateSize, '\0');
    savestate->VarArray(buffer.data(), stateSize);
    if (savestate->Error)
        return false;

    return rc_client_deserialize_progress_sized(client, reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size()) == RC_OK;
}

void RetroAchievementsManager::applyConfig()
{
    rc_client_set_hardcore_enabled(client, emuInstance->getGlobalConfig().GetBool("RA.Hardcore"));
    rc_client_set_encore_mode_enabled(client, emuInstance->getGlobalConfig().GetBool("RA.Encore"));
    rc_client_set_unofficial_enabled(client, emuInstance->getGlobalConfig().GetBool("RA.Unofficial"));
}

bool RetroAchievementsManager::ensureLoggedIn(std::string& error)
{
    if (isLoggedIn())
        return true;

    std::string username = emuInstance->getGlobalConfig().GetString("RA.Username");
    std::string token = emuInstance->getGlobalConfig().GetString("RA.Token");
    if (username.empty() || token.empty())
    {
        error = "not configured";
        return false;
    }

    return loginWithToken(username, token, error);
}

bool RetroAchievementsManager::loadCurrentGame(std::string& error)
{
    melonDS::NDS* nds = emuInstance->getNDS();
    if (!nds)
    {
        error = "no active emulator instance";
        return false;
    }

    const melonDS::NDSCart::CartCommon* cart = nds->GetNDSCart();
    if (!cart)
    {
        onGameStopped();
        error.clear();
        return false;
    }

    applyConfig();
    onGameStopped();

    const uint32_t consoleId = cart->GetHeader().IsDSi() ? RC_CONSOLE_NINTENDO_DSI : RC_CONSOLE_NINTENDO_DS;
    std::string romName = emuInstance->getCurrentROMName();
    if (romName.empty())
        romName = "game.nds";

    SyncResult result;
    rc_client_begin_identify_and_load_game(client, consoleId, romName.c_str(), cart->GetROM(), cart->GetROMLength(), &RetroAchievementsManager::SyncCallback, &result);
    if (!result.called || result.result != RC_OK)
    {
        error = result.error.empty() ? "failed to load game data" : result.error;
        gameLoaded = false;
        return false;
    }

    gameLoaded = rc_client_is_game_loaded(client) != 0;
    if (!gameLoaded)
    {
        error = "game did not load";
        return false;
    }

    rc_client_user_game_summary_t summary {};
    rc_client_get_user_game_summary(client, &summary);
    emuInstance->osdAddMessage(kColorInfo, "RetroAchievements: %u/%u unlocked", summary.num_unlocked_achievements, summary.num_core_achievements);
    return true;
}

void RetroAchievementsManager::handleEvent(const rc_client_event_t* event)
{
    switch (event->type)
    {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        if (event->achievement && event->achievement->title)
            emuInstance->osdAddMessage(kColorSuccess, "Achievement unlocked: %s", event->achievement->title);
        break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
        if (event->achievement && event->achievement->title && event->achievement->measured_progress[0] != '\0')
            emuInstance->osdAddMessage(kColorWarn, "%s (%s)", event->achievement->title, event->achievement->measured_progress);
        break;

    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
        if (event->leaderboard && event->leaderboard->title)
            emuInstance->osdAddMessage(kColorInfo, "Leaderboard started: %s", event->leaderboard->title);
        break;

    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
        if (event->leaderboard && event->leaderboard->title)
            emuInstance->osdAddMessage(kColorSuccess, "Leaderboard submitted: %s", event->leaderboard->title);
        break;

    case RC_CLIENT_EVENT_GAME_COMPLETED:
        emuInstance->osdAddMessage(kColorSuccess, "RetroAchievements: game completed");
        break;

    case RC_CLIENT_EVENT_SUBSET_COMPLETED:
        if (event->subset && event->subset->title)
            emuInstance->osdAddMessage(kColorSuccess, "Subset completed: %s", event->subset->title);
        break;

    case RC_CLIENT_EVENT_DISCONNECTED:
        emuInstance->osdAddMessage(kColorWarn, "RetroAchievements disconnected");
        break;

    case RC_CLIENT_EVENT_RECONNECTED:
        emuInstance->osdAddMessage(kColorSuccess, "RetroAchievements reconnected");
        break;

    case RC_CLIENT_EVENT_SERVER_ERROR:
        if (event->server_error && event->server_error->error_message)
            emuInstance->osdAddMessage(kColorError, "RetroAchievements server error: %s", event->server_error->error_message);
        break;

    case RC_CLIENT_EVENT_RESET:
        emuInstance->getEmuThread()->sendMessage(EmuThread::msg_EmuReset);
        break;

    default:
        break;
    }
}

int RetroAchievementsManager::performRequest(const rc_api_request_t* request, std::string& responseBody)
{
    int statusCode = RC_API_SERVER_RESPONSE_CLIENT_ERROR;

    auto doRequest = [&]()
    {
        QNetworkAccessManager manager;
        QNetworkRequest networkRequest(QUrl(QString::fromUtf8(request->url)));

        char userAgentClause[64] = {};
        rc_client_get_user_agent_clause(client, userAgentClause, sizeof(userAgentClause));
        QString userAgent = QStringLiteral("melonDS/") + QStringLiteral(MELONDS_VERSION);
        if (userAgentClause[0] != '\0')
            userAgent += QStringLiteral(" ") + QString::fromUtf8(userAgentClause);
        networkRequest.setHeader(QNetworkRequest::UserAgentHeader, userAgent);

        if (request->content_type)
            networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromUtf8(request->content_type));

        QByteArray postData = request->post_data ? QByteArray(request->post_data) : QByteArray();
        QNetworkReply* reply = request->post_data ? manager.post(networkRequest, postData) : manager.get(networkRequest);

        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        QByteArray body = reply->readAll();
        responseBody.assign(body.constData(), body.size());

        QVariant statusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (statusAttr.isValid())
            statusCode = statusAttr.toInt();
        else if (reply->error() == QNetworkReply::NoError)
            statusCode = 200;
        else if (reply->error() == QNetworkReply::TimeoutError || reply->error() == QNetworkReply::TemporaryNetworkFailureError)
            statusCode = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        else
            statusCode = RC_API_SERVER_RESPONSE_CLIENT_ERROR;

        reply->deleteLater();
    };

    if (QThread::currentThread() == QCoreApplication::instance()->thread())
        doRequest();
    else
        QMetaObject::invokeMethod(QCoreApplication::instance(), doRequest, Qt::BlockingQueuedConnection);

    return statusCode;
}

void RetroAchievementsManager::saveCredentials(const std::string& username, const std::string& token)
{
    Config::Table& cfg = emuInstance->getGlobalConfig();
    cfg.SetString("RA.Username", username);
    cfg.SetString("RA.Token", token);
    Config::Save();
}

void RetroAchievementsManager::clearCredentials()
{
    Config::Table& cfg = emuInstance->getGlobalConfig();
    cfg.SetString("RA.Username", "");
    cfg.SetString("RA.Token", "");
    Config::Save();
}

uint32_t RetroAchievementsManager::ReadMemory(uint32_t address, uint8_t* buffer, uint32_t numBytes, rc_client_t* client)
{
    auto* manager = static_cast<RetroAchievementsManager*>(rc_client_get_userdata(client));
    if (!manager || !manager->emuInstance)
        return 0;

    melonDS::NDS* nds = manager->emuInstance->getNDS();
    if (!nds)
        return 0;

    const melonDS::NDSCart::CartCommon* cart = nds->GetNDSCart();
    const uint32_t consoleId = (cart && cart->GetHeader().IsDSi()) ? RC_CONSOLE_NINTENDO_DSI : RC_CONSOLE_NINTENDO_DS;

    for (uint32_t i = 0; i < numBytes; i++)
    {
        bool ok = false;
        uint32_t translated = translateAddress(consoleId, address + i, ok);
        if (!ok)
            return i;

        buffer[i] = nds->ARM9Read8(translated);
    }

    return numBytes;
}

void RetroAchievementsManager::ServerCall(const rc_api_request_t* request, void (*callback)(const rc_api_server_response_t*, void*), void* callbackData, rc_client_t* client)
{
    auto* manager = static_cast<RetroAchievementsManager*>(rc_client_get_userdata(client));
    if (!manager)
        return;

    std::string responseBody;
    int statusCode = manager->performRequest(request, responseBody);

    rc_api_server_response_t response {
        responseBody.c_str(),
        responseBody.size(),
        statusCode
    };
    callback(&response, callbackData);
}

void RetroAchievementsManager::EventHandler(const rc_client_event_t* event, rc_client_t* client)
{
    auto* manager = static_cast<RetroAchievementsManager*>(rc_client_get_userdata(client));
    if (manager)
        manager->handleEvent(event);
}

void RetroAchievementsManager::SyncCallback(int result, const char* errorMessage, rc_client_t*, void* userdata)
{
    auto* syncResult = static_cast<SyncResult*>(userdata);
    syncResult->called = true;
    syncResult->result = result;
    if (errorMessage)
        syncResult->error = errorMessage;
    else if (result != RC_OK)
        syncResult->error = rc_error_str(result);
}

}
