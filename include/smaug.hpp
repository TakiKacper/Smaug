#pragma once
#include <string>

/*
    Forwards
*/

namespace smaug
{
    struct resource;
    struct resource_handle;
    struct resource_load_promise;

/*
    Client Api
*/

    const std::string&      get_resource_name   (const resource_load_promise& promise);
    const std::string&      get_resource_name   (const resource_handle& handle);

    resource_handle         get_resource        (const std::string& name);

    template<class resouce_subclass>
    bool                    check_resource_type (const resource_handle& handle);

    template<class resouce_subclass>
    const resouce_subclass& use_resource        (const resource_handle& handle);

    resource_handle         empty_handle();
    bool                    is_resource_ready   (const resource_handle& handle);
    bool                    is_handle_empty     (const resource_handle& handle);
    
/*
    Resources loading/unloading
*/
    
    resource_handle         register_resource           (const std::string& name, resource* resource);

    resource_load_promise   take_resource_load_job();
    resource_handle         submit_resource_load_job    (resource_load_promise promise, resource* resource);

    resource*               take_resource_unload_job();

    void                    move_all_resources_to_unload();
}

bool operator==(const smaug::resource_handle& lhs, const smaug::resource_handle& rhs) noexcept;
bool operator!=(const smaug::resource_handle& lhs, const smaug::resource_handle& rhs) noexcept;

struct smaug::resource
{
public:
    resource() {};
    virtual ~resource() {};
    resource(const resource&) = delete;
};

struct smaug::resource_load_promise
{
friend smaug::resource_load_promise smaug::take_resource_load_job();
friend smaug::resource_handle       smaug::submit_resource_load_job(resource_load_promise promise, resource* resource);
private:
    //implicitly resource_meta; not to expose the class to client
    void* meta;
public:
    bool operator==(std::nullptr_t other){
        return meta == other;
    }
    bool operator!(){
        return !meta;
    }
};

/*
    Implementation
*/

#ifdef SMAUG_IMPLEMENTATION

#include <atomic>
#include <cstdint>
#include <string>

/*
    Resources State
*/

enum class resource_state
{
    pending_load,
    loaded,
    pending_unload,
    unloaded
};

struct resource_meta
{
    const std::string*      name;
    resource_state          state;
    std::atomic<uint32_t>   handles;
    smaug::resource*        resource;
};

void move_to_unload(resource_meta* meta);

struct smaug::resource_handle
{
    resource_meta* meta;

    ~resource_handle() {
        meta->handles--;
        if (meta->handles == 0)
            move_to_unload(meta);
        meta = nullptr;
    }

    resource_handle(std::nullptr_t) {
        meta = nullptr;
    }

    resource_handle(resource_meta* _meta) : meta(_meta) {
        meta->handles++;
    }

    resource_handle(const resource_handle& other)
    {
        meta = other.meta;
        if (meta) meta->handles++;
    }

    resource_handle& operator=(const resource_handle& other)
    {
        meta = other.meta;
        if (meta) meta->handles++;
        return *this;
    }
};

smaug::resource_handle smaug::empty_handle() {
    return resource_handle(nullptr);
}

bool smaug::is_resource_ready(const resource_handle& handle) {
    return handle.meta->state == resource_state::loaded;
}

bool smaug::is_handle_empty(const resource_handle& handle) {
    return handle.meta == nullptr;
}

const std::string& smaug::get_resource_name(const resource_handle& handle) {
    return *handle.meta->name;
}

template<class resouce_subclass>
bool smaug::check_resource_type(const resource_handle& handle) {
    return dynamic_cast<resouce_subclass*>(handle.meta->resource) != nullptr;
}

template<class resouce_subclass>
const resouce_subclass& smaug::use_resource(const resource_handle& handle) {
    auto ptr = handle.meta->resource;
    return *static_cast<const resouce_subclass*>(ptr);
}

bool operator==(const smaug::resource_handle& lhs, const smaug::resource_handle& rhs) noexcept {
    return lhs.meta == rhs.meta;
}

bool operator!=(const smaug::resource_handle& lhs, const smaug::resource_handle& rhs) noexcept {
    return lhs.meta != rhs.meta;
}

/*
    Resources Registry
*/

#include <mutex>
#include <unordered_map>
#include <deque>

namespace {
    std::mutex resources_registry_mutex;
    std::unordered_map<std::string, resource_meta*> resources_registry;
    
    std::mutex pending_load_registry_mutex;
    std::deque<resource_meta*> pending_load_registry;
    
    std::mutex pending_unload_registry_mutex;
    std::deque<resource_meta*> pending_unload_registry;
}

smaug::resource_handle smaug::get_resource(const std::string& name)
{
    std::lock_guard<std::mutex> lock(resources_registry_mutex);

    //Search for meta handle
    auto itr = resources_registry.find(name);

    //meta handle not found, create one and issue resource load
    if (itr == resources_registry.end())
    {
        resource_meta* meta;
        meta->resource = nullptr;
        meta = new resource_meta;
    
        meta->state = resource_state::pending_load;
        meta->handles = 0;

        auto itr = resources_registry.insert({name, std::move(meta)}).first;
        itr->second->name = &itr->first;   //link resource meta with res name, stored as a map key

        std::lock_guard<std::mutex> lock(pending_load_registry_mutex);
        pending_load_registry.push_back(itr->second);
        
        return smaug::resource_handle(itr->second);
    }

    //Return proper handle
    return smaug::resource_handle(itr->second);
}

smaug::resource_load_promise smaug::take_resource_load_job()
{
    //Nothing to load
    if (!pending_load_registry.size()) goto _return_empty_promise;

    //Loop till found something
    {
        std::lock_guard<std::mutex> lock{pending_load_registry_mutex};
        auto itr = pending_load_registry.begin();
        while (itr != pending_load_registry.end())
        {
            auto meta = *itr;
            itr = pending_load_registry.erase(itr);

            //Resource might have been abonded and directed to unload since request
            //Therefore this check
            if (meta->state == resource_state::pending_load)
            {
                smaug::resource_load_promise x;
                x.meta = meta;
                return x;
            }
        }
    }

    //Nothing to load
_return_empty_promise:
    smaug::resource_load_promise p;
    p.meta = nullptr;
    return p;
}

smaug::resource_handle smaug::submit_resource_load_job(resource_load_promise promise, resource* resource)
{
    smaug::resource_handle handle{reinterpret_cast<resource_meta*>(promise.meta)};

    //Mark resource as loaded if resource != nullptr
    if (resource)
    {
        handle.meta->resource = resource;
        handle.meta->state = resource_state::loaded;
    }
    return handle;
}

smaug::resource* smaug::take_resource_unload_job()
{
    //Nothing to unload
    if (!pending_unload_registry.size()) return nullptr;

    //Loop till found something
    std::lock_guard<std::mutex> lock{pending_load_registry_mutex};
    auto itr = pending_unload_registry.begin();
    while (itr != pending_unload_registry.end())
    {
        auto meta = *itr;
        itr = pending_unload_registry.erase(itr);

        //Resource might have got into use since it was issued to unload
        //Therefore this check
        if (meta->state == resource_state::pending_unload)
        {
            meta->state = resource_state::unloaded;
            meta->resource = nullptr;
            
            return meta->resource;
        }
    }

    //Nothing to unload
    return nullptr;
}

smaug::resource_handle smaug::register_resource(const std::string& name, resource* resource)
{
    std::lock_guard<std::mutex> lock(resources_registry_mutex);

    //Search for meta handle
    auto itr = resources_registry.find(name);

    //meta handle not found, create one
    if (itr == resources_registry.end())
    {
        //Create meta handle
        resource_meta* meta;
        
        meta = new resource_meta;
        meta->resource = resource;

        meta->handles = 0;
        meta->state = resource_state::loaded;

        //Push resource
        auto itr = resources_registry.insert({name, std::move(meta)}).first;
        itr->second->name = &itr->first;   //link resource meta with res name, stored as a map key

        return smaug::resource_handle(meta);
    }

    //Handle found, change it's state
    itr->second->state = resource_state::loaded;
    itr->second->resource = resource;
 
    return smaug::resource_handle(itr->second);
}

void move_to_unload(resource_meta* meta)
{
    meta->state = resource_state::pending_unload;
    std::lock_guard<std::mutex> lock(pending_unload_registry_mutex);
    pending_unload_registry.push_back(meta);
}

void smaug::move_all_resources_to_unload()
{
    std::lock_guard<std::mutex> lock(resources_registry_mutex);

    for (auto& kv : resources_registry)
        move_to_unload(kv.second);
}

#endif
