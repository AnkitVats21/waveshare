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

private:
    InvidiousInstanceResolver();

    std::vector<std::string> _instances;
    size_t _currentIndex = 0;
    bool _initialized = false;

    bool queryPublicInstanceList();
    bool testInstance(const std::string& host);
};
