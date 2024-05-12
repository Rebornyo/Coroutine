/**
 * @file scheduler.cpp
 * @brief 协程调度器实现
 * @version 0.1
 * @date 2024-03-24
 */
#include "scheduler.h"
#include "macro.h"

namespace event
{

    static event::Logger::ptr g_logger = EVENT_LOG_NAME("system");

    /// 当前线程的调度器，同一个调度器下的所有线程共享同一个实例
    static thread_local Scheduler *t_scheduler = nullptr;
    /// 当前线程的调度协程，每个线程都独有一份
    static thread_local Cor *t_scheduler_cor = nullptr;

    Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name)
    {
        EVENT_ASSERT(threads > 0);

        m_useCaller = use_caller;
        m_name = name;

        if (use_caller)
        {
            --threads;
            event::Cor::GetThis(); // 初始化主协程
            EVENT_ASSERT(GetThis() == nullptr);
            t_scheduler = this;

            /**
             * caller线程的主协程不会被线程的调度协程run进行调度，而且，线程的调度协程停止时，应该返回caller线程的主协程
             * 在user caller情况下，把caller线程的主协程暂时保存起来，等调度协程结束时，再resume caller协程
             */
            m_rootCor.reset(new Cor(std::bind(&Scheduler::run, this), 0, false));

            event::Thread::SetName(m_name);
            t_scheduler_cor = m_rootThread.get();
            m_rootThread = event::GetCorId();
            m_threadIds.push_back(m_rootThread);
        }
        else
        {
            m_rootThread = -1;
        }
        m_threadCount = threads;
    }

    Scheduler *Scheduler::GetThis()
    {
        return t_scheduler;
    }

    Cor *Scheduler::GetMainCor()
    {
        return t_scheduler_cor;
    }

    Scheduler::~Scheduler()
    {
        EVENT_LOG_DEBUG(g_logger) << "Scheduler::~Scheduler()";
        EVENT_ASSERT(m_stopping);
        if (GetThis() == this)
        {
            t_scheduler = nullptr;
        }
    }

    void Scheduler::start()
    {
        EVENT_LOG_DEBUG(g_logger) << "start";
        MutexType::Lock lock(m_mutex);
        if (m_stopping)
        {
            EVENT_LOG_ERROR(g_logger) << "Scheduler is stopped";
            return;
        }
        EVENT_ASSERT(m_threads.empty());
        m_threads.resize(m_threadCount);
        for (size_t i = 0; i < m_threadCount; i++)
        {
            m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this),
                                          m_name + "_" + std::to_string(i)));
            m_threadIds.push_back(m_threads[i]->getId());
        }
    }

    bool Scheduler::stopping()
    {
        MutexType::Lock lock(m_mutex);
        return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
    }

    void Scheduler::tickle()
    {
        EVENT_LOG_DEBUG(g_logger) << "ticlke";
    }

    void Scheduler::idle()
    {
        EVENT_LOG_DEBUG(g_logger) << "idle";
        while (!stopping())
        {
            event::Cor::GetThis()->yield();
        }
    }

    void Scheduler::stop()
    {
        EVENT_LOG_DEBUG(g_logger) << "stop";
        if (stopping())
        {
            return;
        }
        m_stopping = true;

        /// 如果use caller，那只能由caller线程发起stop
        if (m_useCaller)
        {
            EVENT_ASSERT(GetThis() == this);
        }
        else
        {
            EVENT_ASSERT(GetThis() != this);
        }

        for (size_t i = 0; i < m_threadCount; i++)
        {
            tickle();
        }

        if (m_rootFiber)
        {
            tickle();
        }

        /// 在use caller情况下，调度器协程结束时，应该返回caller协程
        if (m_rootCor)
        {
            m_rootCor->resume();
            EVENT_LOG_DEBUG(g_logger) << "m_rootCor end";
        }

        std::vector<Thread::ptr> thrs;
        {
            MutexType::Lock lock(m_mutex);
            thrs.swap(m_threads);
        }
        for (auto &i : thrs)
        {
            i->join();
        }
    }

    void Scheduler::run()
    {
        EVENT_LOG_DEBUG(g_logger) << "run";
        setThis();
        if (event::GetThreadId() != m_rootThread)
        {
            t_scheduler_cor = event::Cor::GetThis().get();
        }

        Cor::ptr idle_cor(new Cor(std::bind(&Scheduler::idle, this)));
        Cor::ptr cb_cor;

        ScheduleTask task;
        while (true)
        {
            task.reset();
            bool tickle_me = false; // 是否tickle其他线程进行任务调度
            {
                MutexType::Lock lock(m_mutex);
                auto it = m_tasks.begin();
                // 遍历所有调度任务
                while (it != m_tasks.end())
                {
                    if (it->thread != -1 && it->thread != event::GetThreadId())
                    {
                        // 指定了调度线程，但不是在当前线程上调度，标记一下需要通知其他线程进行调度，然后跳过这个任务，继续下一个
                        ++it;
                        tickle_me = true;
                        continue;
                    }

                    // 找到一个未指定线程，或是指定了当前线程的任务
                    EVENT_ASSERT(it->cor || it->cb);
                    if (it->cor)
                    {
                        // 任务队列时的协程一定是READY状态，谁会把RUNNING或TERM状态的协程加入调度呢？
                        EVENT_ASSERT(it->cor->getState() == Cor::READY);
                    }
                    // 当前调度线程找到一个任务，准备开始调度，将其从任务队列中剔除，活动线程数加1
                    task = *it;
                    m_tasks.erase(it++);
                    ++m_activeThreadCount;
                    break;
                }
                // 当前线程拿完一个任务后，发现任务队列还有剩余，那么tickle一下其他线程
                tickle_me |= (it != m_tasks.end());
            }

            if (tickle_me)
            {
                tickle();
            }

            if (task.cor)
            {
                // resume协程，resume返回时，协程要么执行完了，要么半路yield了，总之这个任务就算完成了，活跃线程数减一
                task.cor->resume();
                --m_activeThreadCount;
                task.reset();
            }
            else if (task.cb)
            {
                if (cb_cor)
                {
                    cb_cor->reset(task.cb);
                }
                else
                {
                    cb_cor.reset(new Cor(task.cb));
                }
                task.reset();
                cb_cor->resume();
                --m_activeThreadCount;
                cb_cor.reset();
            }
            else
            {
                // 进到这个分支情况一定是任务队列空了，调度idle协程即可
                if (idle_cor->getState() == Cor::TERM)
                {
                    // 如果调度器没有调度任务，那么idle协程会不停地resume/yield，不会结束，如果idle协程结束了，那一定是调度器停止了
                    EVENT_LOG_DEBUG(g_logger) << "idle cor term";
                    break;
                }
                ++m_idleThreadCount;
                idle_cor->resume();
                --m_idleThreadCount;
            }
        }
        EVENT_LOG_DEBUG(g_logger) << "Scheduler::run() exit";
    }

}
