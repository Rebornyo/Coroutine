/**
 * @file m_cor.cpp
 * @brief 协程实现
 * @version 0.1
 * @date 2024-03-23
 */

#include <atomic>
#include "m_cor.h"
#include "config.h"
#include "log.h"
#include "macro.h"

namespace event
{
    static Logger::ptr g_logger = EVENT_LOG_NAME("system");

    /// 全局静态变量，用于生成协程id
    static std::atomic<uint64_t> e_cor_id{0};
    /// 全局静态变量，用于统计当前的协程数
    static std::atomic<uint64_t> e_cor_count{0};

    /// 线程局部变量，当前线程正在运行的协程
    static thread_local Cor *t_cor = nullptr;
    /// 线程局部变量，当前线程的主协程，切换到这个协程就相当于切换到主线程中运行，智能指针形式
    static thread_local Cor::ptr t_thread_cor = nullptr;

    /// 协程栈大小，可通过配置文件读取，默认128k
    static ConfigVar<uint32_t>::ptr g_cor_stack_size =
        Config::Lookup<uint32_t>("cor.stack_size", 128 * 1024, "cor stack size");

    /**
     * @brief malloc栈内存分配器
     */
    class MallocStackAllocator
    {
    public:
        static void *Alloc(size_t size) { return malloc(size); }
        static void Dealloc(void *vp, size_t size) { return free(vp); }
    };

    using StackAllocator = MallocStackAllocator;

    /// 获得当前线程正在运行的协程的Id
    uint64_t Cor::GetCorId()
    {
        if (t_cor)
        {
            return t_cor->getId();
        }
        return 0;
    }

    Cor::Cor()
    {
        SetThis(this);
        m_state = RUNNING;

        /// 获得当前上下文，如果getcontext成功，它将返回0，否则返回非0值
        if (getcontext(&m_ctx))
        {
            EVENT_ASSERT2(false, "getcontext");
        }

        ++e_cor_count;
        m_id = e_cor_id++; // 协程id从0开始，用完加1

        EVENT_LOG_DEBUG(g_logger) << "Cor::Cor() main id = " << m_id;
    }

    void Cor::SetThis(Cor *c)
    {
        t_cor = c;
    }

    /**
     * 获取当前协程，同时充当初始化当前线程主协程的作用，这个函数在使用协程之前要调用一下
     */
    Cor::ptr Cor::GetThis()
    {
        if (t_cor)
        {
            return t_cor->shared_from_this();
        }

        Cor::ptr main_cor(new Cor);
        EVENT_ASSERT(t_cor == main_cor.get());
        t_thread_cor = main_cor;
        return t_cor->shared_from_this();
    }

    /**
     * 带参数的构造函数，用于创建其他协程，需要分配栈
     */
    Cor::Cor(std::function<void()> cb, size_t stacksize)
        : m_id(e_cor_id++),
          m_cb(cb)
    {
        ++e_cor_count;
        m_stacksize = stacksize ? stacksize : g_cor_stack_size->getValue();
        m_stack = StackAllocator::Alloc(m_stacksize);

        /// 将m_ctx初始化并保存当前的上下文
        if (getcontext(&m_ctx))
        {
            EVENT_ASSERT2(false, "getcontext");
        }

        /// uc_link指向一个上下文，ucontext_t *uc_link，当当前上下文结束时，将返回执行该上下文
        m_ctx.uc_link = nullptr;
        /// ss_sp 是栈空间的指针，指向当前栈所在的位置
        m_ctx.uc_stack.ss_sp = m_stack;
        /// ss_size 是整个栈的大小
        m_ctx.uc_stack.ss_size = m_stacksize;

        /// 使用这个函数对上下文进行处理，可以创建一个新的上下文
        makecontext(&m_ctx, &Cor::MainFunc, 0);

        EVENT_LOG_DEBUG(g_logger) << "Cor::Cor() id = " << m_id;
    }

    /**
     * 协程析构，对线程的主协程析构时需要进行特殊处理，因为主协程没有分配栈和cb
     */
    Cor::~Cor()
    {
        EVENT_LOG_DEBUG(g_logger) << "Cor::~Cor() id = " << m_id;
        --e_cor_count;
        if (m_stack)
        {
            // 有栈，说明是子协程，需要确保子协程一定是结束状态
            EVENT_ASSERT(m_state == TERM);
            StackAllocator::Dealloc(m_stack, m_stacksize);
            EVENT_LOG_DEBUG(g_logger) << "dealloc stack, id = " << m_id;
        }
        else
        {
            // 主协程
            EVENT_ASSERT(!m_cb);              // 主协程没有入口函数cb
            EVENT_ASSERT(m_state == RUNNING); // 主协程一定是执行状态

            Cor *cur = t_cor; // 当前协程就是自己, t_cor:当前线程正在运行的协程
            if (cur == this)
            {
                SetThis(nullptr);
            }
        }
    }

    /**
     * 仅对处于TERM状态的协程才进行重置，即重复利用已结束的协程，复用其栈空间，不重新创建栈
     */
    void Cor::reset(std::function<void()> cb)
    {
        EVENT_ASSERT(m_stack);
        EVENT_ASSERT(m_state == TERM);
        m_cb = cb;
        if (getcontext(&m_ctx))
        {
            EVENT_ASSERT2(false, "getcontext");
        }

        m_ctx.uc_link = nullptr;
        m_ctx.uc_stack.ss_sp = m_stack;
        m_ctx.uc_stack.ss_size = m_stacksize;

        makecontext(&m_ctx, &Cor::MainFunc, 0);
        m_state = READY;
    }

    void Cor::resume()
    {
        EVENT_ASSERT(m_state != TERM && m_state != RUNNING);
        SetThis(this);
        m_state = RUNNING;

        if (swapcontext(&(t_thread_cor->m_ctx), &m_ctx))
        {
            EVENT_ASSERT2(false, "swapcontext");
        }
    }

    void yield()
    {
        /// RUNNING/TERM的协程可调用，其中运行完的协程（TERM）会自动yield一次，用于回到主协程
        EVENT_ASEERT(m_state == RUNNING || m_state == TERM);
        SetThis(t_thread_cor.get());
        if (m_stata != TERM)
        {
            m_state = READY;
        }

        if (swapcontext(&m_ctx, &(t_thread_cor->m_ctx)))
        {
            EVENT_ASSERT2(false, "swapcontext");
        }
    }

    /**
     * @brief 协程入口函数
     * @note 对用户传入的协程入口函数上进行了一次封装，
     * 类似于线程模块对线程入口函数的封装。通过封装协程入口函数，
     * 可以实现协程在结束自动执行yield的操作，框架不处理协程函数出现异常的情况。
     */
    void Cor::MainFunc()
    {
        Cor::ptr cur = GetThis(); // GetThis 的shared_from_this方法让引用计数加1
        EVENT_ASSERT(cur);

        cur->m_cb(); // 这里真正执行协程的入口函数
        cur->m_cb = nullptr;
        cur->m_state = TERM; // 后面yield回到主协程

        auto raw_ptr = cur.get(); // 手动让t_cor的引用计数减1
        cur.reset();              // 这是shared_ptr的reset操作，它所管理的对象的引用计数会减1
        raw_ptr->yield();         // 协程结束时自动yield，以回到主协程
    }

}
