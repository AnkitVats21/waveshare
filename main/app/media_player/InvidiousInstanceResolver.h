#pragma once

#include <string>
#include <vector>


class InvidiousInstanceResolver {
public:
    static InvidiousInstanceResolver& getInstance();

    // Resolves and returns the current best active instance host (e.g. "inv.nadeko.net")
    std::string getActiveInstance();

    // Marks the current instance as failed and rotates to the next available instance
    void markInstanceFailed();

    // Discovers alive instances from api.invidious.io or internal fallback list
    bool refreshInstances();

    // Sets a custom instance host (e.g. "raspberrypi:8080" or "192.168.1.50:8080")
    void setCustomInstance(const std::string& host);

private:
    InvidiousInstanceResolver();

    std::string _customHost;

    std::vector<std::string> _instances;
    size_t _currentIndex = 0;
    bool _initialized = false;

    bool queryPublicInstanceList();
    bool testInstance(const std::string& host);
};
