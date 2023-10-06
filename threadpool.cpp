#include "threadpool.h"
#include <functional>
#include <thread>
#include <chrono>
#include <iostream>
#include <mutex>


const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 200;
const int THREAD_MAX_IDLE_TIME = 60; //单位：秒

ThreadPool::ThreadPool():
            poolMode_(PoolMode::MODE_FIXED),
            initThreadSize_(0),
            idleThreadSize_(0),
            threadThreshHold_(THREAD_MAX_THRESHHOLD),
            curThreadSize_(0),
            taskSize_(0),
            taskQueThreshHold_(TASK_MAX_THRESHHOLD),
            isPoolRunning_(false)
            {}

//将线程池里面的线程资源都释放掉
ThreadPool::~ThreadPool()
{
    isPoolRunning_ = false;

    // 线程会在notEmpty_处wait，释放锁，处于等待状态。所以现在线程池里，线程要么是处于等待状态，要么是正在运行任务
    // 唤醒处于等待状态的线程，他们现在处于阻塞状态，要抢锁
    //notEmpty_.notify_all();

    // 等待线程池里面所有的线程返回，有两种状态：阻塞 & 正在执行任务中
    // 用户线程执行 ~ThreadPool(), 和线程池里的线程是两种线程，需要线程间的通信
    std::unique_lock<std::mutex> lock(mtx_);

    notEmpty_.notify_all();
    exitCond_.wait(lock, [&]()->bool { threads_.size() == 0; }); // 等待线程对象全部被回收

}

void ThreadPool::setMode(PoolMode mode)
{
    if(checkRunningState()){
        return;
    }
    poolMode_ = mode;

}
void ThreadPool::setTaskQueThreshHold(int threshHold)  //设置任务队列阈值
{
    if(checkRunningState()) return;
    taskQueThreshHold_ = threshHold;
}
void ThreadPool::setThreadThreshHold(int threshHold)  //设置线程阈值,只有在cached模式下才能设置
{
    if(checkRunningState) return;
    if(poolMode_ == PoolMode::MODE_CACHED){
        threadThreshHold_ = threshHold;
    }
}

Result ThreadPool::submitTask(std::shared_ptr<Task> sp)  // 提交任务
{
    //获取锁
    std::unique_lock<std::mutex> lock(mtx_);
    //线程的通信  等待任务队列有空余
    // while(taskQueue_.size() == taskQueThreshHold_)
    // {
    //     notFull_.wait(lock);
    // }
    // 提交任务，最长阻塞时间不超过1s，否则任务提交失败，返回
    //notFull_.wait(lock, [&]()->bool { return taskQueue_.size() < taskQueThreshHold_; });
    if(!notFull_.wait_for(lock, std::chrono::seconds(1), [&]()->bool { return taskQueue_.size() < taskQueThreshHold_; }))
    {
        //等待了1s，任务提交失败
        std::cerr << "task queue is full. submit task fail." << std::endl;
        return Result(sp, false);
    }


    //如果有空余了，就将任务放入任务队列
    taskQueue_.emplace(sp);
    taskSize_++;
    //因为新放了任务，任务队列肯定不空了，在notEmpty上进行通知
    notEmpty_.notify_all();

    // cached模式下，根据空闲线程和任务数量的情况，判断是否需要新建线程
    if(poolMode_ == PoolMode::MODE_CACHED
      && taskSize_ > idleThreadSize_
      && curThreadSize_ < threadThreshHold_)
      {
        // 创建新线程
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        //threads_.emplace_back(ptr);
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        threads_[threadId]->start(); // 让新建的线程运行起来
        curThreadSize_++;
        idleThreadSize_++;
      }

    return Result(sp);
    // return task->Result  这种是不行的，因为task在run之后，就结束了生命周期,Result是要用户使用的,Result的生命周期是长于task的
}

void ThreadPool::threadFunc(int threadId)
{
    auto lastTime = std::chrono::high_resolution_clock().now();

// 所有任务必须执行完成，线程池才可以回收所有资源
    //while(isPoolRunning_)
    for(;;)
    {
        std::shared_ptr<Task> task;
        {
            //先获取锁
            std::unique_lock<std::mutex> lock(mtx_);

            // cached模式下，超过initThreadSize的线程，如果距离上次执行的时间超过了60s，需要回收
            // 当前时间 - 上次执行时间  >= 60s
            // 锁 + 双重判断
            //while(isPoolRunning_ && taskQueue_.size() == 0){
            while(taskQueue_.size() == 0){

                if(!isPoolRunning_){
                    threads_.erase(threadId);
                    std::cout << "threadId: " << std::this_thread::get_id() << " exit!" << std::endl;
                    exitCond_.notify_all();
                    return;  // 线程函数结束，线程结束
                }
                if(poolMode_ == PoolMode::MODE_CACHED)
                {
                // 因为要判断空闲时间，我们让它每1s返回一次，进行：当前时间-上次执行时间
                // 返回可能是因为：超时返回；
                    if(std::cv_status::timeout ==
                        notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                        {
                            auto now = std::chrono::high_resolution_clock().now(); //当前时间
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now-lastTime);
                            if(dur.count() >= THREAD_MAX_IDLE_TIME
                               && curThreadSize_ > initThreadSize_){
                                //开始回收当前线程
                                //线程数量相关变量的修改
                                //将线程从列表中移除
                                //我们需要一个映射关系，threadFunc找到列表中的thread. threadId => thread对象 =>删除
                                threads_.erase(threadId);
                                curThreadSize_--;
                                idleThreadSize_--;

                                std::cout << "threadId: " << std::this_thread::get_id() << " exit!" << std::endl;
                                return;
                            }
                        }
                }
                  else {
                //如果任务队列空的话，要等待
                   //notEmpty_.wait(lock, [&]()->bool {return taskQueue_.size() > 0; });
                   notEmpty_.wait(lock);
                }
                // if(!isPoolRunning_){  // 线程池要结束了，回收线程资源
                //     threads_.erase(threadId);
                //     std::cout << "threadId: " << std::this_thread::get_id() << " exit!" << std::endl;
                //     exitCond_.notify_all();
                //     return;
                // }
            }

            // if(!isPoolRunning_){  // 线程池要结束了，回收线程资源
            //     break;
            // }

            idleThreadSize_--;

            //任务队列不空，取一个任务出来
            auto task = taskQueue_.front();
            taskQueue_.pop();
            taskSize_--;

            //如果还有任务，通知其他线程取任务
            if(taskQueue_.size() > 0){
                notEmpty_.notify_all();
            }

            //取出任务，任务队列不满，可以继续生产任务
            notFull_.notify_all();

            //把锁释放掉
        }

        //线程执行任务
        if(task != nullptr){
            task->exec();
        }

        idleThreadSize_++;

        auto lastTime = std::chrono::high_resolution_clock().now();

    }

    // 线程池结束
    // threads_.erase(threadId);
    // std::cout << "threadId: " << std::this_thread::get_id() << " exit!" << std::endl;
    // exitCond_.notify_all();
}

bool ThreadPool::checkRunningState()
{
    return isPoolRunning_;
}

void ThreadPool::start(int initThreadSize) // 开启线程池
{
    isPoolRunning_ = true;

    //线程的初始个数
    initThreadSize_ = initThreadSize;
    curThreadSize_ = initThreadSize_;
    for(int i = 0;i < initThreadSize_;i++){
        //创建线程对象，把线程函数给到线程对象
        auto ptr = std::make_unique<Thread>(std::bind(&threadFunc, this, std::placeholders::_1));
        //threads_.emplace_back(std::move(ptr));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
    }
    for(int i = 0;i < initThreadSize_;i++){
        // 启动每一个线程
        //threads_[i]->start();
        threads_[i]->start();
        idleThreadSize_++; // 记录空闲线程的数量
    }
}

///////// Thread方法实现
int Thread::generate_ = 0;
Thread::Thread(ThreadFunc func):func_(func), threadId_(generate_++){}

Thread::~Thread()
{

}

void Thread::start()
{
    std::thread t(func_, threadId_);
    t.detach();
}

int Thread::getId() const
{
    return threadId_; 
}

/////////// Task方法实现
Task::Task():result_(nullptr){}

void Task::exec()
{
    if(result_ != nullptr){
        result_->setValue(run());
    }
}

void Task::setResult(Result* res)
{
    result_ = res;
}

//////////// Result方法的实现
Result::Result(std::shared_ptr<Task> task, bool isValid)
              :isValid_(isValid),
               task_(task)
               {
                task_->setResult(this);
               }


Any Result::get(){
    if(isValid_){
        return "";
    }
    sem_.wait();  // 阻塞，等待线程执行完成
    return std::move(any_);
}

void Result::setValue(Any any)  // 谁掉用的？
{
    any_ = std::move(any);
    sem_.post();
}