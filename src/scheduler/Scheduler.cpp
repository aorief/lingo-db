#include "lingodb/utility/Tracer.h"
#include <atomic>
#include <boost/context/fiber.hpp>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <iostream>
#include <thread>

#include "lingodb/scheduler/Scheduler.h"
#include "lingodb/scheduler/Task.h"

namespace lingodb::scheduler {
class Worker;
namespace {
static thread_local Worker* currentWorker;
} // end namespace

struct TaskWrapper;
#ifdef ASAN_ACTIVE

class Fiber {
   std::thread thread;
   std::atomic<bool> isRunning = false;
   bool done = true;
   Worker* worker = nullptr;
   TaskWrapper* task = nullptr;
   std::condition_variable cvMain, cvFiber;
   std::mutex mtx;

   public:
   void setup();
   void teardown();
   Fiber() = default;

   ~Fiber() {
      if (thread.joinable()) {
         //resume(); // just in case it's yielded
         thread.join();
      }
   }
   bool run(Worker* w, TaskWrapper* tw, const std::function<void()>&& f) {
      worker = w;
      task = tw;
      done = false;
      isRunning = true;
      std::unique_lock<std::mutex> lk(mtx);
      if (thread.joinable()) {
         thread.join();
      }
      thread = std::thread([this, f]() {
         currentWorker = worker;
         setup();
         f(); // execute task
         std::unique_lock<std::mutex> lk(mtx);
         isRunning = false;
         done = true;
         teardown();
         cvMain.notify_one(); // notify resume() that we're done
      });

      // Wait for initial yield before we return
      cvMain.wait(lk, [&]() { return !isRunning; });

      return done;
   }
   bool resume() {
      assert(!isRunning);
      assert(!done);
      isRunning = true;
      std::unique_lock<std::mutex> lk(mtx);
      cvFiber.notify_one();
      cvMain.wait(lk, [&]() { return !isRunning; });
      return done;
   }

   void yield() {
      assert(isRunning);
      assert(!done);
      std::unique_lock<std::mutex> lk(mtx);
      isRunning = false;
      teardown();
      cvMain.notify_one();
      cvFiber.wait(lk, [&]() { return isRunning.load(); });
      setup();
   }

   bool isYielded() {
      return !isRunning;
   }

   Worker* getWorker() {
      return worker;
   }

   TaskWrapper* getTask() {
      return task;
   }
};

#else
class Fiber {
   static constexpr size_t stackSize = 1 << 20;

   struct LocalAllocator {
      Fiber* fiber = nullptr;

      boost::context::stack_context allocate() {
         assert(!fiber->stackAllocated);
         boost::context::stack_context sctx;
         sctx.size = stackSize;
         sctx.sp = fiber->stackSpace + stackSize;
         fiber->stackAllocated = true;
         return sctx;
      }

      void deallocate(boost::context::stack_context&) noexcept {
         assert(fiber->stackAllocated);
         fiber->stackAllocated = false;
      }

      friend class Fiber;
   };

   alignas(64) std::byte stackSpace[stackSize];
   bool stackAllocated = false;
   std::atomic<bool> isRunning = false;
   bool done = true;
   boost::context::fiber fiber;
   boost::context::fiber sink;
   Worker* worker = nullptr;
   TaskWrapper* task = nullptr;

   public:
   void setup();
   void teardown();

   bool run(Worker* worker, TaskWrapper* taskWrapper, const std::function<void()>&& f) {
      this->worker = worker;
      this->task = taskWrapper;
      done = false;
      isRunning = true;
      fiber = boost::context::fiber{std::allocator_arg, LocalAllocator{this}, [&](boost::context::fiber&& boostSink) {
                                       sink = std::move(boostSink);
                                       setup();
                                       f();
                                       teardown();
                                       isRunning = false;
                                       done = true;
                                       return std::move(sink);
                                    }}
                 .resume();
      return done;
   }

   Worker* getWorker() {
      return worker;
   }

   TaskWrapper* getTask() {
      return task;
   }

   bool resume() {
      assert(!isRunning);
      assert(!done);
      isRunning = true;
      setup();
      fiber = std::move(fiber).resume();
      teardown();
      return done;
   }

   void yield() {
      assert(isRunning);
      assert(!done);
      isRunning = false;
      teardown();
      sink = std::move(sink).resume();
   }
   bool isYielded() {
      return !isRunning;
   };

   Fiber() = default;

   ~Fiber() {
      assert(done);
   }
};
#endif

struct TaskWrapper {
   std::unique_ptr<Task> task;

   std::atomic<bool> coolingDown = false;
   bool finalized = false;
   TaskWrapper* next = nullptr;
   TaskWrapper* prev = nullptr;
   std::atomic<int64_t> yieldedFibers = 0;
   std::atomic<int64_t> nonCompletedFibers = 0;
   std::atomic<int64_t> deployedOnWorkers = 0;
   std::function<void()> onFinalize = nullptr;
   std::mutex finalizeMutex = {};

   void finalize();

   bool done() {
      return !task->hasWork() && nonCompletedFibers.load() == 0;
   }

   bool startFiber() {
      if (task->hasWork()) {
         nonCompletedFibers++;
         return true;
      }
      return false;
   }

   bool finishFiber() {
      if (nonCompletedFibers.fetch_sub(1) == 1 && !task->hasWork()) {
         return true;
      }
      return false;
   }

   void yieldFiber() {
      yieldedFibers++;
   }

   void unYieldFiber() {
      yieldedFibers--;
   }
};

void Fiber::teardown() {
   assert(task && task->task);
   task->task->teardown();
}
void Fiber::setup() {
   assert(task && task->task);
   task->task->setup();
}
class Scheduler {
   size_t numWorkers;

   std::atomic<bool> shutdown{false};
   std::vector<std::thread> workerThreads;
   Worker* idleWorkers = nullptr;
   std::mutex taskQueueMutex;
   std::mutex taskReturnMutex;
   std::mutex taskDeleteMutex;
   TaskWrapper* taskHead = nullptr;
   TaskWrapper* taskTail = nullptr;

   //queue for tasks that are cooling down i.e. they don't have work left to do, but are also not yet finished.
   std::mutex coolingDownMutex;
   TaskWrapper* coolingDownHead = nullptr;
   TaskWrapper* coolingDownTail = nullptr;

   public:
   Scheduler(size_t numWorkers = std::thread::hardware_concurrency()) : numWorkers(numWorkers) {
   }
   size_t getNumWorkers() {
      return numWorkers;
   }

   std::mutex& getTaskDeleteMutex() {
      return taskDeleteMutex;
   }

   void putWorkerToSleep(Worker* worker);

   void start();

   void stop();

   void join() {
      for (auto& workerThread : workerThreads) {
         workerThread.join();
      }
   }

   bool isShutdown() {
      return shutdown.load();
   }

   //insert task into "active task queue"
   void enqueueTask(TaskWrapper* wrapper);

   void enqueueTask(std::unique_ptr<Task>&& task) {
      enqueueTask(new TaskWrapper{std::move(task)});
   }

   //remove task from "active task queue"
   void dequeueTaskLocked(TaskWrapper* task) {
      if (task->prev) {
         task->prev->next = task->next;
      } else {
         taskHead = task->next;
      }
      if (task->next) {
         task->next->prev = task->prev;
      } else {
         taskTail = task->prev;
      }
      task->prev = nullptr;
      task->next = nullptr;
   }

   TaskWrapper* getTask() {
      std::lock_guard<std::mutex> lock(taskQueueMutex);
      auto* potentialTask = taskHead;
      size_t minYieldCount = std::numeric_limits<size_t>::max();
      TaskWrapper* minYieldTask = nullptr;
      while (potentialTask) {
         //if we see a task that has no work left, we move it to the cooling down queue
         if (!potentialTask->task->hasWork()) {
            auto* toCoolDown = potentialTask;
            potentialTask = potentialTask->next;
            dequeueTaskLocked(toCoolDown);
            std::lock_guard<std::mutex> lock1(coolingDownMutex);
            if (coolingDownTail) {
               coolingDownTail->next = toCoolDown;
               toCoolDown->prev = coolingDownTail;
               coolingDownTail = toCoolDown;
            } else {
               coolingDownHead = toCoolDown;
               coolingDownTail = toCoolDown;
            }
            toCoolDown->coolingDown = true;
            continue;
         }
         auto yieldedFibers = potentialTask->yieldedFibers.load();
         if (yieldedFibers == 0) {
            potentialTask->deployedOnWorkers++;
            return potentialTask;
         } else {
            if (yieldedFibers < 0 || size_t(yieldedFibers) < minYieldCount) {
               minYieldCount = yieldedFibers;
               minYieldTask = potentialTask;
            }
         }
         potentialTask = potentialTask->next;
      }
      if (minYieldTask) {
         minYieldTask->deployedOnWorkers++;
      }
      return minYieldTask;
   }

   bool returnTask(TaskWrapper* task) {
      // Multiple worker may call here concurrently. Avoid one worker already called `delete task`
      // but another worker is still at `task->finalized`
      std::lock_guard<std::mutex> lock(taskReturnMutex);
      auto deployedNum = task->deployedOnWorkers.fetch_sub(1);
      if (task->finalized) {
         if (deployedNum == 1) {
            std::lock_guard<std::mutex> guardDelete(taskDeleteMutex);
            delete task;
            return true;
         }
      }
      return false;
   }

   void finalizeTask(TaskWrapper* task) {
      if (task->coolingDown) {
         //simple case: already in cooling down queue
         // -> only need to lock cooling down queue
         std::lock_guard<std::mutex> lock(coolingDownMutex);
         if (task->prev) {
            task->prev->next = task->next;
         } else {
            coolingDownHead = task->next;
         }
         if (task->next) {
            task->next->prev = task->prev;
         } else {
            coolingDownTail = task->prev;
         }
      } else {
         //not in cooling down queue
         // -> need to lock both task queue and cooling down queue
         bool alreadyCoolingDown;
         {
            std::lock_guard<std::mutex> lock(taskQueueMutex);
            alreadyCoolingDown = task->coolingDown;
            if (!alreadyCoolingDown) {
               dequeueTaskLocked(task);
            }
         }
         // - If alreadyCoolingDown is true, last attempt to dequeue task is failed. task is appedn to coolingDown queue. need to remove it.
         // - If alreadyCoolingDown is false, last attempt to dequeue task is ok. task is not inserted to coolingDown queue. no need to remove it.
         if (alreadyCoolingDown) {
            std::lock_guard<std::mutex> lock(coolingDownMutex);
            if (task->prev) {
               task->prev->next = task->next;
            } else {
               coolingDownHead = task->next;
            }
            if (task->next) {
               task->next->prev = task->prev;
            } else {
               coolingDownTail = task->prev;
            }
         }
      }
      task->finalize();
   }
};

class Worker {
   class FiberAllocator {
      static constexpr size_t initiallyAllocated = 2;
      size_t numAllocated;
      size_t maxFibers;
      std::deque<std::unique_ptr<Fiber>> allocatedAndAvailableFibers;

      public:
      explicit FiberAllocator(size_t maxFibers) : numAllocated(initiallyAllocated), maxFibers(maxFibers) {
         for (size_t i = 0; i < initiallyAllocated; i++) {
            allocatedAndAvailableFibers.push_back(std::make_unique<Fiber>());
         }
      }

      bool canAllocate() {
         return numAllocated < maxFibers || !allocatedAndAvailableFibers.empty();
      }

      std::unique_ptr<Fiber> allocate() {
         if (allocatedAndAvailableFibers.empty()) {
            if (numAllocated < maxFibers) {
               numAllocated++;
               return std::make_unique<Fiber>();
            } else {
               return {};
            }
         } else {
            auto fiber = std::move(allocatedAndAvailableFibers.back());
            allocatedAndAvailableFibers.pop_back();
            return fiber;
         }
      }

      void deallocate(std::unique_ptr<Fiber>&& fiber) {
         allocatedAndAvailableFibers.push_back(std::move(fiber));
      }
   };

   Scheduler& scheduler;

   //for managing a pool of fibers/stacks
   FiberAllocator fiberAllocator;

   //all fibers that are currently runnable
   std::mutex fiberMutex;
   std::deque<std::unique_ptr<Fiber>> runnableFibers;

   std::unique_ptr<Fiber> currentFiber;

   std::unordered_map<TaskWrapper*, std::unique_ptr<Fiber>> waitingOnTasks;
   std::atomic<size_t> numWaitingFibers = 0;

   TaskWrapper* currentTask = nullptr;

   using TimePoint = std::chrono::time_point<std::chrono::system_clock>;
   TimePoint startWaitTime = TimePoint::min();

   public:
   //for cheaply collecting idle workers
   Worker* nextIdleWorker = nullptr;
   bool isInIdleList = false;
   // for sleeping / waking up if there is nothing to do
   std::mutex mutex;
   std::condition_variable cv;
   size_t workerId;
   bool allowedToSleep = true;

   Worker(Scheduler& scheduler, size_t id) : scheduler(scheduler), fiberAllocator(64), workerId(id) {
   }

   void wakeupWorker() {
      {
         std::unique_lock<std::mutex> lock(mutex);
         allowedToSleep = false;
         cv.notify_one();
      }
   }

   void awaitChildTask(std::unique_ptr<Task> task) {
      TaskWrapper* taskWrapper = new TaskWrapper{std::move(task)};
      Fiber* toYield;
      {
         std::unique_lock<std::mutex> fiberLock(fiberMutex);
         waitingOnTasks[taskWrapper] = std::move(currentFiber);
         numWaitingFibers++;
         toYield = waitingOnTasks[taskWrapper].get();
         taskWrapper->onFinalize = [&] {
            {
               std::unique_lock<std::mutex> fiberLock2(fiberMutex);
               assert(waitingOnTasks.contains(taskWrapper));
               assert(waitingOnTasks[taskWrapper]);
               while (!waitingOnTasks[taskWrapper]->isYielded()) {}
               runnableFibers.push_back(std::move(waitingOnTasks[taskWrapper]));
               waitingOnTasks.erase(taskWrapper);
               numWaitingFibers--;
            }
            wakeupWorker();
         };
         scheduler.enqueueTask(taskWrapper);
      }
      toYield->yield();
   }

   void work() {
      while (!scheduler.isShutdown()) {
         {
            std::lock_guard<std::mutex> lock(fiberMutex);

            if (!runnableFibers.empty()) {
               currentFiber = std::move(runnableFibers.front());
               runnableFibers.pop_front();
            }
         }
         auto handleFiberComplete = [&]() {
            auto* task = currentFiber->getTask();
            if (task) {
               if (task->finishFiber()) {
                  scheduler.finalizeTask(task);
               }
            }
            fiberAllocator.deallocate(std::move(currentFiber));
         };
         if (currentFiber) {
            if (currentFiber->resume()) {
               //unyield because it was previously registered as yielded
               if (currentFiber->getTask()) {
                  currentFiber->getTask()->unYieldFiber();
               }
               auto* resumeTask = currentFiber->getTask();
               handleFiberComplete();
               scheduler.returnTask(resumeTask);
            }
            assert(!currentFiber);
         }
         if (fiberAllocator.canAllocate()) {
            TaskWrapper* currTask = nullptr;
            {
               std::unique_lock<std::mutex> lock(mutex);
               if (this->currentTask) {
                  currTask = this->currentTask;
                  this->currentTask = nullptr;
               }
            }
            if (currTask && !currTask->task->hasWork()) {
               scheduler.returnTask(currTask);
               currTask = nullptr;
               continue;
            }
            if (!currTask) {
               currTask = scheduler.getTask();
            }

            if (currTask) {
               // Step 1. try startFiber. it's possible task is already exhausted. Task should be
               // return if exhausted.
               if (!currTask->startFiber()) {
                  scheduler.returnTask(currTask);
                  continue;
               }
               // Step 2. try reserve a piece of work.
               if (!currTask->task->allocateWork()) {
                  // reserveWork false and finishFiber true means no possible for new run and all
                  // runs are done. Then it is safe to finalize a task.
                  // ## An extra reserveWork call is necessary:
                  // Imagine a task of 2 unit and 1 thread. [reserveWork, consumeWork,
                  //                                         reserveWork, consumeWork] called sequentially
                  // After the second consumeWork, finishFiber in `handleFiberComplete` still return
                  // true Althought it has no more work. A third reserveWork will set work to exhausted.
                  // ## `finalizeTask` is called only once:
                  // - scenario 1: 1 worker inside this if, other workers are before startFiber. Because
                  //     work is already exhausted, all other workers will have startFiber return false.
                  // - scenario 2: there are few workers inside this if or after startFiber(nonCompletedFibers>0)
                  //     Only last worker end up with finishFiber return true.
                  if (currTask->finishFiber()) {
                     scheduler.finalizeTask(currTask);
                  }
                  scheduler.returnTask(currTask);
                  continue;
               }
               //work on (part of) (new) task
               currentFiber = fiberAllocator.allocate();
               assert(currentFiber);
               // Step 3. consume reserved work
               auto fiberDone = currentFiber->run(this, currTask, [&] {
                  currTask->task->performWork();
               });
               if (fiberDone) {
                  this->startWaitTime = TimePoint::min();
                  handleFiberComplete();
               } else {
                  currTask->yieldFiber();
                  // yielded fiber does't finished. continue and not execute following cleanup logic.
                  continue;
               }
               if (currTask->task->hasWork()) {
                  std::lock_guard<std::mutex> lock(mutex);
                  this->currentTask = currTask;
               } else {
                  scheduler.returnTask(currTask);
               }
               //assert(!currentFiber);

               continue;
            } else {
               scheduler.putWorkerToSleep(this);
            }

         } else {
            // we have a lot of fibers -> so don't sleep :)
            continue;
         }
      }
   }
};

namespace {
Scheduler* scheduler;
static std::atomic<size_t> numSchedulerUsers = 0;

void stopCurrentScheduler() {
   if (scheduler) {
      scheduler->stop();
   }
}
} // end namespace

void Scheduler::start() {
   scheduler = this;
   for (size_t i = 0; i < numWorkers; i++) {
      workerThreads.emplace_back([this, i] {
#if defined(__APPLE__) && defined(__arm64__)
         // on apple silicon: signal os to prefer performance cores for scheduling this thread
         // https://developer.apple.com/library/archive/documentation/Performance/Conceptual/power_efficiency_guidelines_osx/PrioritizeWorkAtTheTaskLevel.html
         // we silently ignore errors, since this setting is not mission-critical.
         pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
#ifdef TRACER
         utility::Tracer::ensureThreadLocalTraceRecordList();
#endif
         Worker worker(*this, i);
         currentWorker = &worker;
         worker.work();
         currentWorker = nullptr;
      });
   }
}

void Scheduler::stop() {
   shutdown.store(true);
   std::unique_lock<std::mutex> lock(taskQueueMutex);
   size_t cntr = 0;
   while (idleWorkers) {
      assert(cntr++ < numWorkers);
      auto* currWorker = idleWorkers;
      currWorker->isInIdleList = false;
      idleWorkers = currWorker->nextIdleWorker;
      currWorker->cv.notify_one();
   }
}

void Scheduler::enqueueTask(TaskWrapper* wrapper) {
   {
      std::lock_guard<std::mutex> lock(taskQueueMutex);
      if (taskTail) {
         taskTail->next = wrapper;
         wrapper->prev = taskTail;
         taskTail = wrapper;
      } else {
         taskHead = wrapper;
         taskTail = wrapper;
      }
   }
   std::lock_guard<std::mutex> lock(taskQueueMutex);
   size_t cntr = 0;
   while (idleWorkers) {
      assert(cntr++ < numWorkers);
      Worker* currWorker = idleWorkers;
      std::unique_lock<std::mutex> workerLock(currWorker->mutex);
      currWorker->isInIdleList = false;
      idleWorkers = idleWorkers->nextIdleWorker;

      currWorker->cv.notify_one();
   }
}

void Scheduler::putWorkerToSleep(Worker* worker) {
   if (isShutdown()) {
      return;
   }
   std::unique_lock<std::mutex> lock(taskQueueMutex);
   std::unique_lock<std::mutex> workerLock(worker->mutex);
   if (worker->allowedToSleep) {
      if (!worker->isInIdleList) {
         worker->nextIdleWorker = idleWorkers;
         idleWorkers = worker;
         worker->isInIdleList = true;
      }
      lock.unlock();
      worker->cv.wait(workerLock);
   } else {
      worker->allowedToSleep = true;
   }
}

void TaskWrapper::finalize() {
   std::unique_lock<std::mutex> lock(finalizeMutex);
   if (!finalized) { //this check is important! In case finalize is called multiple times which can happen in edge cases
      if (onFinalize) {
         onFinalize();
      }
      finalized = true;
   }
}

void awaitEntryTask(std::unique_ptr<Task> task) {
   TaskWrapper* taskWrapper = new TaskWrapper{std::move(task)};
   std::condition_variable cvFinished;
   std::unique_lock<std::mutex> blockDelete;
   bool finished = false;
   taskWrapper->onFinalize = [&]() {
      blockDelete = std::unique_lock<std::mutex>(scheduler->getTaskDeleteMutex());
      finished = true;
      cvFinished.notify_one();
   };
   std::unique_lock<std::mutex> lk(taskWrapper->finalizeMutex);
   scheduler->enqueueTask(taskWrapper);
   cvFinished.wait(lk, [&]() { return finished; });
}
void awaitChildTask(std::unique_ptr<Task> task) {
   currentWorker->awaitChildTask(std::move(task));
}

std::unique_ptr<SchedulerHandle> startScheduler(size_t numWorkers) {
#if defined(__APPLE__) && defined(__arm64__)
   // on apple silicon: set QOS_CLASS_USER_INTERACTIVE for main thread on scheduler startup
   // we silently ignore errors here, since this setting is not mission-critical.
   pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

   if (!scheduler) {
      // two ways of setting number of workers.
      // - if provided actual param for createScheduler, this will override LINGODB_PARALLELISM system var
      // - if no actual param provided, LINGODB_PARALLELISM is used
      // - if no actual param and no LINGODB_PARALLELISM provided, hardware_concurrency() is used
      if (numWorkers == 0) {
         // LINGODB_PARALLELISM is one of ["OFF", positive integer]
         numWorkers = std::thread::hardware_concurrency();
         if (const char* mode = std::getenv("LINGODB_PARALLELISM")) {
            if (std::string(mode) == "OFF") {
               numWorkers = 1;
            } else if (std::stol(mode) > 0) {
               numWorkers = std::stol(mode);
            }
         }
      }
      scheduler = new Scheduler(numWorkers);
      scheduler->start();
   }
   return std::make_unique<SchedulerHandle>();
}

size_t getNumWorkers() {
   if (scheduler == nullptr) {
      assert(false);
      return 0;
   }
   return scheduler->getNumWorkers();
}
size_t currentWorkerId() {
   if (currentWorker) {
      return currentWorker->workerId;
   }
   assert(false);
   return std::numeric_limits<size_t>::max();
}
} // namespace lingodb::scheduler

lingodb::scheduler::SchedulerHandle::SchedulerHandle() {
   numSchedulerUsers++;
}
lingodb::scheduler::SchedulerHandle::~SchedulerHandle() {
   if (numSchedulerUsers.fetch_sub(1) == 1) {
      lingodb::scheduler::stopCurrentScheduler();
      scheduler->join();
      delete scheduler;
      scheduler = nullptr;
   }
}
