//#include "threadpool.h"
#include <memory>
#include <iostream>

/*class MyTask : public Task
{
public:
  Any run()
  {
    int sum = 0;
    for(int i = 1;i <= 1000;i++) sum += i;
    return sum;
  }
}; */

/*怎么设计run函数的返回值，可以表示任意类型？
1. 用模板？   no.  virtual和模板类不能在一起。因为virtual需要虚函数表里指针指向函数地址，但是模板类的话，就还没有地址呢
2. Java中有 Object，它是其他类类型的基类
C++17中 Any类型
*/

/*int main()
{
   {
       ThreadPool pool;
       pool.setMode(PoolMode::MODE_CACHED);

       pool.start(4);

       Result res = pool.submitTask(std::make_shared<MyTask>());

       //int sum = res.get().cast_<int>();
   }

   getchar();
} */

int main()
{
  int a = 1, b = 2;
  std::cout << a+b << std::endl;
  return 0;
}

/*
Any类型能接收任意类型
1. 肯定要用到模板
2. run函数返回一个 Test类型，Any类型能接收它，只能是基类和派生类的关系
class Test
{
public:
  int a;
  int b;
};

Any run()
{
    Test test(1,2);
    return test
}
*/

// template <typename T>
//   class Base{
//     public:
//       Base(T data):data_(data){}
//     private:
//     T data_;
//   };

// template <typename T>
// class Any
// {
// public:
//   Any(T data):base(new Base(data)){}

// private:
//   std::unique_ptr<Base<T>> base;
// };

