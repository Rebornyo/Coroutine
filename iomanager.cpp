/**
 * @file iomanager.cpp
 * @brief IO协程调度器实现
 * @version 0.1
 * @date 2024-03-26
 */

#include <unistd.h>    // for pipe()
#include <sys/epoll.h> // for epoll_xxx()
#include <fcntl.h>     // for fcntl()
#include "iomanager.h"
#include "log.h"
#include "macro.h"

namespace event
{

    enum EpollCtlOp
    {
    };
    static std::ostream &operator<<(std::ostream &os, const EpollCtlOp &op)
    {
        switch ((int)op)
        {
#define XX(ctl) \
    case ctl:   \
        return os << #ctl;
            XX(EPOLL_CTL_ADD);
            XX(EPOLL_CTL_MOD);
            XX(EPOLL_CTL_DEL);
#undef XX
        default:
            return os << (int)op;
        }
    }

    static std::ostream &operator<<(std::ostream &os, EPOLL_EVENTS events)
    {
        if (!events)
        {
            return os << "0";
        }
        bool first = true;
#define XX(E)          \
    if (events & E)    \
    {                  \
        if (!first)    \
        {              \
            os << "|"; \
        }              \
        os << #E;      \
        first = false; \
    }
        XX(EPOLLIN);
        XX(EPOLLPRI);
        XX(EPOLLOUT);
        XX(EPOLLRDNORM);
        XX(EPOLLRDBAND);
        XX(EPOLLWRNORM);
        XX(EPOLLWRBAND);
        XX(EPOLLMSG);
        XX(EPOLLERR);
        XX(EPOLLHUP);
        XX(EPOLLRDHUP);
        XX(EPOLLONESHOT);
        XX(EPOLLET);
#undef XX
        return os;
    }

    IOManager::FdContext::EventContext &IOManager::FdContext::getEventContext(IOManager::Event event)
    {
        switch (event)
        {
        case IOManager::READ:
            return read;
        case IOManager::WRITE:
            return write;
        default:
            EVENT_ASSERT2(false, "getContext");
        }
        throw std::invalid_argument("getContext invalid event");
    }

    void IOManager::FdContext::resetEventContext(EventContext &ctx)
    {
        ctx.scheduler = nullptr;
        ctx.cor.reset();
        ctx.cb = nullptr;
    }

    void IOManager::FdContext::triggerEvent(IOManager::Event event)
    {
        // 待触发的事件必须已被注册过
        EVENT_ASSERT(events & event);
        /**
         *  清除该事件，表示不再关注该事件了
         * 也就是说，注册的IO事件是一次性的，如果想持续关注某个socket fd的读写事件，那么每次触发事件之后都要重新添加
         */
        events = (Event)(events & ~event);
        // 调度对应的协程
        EventContext &ctx = getEventContext(event);
        if (ctx.cb)
        {
            ctx.scheduler->schedule(ctx.cb);
        }
        else
        {
            ctx.scheduler->schedule(ctx.cor);
        }
        resetEventContext(ctx);
        return;
    }

    IOManager::IOManager(size_t threads, bool use_caller, const std::string &name)
        : Scheduler(threads, use_caller, name)
    {
        m_epfd = epoll_create(5000); // 最多监听数目
        EVENT_ASSERT(m_epfd > 0);

        int rt = pipe(m_tickleFds); // 创建管道，传入int m_tickleFds[2]
        EVENT_ASSERT(!rt);

        // 关注pipe读句柄的可读事件，用于tickle协程
        epoll_event event;
        memset(&event, 0, sizeof(epoll_event));
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = m_tickleFds[0];

        // 非阻塞方式，配合边缘触发
        rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
        EVENT_ASSERT(!rt);
        // 第二个参数表示对epoll监控描述符控制的动作，在此EPOLL_CTL_ADD 表注册新的fd到epfd，
        // m_tickleFds[0]:需要监听的文件描述符，event：告诉内核需要监听的事件
        rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);

        contextResize(32);

        start();
    }

    IOManager::~IOManager()
    {
        stop();
        close(m_epfd);
        close(m_tickleFds[0]);
        close(m_tickleFds[1]);

        for (size_t i = 0; i < m_fdContexts.size(); ++i)
        {
            if (m_fdContexts[i])
            {
                delete m_fdContexts[i];
            }
        }
    }

    void IOManager::contextResize(size_t size)
    {
        m_fdContexts.resize(size);

        for (size_t i = 0; i < m_fdContexts.size(); ++i)
        {
            if (!m_fdContexts[i])
            {
                m_fdContexts[i] = new FdContext;
                m_fdContexts[i]->fd = i;
            }
        }
    }

    int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
    {
        // 找到fd对应的FdContext，如果不存在，那就分配一个
        FdContext *fd_ctx = nullptr;
        RWMutexType::ReadLock lock(m_mutex);
        if ((int)m_fdContexts.size() > fd)
        {
            fd_ctx = m_fdContexts[fd];
            lock.unlock();
        }
        else
        {
            lock.unlock();
            RWMutexType::WriteLock lock2(m_mutex);
            contextResize(fd * 1.5);
            fd_ctx = m_fdContexts[fd];
        }

        // 同一个fd不允许重复添加相同的事件
        FdContext::MutexType::Lock lock2(fd_ctx->mutex);
        if (EVENT_UNLIKELY(fd_ctx->events & event)) // 重复事件
        {
            EVENT_LOG_ERROR(g_logger) << "addEvent assert fd=" << fd
                                      << " event=" << (EPOLL_EVENTS)event
                                      << " fd_ctx.event=" << (EPOLL_EVENTS)fd_ctx->events;
            EVENT_ASSERT(!(fd_ctx->events & event));
        }

        // 将新的事件加入epoll_wait，使用epoll_event的私有指针存储FdContext的位置
        int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        epoll_event epevent;
        epevent.events = EPOLLET | fd_ctx->events | event;
        epevent.data.ptr = fd_ctx;
        // op: 如果已添加该事件，动作：修改已经注册的fd的监听事件；
        // 如果还没添加该事件，动作：注册新的fd到epfd
        int rt = epoll_ctl(m_epfd, op, fd, &epevent);
        if (rt)
        {
            SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                      << (EpollCtlOp)op << ", " << fd << ", " << (EPOLL_EVENTS)epevent.events << "):"
                                      << rt << " (" << errno << ") (" << strerror(errno) << ") fd_ctx->events="
                                      << (EPOLL_EVENTS)fd_ctx->events;
            return -1;
        }

        // 待执行IO事件数加1
        ++m_pendingEventCount;

        // 找到这个fd的event事件对应的EventContext，对其中的scheduler, cb, cor进行赋值
        fd_ctx->events = (Event)(fd_ctx->events | event);
        FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
        EVENT_ASSERT(!event_ctx.scheduler && !event_ctx.cor && !event_ctx.cb);

        // 赋值scheduler和回调函数，如果回调函数为空，则把当前协程当成回调执行体
        event_ctx.scheduler = Scheduler::GetThis();
        if (cb)
        {
            event_ctx.cb.swap(cb);
        }
        else
        {
            event_ctx.cor = Cor::GetThis();
            EVENT_ASSERT2(event_ctx.cor->getState() == Cor::RUNNING, "state=" << event_ctx.cor->getState());
        }
        return 0;
    }

    bool IOManager::delEvent(int fd, Event event)
    {
        // 找到fd对应的FdContext
        RWMutexType::ReadLock lock(m_mutex);
        if ((int)m_fdContexts.size() <= fd)
        { // 不存在该fd
            return false;
        }
        FdContext *fd_ctx = m_fdContexts[fd];
        lock.unlock();

        FdContext::MutexType::Lock lock2(fd_ctx->mutex);
        if (EVENT_UNLIKELY(!(fd_ctx->events & event)))
        {
            return false;
        }

        // 清除指定的事件，表示不关心这个事件了，如果清除之后结果为0，则从epoll_wait中删除该文件描述符
        Event new_events = (Event)(fd_ctx->events & ~event);
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = EPOLLET | new_events; // 边缘触发
        epevent.data.ptr = fd_ctx;

        int rt = epoll_ctl(m_epfd, op, fd, &epevent);
        if (rt) {
            EVENT_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                    << (EpollCtlOp)op << ", " << fd << ", " << (EPOLL_EVENTS)epevent.events << "):"
                                    << rt << " (" << errno << ") (" << strerror(errno) << ")";
            return false;
        }

        // 待执行事件数减1
        --m_pendingEventCount;
        // 重置该fd对应的event事件上下文
        fd_ctx->events = new_events;
        FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
        fd_ctx->resetEventContext(event_ctx);
        return true;
        }
}
