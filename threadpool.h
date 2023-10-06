#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>

///////////////
class Any
{
public:
  Any() = default;
  ~Any() = default;
  Any(Any&) = delete;
  Any& operator=(const Any&) = delete;
  Any(Any&&) = default;
  Any& operator=(Any&&) = default;

  template <typename T>
  Any(T data):base_(std::make_unique<Derive<T>>(data)){}

  // 把Any对象里面存储的data数据提取出来
  template <typename T>
  T cast_(){
    // 我们需要把Base对象转换成Derive对象
    // 基类 =》派生类    dynamic_cast  RTTI
    Derive<T> *pd = dynamic_cast<Derive<T>*>(base_.get());
    if(pd == nullptr){
        throw "type is unmatch!!!";
    }
    
    return pd->data_;
  }

// 基类类型
  class Base{
  public:
    virtual ~Base() = default;
  };

// 派生类类型
  template <typename T>
  class Derive : public Base{
    private:
      T data_;
    public:
      Derive(T data):data_(data){}
  };
private:

// 基类指针 指向 派生类
  std::unique_ptr<Base> base_;
};

//////////////
// mutex + condition_variable实现semaphore
class Semaphore
{
public:
  Semaphore(int limit = 0):resLimit_(limit), isExit_(true) {}
  ~Semaphore()
  {
    isExit_ = false;
  }

//获取资源
  void wait(){
    if(!isExit_) return;
    std::unique_lock<std::mutex> lock(mtx_);
    cond_.wait(lock, [&]()->bool { return resLimit_ > 0; });
    resLimit_--;
  }

//释放资源
  void post(){
    if(!isExit_) return;
    std::unique_lock<std::mutex> lock(mtx_);
    resLimit_++;
    cond_.notify_all();
  }

private:
  std::atomic_bool isExit_;
  int resLimit_;
  std::mutex mtx_;
  std::condition_variable cond_;
};


////////////
// 实现 接收提交到Task队列中的任务执行完后的返回结果
class Result
{
public:
  Result(std::shared_ptr<Task> task, bool isValid = true);
  ~Result() = default;
  Result(Result&&) = default;

  // setValue方法，获取任务执行完的返回值
  void setValue(Any any);

  // get方法，用户调用这个方法获得task的返回值
  Any get();

private:
  Any any_; // 存储任务的返回值
  Semaphore sem_; // 用于线程间通信
  std::shared_ptr<Task> task_;  // 指向对应的任务对象
  std::atomic_bool isValid_;  // 返回值是否有效
};


/*
example:
ThreadPool pool;
pool.start(4);

class MyTask : public Task{
    public:
      void run(){ //线程代码 }
};

pool.submitTask(std::make_shared<MyTask>());

*/
class ThreadPool
{
public:
  ThreadPool();
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  void setMode(PoolMode mode);
  void setTaskQueThreshHold(int threshHold);  //设置任务队列阈值
  void setThreadThreshHold(int threshHold); //设置线程阈值
  Result submitTask(std::shared_ptr<Task> sp);  // 提交任务
  // 线程初始的默认值为当前cpu的核心数量
  void start(int initThreadSize = std::thread::hardware_concurrency()); // 开启线程池

private:
  void threadFunc(int); // 线程的运行函数
  bool checkRunningState();

private:
 // std::vector<std::unique_ptr<Thread>> threads_;  // 线程, 使用智能指针，这样内存会自动释放。裸指针的话，还需要我们手动释放
  std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表
  std::size_t initThreadSize_;  // 线程的初始数量
  std::atomic_uint idleThreadSize_; //空闲线程的数量
  std::atomic_uint curThreadSize_; //记录当前线程池里线程的总数量
  std::size_t threadThreshHold_;  //线程数量的阈值


// 用智能指针，不能用裸指针，因为不知道传入的任务对象是不是临时对象
  std::queue<std::shared_ptr<Task>> taskQueue_;
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

  Thread(ThreadFunc func);
  ~Thread();
  void start();
  int getId() const;
private:
  ThreadFunc func_;
  static int generate_;
  int threadId_; //保存线程id --- 不是真的线程id，是我们generate自增
};

enum class PoolMode
{
    MODE_FIXED,  // 数量固定
    MODE_CACHED,  // 动态变化
};

class Task
{
public:
  Task();
  ~Task() = default;

  virtual Any run() = 0;
  void exec();
  void setResult(Result* res);

private:
  Result* result_;  // 不能用智能指针，要不然会和Result里的shared_ptr形成循环引用
};

#endif