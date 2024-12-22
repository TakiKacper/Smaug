#pragma once

#include <cstdint>
#include <string>
#include <atomic>

/*
    Forwards
*/

namespace smaug
{
    struct resource;
    struct resource_handle;

    struct resource_load_promise;
    struct resource_unload_promise;

    using sync_load_resource_callback = resource*(*)(const std::string& resource_name);

    bool is_resource_ready(const resource_handle& handle);
    bool is_handle_valid(const resource_handle& handle);
    resource_handle empty_handle();

    template<class resource_subclass>
    const resource_subclass* dynamic_resource_cast(const resource_handle& handle);

    template<class resource_subclass>
    const resource_subclass* static_resource_cast(const resource_handle& handle); 

    resource_handle get_resource(const std::string& name);
    resource_handle register_resource(const std::string& name, resource* resource, bool automanaged);

    bool is_loading_job_available();
    resource_load_promise* take_resource_loading_job();
    void commit_resource_loading_job(resource_load_promise* promise);

    bool is_unloading_job_available();
    resource_unload_promise* take_resource_unloading_job();
    void commit_resource_unloading_job(resource_unload_promise* promise);

    void mark_all_resources_for_unload();
}

struct smaug_internal;

/*
    Resource
*/

struct smaug::resource
{
    virtual ~resource() = 0;
};

#ifdef SMAUG_IMPLEMENTATION
    smaug::resource::~resource(){};
#endif

/*
    Resource Handles
*/

struct smaug::resource_handle
{
private:
    friend bool smaug::is_resource_ready(const resource_handle& handle);
    friend bool smaug::is_handle_valid(const resource_handle& handle);
    friend resource_handle smaug::empty_handle();

    template<class resource_subclass>
    friend const resource_subclass* smaug::dynamic_resource_cast(const resource_handle& handle);

    template<class resource_subclass>
    friend const resource_subclass* smaug::static_resource_cast(const resource_handle& handle);

    friend smaug::resource_handle smaug::register_resource(const std::string& name, resource* resource, bool automanaged);
    friend smaug::resource_handle smaug::get_resource(const std::string& name);
    
    friend smaug::resource_load_promise* smaug::take_resource_loading_job();
    friend void smaug::commit_resource_loading_job(resource_load_promise* promise);
    
    friend smaug::resource_unload_promise* smaug::take_resource_unloading_job();
    friend void smaug::commit_resource_unloading_job(resource_unload_promise* promise);

    friend void smaug::mark_all_resources_for_unload();

    friend smaug::resource_load_promise;
    friend smaug::resource_unload_promise;

    friend ::smaug_internal;

    resource_handle(        
        const std::string&  resource_uid,
        resource*           resource,
        uint8_t             state,          //implicitly of type resource_state
        bool                automanaged
    );

    struct resource_metadata;
    resource_metadata* meta;

public:
    resource_handle(const resource_handle& other);
    ~resource_handle();

    void operator=(const resource_handle& other) noexcept;
    bool operator==(const resource_handle& other) const noexcept;
    bool operator!=(const resource_handle& other) const noexcept;
};

/*
    Promises
*/

struct smaug::resource_load_promise
{
    const std::string* resource_name;

    resource* loaded_resource = nullptr;
    bool success = false;

    friend resource_load_promise* smaug::take_resource_loading_job();

private:
    resource_load_promise(resource_handle handle);
};

struct smaug::resource_unload_promise
{
    const std::string* resource_name;
    resource* const resource_to_unload = nullptr;

    bool success = false;

    friend resource_unload_promise* smaug::take_resource_unloading_job();

private:
    resource_unload_promise(resource_handle handle);
};

/*
    Implementation
*/

#ifdef SMAUG_IMPLEMENTATION

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <stdexcept>
#include <queue>
#include <list>
#include <thread>

/*
    Forwards
*/

static std::shared_mutex resource_registry_mutex;
static std::unordered_map<std::string, smaug::resource_handle> resource_registry;

static std::mutex load_queue_mutex;
static std::queue<smaug::resource_handle> load_queue;

static std::mutex unload_queue_mutex;
static std::list<smaug::resource_handle> unload_queue;

struct smaug_internal
{
    static smaug::resource_handle safe_create_handle(const std::string& resource_name, bool automanaged);
    static smaug::resource_handle safe_get_handle(const std::string& resource_name);

    static void queue_load  (const smaug::resource_handle& handle);
    static void queue_unload(const smaug::resource_handle& handle);
};

/*
    Resource Handle
*/

enum class resource_state : uint8_t
{
    loaded          = 0,
    unloaded        = 1,

    pending_load    = 2,
    pending_unload  = 4,

    during_load     = 8,
    during_unload   = 16,

    invalid         = 32
};

struct smaug::resource_handle::resource_metadata
{
    //constants
    const std::string           resource_uid;
    const bool                  automanaged;

    //auto managed do not touch
    std::atomic<uint64_t>       references;

    //can be changed
    resource*                   handled_resource;
    resource_state              state;

    resource_metadata(
        const std::string&      _uid, 
        const bool              _automanged) : 
        resource_uid(_uid), 
        automanaged(_automanged)
        {};
};

smaug::resource_handle::resource_handle(        
    const std::string&  resource_uid,
    resource*           resource,
    uint8_t             state,
    bool                automanaged
)
{
    meta = new resource_metadata{resource_uid, automanaged};
    
    meta->references = 1;
    meta->handled_resource = resource;
    meta->state = reinterpret_cast<resource_state&>(state);
}

smaug::resource_handle::resource_handle(const resource_handle& other) :
    meta(other.meta)
{
    if (meta != nullptr)
        meta->references++;
}

smaug::resource_handle::~resource_handle()
{
    if (meta != nullptr)
    {
        meta->references--;

        //if resource is automanaged and no one is using it, unload it
        if (meta->references <= 1 && meta->automanaged && meta->state == resource_state::loaded)
            smaug_internal::queue_unload(*this);

        if (meta->references == 0)
            delete meta;
    }
}

void smaug::resource_handle::operator=(const resource_handle& other) noexcept
{
    if (meta != nullptr)
        meta->references--;

    meta = other.meta;

    if (meta != nullptr)
        meta->references++;
}

bool smaug::resource_handle::operator==(const resource_handle& other) const noexcept
{
    return meta == other.meta;
}

bool smaug::resource_handle::operator!=(const resource_handle& other) const noexcept
{
    return meta != other.meta;
}

template<class resource_subclass>
const resource_subclass* smaug::static_resource_cast(const resource_handle& handle)
{
    auto res = handle.meta->handled_resource;
    return static_cast<resource_subclass*>(res);
}

template<class resource_subclass>
const resource_subclass* smaug::dynamic_resource_cast(const resource_handle& handle)
{
    auto res = handle.meta->handled_resource;
    return dynamic_cast<resource_subclass*>(res);
}

/*
    Promises
*/

smaug::resource_load_promise::resource_load_promise(resource_handle handle) :
    resource_name(&handle.meta->resource_uid) 
{};

smaug::resource_unload_promise::resource_unload_promise(resource_handle handle) :
    resource_name(&handle.meta->resource_uid),
    resource_to_unload(handle.meta->handled_resource)
{};

/*
    Implementation Utilities
*/

//thread safe create handle
smaug::resource_handle smaug_internal::safe_create_handle(const std::string& resource_name, bool automanaged)
{
    //we're going to modify the registry so we obtain the unique lock
    resource_registry_mutex.lock();

    //check if other thread hasn't added this handle while we were waiting for lock
    auto itr = resource_registry.find(resource_name);
    if (itr != resource_registry.end()) 
    {
        auto handle = itr->second;
        resource_registry_mutex.unlock();
        return handle;
    }
    
    itr = resource_registry.insert(
        {
            resource_name, 
            smaug::resource_handle{
                resource_name,
                nullptr, 
                (uint8_t)resource_state::unloaded, 
                automanaged
            }
        }
    ).first;

    {
        auto handle = itr->second;

        resource_registry_mutex.unlock();

        return handle;
    }
}

//thread safe get handle
//may return invalid (nullptr) handle
smaug::resource_handle smaug_internal::safe_get_handle(const std::string& resource_name)
{
    resource_registry_mutex.lock_shared();

    auto itr = resource_registry.find(resource_name);
    smaug::resource_handle handle = itr != resource_registry.end() ? itr->second : smaug::empty_handle(); 

    resource_registry_mutex.unlock_shared();

    return handle;
}

void smaug_internal::queue_load(const smaug::resource_handle& handle)
{
    load_queue_mutex.lock();

    load_queue.push(handle);
    handle.meta->state = resource_state::pending_load;

    load_queue_mutex.unlock();
}

void smaug_internal::queue_unload(const smaug::resource_handle& handle)
{
    load_queue_mutex.lock();
    
    unload_queue.push_back(handle);
    handle.meta->state = resource_state::pending_unload;

    load_queue_mutex.unlock();
}

/*
    API implementation
*/

smaug::resource_handle smaug::register_resource(const std::string& name, resource* resource, bool automanaged)
{  
    {
        std::shared_lock<std::shared_mutex> shared_lock{resource_registry_mutex};

        if (resource_registry.find(name) != resource_registry.end())
            throw std::runtime_error{"Smaug Error: Resource named \"" + name + "\" had been already registered"};
    }

    resource_handle handle = smaug_internal::safe_create_handle(name, automanaged);

    handle.meta->handled_resource = resource;
    handle.meta->state            = resource_state::loaded;

    return handle;
}

smaug::resource_handle smaug::get_resource(const std::string& name)
{
    auto handle = smaug_internal::safe_get_handle(name);

    //if resource havent been found
    if (!is_handle_valid(handle))
    {
        handle = smaug_internal::safe_create_handle(name, true);
        smaug_internal::queue_load(handle);
    }

_get_resource_switch:
    switch (handle.meta->state)
    {
    case resource_state::pending_load: 
    case resource_state::during_load: 
    case resource_state::loaded: 
    case resource_state::invalid:
        break;

    //if it is during unload, then there is another thread that handles unloading, we can simply wait
    case resource_state::during_unload:
        while (handle.meta->state == resource_state::during_unload)
            std::this_thread::yield();
        goto _get_resource_switch;

    //we need to remove resource from the queue
    case resource_state::pending_unload:
        unload_queue_mutex.lock();

        for (auto qitr = unload_queue.begin(); qitr != unload_queue.end(); qitr++)
            if (*qitr == handle)
            {
                unload_queue.erase(qitr);
                handle.meta->state = resource_state::loaded;
                break;
            }
        
        unload_queue_mutex.unlock();
        goto _get_resource_switch;

    case resource_state::unloaded:
        smaug_internal::queue_load(handle);
        break;
    }

    return handle;
}

bool smaug::is_loading_job_available()
{
    return load_queue.size() != 0;
}

bool smaug::is_unloading_job_available()
{
    return unload_queue.size() != 0;
}

smaug::resource_load_promise* smaug::take_resource_loading_job()
{
    resource_load_promise* promise = nullptr;
    std::lock_guard<std::mutex> guard(load_queue_mutex);

    while (true)
    {
        if (load_queue.empty()) return nullptr;

        resource_handle job = load_queue.front();
        load_queue.pop();

        if (job.meta->state != resource_state::pending_load)
            continue;

        job.meta->state = resource_state::during_load;
        promise = new resource_load_promise{job};
        break;
    }

    return promise;
}

smaug::resource_unload_promise* smaug::take_resource_unloading_job()
{
    resource_unload_promise* promise = nullptr;
    std::lock_guard<std::mutex> guard(unload_queue_mutex);

    while (true)
    {
        if (unload_queue.empty()) return nullptr;

        resource_handle job = unload_queue.front();
        unload_queue.erase(unload_queue.begin());

        if (job.meta->state != resource_state::pending_unload)
            continue;

        job.meta->state = resource_state::during_unload;
        promise = new resource_unload_promise{job};
        break;
    }

    return promise;
}

void smaug::commit_resource_loading_job(resource_load_promise* promise)
{
    //with assumtion the handle for given resource exists and is valid
    auto handle = smaug_internal::safe_get_handle(*promise->resource_name);
    
    if (promise->success)
    {
        handle.meta->state = resource_state::loaded;
        handle.meta->handled_resource = promise->loaded_resource;
    }
    else
    {
        handle.meta->state = resource_state::invalid;
        handle.meta->handled_resource = nullptr;
    }
}

void smaug::commit_resource_unloading_job(resource_unload_promise* promise)
{
    //with assumtion the handle for given resource exists and is valid
    auto handle = smaug_internal::safe_get_handle(*promise->resource_name);
    
    if (promise->success)
    {
        handle.meta->state = resource_state::unloaded;
        handle.meta->handled_resource = nullptr;
    }
    else
    {
        throw std::runtime_error{"Smaug Error: Resource \"" + *promise->resource_name + "\" could not be unloaded"};
    }
}

void smaug::mark_all_resources_for_unload()
{
    resource_registry_mutex.lock_shared();

    for (auto& pair : resource_registry)
    {
        auto handle = pair.second;
        if (handle.meta->state == resource_state::loaded && handle.meta->automanaged)
            smaug_internal::queue_unload(handle);
    }

    resource_registry_mutex.unlock_shared();
}

bool smaug::is_handle_valid(const resource_handle& handle)
{
    return handle.meta != nullptr && handle.meta->state != resource_state::invalid;
}

bool smaug::is_resource_ready(const resource_handle& handle)
{
    return  handle.meta != nullptr && handle.meta->state == resource_state::loaded;
}

smaug::resource_handle smaug::empty_handle()
{
    return smaug::resource_handle{"", nullptr, (uint8_t)resource_state::invalid, false};
}

#endif
