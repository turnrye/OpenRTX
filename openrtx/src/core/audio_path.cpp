/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/audio_path.h"
#include <map>
#include <pthread.h>
#include <set>

/**
 * \internal
 * Data structure representing an audio path with source, destination and
 * priority.
 */
struct Path
{
    int8_t source      = -1;   ///< Source endpoint of the path.
    int8_t destination = -1;   ///< Destination endpoint of the path.
    int8_t priority    = -1;   ///< Path priority level.

    Path(enum AudioSource src, enum AudioSink sink, enum AudioPriority prio)
    {
        source = static_cast<int8_t>(src);
        destination = static_cast<int8_t>(sink);
        priority = static_cast<int8_t>(prio);
    }

    bool isValid() const
    {
        return (source      != -1) &&
               (destination != -1) &&
               (priority    != -1);
    }

    void open() const
    {
        if(isValid() == false)
            return;

        enum AudioSource src  = (enum AudioSource) source;
        enum AudioSink   sink = (enum AudioSink)   destination;

        audio_connect(src, sink);
    }

    void close() const
    {
        if(isValid() == false)
            return;

        enum AudioSource src  = (enum AudioSource) source;
        enum AudioSink   sink = (enum AudioSink)   destination;

        audio_disconnect(src, sink);
    }

    bool isCompatible(const Path& other) const
    {
        if((isValid() == false) || (other.isValid() == false))
            return false;

        enum AudioSource p1Source = (enum AudioSource) source;
        enum AudioSource p2Source = (enum AudioSource) other.source;
        enum AudioSink   p1Sink   = (enum AudioSink)   destination;
        enum AudioSink   p2Sink   = (enum AudioSink)   other.destination;

        return audio_checkPathCompatibility(p1Source, p1Sink, p2Source, p2Sink);
    }

    bool operator<(const Path& other) const
    {
        if((isValid() == false) || (other.isValid() == false))
            return false;

        return (source      < other.source) &&
               (destination < other.destination);
    }
};

/**
 * \internal
 * Data structure representing an established audio route.
 */
struct Route
{
    Path path;                    ///< Path associated to this route.
    std::set< int > suspendList;  ///< Suspended paths with lower priority.
    std::set< int > suspendedBy;  ///< List of paths which suspended this route.

    bool isActive() const
    {
        return suspendedBy.empty();
    }
};


static std::set< int >        activePaths;      // IDs of currently active paths.
static std::map< int, Route > routes;           // Route data of currently active paths.
static int                    pathCounter = 1;  // Counter for path ID generation.

/*
 * The routing tables above are shared state: audioPath_request() and
 * audioPath_release() mutate them from the UI and RTX threads while
 * audioPath_getInfo() / audioPath_getStatus() read them from the codec
 * thread. Without serialisation this is a data race on the std::map/std::set
 * internals, which shows up as intermittent crashes inside the container code.
 * A single mutex around every public entry point is enough: the critical
 * sections are short and none of them re-enter the audio_path API.
 */
static pthread_mutex_t pathMutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * \internal
 * Scope guard that holds pathMutex for the duration of a public call.
 * Destructors run on every return path (exceptions are disabled), so the
 * mutex is always released.
 */
class PathLock
{
public:
    PathLock()
    {
        pthread_mutex_lock(&pathMutex);
    }

    ~PathLock()
    {
        pthread_mutex_unlock(&pathMutex);
    }

    PathLock(const PathLock&)            = delete;
    PathLock& operator=(const PathLock&) = delete;
};


pathId audioPath_request(enum AudioSource source, enum AudioSink sink,
                         enum AudioPriority prio)
{
    const Path path(source, sink, prio);
    if (!path.isValid())
        return -1;

    PathLock lock;

    std::set< int > pathsToSuspend;

    // Check if this new path can be activated, otherwise return -1
    for (const auto& id : activePaths)
    {
        const Path& activePath = routes.at(id).path;
        if(path.isCompatible(activePath))
            continue;

        // Not compatible where active one has higher priority
        if(activePath.priority >= path.priority)
            return -1;

        // Active path has lower priority than this new one
        pathsToSuspend.insert(id);
    }

    // New path can be activated
    const int newPathId = pathCounter;
    pathCounter += 1;
    const Route newRoute{path, pathsToSuspend, {}};

    // Move active paths that should be suspended to the suspend-list and
    // close them to free resources for the new path.
    for(const auto& id : pathsToSuspend)
    {
        activePaths.erase(id);
        routes.at(id).suspendedBy.insert(newPathId);
        routes.at(id).path.close();
    }

    // Set this new path as active and open it
    routes.insert(std::make_pair(newPathId, newRoute));
    activePaths.insert(newPathId);
    path.open();

    return newPathId;
}

pathInfo_t audioPath_getInfo(const pathId id)
{
    pathInfo_t info = {0, 0, 0, 0};

    PathLock lock;

    const auto it = routes.find(id);
    if(it == routes.end())
    {
        info.status = PATH_CLOSED;
        return info;
    }

    info.source = it->second.path.source;
    info.sink   = it->second.path.destination;
    info.prio   = it->second.path.priority;
    if(it->second.isActive())
        info.status = PATH_OPEN;
    else
        info.status = PATH_SUSPENDED;

    return info;
}

enum PathStatus audioPath_getStatus(const pathId id)
{
    PathLock lock;

    const auto it = routes.find(id);

    if(it == routes.end())
        return PATH_CLOSED;

    if(it->second.isActive())
        return PATH_OPEN;

    return PATH_SUSPENDED;
}

void audioPath_release(const pathId id)
{
    PathLock lock;

    auto it = routes.find(id);
    if(it == routes.end())  // Does not exists
        return;

    Route routeToRemove = it->second;
    routes.erase(it);
    activePaths.erase(id);

    // If path is active, close it
    if(routeToRemove.isActive())
        routeToRemove.path.close();

    /*
     * For each path that suspended the one to be removed:
     * - remove the ID from its suspend list.
     * - add to its suspend list the paths suspended by the one being removed.
     */
    for(const auto& i : routeToRemove.suspendedBy)
    {
        auto& suspendList = routes.at(i).suspendList;
        suspendList.erase(id);

        for(const auto& j : routeToRemove.suspendList)
            suspendList.insert(j);
    }

    /*
     * For each path suspended by the one to be removed:
     * - remove the ID from their suspended-by list.
     * - add to their suspended-by list the paths which suspended the one being
     *   removed.
     * - if the path to be removed was not suspended by any other path, resume
     *   the path.
     */
    for(const auto& i : routeToRemove.suspendList)
    {
        auto& suspendedBy = routes.at(i).suspendedBy;
        suspendedBy.erase(id);

        if(routeToRemove.suspendedBy.empty() == false)
        {
            // If I was suspended, propagate who suspended me
            for(const auto& j : routeToRemove.suspendedBy)
                suspendedBy.insert(j);
        }
        else
        {
            // This path can be started again
            if(suspendedBy.empty())
            {
                activePaths.insert(i);
                routes.at(i).path.open();
            }
        }
    }
}
