// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <list>
#include <mutex>
#include <string>
#include <vector>

#include <mesos/hook.hpp>

#include <mesos/module/hook.hpp>

#include <process/collect.hpp>
#include <process/future.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "hook/manager.hpp"
#include "module/manager.hpp"

using std::list;
using std::map;
using std::string;
using std::vector;

using process::collect;
using process::Future;

using mesos::modules::ModuleManager;

namespace mesos {
namespace internal {

static std::mutex mutex;
static LinkedHashMap<string, Hook*> availableHooks;


Try<Nothing> HookManager::initialize(const string& hookList)
{
  synchronized (mutex) {
    const vector<string> hooks = strings::split(hookList, ",");
    foreach (const string& hook, hooks) {
      if (availableHooks.contains(hook)) {
        return Error("Hook module '" + hook + "' already loaded");
      }

      if (!ModuleManager::contains<Hook>(hook)) {
        return Error("No hook module named '" + hook + "' available");
      }

      // Let's create an instance of the hook module.
      Try<Hook*> module = ModuleManager::create<Hook>(hook);
      if (module.isError()) {
        return Error(
            "Failed to instantiate hook module '" + hook + "': " +
            module.error());
      }

      // Add the hook module to the list of available hooks.
      availableHooks[hook] = module.get();
    }
  }

  return Nothing();
}


Try<Nothing> HookManager::unload(const string& hookName)
{
  synchronized (mutex) {
    if (!availableHooks.contains(hookName)) {
      return Error(
          "Error unloading hook module '" + hookName + "': module not loaded");
    }

    // Now remove the hook from the list of available hooks.
    availableHooks.erase(hookName);
  }

  return Nothing();
}


bool HookManager::hooksAvailable()
{
  synchronized (mutex) {
    return !availableHooks.empty();
  }
}


Labels HookManager::masterLaunchTaskLabelDecorator(
    const TaskInfo& taskInfo,
    const FrameworkInfo& frameworkInfo,
    const SlaveInfo& slaveInfo)
{
  synchronized (mutex) {
    // We need a mutable copy of the task info and set the new
    // labels after each hook invocation. Otherwise, the last hook
    // will be the only effective hook setting the labels.
    TaskInfo taskInfo_ = taskInfo;

    foreach (const string& name, availableHooks.keys()) {
      Hook* hook = availableHooks[name];
      const Result<Labels> result =
        hook->masterLaunchTaskLabelDecorator(
            taskInfo_,
            frameworkInfo,
            slaveInfo);

      // NOTE: If the hook returns None(), the task labels won't be
      // changed.
      if (result.isSome()) {
        taskInfo_.mutable_labels()->CopyFrom(result.get());
      } else if (result.isError()) {
        LOG(WARNING) << "Master label decorator hook failed for module '"
                    << name << "': " << result.error();
      }
    }

    return taskInfo_.labels();
  }
}


void HookManager::masterSlaveLostHook(const SlaveInfo& slaveInfo)
{
  foreach (const string& name, availableHooks.keys()) {
    Hook* hook = availableHooks[name];
    Try<Nothing> result = hook->masterSlaveLostHook(slaveInfo);
    if (result.isError()) {
      LOG(WARNING) << "Master agent-lost hook failed for module '"
                   << name << "': " << result.error();
    }
  }
}


Labels HookManager::slaveRunTaskLabelDecorator(
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const FrameworkInfo& frameworkInfo,
    const SlaveInfo& slaveInfo)
{
  synchronized (mutex) {
    TaskInfo taskInfo_ = taskInfo;

    foreach (const string& name, availableHooks.keys()) {
      Hook* hook = availableHooks[name];
      const Result<Labels> result = hook->slaveRunTaskLabelDecorator(
          taskInfo_, executorInfo, frameworkInfo, slaveInfo);

      // NOTE: If the hook returns None(), the task labels won't be
      // changed.
      if (result.isSome()) {
        taskInfo_.mutable_labels()->CopyFrom(result.get());
      } else if (result.isError()) {
        LOG(WARNING) << "Agent label decorator hook failed for module '"
                    << name << "': " << result.error();
      }
    }

    return taskInfo_.labels();
  }
}


Environment HookManager::slaveExecutorEnvironmentDecorator(
    ExecutorInfo executorInfo)
{
  synchronized (mutex) {
    foreach (const string& name, availableHooks.keys()) {
      Hook* hook = availableHooks[name];
      const Result<Environment> result =
        hook->slaveExecutorEnvironmentDecorator(executorInfo);

      // NOTE: If the hook returns None(), the environment won't be
      // changed.
      if (result.isSome()) {
        executorInfo.mutable_command()->mutable_environment()->CopyFrom(
            result.get());
      } else if (result.isError()) {
        LOG(WARNING) << "Agent environment decorator hook failed for module '"
                    << name << "': " << result.error();
      }
    }

    return executorInfo.command().environment();
  }
}


Future<map<string, string>>
  HookManager::slavePreLaunchDockerEnvironmentDecorator(
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const string& containerName,
      const string& sandboxDirectory,
      const string& mappedDirectory,
      const Option<map<string, string>>& env)
{
  // We execute these hooks according to their ordering so any conflicting
  // environment variables can be deterministically resolved
  // (the last hook takes priority).
  list<Future<Option<Environment>>> futures;

  foreach (const string& name, availableHooks.keys()) {
    Hook* hook = availableHooks[name];

    // Chain together each hook.
    futures.push_back(
        hook->slavePreLaunchDockerEnvironmentDecorator(
            taskInfo,
            executorInfo,
            containerName,
            sandboxDirectory,
            mappedDirectory,
            env));
  }

  return collect(futures)
    .then([](const list<Option<Environment>>& results)
        -> Future<map<string, string>> {
      // Combine all the Environments.
      map<string, string> environment;

      foreach (const Option<Environment>& result, results) {
        if (result.isNone()) {
          continue;
        }

        foreach (const Environment::Variable& variable, result->variables()) {
          environment[variable.name()] = variable.value();
        }
      }

      return environment;
    });
}


void HookManager::slavePreLaunchDockerHook(
    const ContainerInfo& containerInfo,
    const CommandInfo& commandInfo,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& containerName,
    const string& sandboxDirectory,
    const string& mappedDirectory,
    const Option<Resources>& resources,
    const Option<map<string, string>>& env)
{
  foreach (const string& name, availableHooks.keys()) {
    Hook* hook = availableHooks[name];
    Try<Nothing> result =
      hook->slavePreLaunchDockerHook(
          containerInfo,
          commandInfo,
          taskInfo,
          executorInfo,
          containerName,
          sandboxDirectory,
          mappedDirectory,
          resources,
          env);
    if (result.isError()) {
      LOG(WARNING) << "Agent pre launch docker hook failed for module '"
                   << name << "': " << result.error();
    }
  }
}


void HookManager::slavePostFetchHook(
    const ContainerID& containerId,
    const string& directory)
{
  foreach (const string& name, availableHooks.keys()) {
    Hook* hook = availableHooks[name];
    Try<Nothing> result = hook->slavePostFetchHook(containerId, directory);
    if (result.isError()) {
      LOG(WARNING) << "Agent post fetch hook failed for module "
                   << "'" << name << "': " << result.error();
    }
  }
}


void HookManager::slaveRemoveExecutorHook(
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo)
{
  foreach (const string& name, availableHooks.keys()) {
    Hook* hook = availableHooks[name];
    const Try<Nothing> result =
      hook->slaveRemoveExecutorHook(frameworkInfo, executorInfo);
    if (result.isError()) {
      LOG(WARNING) << "Agent remove executor hook failed for module '"
                   << name << "': " << result.error();
    }
  }
}


TaskStatus HookManager::slaveTaskStatusDecorator(
    const FrameworkID& frameworkId,
    TaskStatus status)
{
  synchronized (mutex) {
    foreach (const string& name, availableHooks.keys()) {
      Hook* hook = availableHooks[name];
      const Result<TaskStatus> result =
        hook->slaveTaskStatusDecorator(frameworkId, status);

      // NOTE: Labels/ContainerStatus remain unchanged if the hook returns
      // None().
      if (result.isSome()) {
        if (result.get().has_labels()) {
          status.mutable_labels()->CopyFrom(result.get().labels());
        }

        if (result.get().has_container_status()) {
          status.mutable_container_status()->CopyFrom(
              result.get().container_status());
        }
      } else if (result.isError()) {
        LOG(WARNING) << "Agent TaskStatus decorator hook failed for "
                     << "module '" << name << "': " << result.error();
      }
    }

    return status;
  }
}

Resources HookManager::slaveResourcesDecorator(
    const SlaveInfo& slaveInfo)
{
  // We need a mutable copy of the Resources object. Each hook will see the
  // changes made by previous hooks, so the order of execution matters. The
  // execution order is currently unspecified since availableHooks uses a
  // hashmap.
  SlaveInfo slaveInfo_ = slaveInfo;

  synchronized (mutex) {
    foreach (const string& name, availableHooks.keys()) {
      Hook* hook = availableHooks[name];
      const Result<Resources> result =
        hook->slaveResourcesDecorator(slaveInfo_);

      // NOTE: Resources remain unchanged if the hook returns None().
      if (result.isSome()) {
        slaveInfo_.mutable_resources()->CopyFrom(result.get());
      } else if (result.isError()) {
        LOG(WARNING) << "Agent Resources decorator hook failed for "
                     << "module '" << name << "': " << result.error();
      }
    }

    return slaveInfo_.resources();
  }
}

Attributes HookManager::slaveAttributesDecorator(
    const SlaveInfo& slaveInfo)
{
  // We need a mutable copy of the Attributes object. Each hook will see the
  // changes made by previous hooks, so the order of execution matters. The
  // execution order is currently unspecified since availableHooks uses a
  // hashmap.
  SlaveInfo slaveInfo_ = slaveInfo;

  synchronized (mutex) {
    foreach (const string& name, availableHooks.keys()) {
      Hook* hook = availableHooks[name];
      const Result<Attributes> result =
        hook->slaveAttributesDecorator(slaveInfo_);

      // NOTE: Attributes remain unchanged if the hook returns None().
      if (result.isSome()) {
        slaveInfo_.mutable_attributes()->CopyFrom(result.get());
      } else if (result.isError()) {
        LOG(WARNING) << "Agent Attributes decorator hook failed for "
                     << "module '" << name << "': " << result.error();
      }
    }

    return slaveInfo_.attributes();
  }
}

} // namespace internal {
} // namespace mesos {
