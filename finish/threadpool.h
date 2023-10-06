#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 200;
const int THREAD_MAX_IDLE_TIME = 60; //单位：秒

class ThreadPool
{
public:
  ThreadPool():
            poolMode_(PoolMode::MODE_FIXED),
            initThreadSize_(0),
            idleThreadSize_(0),
            threadThreshHold_(THREAD_MAX_THRESHHOLD),
            curThreadSize_(0),
            taskSize_(0),
            taskQueThreshHold_(TASK_MAX_THRESHHOLD),
            isPoolRunning_(false)
            {}
  ~ThreadPool()
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
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  void setMode(PoolMode mode)
  {
    if(checkRunningState()){
        return;
    }
    poolMode_ = mode;
  }
  void setTaskQueThreshHold(int threshHold){  //设置任务队列阈值
    if(checkRunningState()) return;
    taskQueThreshHold_ = threshHold;
  }
  void setThreadThreshHold(int threshHold){  //设置线程阈值
    if(checkRunningState) return;
    if(poolMode_ == PoolMode::MODE_CACHED){
        threadThreshHold_ = threshHold;
    }
  }


// 使用可变参模板编程，让submitTask可以接收任意任务函数和任意数量的参数
  template <typename Fun, typename ... Args>
  auto submitTask(Fun&& func, Args&& ...args) -> std::future<decltype(func(args...))>
  {
    using Rtype = decltype(func(args...));
    auto task = std::make_shared<std::packaged_task<Rtype()>>(
        std::bind(std::forward<Fun>(func), std::forward<Args>(args...)));
    future<Rtype> result = task->get_future();

    std::unique_lock<std::mutex> lock(mtx_);
    //线程的通信  等待任务队列有空余
    // 提交任务，最长阻塞时间不超过1s，否则任务提交失败，返回
    if(!notFull_.wait_for(lock, std::chrono::seconds(1), [&]()->bool { return taskQueue_.size() < taskQueThreshHold_; }))
    {
        //等待了1s，任务提交失败
        std::cerr << "task queue is full. submit task fail." << std::endl;
        auto task = std::make_shared<std::packaged_task<Rtype()>>(
            []() ->Rtype { return Rtype(); });
        (*task)();
        return task.get_future();
    }


    //如果有空余了，就将任务放入任务队列
    //taskQueue_.emplace(sp);
    // using Task = std::function<void()>
    taskQueue_.emplace([task](){ (*task)(); });

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

    return result;
  }
  // 线程初始的默认值为当前cpu的核心数量
  void start(int initThreadSize = std::thread::hardware_concurrency()) // 开启线程池
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

private:
  void threadFunc(int threadId) // 线程的运行函数
  {
     auto lastTime = std::chrono::high_resolution_clock().now();

// 所有任务必须执行完成，线程池才可以回收所有资源
    //while(isPoolRunning_)
    for(;;)
    {
        Task task;
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
                   notEmpty_.wait(lock);
                }
            }

            idleThreadSize_--;

            //任务队列不空，取一个任务出来
            task = taskQueue_.front();
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

        //线程执行任务    function<void()>
        if(task != nullptr){
            task();
        }

        idleThreadSize_++;

        auto lastTime = std::chrono::high_resolution_clock().now();

    }

  }

  bool checkRunningState()
  {
    return isPoolRunning_;
  }

private:
 // std::vector<std::unique_ptr<Thread>> threads_;  // 线程, 使用智能指针，这样内存会自动释放。裸指针的话，还需要我们手动释放
  std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表
  std::size_t initThreadSize_;  // 线程的初始数量
  std::atomic_uint idleThreadSize_; //空闲线程的数量
  std::atomic_uint curThreadSize_; //记录当前线程池里线程的总数量
  std::size_t threadThreshHold_;  //线程数量的阈值


// Task  =>  函数对象
  using Task = std::function<void()>;
  std::queue<Task> taskQueue_;
  std::atomic_uint taskSize_;  // 任务数量
  std::size_t taskQueThreshHold_;  // 任务队列阈值
  std::mutex mtx_;
  std::condition_variable notFull_; // 表示任务队列不满
  std::condition_variable notEmpty_; // 表示任务队列不空
  std::condition_variable exitCond_; // 等待线程所有资源回收

  PoolMode poolMode_;  //线程池类型

  std::atomic_bool isPoolRunning_; // 线程池是否start
};

class Thread
{
public:
  using ThreadFunc = std::function<void(int)>;

  Thread(ThreadFunc func):func_(func), threadId_(generate_++){}
  ~Thread();
  void start()
  {
    std::thread t(func_, threadId_);
    t.detach();
  }
  int getId() const
  {
    return threadId_; 
  }
private:
  ThreadFunc func_;
  static int generate_;
  int threadId_; //保存线程id --- 不是真的线程id，是我们generate自增
};

int Thread::generate_ = 0;

enum class PoolMode
{
    MODE_FIXED,  // 数量固定
    MODE_CACHED,  // 动态变化
};

#endif