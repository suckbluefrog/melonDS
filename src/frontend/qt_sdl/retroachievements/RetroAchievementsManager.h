#ifndef MELONDS_QTSDL_RETROACHIEVEMENTSMANAGER_H
#define MELONDS_QTSDL_RETROACHIEVEMENTSMANAGER_H

#include <string>

#include "Savestate.h"

struct rc_api_request_t;
struct rc_api_server_response_t;
struct rc_client_event_t;
struct rc_client_t;

class EmuInstance;

namespace RetroAchievements
{

class RetroAchievementsManager
{
public:
    explicit RetroAchievementsManager(EmuInstance* emuInstance);
    ~RetroAchievementsManager();

    bool isEnabled() const;
    void setEnabled(bool enabled);

    bool isHardcoreEnabled() const;
    void setHardcoreEnabled(bool enabled);

    bool isLoggedIn() const;

    bool loginWithPassword(const std::string& username, const std::string& password, std::string& error);
    bool loginWithToken(const std::string& username, const std::string& token, std::string& error);
    void logout();

    void onGameChanged();
    void onGameStopped();

    void reset();
    void frameUpdate();
    void idle();
    bool doSavestate(melonDS::Savestate* savestate);

private:
    struct SyncResult
    {
        bool called = false;
        int result = 0;
        std::string error;
    };

    EmuInstance* emuInstance;
    rc_client_t* client;
    bool gameLoaded;
    int pollDivider;
    int pollCounter;

    void applyConfig();
    bool ensureLoggedIn(std::string& error);
    bool loadCurrentGame(std::string& error);
    void handleEvent(const rc_client_event_t* event);
    int performRequest(const rc_api_request_t* request, std::string& responseBody);
    void saveCredentials(const std::string& username, const std::string& token);
    void clearCredentials();
    void resetPollState();

    static uint32_t ReadMemory(uint32_t address, uint8_t* buffer, uint32_t numBytes, rc_client_t* client);
    static void ServerCall(const rc_api_request_t* request, void (*callback)(const rc_api_server_response_t*, void*), void* callbackData, rc_client_t* client);
    static void EventHandler(const rc_client_event_t* event, rc_client_t* client);
    static void SyncCallback(int result, const char* errorMessage, rc_client_t* client, void* userdata);
};

}

#endif // MELONDS_QTSDL_RETROACHIEVEMENTSMANAGER_H
