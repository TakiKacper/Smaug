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
    class resource_manager;

    bool is_resource_ready(const resource_handle& handle);
    bool is_handle_valid(const resource_handle& handle);
    resource_handle empty_handle();

    template<class resource_subclass>
    const resource_subclass* dynamic_resource_cast(const resource_handle& handle);

    template<class resource_subclass>
    const resource_subclass* static_resource_cast(const resource_handle& handle);

    struct resource_load_promise;
    struct resource_unload_promise;
}

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
    friend smaug::resource_manager;

    template<class resource_subclass>
    friend const resource_subclass* smaug::dynamic_resource_cast(const resource_handle& handle);

    template<class resource_subclass>
    friend const resource_subclass* smaug::static_resource_cast(const resource_handle& handle);
    
    friend bool smaug::is_resource_ready(const resource_handle& handle);
    friend bool smaug::is_handle_valid(const resource_handle& handle);
    friend resource_handle smaug::empty_handle();

    friend smaug::resource_load_promise;
    friend smaug::resource_unload_promise;

private:
    enum class resource_state : uint8_t
    {
        loaded,
        unloaded,

        pending_load,
        pending_unload,

        during_load,
        during_unload,

        invalid
    };

    struct resource_metadata
    {
        //constants
        const std::string           resource_uid;
        const bool                  automanaged;
        const resource_manager*     manager;

        //auto managed do not touch
        std::atomic<uint64_t>       references;

        //can be changed
        resource*                   handled_resource;
        resource_state              state;

        resource_metadata(
            const std::string&      _uid, 
            const bool              _automanged,
            const resource_manager* _manager) : 
            resource_uid(_uid), 
            automanaged(_automanged),
            manager(_manager) {};
    };
    resource_metadata* meta;

    resource_handle()
    {
        meta = nullptr;
    }

    resource_handle(
        const std::string&  resource_uid,
        const resource_manager* manager,
        resource*           resource,
        resource_state      state,
        bool                automanaged
    )
    {
        meta = new resource_metadata{resource_uid, automanaged, manager};
        
        meta->references = 1;
        meta->handled_resource = resource;
        meta->state = state;
    }

public:
    resource_handle(const resource_handle& other) :
        meta(other.meta)
    {
        if (meta != nullptr)
            meta->references++;
    };

    ~resource_handle()
    {
        if (meta != nullptr)
        {
            meta->references--;
            if (meta->references == 0)
                delete meta;
        }
    }

    inline void operator=(const resource_handle& other) noexcept
    {
        if (meta != nullptr)
            meta->references--;

        meta = other.meta;

        if (meta != nullptr)
            meta->references++;
    }

    inline bool operator==(const resource_handle& other) const noexcept
    {
        return meta == other.meta;
    }

    inline bool operator!=(const resource_handle& other) const noexcept
    {
        return meta != other.meta;
    }
};

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

struct smaug::resource_load_promise
{
    friend resource_manager;

    const std::string* resource_name;

    resource* loaded_resource = nullptr;
    bool success = false;

private:
    resource_load_promise(resource_handle handle) :
        resource_name(&handle.meta->resource_uid) {};
};

struct smaug::resource_unload_promise
{
    friend resource_manager;

    const std::string* resource_name;
    resource* const resource_to_unload = nullptr;

    bool success = false;

private:
    resource_unload_promise(resource_handle handle) :
        resource_name(&handle.meta->resource_uid),
        resource_to_unload(handle.meta->handled_resource)
    {};
};

/*
    Resource Manager
*/

class smaug::resource_manager
{
public:
    using sync_load_resource_callback = resource*(*)(const std::string& resource_name);

private:
    using resource_state = resource_handle::resource_state;

    struct implementation;
    implementation* impl;

    resource_handle safe_create_handle(const std::string& resource_name, bool automanaged);
    resource_handle safe_get_handle(const std::string& resource_name);

    void queue_load(const resource_handle& handle);
    void queue_unload(const resource_handle& handle);

public:
    resource_manager();
    ~resource_manager();

    resource_handle register_resource(const std::string& name, resource* resource, bool automanaged);
    resource_handle get_resource(const std::string& name);

    bool is_loading_job_available();
    resource_load_promise* take_resource_loading_job();
    void commit_resource_loading_job(resource_load_promise* promise);

    bool is_unloading_job_available();
    resource_unload_promise* take_resource_unloading_job();
    void commit_resource_unloading_job(resource_unload_promise* promise);
};

/*
    Manager Implementation
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

struct smaug::resource_manager::implementation
{
    std::shared_mutex resource_registry_mutex;
    std::unordered_map<std::string, resource_handle> resource_registry;

    std::mutex load_queue_mutex;
    std::queue<resource_handle> load_queue;

    std::mutex unload_queue_mutex;
    std::list<resource_handle> unload_queue;
};

smaug::resource_manager::resource_manager()
{
    impl = new implementation;
}

smaug::resource_manager::~resource_manager()
{
    delete impl;
}

//thread safe create handle
smaug::resource_handle smaug::resource_manager::safe_create_handle(const std::string& resource_name, bool automanaged)
{
    //we're going to modify the registry so we obtain the unique lock
    impl->resource_registry_mutex.lock();

    //check if other thread hasn't added this handle while we were waiting for lock
    auto itr = impl->resource_registry.find(resource_name);
    if (itr != impl->resource_registry.end()) 
    {
        auto handle = itr->second;
        impl->resource_registry_mutex.unlock();
        return handle;
    }
    
    itr = impl->resource_registry.insert(
        {
            resource_name, 
            resource_handle{
                resource_name, 
                this,
                nullptr, 
                resource_state::unloaded, 
                automanaged
            }
        }
    ).first;

    {
        auto handle = itr->second;

        impl->resource_registry_mutex.unlock();

        return handle;
    }
}

//thread safe get handle
//may return invalid (nullptr) handle
smaug::resource_handle smaug::resource_manager::safe_get_handle(const std::string& resource_name)
{
    impl->resource_registry_mutex.lock_shared();

    auto itr = impl->resource_registry.find(resource_name);
    resource_handle handle = itr != impl->resource_registry.end() ? itr->second : resource_handle{}; 

    impl->resource_registry_mutex.unlock_shared();

    return handle;
}

void smaug::resource_manager::queue_load(const resource_handle& handle)
{
    impl->load_queue_mutex.lock();

    impl->load_queue.push(handle);
    handle.meta->state = resource_state::pending_load;

    impl->load_queue_mutex.unlock();
}

void smaug::resource_manager::queue_unload(const resource_handle& handle)
{
    impl->load_queue_mutex.lock();
    
    impl->load_queue.push(handle);
    handle.meta->state = resource_state::pending_unload;

    impl->load_queue_mutex.unlock();
}

smaug::resource_handle smaug::resource_manager::register_resource(const std::string& name, resource* resource, bool automanaged)
{  
    {
        std::shared_lock<std::shared_mutex> shared_lock{impl->resource_registry_mutex};

        if (impl->resource_registry.find(name) != impl->resource_registry.end())
            throw std::runtime_error{"Smaug Error: Resource named \"" + name + "\" had been already registered"};
    }

    resource_handle handle = safe_create_handle(name, automanaged);

    handle.meta->handled_resource = resource;
    handle.meta->state            = resource_state::loaded;

    return handle;
}

smaug::resource_handle smaug::resource_manager::get_resource(const std::string& name)
{
    auto handle = safe_get_handle(name);

    //if resource havent been found
    if (!is_handle_valid(handle))
    {
        handle = safe_create_handle(name, true);
        queue_load(handle);
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
        impl->unload_queue_mutex.lock();

        for (auto qitr = impl->unload_queue.begin(); qitr != impl->unload_queue.end(); qitr++)
            if (*qitr == handle)
            {
                impl->unload_queue.erase(qitr);
                handle.meta->state = resource_state::loaded;
            }
        
        impl->unload_queue_mutex.unlock();
        goto _get_resource_switch;

    case resource_state::unloaded:
        queue_load(handle);
        break;
    }

    return handle;
}

bool smaug::resource_manager::is_loading_job_available()
{
    return impl->load_queue.size() != 0;
}

bool smaug::resource_manager::is_unloading_job_available()
{
    return impl->unload_queue.size() != 0;
}

smaug::resource_load_promise* smaug::resource_manager::take_resource_loading_job()
{
    resource_load_promise* promise = nullptr;
    std::lock_guard<std::mutex> guard(impl->load_queue_mutex);

    while (true)
    {
        if (impl->load_queue.empty()) return nullptr;

        resource_handle job = impl->load_queue.front();
        impl->load_queue.pop();

        if (job.meta->state != resource_state::pending_load)
            continue;

        job.meta->state = resource_state::during_load;
        promise = new resource_load_promise{job};
        break;
    }

    return promise;
}

smaug::resource_unload_promise* smaug::resource_manager::take_resource_unloading_job()
{
    resource_unload_promise* promise = nullptr;
    std::lock_guard<std::mutex> guard(impl->unload_queue_mutex);

    while (true)
    {
        if (impl->unload_queue.empty()) return nullptr;

        resource_handle job = impl->unload_queue.front();
        impl->unload_queue.erase(impl->unload_queue.begin());

        if (job.meta->state != resource_state::pending_unload)
            continue;

        job.meta->state = resource_state::during_unload;
        promise = new resource_unload_promise{job};
        break;
    }

    return promise;
}

void smaug::resource_manager::commit_resource_loading_job(resource_load_promise* promise)
{
    //with assumtion the handle for given resource exists and is valid
    auto handle = safe_get_handle(*promise->resource_name);
    
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

void smaug::resource_manager::commit_resource_unloading_job(resource_unload_promise* promise)
{
    //with assumtion the handle for given resource exists and is valid
    auto handle = safe_get_handle(*promise->resource_name);
    
    if (promise->success)
    {
        handle.meta->state = resource_state::unloaded;
        handle.meta->handled_resource = nullptr;
    }
    else
    {
        queue_unload(handle);
    }
}

#endif

/*
    Utilities
*/

#ifdef SMAUG_IMPLEMENTATION

bool smaug::is_handle_valid(const resource_handle& handle)
{
    return handle.meta != nullptr && handle.meta->state != resource_handle::resource_state::invalid;
}

bool smaug::is_resource_ready(const resource_handle& handle)
{
    return  handle.meta != nullptr && handle.meta->state == resource_handle::resource_state::loaded;
}

smaug::resource_handle smaug::empty_handle()
{
    return smaug::resource_handle{};
}

#endif
