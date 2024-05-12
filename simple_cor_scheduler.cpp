#include "m_cor.h"

/// @brief 单线程
class Scheduler
{
public:
    /**
     * @brief 添加协程调度任务
     */
    void schedule(event::Cor::ptr task)
    {
        m_tasks.push_back(task);
    }

    /**
     * @brief 执行调度任务
     */
    void run()
    {
        event::Cor::ptr task;
        auto it = m_tasks.begin();

        while (it != m_tasks.end())
        {
            task = *it;
            m_tasks.erase(it++);
            task->resume();
        }
    }

private:
    /// 任务队列
    std::list<event::Cor::ptr> m_tasks;
};

void test_Cor(int i)
{
    std::cout << i << std::endl;
}

int main()
{
    /// 初始化当前线程的主协程
    event::Cor::GetThis();

    /// 创建调度器
    Scheduler sc;

    /// 添加调度任务
    for (auto i = 0; i < 10; i++)
    {
        event::Cor::ptr cor(new event::Cor(std::bind(test_Cor, i)));
        sc.schedule(cor);
    }

    /// 执行调度任务
    sc.run();

    return 0;
}
