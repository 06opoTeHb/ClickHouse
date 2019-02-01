#include "ExternalLoader.h"
#include <Core/Defines.h>
#include <Common/StringUtils/StringUtils.h>
#include <Common/MemoryTracker.h>
#include <Common/Exception.h>
#include <Common/typeid_cast.h>
#include <Common/setThreadName.h>
#include <Parsers/ASTCreateQuery.h>
#include <ext/scope_guard.h>
#include <Poco/Util/Application.h>
#include <cmath>

namespace DB
{

namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
extern const int BAD_ARGUMENTS;
extern const int EXTERNAL_LOADABLE_ALREADY_EXISTS;
extern const int EXTERNAL_LOADABLE_IS_MISSING;
}


ExternalLoadableLifetime::ExternalLoadableLifetime(const Poco::Util::AbstractConfiguration & config,
                                                   const std::string & config_prefix)
{
    const auto & lifetime_min_key = config_prefix + ".min";
    const auto has_min = config.has(lifetime_min_key);

    min_sec = has_min ? config.getUInt64(lifetime_min_key) : config.getUInt64(config_prefix);
    max_sec = has_min ? config.getUInt64(config_prefix + ".max") : min_sec;
}


ExternalLoadableLifetime::ExternalLoadableLifetime(const ASTKeyValueFunction * lifetime)
{
    if (lifetime->name != "lifetime")
        throw Exception("ExternalLoadableLifetime: AST should be in the following form LIFETIME(MIN 0 MAX 1)", ErrorCodes::BAD_ARGUMENTS);

    for (const auto & child : lifetime->elements->children)
    {
        const auto & pair = typeid_cast<const ASTPair &>(*child.get());
        if (pair.first == "min")
            min_sec = typeid_cast<const ASTLiteral &>(*pair.second.get()).value.get<UInt64>();
        else if (pair.first == "max")
            max_sec = typeid_cast<const ASTLiteral &>(*pair.second.get()).value.get<UInt64>();
    }

    if (min_sec > max_sec)
    {
        throw Exception("ExternalLoadableLifetime: min_sec can't be greater than max_sec. min_sec="
                        + std::to_string(min_sec) + " max_sec=" + std::to_string(max_sec) , ErrorCodes::BAD_ARGUMENTS);
    }
}


void ExternalLoader::reloadPeriodically()
{
    setThreadName("ExterLdrReload");

    while (true)
    {
        if (destroy.tryWait(update_settings.check_period_sec * 1000))
            return;

        reloadAndUpdate();
    }
}


ExternalLoader::ExternalLoader(const Poco::Util::AbstractConfiguration & config_main,
                               const ExternalLoaderUpdateSettings & update_settings,
                               const ExternalLoaderConfigSettings & config_settings,
                               std::unique_ptr<IConfigRepository> config_repository,
                               Logger * log, const std::string & loadable_object_name)
    : config_main(config_main)
    , update_settings(update_settings)
    , config_settings(config_settings)
    , config_repository(std::move(config_repository))
    , log(log)
    , object_name(loadable_object_name)
{
}


void ExternalLoader::init(bool throw_on_error)
{
    if (is_initialized)
        return;

    is_initialized = true;

    {
        /// During synchronous loading of external dictionaries at moment of query execution,
        /// we should not use per query memory limit.
        auto temporarily_disable_memory_tracker = getCurrentMemoryTrackerActionLock();

        reloadAndUpdate(throw_on_error);
    }

    reloading_thread = ThreadFromGlobalPool{&ExternalLoader::reloadPeriodically, this};
}


ExternalLoader::~ExternalLoader()
{
    destroy.set();
    /// It can be partially initialized
    if (reloading_thread.joinable())
        reloading_thread.join();
}


void ExternalLoader::addObjectFromDatabase(
    const std::string & database_name,
    const std::string & object_name,
    std::shared_ptr<IExternalLoadable> loadable_object)
{
    std::string name = database_name + '.' + object_name;
    LOG_DEBUG(log, "ADD OBJECT " + name);

    std::lock_guard map_lock{database_objects_map_mutex};
    if (loadable_objects_from_databases.find(name) != std::end(loadable_objects_from_databases))
        throw Exception("Can't add loadable object. " + name + " already exists.", ErrorCodes::EXTERNAL_LOADABLE_ALREADY_EXISTS);

    auto info = LoadableInfo{std::move(loadable_object), ConfigurationSourceType::DDL, name, {}};
    loadable_objects_from_databases.emplace(name, info);
    update_times[name] = getNextUpdateTime(info.loadable);
}


void ExternalLoader::removeObject(const std::string & database_name, const std::string & object_name)
{
    std::string name = database_name + "." + object_name;
    std::lock_guard map_lock{database_objects_map_mutex};
    if (loadable_objects_from_databases.find(name) == std::end(loadable_objects_from_databases))
        throw Exception("Can't remove object. " + name + " is missing.", ErrorCodes::EXTERNAL_LOADABLE_IS_MISSING);

    loadable_objects_from_databases.erase(name);
}


void ExternalLoader::reloadAndUpdate(bool throw_on_error)
{
    reloadFromConfigFiles(throw_on_error);

    /// list of recreated loadable objects to perform delayed removal from unordered_map
    std::list<std::string> recreated_failed_loadable_objects;

    std::lock_guard all_lock(all_mutex);

    /// retry loading failed loadable objects
    for (auto & [name, object_info] : failed_loadable_objects)
    {
        if (std::chrono::system_clock::now() < object_info.next_attempt_time)
            continue;

        try
        {
            auto loadable_ptr = object_info.loadable->clone();
            if (const auto exception_ptr = loadable_ptr->getCreationException())
            {
                /// recalculate next attempt time
                std::uniform_int_distribution<UInt64> distribution(
                    0, static_cast<UInt64>(std::exp2(object_info.error_count)));

                std::chrono::seconds delay(std::min<UInt64>(
                    update_settings.backoff_max_sec,
                    update_settings.backoff_initial_sec + distribution(rnd_engine)));
                object_info.next_attempt_time = std::chrono::system_clock::now() + delay;

                ++object_info.error_count;
                std::rethrow_exception(exception_ptr);
            }
            else
            {
                std::lock_guard lock{map_mutex};
                update_times[name] = getNextUpdateTime(loadable_ptr);
                const auto dict_it = loadable_objects_from_filesystem.find(name);
                dict_it->second.loadable = std::move(loadable_ptr);

                /// clear stored exception on success
                dict_it->second.exception = std::exception_ptr{};

                recreated_failed_loadable_objects.push_back(name);
            }
        }
        catch (...)
        {
            tryLogCurrentException(log, "Failed reloading '" + name + "' " + object_name);
            if (throw_on_error)
                throw;
        }
    }

    /// do not undertake further attempts to recreate these loadable objects
    for (const auto & name : recreated_failed_loadable_objects)
        failed_loadable_objects.erase(name);

    updateAll(throw_on_error);
}


/// This function should be called under map_mutex
bool ExternalLoader::checkLoadableObjectToUpdate(LoadableInfo object)
{
    /// If the loadable object failed to load or even failed to initialize.
    if (!object.loadable)
        return false;

    const LoadablePtr & current = object.loadable;
    const auto & lifetime = current->getLifetime();

    /// do not update loadable object with zero as lifetime
    if (lifetime.min_sec == 0 || lifetime.max_sec == 0)
        return false;

    if (!current->supportUpdates())
        return false;

    auto update_time = update_times[current->getName()];
    if (std::chrono::system_clock::now() < update_time)
        return false;

    if (!current->isModified())
        return false;

    return true;
}


void ExternalLoader::updateObjects(
    ObjectsMap & loadable_objects,
    std::mutex & mutex,
    bool throw_on_error)
{
    std::vector<std::pair<String, LoadablePtr>> objects_to_update;

    /// Collect objects that needs to be updated under lock. Then create new versions without lock and assign under lock.
    {
        std::lock_guard lock{mutex};
        for (auto & [name, object] : loadable_objects)
        {
            LOG_DEBUG(log, "CHECK OBJECT " + name);
            if (checkLoadableObjectToUpdate(object))
                objects_to_update.emplace_back(name, object.loadable);
        }
    }

    for (auto & [name, current] : objects_to_update)
    {
        LOG_DEBUG(log, "TO UPDATE OBJECT " + name);
        LoadablePtr new_version;
        std::exception_ptr exception;

        try
        {
            new_version = current->clone();
            exception = new_version->getCreationException();
        }
        catch (...)
        {
            exception = std::current_exception();
        }

        std::lock_guard map_lock{mutex};
        auto it = loadable_objects.find(name);
        if (it == loadable_objects.end())
            continue;

        update_times[name] = getNextUpdateTime(current);
        it->second.exception = exception;
        if (!exception)
        {
            it->second.loadable.reset();
            it->second.loadable = std::move(new_version);
        }
        else
        {
            tryLogException(exception, log, "Cannot update " + object_name + " '" + name + "', leaving old version");
            if (throw_on_error)
                std::rethrow_exception(exception);
        }
    }
}


void ExternalLoader::updateAll(bool throw_on_error)
{
    updateObjects(loadable_objects_from_filesystem, map_mutex, throw_on_error);
    updateObjects(loadable_objects_from_databases, database_objects_map_mutex, throw_on_error);
}


void ExternalLoader::reloadFromConfigFiles(const bool throw_on_error, const bool force_reload, const std::string & only_dictionary)
{
    const auto config_paths = config_repository->list(config_main, config_settings.path_setting_name);
    for (const auto & config_path : config_paths)
    {
        try
        {
            reloadFromConfigFile(config_path, throw_on_error, force_reload, only_dictionary);
        }
        catch (...)
        {
            tryLogCurrentException(log, "reloadFromConfigFile has thrown while reading from " + config_path);
            if (throw_on_error)
                throw;
        }
    }

    /// erase removed from config loadable objects
    std::lock_guard lock{map_mutex};

    std::list<std::string> removed_loadable_objects_names;
    for (const auto & loadable : loadable_objects_from_filesystem)
    {
        const auto & current_config = loadable_objects_defined_in_config[loadable.second.origin];
        if (current_config.find(loadable.first) == std::end(current_config))
            removed_loadable_objects_names.emplace_back(loadable.first);
    }

    for (const auto & name : removed_loadable_objects_names)
        loadable_objects_from_filesystem.erase(name);
}


void ExternalLoader::reloadFromConfigFile(const std::string & config_path, const bool throw_on_error,
                                          const bool force_reload, const std::string & loadable_name)
{
    if (config_path.empty() || !config_repository->exists(config_path))
    {
        LOG_WARNING(log, "config file '" + config_path + "' does not exist");
        return;
    }

    std::lock_guard all_lock(all_mutex);
    auto modification_time_it = last_modification_times.find(config_path);
    if (modification_time_it == std::end(last_modification_times))
        modification_time_it = last_modification_times.emplace(config_path, Poco::Timestamp{0}).first;

    auto & config_last_modified = modification_time_it->second;
    const auto last_modified = config_repository->getLastModificationTime(config_path);
    if (!force_reload && last_modified <= config_last_modified)
        return;

    // TODO: maybe insert here if
    auto loaded_config = config_repository->load(config_path, config_main.getString("path", DBMS_DEFAULT_PATH));
    loadable_objects_defined_in_config[config_path].clear();

    /// Definitions of loadable objects may have changed, recreate all of them

    /// If we need update only one object, don't update modification time: might be other objects in the config file
    if (loadable_name.empty())
        config_last_modified = last_modified;

    /// get all objects' definitions
    Poco::Util::AbstractConfiguration::Keys keys;
    loaded_config->keys(keys);

    /// for each loadable object defined in xml config
    for (const auto & key : keys)
    {
        if (!startsWith(key, config_settings.external_config))
        {
            if (!startsWith(key, "comment") && !startsWith(key, "include_from"))
                LOG_WARNING(log, config_path << ": unknown node in file: '" << key
                                             << "', expected '" << config_settings.external_config << "'");
            continue;
        }

        std::string name;
        try
        {
            name = loaded_config->getString(key + "." + config_settings.external_name);
            if (name.empty())
            {
                LOG_WARNING(log, config_path << ": " + config_settings.external_name + " name cannot be empty");
                continue;
            }

            loadable_objects_defined_in_config[config_path].emplace(name);
            if (!loadable_name.empty() && name != loadable_name)
                continue;

            decltype(loadable_objects_from_filesystem.begin()) object_it;
            {
                std::lock_guard lock{map_mutex};
                object_it = loadable_objects_from_filesystem.find(name);

                /// Object with the same name was declared in other config file.
                if (object_it != std::end(loadable_objects_from_filesystem) &&
                    object_it->second.source_type == ConfigurationSourceType::DDL)
                {
                    throw Exception(object_name + " '" + name + "' from file " + config_path + " already declared in ddl.",
                                    ErrorCodes::EXTERNAL_LOADABLE_ALREADY_EXISTS);
                }

                if (object_it != std::end(loadable_objects_from_filesystem) && object_it->second.origin != config_path)
                    throw Exception(object_name + " '" + name + "' from file " + config_path
                                    + " already declared in file " + object_it->second.origin,
                                    ErrorCodes::EXTERNAL_LOADABLE_ALREADY_EXISTS);

            }

            auto object_ptr = create(name, *loaded_config, key);

            /// If the object could not be loaded.
            if (const auto exception_ptr = object_ptr->getCreationException())
            {
                // TODO: maybe carry out in separate method
                std::chrono::seconds delay(update_settings.backoff_initial_sec);
                // TODO: maybe we need mutex for failed_loadable_objects
                const auto failed_dict_it = failed_loadable_objects.find(name);
                FailedLoadableInfo info{std::move(object_ptr), std::chrono::system_clock::now() + delay, 0};
                if (failed_dict_it != std::end(failed_loadable_objects))
                    (*failed_dict_it).second = std::move(info);
                else
                    failed_loadable_objects.emplace(name, std::move(info));

                std::rethrow_exception(exception_ptr);
            }
            else if (object_ptr->supportUpdates())
                update_times[name] = getNextUpdateTime(object_ptr);

            std::lock_guard lock{map_mutex};

            // TODO: кажется, что этот object_it может инвалидироваться
            /// add new loadable object or update an existing version
            object_it = loadable_objects_from_filesystem.find(name);
            if (object_it == std::end(loadable_objects_from_filesystem))
            {
                auto info = LoadableInfo{
                    std::move(object_ptr),
                    ConfigurationSourceType::Filesystem,
                    config_path,
                    /*exception_ptr*/ {},
                };
                loadable_objects_from_filesystem.emplace(name, std::move(info));
            }
            else
            {
                // TODO: check it
                // object_it->second.loadable.reset();
                object_it->second.loadable = std::move(object_ptr);

                /// erase stored exception on success
                object_it->second.exception = std::exception_ptr{};
                failed_loadable_objects.erase(name);
            }
        }
        catch (...)
        {
            if (!name.empty())
            {
                /// If the loadable object could not load data or even failed to initialize from the config.
                /// - all the same we insert information into the `loadable_objects`, with the zero pointer `loadable`.

                std::lock_guard lock{map_mutex};
                const auto exception_ptr = std::current_exception();
                const auto loadable_it = loadable_objects_from_filesystem.find(name);
                if (loadable_it == std::end(loadable_objects_from_filesystem))
                {
                    auto info = LoadableInfo{
                        nullptr,
                        ConfigurationSourceType::Filesystem,
                        config_path,
                        exception_ptr,
                    };
                    loadable_objects_from_filesystem.emplace(name, std::move(info));
                }
                else
                    loadable_it->second.exception = exception_ptr;
            }

            tryLogCurrentException(log, "Cannot create " + object_name + " '"
                                        + name + "' from config path " + config_path);

            if (throw_on_error)
                throw;
        }
    }
}


void ExternalLoader::reload()
{
    reloadFromConfigFiles(true, true);
}


void ExternalLoader::reload(const std::string & name)
{
    // TODO: некорретно работает, если вызвать из reloadDictionary
    reloadFromConfigFiles(true, true, name);

    /// Check that specified object was loaded
    std::lock_guard lock{map_mutex};
    if (!loadable_objects_from_filesystem.count(name))
        throw Exception("Failed to load " + object_name + " '" + name + "' during the reload process", ErrorCodes::BAD_ARGUMENTS);
}


ExternalLoader::LoadablePtr ExternalLoader::getLoadableImpl(const std::string & name, bool throw_on_error) const
{
    std::lock_guard lock{map_mutex};

    // TODO: maybe carry out in separate method
    auto it = loadable_objects_from_filesystem.find(name);
    if (it == std::end(loadable_objects_from_filesystem))
    {
        if (throw_on_error)
            throw Exception("No such " + object_name + ": " + name, ErrorCodes::BAD_ARGUMENTS);
        return nullptr;
    }

    if (!it->second.loadable && throw_on_error)
    {
        if (it->second.exception)
            std::rethrow_exception(it->second.exception);
        else
            throw Exception{object_name + " '" + name + "' is not loaded", ErrorCodes::LOGICAL_ERROR};
    }

    return it->second.loadable;
}


ExternalLoader::LoadablePtr ExternalLoader::getLoadableFromDatabasesImpl(const std::string & name, bool throw_on_error) const
{
    std::lock_guard lock{database_objects_map_mutex};

    auto it = loadable_objects_from_databases.find(name);
    if (it == std::end(loadable_objects_from_databases))
    {
        if (throw_on_error)
            throw Exception("No such " + object_name + ": " + name, ErrorCodes::BAD_ARGUMENTS);
        return nullptr;
    }

    if (!it->second.loadable && throw_on_error)
    {
        if (it->second.exception)
            std::rethrow_exception(it->second.exception);
        else
            throw Exception(object_name + " '" + name + "' is not loaded", ErrorCodes::LOGICAL_ERROR);
    }

    return it->second.loadable;
}



ExternalLoader::LoadablePtr ExternalLoader::getLoadable(const std::string & name) const
{
    return getLoadableImpl(name, true);
}


ExternalLoader::LoadablePtr ExternalLoader::getLoadable(const std::string & database_name, const std::string & name) const
{
    if (database_name.empty() || name.empty())
        return {};

    return getLoadableFromDatabasesImpl(database_name + '.' + name, false);
}


ExternalLoader::LoadablePtr ExternalLoader::tryGetLoadable(const std::string & name) const
{
    return getLoadableImpl(name, false);
}

ExternalLoader::LoadablePtr ExternalLoader::tryGetLoadable(const std::string & database_name, const std::string & name) const
{
    if (database_name.empty() || name.empty())
        return {};

    return getLoadableFromDatabasesImpl(database_name + '.' + name, false);
}


ExternalLoader::LockedObjectsMap ExternalLoader::getObjectsMap() const
{
    return LockedObjectsMap(map_mutex, loadable_objects_from_filesystem);
}


ExternalLoader::TimePoint ExternalLoader::getNextUpdateTime(const LoadablePtr & loadable)
{
    const auto & lifetime = loadable->getLifetime();
    if (lifetime.max_sec < lifetime.min_sec)
        return TimePoint(std::chrono::seconds(0));

    // TODO: maybe check that min, max not equals zero

    std::uniform_int_distribution<UInt64> distribution(lifetime.min_sec, lifetime.max_sec);
    return std::chrono::system_clock::now() + std::chrono::seconds{distribution(rnd_engine)};
}

}
