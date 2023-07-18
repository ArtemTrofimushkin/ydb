#pragma once
#include <library/cpp/actors/core/events.h>
#include <library/cpp/actors/core/event_local.h>
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <ydb/library/accessor/accessor.h>
#include <ydb/core/tx/conveyor/usage/abstract.h>
#include <ydb/library/services/services.pb.h>
#include <ydb/library/conclusion/result.h>
#include <library/cpp/actors/core/log.h>
#include <library/cpp/actors/core/hfunc.h>

namespace NKikimr::NConveyor {

class TWorkerTask {
private:
    YDB_READONLY_DEF(ITask::TPtr, Task);
    YDB_READONLY_DEF(NActors::TActorId, OwnerId);
    YDB_READONLY(TMonotonic, CreateInstant, TMonotonic::Now());
    std::optional<TMonotonic> StartInstant;
public:
    void OnBeforeStart() {
        StartInstant = TMonotonic::Now();
    }

    TMonotonic GetStartInstant() const {
        Y_VERIFY(!!StartInstant);
        return *StartInstant;
    }

    TWorkerTask(ITask::TPtr task, const NActors::TActorId& ownerId)
        : Task(task)
        , OwnerId(ownerId) {
        Y_VERIFY(task);
    }

    bool operator<(const TWorkerTask& wTask) const {
        return Task->GetPriority() < wTask.Task->GetPriority();
    }
};

struct TEvInternal {
    enum EEv {
        EvNewTask = EventSpaceBegin(NActors::TEvents::ES_PRIVATE),
        EvTaskProcessedResult,
        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(NActors::TEvents::ES_PRIVATE), "expected EvEnd < EventSpaceEnd");

    class TEvNewTask: public NActors::TEventLocal<TEvNewTask, EvNewTask> {
    private:
        TWorkerTask Task;
    public:
        TEvNewTask() = default;

        const TWorkerTask& GetTask() const {
            return Task;
        }

        explicit TEvNewTask(const TWorkerTask& task)
            : Task(task) {
        }
    };

    class TEvTaskProcessedResult:
        public NActors::TEventLocal<TEvTaskProcessedResult, EvTaskProcessedResult>,
        public TConclusion<ITask::TPtr> {
    private:
        using TBase = TConclusion<ITask::TPtr>;
        YDB_READONLY_DEF(TMonotonic, StartInstant);
        YDB_READONLY_DEF(NActors::TActorId, OwnerId);
    public:
        TEvTaskProcessedResult(const TWorkerTask& originalTask, const TString& errorMessage)
            : TBase(TConclusionStatus::Fail(errorMessage))
            , StartInstant(originalTask.GetStartInstant())
            , OwnerId(originalTask.GetOwnerId()) {

        }
        TEvTaskProcessedResult(const TWorkerTask& originalTask, ITask::TPtr result)
            : TBase(result)
            , StartInstant(originalTask.GetStartInstant())
            , OwnerId(originalTask.GetOwnerId()) {

        }
    };
};

class TWorker: public NActors::TActorBootstrapped<TWorker> {
private:
    using TBase = NActors::TActorBootstrapped<TWorker>;
public:
    void HandleMain(TEvInternal::TEvNewTask::TPtr& ev);

    STATEFN(StateMain) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvInternal::TEvNewTask, HandleMain);
            default:
                ALS_ERROR(NKikimrServices::TX_CONVEYOR) << "unexpected event for task executor: " << ev->GetTypeRewrite();
                break;
        }
    }

    void Bootstrap() {
        Become(&TWorker::StateMain);
    }

    TWorker(const TString& conveyorName)
        : TBase("CONVEYOR::" + conveyorName + "::WORKER")
    {

    }
};

}
