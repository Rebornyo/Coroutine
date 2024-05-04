/**
 * @file m_cor.h
 * @brief 协程模块
 * @details 基于ucontext_t实现，非对称协程
 * @version 0.1
 * @date 2024-03-23
*/

#ifndef __event_Cor_H__
#define __event_Cor_H__

#include <functional>
#include <memory>
#include <ucontext.h>
#include "thread.h"

namespace event {

/**
 * @brief 协程类
*/
class Cor : public std::enable_shared_from this<Cor> {
public:
    typedef std::shared_ptr<Cor> ptr;
     
     /**
      * @brief 协程状态
      * @details 定义三态转换关系，包括准备运行(READY)，正在运行(RUNNING)，
      * 运行结束(TERM)。不区分协程的初始状态，初始即READY，不区分是异常结束还是正常结束，
      * 只要结束就是TERM状态，也不区分HOLD状态，协程只要未结束也非运行态，就是READY状态
     */
    enum State {
        /// 就绪态，刚创建或者yield之后的状态
        READY,
        /// 运行态，resume之后的状态
        RUNNING,
        /// 结束态，协程的回调函数执行完之后的状态
        TERM
    };
private:
    /**
     * @brief 构造函数
     * @attention 当创建线程的第一个协程时才会调用无参构造函数，也就是线程主函数对应的协程，
     * 这个协程只能由GetThis()方法调用，所以定义成私有方法
    */
    Cor();   

public:
    /**
     * @brief 构造函数，用于创建用户协程，
     * 协程相当于一个可以随意切换的函数，在实现协程时需要给协程绑定一个运行函数，
     * 还需要给协程提供一个运行的栈空间
     * @param[] cb 协程入口函数
     * @param[] stacksize 栈大小
    */
    Cor(std::function<void()> cb, size_t stacksize = 0);

    /**
     * @brief 析构函数
    */
    ~Cor();

    /**
        * @brief 重置协程状态和入口函数，复用栈空间，不重新创建栈
        * @param[] cb
    */
    void reset(std::function<void()> cb);

    /**
     * @brief 将当前协程切换到执行状态
     * @details 当前协程和正在运行的协程进行交换，前者状态变成RUNNING，后者状态变成READY
     */
    void resume();

    /**
     * @brief 当前协程让出执行权
     * @details 当前协程与上次resume时退到后台的协程进行交换，前者状态变成READY，
     * 后者状态变成RUNNING
     */
    void yield();

    /**
     * @brief 获取协程ID
     * @attention 只有成员函数能在函数后面用const修饰，该函数不能修改对象内的任何成员，只能读
     */
    uint64_t getId() const {return m_id;}
  
    /**
     * @brief 获取协程状态
     */
    State getState() const {return m_state;}

public:
    /**
     * @brief 设置当前正在运行的协程，即设置线程局部变量t_cor的值
     */
    static void SetThis(Cor *c);

    /**
     * @brief 返回当前线程正在执行的协程
     * @details 如果当前线程还未创建协程，则创建线程的第一个协程，且该协程为当前线程的主协程
     * 其他协程都通过这个协程来调度，也就是说其他协程结束时都要切回主协程
     * 由主协程重新选择新的协程进行resume
     * @attention 线程如果要创建协程，那么应该首先执行一下Cor::GetThis()操作，以初始化主函数协程
    */
   static Cor::ptr GetThis();

    /**
        * @brief 获取总协程数
    */
    static uint64_t TotalCors();

    /**
     * @brief 协程入口函数??
    */
   static void MainFunc();

   /**
    * @brief 获取当前线程正在运行的协程的ID
   */
  static uint64_t GetCorId();

private:
    /// 协程id
    uint64_t m_id = 0;
    /// 协程栈大小
    uint32_t m_stacksize = 0;
    /// 协程状态
    State m_state = READY;
    /// 协程上下文
    ucontext_t m_ctx;
    /// 协程栈地址
    void m_stack = nullptr;
    /// 协程入口函数
    std::function<void()> m_cb;
};

} //namspace event

#endif
